#include "neon/core/pack.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "neon/core/serialize.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace neon::core {
namespace {

// Fixed magic that must appear at the start of a pack stream.
const uint8_t kPackMagic[8] = {'N', 'E', 'O', 'N', 'P', 'A', 'C', 'K'};

// Fixed bytes per directory entry after the path: offset(8) + compSize(4) +
// rawSize(4) + crc32(4) + method(1).
constexpr size_t kEntryTailBytes = 21;

// Smallest possible serialized entry: pathLen(4) + at least one path byte + tail.
constexpr size_t kMinEntryBytes = 4 + 1 + kEntryTailBytes;

void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }

void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void PutU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (56 - 8 * i)));
}

void PatchU32(std::vector<uint8_t>& b, size_t pos, uint32_t v) {
    assert(pos + 4 <= b.size());
    b[pos + 0] = static_cast<uint8_t>(v >> 24);
    b[pos + 1] = static_cast<uint8_t>(v >> 16);
    b[pos + 2] = static_cast<uint8_t>(v >> 8);
    b[pos + 3] = static_cast<uint8_t>(v & 0xFFu);
}

uint32_t GetU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t GetU64(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) | (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) | (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) | (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) | static_cast<uint64_t>(p[7]);
}

} // namespace

core::Status PackWriter::AddFile(const std::string& virtualPath, const std::vector<uint8_t>& data) {
    if (virtualPath.empty()) return core::Status::Err("pack: empty path");
    if (virtualPath.size() > UINT32_MAX)
        return core::Status::Err("pack: path too long: " + virtualPath);
    for (const Entry& e : entries_)
        if (e.path == virtualPath)
            return core::Status::Err("pack: duplicate path: " + virtualPath);
    // The per-entry size field is u32; a blob that does not fit would otherwise
    // be silently truncated into a corrupt pack, so reject it up front.
    if (data.size() > UINT32_MAX)
        return core::Status::Err("pack: file too large to pack: " + virtualPath);
    entries_.push_back({virtualPath, data});
    return core::Status::Ok(true);
}

std::vector<uint8_t> PackWriter::Build() const {
    std::vector<Entry> sorted = entries_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });

    // Serialize the header and the index first so every block offset is known
    // before any data is appended.
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), kPackMagic, kPackMagic + 8);
    PutU32(buf, kPackVersion);
    PutU32(buf, 0); // index CRC placeholder, patched below
    PutU32(buf, static_cast<uint32_t>(sorted.size()));

    // u64 accumulator: a pack whose index exceeds 4 GiB (64-bit builds) must
    // not overflow a 32-bit size_t before the u64 offset path starts.
    uint64_t blockStart = kPackHeaderBytes + 4; // past the fileCount field
    for (const Entry& e : sorted) blockStart += 4 + e.path.size() + kEntryTailBytes;

    uint64_t offset = blockStart;
    for (const Entry& e : sorted) {
        assert(e.path.size() <= UINT32_MAX && e.data.size() <= UINT32_MAX);
        PutU32(buf, static_cast<uint32_t>(e.path.size()));
        buf.insert(buf.end(), e.path.begin(), e.path.end());
        PutU64(buf, offset);
        const uint32_t size = static_cast<uint32_t>(e.data.size());
        PutU32(buf, size); // compSize == rawSize: entries are stored raw
        PutU32(buf, size); // rawSize
        PutU32(buf, Crc32(e.data.data(), e.data.size()));
        PutU8(buf, kPackCompressionStore);
        offset += size;
    }

    // Stamp the index CRC over fileCount + all entries (everything after the
    // fixed header).
    const uint32_t indexCrc = Crc32(buf.data() + kPackHeaderBytes, buf.size() - kPackHeaderBytes);
    PatchU32(buf, 8 + 4, indexCrc);

    // Append data blocks in the same sorted order.
    for (const Entry& e : sorted) buf.insert(buf.end(), e.data.begin(), e.data.end());

    return buf;
}

PackReader::PackReader(const std::vector<uint8_t>& bytes) : buf_(bytes) { ParseIndex(); }

bool PackReader::Valid() const { return error_.empty(); }

const std::string& PackReader::Error() const { return error_; }

size_t PackReader::FileCount() const { return entries_.size(); }

bool PackReader::Has(const std::string& virtualPath) const {
    if (!Valid()) return false;
    auto it = std::lower_bound(entries_.begin(), entries_.end(), virtualPath,
                               [](const Entry& e, const std::string& p) { return e.path < p; });
    return it != entries_.end() && it->path == virtualPath;
}

std::vector<std::string> PackReader::Enumerate() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.path);
    return out;
}

bool PackReader::ParseIndex() {
    if (buf_.size() < kPackHeaderBytes) {
        error_ = "pack: buffer too short for header";
        return false;
    }
    if (std::memcmp(buf_.data(), kPackMagic, sizeof(kPackMagic)) != 0) {
        error_ = "pack: bad magic";
        return false;
    }
    if (GetU32(buf_.data() + 8) != kPackVersion) {
        error_ = "pack: unsupported version";
        return false;
    }
    if (buf_.size() < kPackHeaderBytes + 4) {
        error_ = "pack: truncated file count";
        return false;
    }
    const uint32_t count = GetU32(buf_.data() + 16);
    // Each entry needs at least kMinEntryBytes in the index; reject a count the
    // buffer could not possibly hold before allocating/looping.
    if (count > (buf_.size() - (kPackHeaderBytes + 4)) / kMinEntryBytes) {
        error_ = "pack: implausible file count";
        return false;
    }

    size_t pos = kPackHeaderBytes + 4;
    std::vector<Entry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 4 > buf_.size()) {
            error_ = "pack: truncated entry path";
            return false;
        }
        const uint32_t pathLen = GetU32(buf_.data() + pos);
        pos += 4;
        if (pathLen > buf_.size() - pos) {
            error_ = "pack: entry path length exceeds buffer";
            return false;
        }
        std::string path(reinterpret_cast<const char*>(buf_.data() + pos), pathLen);
        pos += pathLen;
        if (path.empty()) {
            error_ = "pack: empty path in index";
            return false;
        }
        if (pos + kEntryTailBytes > buf_.size()) {
            error_ = "pack: truncated entry";
            return false;
        }
        Entry e;
        e.path = std::move(path);
        e.offset = GetU64(buf_.data() + pos);
        e.compSize = GetU32(buf_.data() + pos + 8);
        e.rawSize = GetU32(buf_.data() + pos + 12);
        e.crc32 = GetU32(buf_.data() + pos + 16);
        e.method = buf_[pos + 20];
        pos += kEntryTailBytes;
        // The writer emits a strictly increasing sorted index; duplicates or
        // out-of-order entries are rejected as corruption.
        if (!entries.empty() && entries.back().path >= e.path) {
            error_ = "pack: index not sorted";
            return false;
        }
        entries.push_back(std::move(e));
    }

    const uint32_t storedCrc = GetU32(buf_.data() + 12);
    const uint32_t actualCrc = Crc32(buf_.data() + 16, pos - 16);
    if (storedCrc != actualCrc) {
        error_ = "pack: index crc mismatch";
        return false;
    }

    for (const Entry& e : entries) {
        if (e.offset > buf_.size() || e.compSize > buf_.size() - static_cast<size_t>(e.offset)) {
            error_ = "pack: block out of bounds: " + e.path;
            return false;
        }
    }

    entries_ = std::move(entries);
    return true;
}

core::Result<std::vector<uint8_t>> PackReader::Read(const std::string& virtualPath) const {
    if (!Valid()) return core::Result<std::vector<uint8_t>>::Err(error_);
    auto it = std::lower_bound(entries_.begin(), entries_.end(), virtualPath,
                               [](const Entry& e, const std::string& p) { return e.path < p; });
    if (it == entries_.end() || it->path != virtualPath)
        return core::Result<std::vector<uint8_t>>::Err("pack: entry not found: " + virtualPath);

    const Entry& e = *it;
    if (e.offset > buf_.size() || e.compSize > buf_.size() - static_cast<size_t>(e.offset))
        return core::Result<std::vector<uint8_t>>::Err("pack: block out of bounds: " + virtualPath);
    if (e.method != kPackCompressionStore)
        return core::Result<std::vector<uint8_t>>::Err("pack: unsupported compression method: " + virtualPath);
    if (e.compSize != e.rawSize)
        return core::Result<std::vector<uint8_t>>::Err("pack: block size mismatch: " + virtualPath);

    const uint8_t* begin = buf_.data() + e.offset;
    std::vector<uint8_t> out(begin, begin + e.compSize);
    if (Crc32(out.data(), out.size()) != e.crc32)
        return core::Result<std::vector<uint8_t>>::Err("pack: crc mismatch: " + virtualPath);
    return core::Result<std::vector<uint8_t>>::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// Unpack: expand a pack's entries into a real directory tree
// ---------------------------------------------------------------------------

namespace {

bool MakeOneDir(const std::string& path) {
#if defined(_WIN32)
    return CreateDirectoryA(path.c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

// Creates every directory component of `path` up to (not including) the final
// segment, so the caller can then open it as a file. Existing directories are
// fine; the whole chain is created when missing.
bool MakeParentDirs(const std::string& path) {
    size_t start = 0;
    while (true) {
        const size_t slash = path.find_first_of("/\\", start);
        if (slash == std::string::npos) break;
        const std::string comp = path.substr(0, slash);
        // A Windows drive root ("C:") already exists; skip creation.
        if (!comp.empty() && !(comp.size() == 2 && comp[1] == ':')) {
            if (!MakeOneDir(comp)) return false;
        }
        start = slash + 1;
    }
    return true;
}

// True when a virtual path could escape `destDir`: absolute (drive letter or
// leading separator) or containing a ".." segment. Packs are produced by the
// packager with normalized relative keys, so this is a defense-in-depth guard.
bool UnsafeEntryPath(const std::string& p) {
    return p.empty() || IsUnsafeRelPath(p);
}

} // namespace

bool IsUnsafeRelPath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (p.size() >= 2 && p[1] == ':') return true;
    size_t start = 0;
    while (start <= p.size()) {
        const size_t slash = p.find_first_of("/\\", start);
        const size_t end = slash == std::string::npos ? p.size() : slash;
        if (end - start == 2 && p.compare(start, 2, "..") == 0) return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

core::Status Unpack(const PackReader& reader, const std::string& destDir) {
    if (!reader.Valid()) return core::Status::Err("unpack: " + reader.Error());
    if (destDir.empty()) return core::Status::Err("unpack: empty destination directory");
    if (!MakeOneDir(destDir))
        return core::Status::Err("unpack: cannot create destination directory '" + destDir + "'");

    for (const std::string& vpath : reader.Enumerate()) {
        if (UnsafeEntryPath(vpath))
            return core::Status::Err("unpack: unsafe entry path: " + vpath);
        core::Result<std::vector<uint8_t>> data = reader.Read(vpath);
        if (!data.Ok()) return core::Status::Err("unpack: " + data.Error());
        const std::string out = destDir + "/" + vpath;
        if (!MakeParentDirs(out)) {
            return core::Status::Err("unpack: cannot create directories for '" + vpath + "'");
        }
        std::FILE* f = std::fopen(out.c_str(), "wb");
        if (!f) return core::Status::Err("unpack: cannot open '" + vpath + "' for writing");
        const std::vector<uint8_t>& bytes = data.Value();
        const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        std::fclose(f);
        if (!ok) return core::Status::Err("unpack: failed to write '" + vpath + "'");
    }
    return core::Status::Ok(true);
}

} // namespace neon::core

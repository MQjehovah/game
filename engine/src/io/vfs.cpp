#include "neon/io/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace neon::io {

namespace {

// True when `path` contains a segment that could escape a mount root.
bool IsSafeSegment(const std::string& seg) {
    return !seg.empty() && seg != "." && seg != ".." && seg.find('\\') == std::string::npos;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/' || b.front() == '/') return a + b;
    return a + "/" + b;
}

} // namespace

bool NormalizeVirtualPath(const std::string& path, std::string& out) {
    out.clear();
    if (path.empty()) return false;
    std::vector<std::string> segs;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) {
                segs.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) segs.push_back(cur);

    std::vector<std::string> stack;
    for (const std::string& seg : segs) {
        if (seg == ".") continue;
        if (seg == "..") {
            if (stack.empty()) return false; // escape above the root
            stack.pop_back();
            continue;
        }
        if (!IsSafeSegment(seg)) return false;
        stack.push_back(seg);
    }
    if (stack.empty()) return false;
    out = stack[0];
    for (size_t i = 1; i < stack.size(); ++i) out += "/" + stack[i];
    return true;
}

// ---------------------------------------------------------------------------
// DiskFileSystem
// ---------------------------------------------------------------------------

DiskFileSystem::DiskFileSystem(std::string rootDir) : root_(std::move(rootDir)) {
    // Normalize trailing slashes so JoinPath behaves.
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
}

std::string DiskFileSystem::Resolve(const std::string& path) const {
    std::string normalized;
    if (!NormalizeVirtualPath(path, normalized)) return {};
    return root_.empty() ? normalized : root_ + "/" + normalized;
}

bool DiskFileSystem::Exists(const std::string& path) const {
    const std::string abs = Resolve(path);
    if (abs.empty()) return false;
#if defined(_WIN32)
    return GetFileAttributesA(abs.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(abs.c_str(), &st) == 0;
#endif
}

core::Result<std::vector<uint8_t>> DiskFileSystem::ReadFile(const std::string& path) const {
    const std::string abs = Resolve(path);
    if (abs.empty()) return core::Result<std::vector<uint8_t>>::Err("io: unsafe path");
    std::ifstream in(abs, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return core::Result<std::vector<uint8_t>>::Err("io: cannot open '" + path + "'");
    const std::streamsize size = in.tellg();
    if (size < 0) return core::Result<std::vector<uint8_t>>::Err("io: cannot size '" + path + "'");
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (in.bad()) return core::Result<std::vector<uint8_t>>::Err("io: read failed '" + path + "'");
    return core::Result<std::vector<uint8_t>>::Ok(std::move(bytes));
}

uint64_t DiskFileSystem::FileMTime(const std::string& path) const {
    const std::string abs = Resolve(path);
    if (abs.empty()) return 0;
#if defined(_WIN32)
    struct _stat64 st;
    if (::_stat64(abs.c_str(), &st) == 0) return static_cast<uint64_t>(st.st_mtime);
#else
    struct stat st;
    if (::stat(abs.c_str(), &st) == 0) return static_cast<uint64_t>(st.st_mtime);
#endif
    return 0;
}

void DiskFileSystem::ListDir(const std::string& absDir, const std::string& prefix,
                             bool recursive, std::vector<std::string>& out) const {
#if defined(_WIN32)
    const std::string pattern = absDir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<std::string, bool>> entries; // name, isDir
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        entries.emplace_back(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(entries.begin(), entries.end());
    for (const auto& e : entries) {
        const std::string rel = prefix.empty() ? e.first : prefix + "/" + e.first;
        if (e.second) {
            if (recursive) ListDir(absDir + "/" + e.first, rel, true, out);
        } else {
            out.push_back(rel);
        }
    }
#else
    DIR* d = ::opendir(absDir.c_str());
    if (!d) return;
    std::vector<std::pair<std::string, bool>> entries;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = absDir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        entries.emplace_back(name, S_ISDIR(st.st_mode));
    }
    ::closedir(d);
    std::sort(entries.begin(), entries.end());
    for (const auto& e : entries) {
        const std::string rel = prefix.empty() ? e.first : prefix + "/" + e.first;
        if (e.second) {
            if (recursive) ListDir(absDir + "/" + e.first, rel, true, out);
        } else {
            out.push_back(rel);
        }
    }
#endif
}

std::vector<std::string> DiskFileSystem::ListFiles(const std::string& dir, bool recursive) const {
    std::string normalized;
    if (!dir.empty() && !NormalizeVirtualPath(dir, normalized)) return {};
    const std::string abs = root_.empty() ? normalized : JoinPath(root_, normalized);
    std::vector<std::string> out;
    if (dir.empty()) {
        ListDir(root_, "", recursive, out);
    } else {
        ListDir(abs, normalized, recursive, out);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// PackFileSystem
// ---------------------------------------------------------------------------

PackFileSystem::PackFileSystem(std::vector<uint8_t> packBytes)
    : bytes_(std::move(packBytes)), reader_(bytes_) {}

bool PackFileSystem::Exists(const std::string& path) const {
    std::string normalized;
    return NormalizeVirtualPath(path, normalized) && reader_.Has(normalized);
}

core::Result<std::vector<uint8_t>> PackFileSystem::ReadFile(const std::string& path) const {
    std::string normalized;
    if (!NormalizeVirtualPath(path, normalized))
        return core::Result<std::vector<uint8_t>>::Err("io: unsafe path");
    return reader_.Read(normalized);
}

uint64_t PackFileSystem::FileMTime(const std::string&) const {
    return 0; // pack content is immutable
}

std::vector<std::string> PackFileSystem::ListFiles(const std::string& dir, bool recursive) const {
    std::string normalized;
    if (!dir.empty() && !NormalizeVirtualPath(dir, normalized)) return {};
    const std::string prefix = normalized.empty() ? "" : normalized + "/";
    std::vector<std::string> out;
    for (const std::string& p : reader_.Enumerate()) {
        if (prefix.empty()) {
            out.push_back(p);
            continue;
        }
        if (p.compare(0, prefix.size(), prefix) != 0) continue;
        if (!recursive) {
            const std::string rest = p.substr(prefix.size());
            if (rest.find('/') != std::string::npos) continue;
        }
        out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// MountStack
// ---------------------------------------------------------------------------

void MountStack::Mount(std::shared_ptr<IFileSystem> layer) {
    if (!layer) return;
    layers_.insert(layers_.begin(), std::move(layer)); // front = highest priority
}

void MountStack::Unmount(const IFileSystem* layer) {
    for (auto it = layers_.begin(); it != layers_.end(); ++it) {
        if (it->get() == layer) {
            layers_.erase(it);
            return;
        }
    }
}

void MountStack::Clear() { layers_.clear(); }

const IFileSystem* MountStack::OwnerOf(const std::string& path) const {
    for (const auto& layer : layers_)
        if (layer->Exists(path)) return layer.get();
    return nullptr;
}

bool MountStack::Exists(const std::string& path) const {
    return OwnerOf(path) != nullptr;
}

core::Result<std::vector<uint8_t>> MountStack::ReadFile(const std::string& path) const {
    for (const auto& layer : layers_) {
        if (!layer->Exists(path)) continue;
        return layer->ReadFile(path);
    }
    return core::Result<std::vector<uint8_t>>::Err("io: not found '" + path + "'");
}

uint64_t MountStack::FileMTime(const std::string& path) const {
    for (const auto& layer : layers_) {
        if (!layer->Exists(path)) continue;
        return layer->FileMTime(path);
    }
    return 0;
}

std::vector<std::string> MountStack::ListFiles(const std::string& dir, bool recursive) const {
    std::set<std::string> merged;
    for (const auto& layer : layers_) {
        const std::vector<std::string> files = layer->ListFiles(dir, recursive);
        merged.insert(files.begin(), files.end());
    }
    return std::vector<std::string>(merged.begin(), merged.end());
}

} // namespace neon::io

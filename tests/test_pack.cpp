#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/core/pack.hpp"
#include "neon/core/serialize.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

// UTF-8 path keys round-trip through the pack as opaque strings; the pack
// never touches the real filesystem, so these are safe on any platform.
const std::string kDirPath = "b/子目录/c.png";
const std::string kTextA = "hello neon pack";
const std::string kTextC = "\x89PNG\r\n\x1a\n" "fake png bytes";

std::vector<uint8_t> ToBytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Big-endian field accessors used to hand-craft corrupted pack streams.
uint32_t GetU32At(const std::vector<uint8_t>& b, size_t pos) {
    return (static_cast<uint32_t>(b[pos]) << 24) | (static_cast<uint32_t>(b[pos + 1]) << 16) |
           (static_cast<uint32_t>(b[pos + 2]) << 8) | static_cast<uint32_t>(b[pos + 3]);
}

uint64_t GetU64At(const std::vector<uint8_t>& b, size_t pos) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) v = (v << 8) | b[pos + i];
    return v;
}

void PutU32At(std::vector<uint8_t>& b, size_t pos, uint32_t v) {
    b[pos + 0] = static_cast<uint8_t>(v >> 24);
    b[pos + 1] = static_cast<uint8_t>(v >> 16);
    b[pos + 2] = static_cast<uint8_t>(v >> 8);
    b[pos + 3] = static_cast<uint8_t>(v & 0xFFu);
}

void PutU64At(std::vector<uint8_t>& b, size_t pos, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[pos + i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}

// Rewrites the index CRC field for a pack whose index occupies
// bytes[16, 16 + indexLen). Lets tests mutate the index while keeping the CRC
// internally consistent, so the failure under test is the specific corruption.
void PatchIndexCrc(std::vector<uint8_t>& bytes, size_t indexLen) {
    const uint32_t crc = core::Crc32(bytes.data() + 16, indexLen);
    PutU32At(bytes, 12, crc);
}

// Field offsets within a pack produced from a single AddFile entry.
struct SingleEntryLayout {
    size_t offPos = 0;    // block offset (u64)
    size_t compPos = 0;   // compressed size (u32)
    size_t methodPos = 0; // compression method (u8)
    size_t indexLen = 0;  // bytes of the index (fileCount + entry)
};

SingleEntryLayout LayoutOf(const std::vector<uint8_t>& bytes) {
    SingleEntryLayout l;
    const size_t entryStart = 16 + 4; // header + fileCount
    const uint32_t pathLen = GetU32At(bytes, entryStart);
    const size_t tailPos = entryStart + 4 + pathLen;
    l.offPos = tailPos;
    l.compPos = tailPos + 8;
    l.methodPos = tailPos + 20; // offset(8)+comp(4)+raw(4)+crc(4) precedes it
    l.indexLen = 4 + (4 + pathLen + 21);
    return l;
}

} // namespace

TEST(PackRoundTrip) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes(kTextA)).Ok());
    CHECK(w.AddFile(kDirPath, ToBytes(kTextC)).Ok());

    std::vector<uint8_t> bytes = w.Build();

    core::PackReader r(bytes);
    CHECK(r.Valid());
    CHECK_EQ(r.FileCount(), 2u);
    CHECK(r.Has("a.txt"));
    CHECK(r.Has(kDirPath));
    CHECK(!r.Has("nope.bin"));

    std::vector<std::string> list = r.Enumerate();
    CHECK_EQ(list.size(), 2u);
    if (list.size() >= 2u) {
        CHECK_EQ(list[0], std::string("a.txt"));
        CHECK_EQ(list[1], kDirPath);
    }

    core::Result<std::vector<uint8_t>> ra = r.Read("a.txt");
    CHECK(ra.Ok());
    CHECK_EQ(std::string(ra.Value().begin(), ra.Value().end()), kTextA);

    core::Result<std::vector<uint8_t>> rc = r.Read(kDirPath);
    CHECK(rc.Ok());
    CHECK_EQ(std::string(rc.Value().begin(), rc.Value().end()), kTextC);
}

TEST(PackMissingFileNotFound) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("x")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());

    core::Result<std::vector<uint8_t>> res = r.Read("missing/文件.txt");
    CHECK(!res.Ok());
    CHECK(res.Error().find("not found") != std::string::npos);
}

TEST(PackCorruptedBlockDetected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("aaaa")).Ok());
    CHECK(w.AddFile(kDirPath, ToBytes("cccccccccccccccc")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    CHECK(!bytes.empty());
    if (bytes.empty()) return;
    bytes.back() ^= 0xFF; // corrupt the trailing data block (c.png)

    core::PackReader r(bytes);
    CHECK(r.Valid()); // index itself is still intact
    core::Result<std::vector<uint8_t>> ok = r.Read("a.txt");
    CHECK(ok.Ok()); // earlier block untouched
    core::Result<std::vector<uint8_t>> bad = r.Read(kDirPath);
    CHECK(!bad.Ok()); // last block fails CRC verification
}

TEST(PackCorruptedIndexRejected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("abc")).Ok());
    CHECK(w.AddFile(kDirPath, ToBytes("xyz")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    // A valid pack always carries the fileCount field right after the 16-byte
    // header; corrupting it must invalidate the index CRC.
    CHECK(bytes.size() >= 20u);
    if (bytes.size() < 20u) return;
    bytes[16] ^= 0xFF;

    core::PackReader r(bytes);
    CHECK(!r.Valid());
    CHECK(!r.Error().empty());
}

TEST(PackWrongVersionRejected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("x")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    bytes[8] = 0; bytes[9] = 0; bytes[10] = 0; bytes[11] = 2; // version 2

    core::PackReader r(bytes);
    CHECK(!r.Valid());
    CHECK(r.Error().find("version") != std::string::npos);
}

TEST(PackTooShortRejected) {
    std::vector<uint8_t> none;
    core::PackReader empty(none);
    CHECK(!empty.Valid());

    std::vector<uint8_t> shortBuf(15, 0); // one byte short of the 16-byte header
    core::PackReader tooShort(shortBuf);
    CHECK(!tooShort.Valid());
}

TEST(PackWrongMagicRejected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("x")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    CHECK(!bytes.empty());
    if (bytes.empty()) return;
    bytes[0] = 'X';

    core::PackReader r(bytes);
    CHECK(!r.Valid());
}

TEST(PackTruncatedRejected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("payload-payload")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    CHECK(bytes.size() >= 20u);
    if (bytes.size() < 20u) return;
    bytes.resize(bytes.size() - 2); // cut into the last data block

    core::PackReader r(bytes);
    CHECK(!r.Valid());
}

TEST(PackTruncatedIndexRejected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("payload")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    // Cut inside the first index entry: the pathLen field survives but the
    // path bytes do not, so pathLen exceeds the remaining buffer.
    CHECK(bytes.size() >= 24u);
    if (bytes.size() < 24u) return;
    bytes.resize(24); // header(16) + fileCount(4) + pathLen(4)

    core::PackReader r(bytes);
    CHECK(!r.Valid());
}

TEST(PackEmptyPack) {
    core::PackWriter w;
    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());
    CHECK_EQ(r.FileCount(), 0u);
    CHECK(r.Enumerate().empty());
    core::Result<std::vector<uint8_t>> res = r.Read("a.txt");
    CHECK(!res.Ok());
}

TEST(PackDuplicatePathRejected) {
    core::PackWriter w;
    CHECK(w.AddFile("dup.txt", ToBytes("first")).Ok());
    CHECK(!w.AddFile("dup.txt", ToBytes("second")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());
    CHECK_EQ(r.FileCount(), 1u);
    core::Result<std::vector<uint8_t>> res = r.Read("dup.txt");
    CHECK(res.Ok());
    CHECK_EQ(std::string(res.Value().begin(), res.Value().end()), std::string("first"));
}

TEST(PackEmptyPathRejected) {
    core::PackWriter w;
    CHECK(!w.AddFile("", ToBytes("data")).Ok());
}

TEST(PackEmptyFileRoundTrip) {
    core::PackWriter w;
    CHECK(w.AddFile("empty.bin", {}).Ok());
    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());
    core::Result<std::vector<uint8_t>> res = r.Read("empty.bin");
    CHECK(res.Ok());
    CHECK(res.Value().empty());
}

TEST(PackUtf8PathsRoundTrip) {
    core::PackWriter w;
    const std::string p1 = "场景/map/平原.txt";
    const std::string p2 = "データ/キャラクター.png";
    const std::string p3 = "emoji/🎮.bin";
    CHECK(w.AddFile(p1, ToBytes("one")).Ok());
    CHECK(w.AddFile(p2, ToBytes("two")).Ok());
    CHECK(w.AddFile(p3, ToBytes("three")).Ok());

    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());
    std::vector<std::string> list = r.Enumerate();
    CHECK_EQ(list.size(), 3u);

    core::Result<std::vector<uint8_t>> a = r.Read(p1);
    core::Result<std::vector<uint8_t>> b = r.Read(p2);
    core::Result<std::vector<uint8_t>> c = r.Read(p3);
    CHECK(a.Ok());
    CHECK(b.Ok());
    CHECK(c.Ok());
    CHECK_EQ(std::string(a.Value().begin(), a.Value().end()), std::string("one"));
    CHECK_EQ(std::string(b.Value().begin(), b.Value().end()), std::string("two"));
    CHECK_EQ(std::string(c.Value().begin(), c.Value().end()), std::string("three"));
}

TEST(PackImplausibleCountRejected) {
    // A tiny crafted buffer claiming an enormous entry count must be rejected
    // without attempting to allocate or loop (DoS guard).
    std::vector<uint8_t> bytes(32, 0);
    std::memcpy(bytes.data(), "NEONPACK", 8);
    bytes[8] = 0; bytes[9] = 0; bytes[10] = 0; bytes[11] = 1; // version 1
    bytes[16] = 0xFF; bytes[17] = 0xFF; bytes[18] = 0xFF; bytes[19] = 0xFF;

    core::PackReader r(bytes);
    CHECK(!r.Valid());
}

TEST(PackInsertionOrderIndependent) {
    core::PackWriter w;
    CHECK(w.AddFile("z.txt", ToBytes("1")).Ok());
    CHECK(w.AddFile("m.txt", ToBytes("2")).Ok());
    CHECK(w.AddFile("a.txt", ToBytes("3")).Ok());
    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());
    std::vector<std::string> list = r.Enumerate();
    CHECK_EQ(list.size(), 3u);
    if (list.size() >= 3u) {
        CHECK_EQ(list[0], std::string("a.txt"));
        CHECK_EQ(list[1], std::string("m.txt"));
        CHECK_EQ(list[2], std::string("z.txt"));
    }
}

TEST(PackDeterministicBuild) {
    const std::vector<uint8_t> a = ToBytes("alpha");
    const std::vector<uint8_t> b = ToBytes("beta beta");
    const std::vector<uint8_t> c = ToBytes("子目录 content");

    core::PackWriter w1;
    CHECK(w1.AddFile("z.txt", a).Ok());
    CHECK(w1.AddFile("a.txt", b).Ok());
    CHECK(w1.AddFile("m/子/x.png", c).Ok());
    std::vector<uint8_t> first = w1.Build();

    core::PackWriter w2; // different insertion order, same content
    CHECK(w2.AddFile("m/子/x.png", c).Ok());
    CHECK(w2.AddFile("a.txt", b).Ok());
    CHECK(w2.AddFile("z.txt", a).Ok());
    std::vector<uint8_t> second = w2.Build();

    CHECK(first == second);
    // Rebuilding the same writer is also stable.
    CHECK(w1.Build() == first);
}

TEST(PackLargeFileRoundTrip) {
    core::PackWriter w;
    const size_t n = 256 * 1024;
    std::vector<uint8_t> big(n);
    uint32_t state = 12345u;
    for (size_t i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        big[i] = static_cast<uint8_t>(state >> 24);
    }
    CHECK(w.AddFile("big.bin", big).Ok());
    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());
    core::Result<std::vector<uint8_t>> res = r.Read("big.bin");
    CHECK(res.Ok());
    CHECK_EQ(res.Value().size(), n);
    CHECK(res.Value() == big);
}

TEST(PackIndexCrcValidBlockOutOfBounds) {
    // Index CRC is valid, but the block offset points past the end of the pack:
    // must be rejected at construction, no crash.
    core::PackWriter w;
    CHECK(w.AddFile("a.bin", ToBytes("hello")).Ok());
    std::vector<uint8_t> bytes = w.Build();

    const SingleEntryLayout layout = LayoutOf(bytes);
    PutU64At(bytes, layout.offPos, static_cast<uint64_t>(bytes.size()) + 100);
    PatchIndexCrc(bytes, layout.indexLen);

    core::PackReader r(bytes);
    CHECK(!r.Valid());
}

TEST(PackIndexCrcValidSizeMismatch) {
    // Index CRC is valid and the block stays in bounds, but compSize != rawSize
    // for a stored entry: construction passes, Read must report the mismatch.
    core::PackWriter w;
    CHECK(w.AddFile("a.bin", ToBytes("hello")).Ok());
    std::vector<uint8_t> bytes = w.Build();

    const SingleEntryLayout layout = LayoutOf(bytes);
    const uint64_t blockOffset = GetU64At(bytes, layout.offPos);
    PutU32At(bytes, layout.compPos, 7); // compSize 7 vs rawSize 5
    bytes.resize(static_cast<size_t>(blockOffset) + 7, 0); // keep the block in bounds
    PatchIndexCrc(bytes, layout.indexLen);

    core::PackReader r(bytes);
    CHECK(r.Valid());
    core::Result<std::vector<uint8_t>> res = r.Read("a.bin");
    CHECK(!res.Ok());
    CHECK(res.Error().find("size mismatch") != std::string::npos);
}

TEST(PackIndexCrcValidUnsupportedMethod) {
    // Index CRC is valid, but the entry claims an unknown compression method:
    // construction passes, Read must report it as unsupported.
    core::PackWriter w;
    CHECK(w.AddFile("a.bin", ToBytes("hello")).Ok());
    std::vector<uint8_t> bytes = w.Build();

    const SingleEntryLayout layout = LayoutOf(bytes);
    bytes[layout.methodPos] = 7;
    PatchIndexCrc(bytes, layout.indexLen);

    core::PackReader r(bytes);
    CHECK(r.Valid());
    core::Result<std::vector<uint8_t>> res = r.Read("a.bin");
    CHECK(!res.Ok());
    CHECK(res.Error().find("unsupported compression") != std::string::npos);
}

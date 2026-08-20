#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/core/pack.hpp"
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

} // namespace

TEST(PackRoundTrip) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes(kTextA)));
    CHECK(w.AddFile(kDirPath, ToBytes(kTextC)));

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
    CHECK(w.AddFile("a.txt", ToBytes("x")));
    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());

    core::Result<std::vector<uint8_t>> res = r.Read("missing/文件.txt");
    CHECK(!res.Ok());
    CHECK(res.Error().find("not found") != std::string::npos);
}

TEST(PackCorruptedBlockDetected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("aaaa")));
    CHECK(w.AddFile(kDirPath, ToBytes("cccccccccccccccc")));
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
    CHECK(w.AddFile("a.txt", ToBytes("abc")));
    CHECK(w.AddFile(kDirPath, ToBytes("xyz")));
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

TEST(PackWrongMagicRejected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("x")));
    std::vector<uint8_t> bytes = w.Build();
    CHECK(!bytes.empty());
    if (bytes.empty()) return;
    bytes[0] = 'X';

    core::PackReader r(bytes);
    CHECK(!r.Valid());
}

TEST(PackTruncatedRejected) {
    core::PackWriter w;
    CHECK(w.AddFile("a.txt", ToBytes("payload-payload")));
    std::vector<uint8_t> bytes = w.Build();
    CHECK(bytes.size() >= 20u);
    if (bytes.size() < 20u) return;
    bytes.resize(bytes.size() - 2); // cut into the last data block

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
    CHECK(w.AddFile("dup.txt", ToBytes("first")));
    CHECK(!w.AddFile("dup.txt", ToBytes("second")));
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
    CHECK(!w.AddFile("", ToBytes("data")));
}

TEST(PackEmptyFileRoundTrip) {
    core::PackWriter w;
    CHECK(w.AddFile("empty.bin", {}));
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
    CHECK(w.AddFile(p1, ToBytes("one")));
    CHECK(w.AddFile(p2, ToBytes("two")));
    CHECK(w.AddFile(p3, ToBytes("three")));

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
    CHECK(w.AddFile("z.txt", ToBytes("1")));
    CHECK(w.AddFile("m.txt", ToBytes("2")));
    CHECK(w.AddFile("a.txt", ToBytes("3")));
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

TEST(PackLargeFileRoundTrip) {
    core::PackWriter w;
    const size_t n = 256 * 1024;
    std::vector<uint8_t> big(n);
    uint32_t state = 12345u;
    for (size_t i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        big[i] = static_cast<uint8_t>(state >> 24);
    }
    CHECK(w.AddFile("big.bin", big));
    std::vector<uint8_t> bytes = w.Build();
    core::PackReader r(bytes);
    CHECK(r.Valid());
    core::Result<std::vector<uint8_t>> res = r.Read("big.bin");
    CHECK(res.Ok());
    CHECK_EQ(res.Value().size(), n);
    CHECK(res.Value() == big);
}

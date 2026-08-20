#include <cstdint>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "neon/core/serialize.hpp"
#include "helpers.hpp"

using namespace neon;

static void AppendTo(core::Deserializer& d, std::vector<float>& out) {
    core::Result<float> f = d.ReadF32();
    CHECK(f.Ok());
    out.push_back(f.Value());
}

TEST(SerializePrimitiveRoundTrip) {
    core::Serializer s;
    s.WriteU32(0xDEADBEEFu);
    s.WriteU64(0x0123456789ABCDEFull);
    s.WriteI32(-1234567);
    s.WriteF32(3.14159265f);
    s.WriteF64(2.718281828459045);
    s.WriteString("");
    s.WriteString("hello neon");
    s.WriteBytes(std::vector<uint8_t>{0x00, 0xFF, 0x10, 0x55});

    core::Deserializer d(s.Data());
    CHECK_EQ(d.Remaining(), s.Size() - core::kHeaderBytes);

    core::Result<uint32_t> u32 = d.ReadU32();
    CHECK(u32.Ok());
    CHECK_EQ(u32.Value(), 0xDEADBEEFu);
    core::Result<uint64_t> u64 = d.ReadU64();
    CHECK(u64.Ok());
    CHECK_EQ(u64.Value(), 0x0123456789ABCDEFull);
    core::Result<int32_t> i32 = d.ReadI32();
    CHECK(i32.Ok());
    CHECK_EQ(i32.Value(), -1234567);
    core::Result<float> f32 = d.ReadF32();
    CHECK(f32.Ok());
    CHECK_NEAR(f32.Value(), 3.14159265, 1e-6);
    core::Result<double> f64 = d.ReadF64();
    CHECK(f64.Ok());
    CHECK_NEAR(f64.Value(), 2.718281828459045, 1e-12);
    core::Result<std::string> empty = d.ReadString();
    CHECK(empty.Ok());
    CHECK_EQ(empty.Value(), std::string());
    core::Result<std::string> str = d.ReadString();
    CHECK(str.Ok());
    CHECK_EQ(str.Value(), std::string("hello neon"));
    core::Result<std::vector<uint8_t>> bytes = d.ReadBytes();
    CHECK(bytes.Ok());
    CHECK_EQ(bytes.Value().size(), 4u);
    CHECK_EQ(bytes.Value()[0], 0x00u);
    CHECK_EQ(bytes.Value()[1], 0xFFu);
    CHECK_EQ(bytes.Value()[2], 0x10u);
    CHECK_EQ(bytes.Value()[3], 0x55u);
    CHECK_EQ(d.Remaining(), 0u);
}

TEST(SerializeVec3VectorRoundTrip) {
    std::vector<math::Vec3> vecs = {
        {1.5f, -2.25f, 3.125f},
        {0.0f, 0.0f, 0.0f},
        {-100.0f, 100.0f, 0.001f},
        {42.0f, -1.0f, 0.5f},
    };

    core::Serializer s;
    s.WriteU32(static_cast<uint32_t>(vecs.size()));
    for (const math::Vec3& v : vecs) {
        s.WriteF32(v.x);
        s.WriteF32(v.y);
        s.WriteF32(v.z);
    }

    core::Deserializer d(s.Data());
    core::Result<uint32_t> count = d.ReadU32();
    CHECK(count.Ok());
    CHECK_EQ(count.Value(), vecs.size());
    for (size_t i = 0; i < vecs.size(); ++i) {
        std::vector<float> comps;
        AppendTo(d, comps);
        AppendTo(d, comps);
        AppendTo(d, comps);
        CHECK_EQ(comps.size(), 3u);
        CHECK_NEAR(comps[0], vecs[i].x, 1e-5);
        CHECK_NEAR(comps[1], vecs[i].y, 1e-5);
        CHECK_NEAR(comps[2], vecs[i].z, 1e-5);
    }
    CHECK_EQ(d.Remaining(), 0u);
}

TEST(SerializeVersionHeader) {
    const uint32_t kVersion = 4;

    core::Serializer s;
    s.WriteVersion(kVersion);
    s.WriteU32(77);

    core::Deserializer ok(s.Data());
    CHECK(ok.Validate());
    core::Result<uint32_t> v = ok.ReadVersion();
    CHECK(v.Ok());
    CHECK_EQ(v.Value(), kVersion);
    CHECK_EQ(ok.ReadU32().Value(), 77u);

    core::Deserializer bad(s.Data());
    core::Result<uint32_t> wrong = bad.ReadVersion(99);
    CHECK(!wrong.Ok());
}

TEST(SerializeCorruptionDetected) {
    core::Serializer s;
    s.WriteVersion(4);
    s.WriteU64(0x0123456789ABCDEFull);

    std::vector<uint8_t> bytes = s.Data();
    bytes[bytes.size() / 2] ^= 0xFF;

    core::Deserializer d(bytes);
    CHECK(!d.Validate());
}

TEST(SerializeTruncatedFails) {
    core::Serializer s;
    s.WriteU32(7);
    s.WriteU32(8);
    s.WriteU32(9);

    std::vector<uint8_t> bytes = s.Data();
    bytes.resize(bytes.size() - 3); // cut off the trailing u32

    core::Deserializer d(bytes);
    CHECK_EQ(d.ReadU32().Value(), 7u);
    CHECK_EQ(d.ReadU32().Value(), 8u);
    CHECK(!d.ReadU32().Ok());
}

TEST(SerializeLargeBuffer) {
    core::Serializer s;
    s.WriteVersion(4);
    const uint32_t n = 100000;
    s.WriteU32(n);
    for (uint32_t i = 0; i < n; ++i) s.WriteF32(static_cast<float>(i) * 0.001f);

    core::Deserializer d(s.Data());
    CHECK(d.Validate());
    CHECK_EQ(d.ReadVersion().Value(), 4u);
    CHECK_EQ(d.ReadU32().Value(), n);
    bool equal = true;
    for (uint32_t i = 0; i < n; ++i) {
        core::Result<float> f = d.ReadF32();
        if (!f.Ok() || f.Value() != static_cast<float>(i) * 0.001f) equal = false;
    }
    CHECK(equal);
    CHECK_EQ(d.Remaining(), 0u);
}

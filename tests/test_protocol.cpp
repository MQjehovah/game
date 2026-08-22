#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "neon/neon.hpp"
#include "neon/net/protocol.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

template <class T>
bool Holds(const net::DecodedMessage& m) {
    return std::holds_alternative<T>(m.payload);
}

} // namespace

TEST(ProtocolJoinRoundTrip) {
    net::MessageCodec codec;
    net::MsgJoin in;
    in.name = "neon_player";
    in.version = 0x01020304u;

    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Join, 7, in);
    CHECK(enc.Ok());
    core::Result<net::DecodedMessage> dec = codec.Decode(enc.Value());
    CHECK(dec.Ok());
    CHECK_EQ(dec.Value().header.version, net::kProtocolVersion);
    CHECK_EQ(dec.Value().header.msgId, static_cast<uint8_t>(net::MsgType::Join));
    CHECK_EQ(dec.Value().header.seq, 7u);
    CHECK(Holds<net::MsgJoin>(dec.Value()));
    const net::MsgJoin& out = std::get<net::MsgJoin>(dec.Value().payload);
    CHECK_EQ(out.name, in.name);
    CHECK_EQ(out.version, in.version);
}

TEST(ProtocolWelcomeRoundTrip) {
    net::MessageCodec codec;
    net::MsgWelcome in{0x1122334455667788ull, 1234u};

    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Welcome, 3, in);
    CHECK(enc.Ok());
    core::Result<net::DecodedMessage> dec = codec.Decode(enc.Value());
    CHECK(dec.Ok());
    CHECK_EQ(dec.Value().header.msgId, static_cast<uint8_t>(net::MsgType::Welcome));
    CHECK_EQ(dec.Value().header.seq, 3u);
    CHECK(Holds<net::MsgWelcome>(dec.Value()));
    const net::MsgWelcome& out = std::get<net::MsgWelcome>(dec.Value().payload);
    CHECK_EQ(out.clientId, in.clientId);
    CHECK_EQ(out.tick, in.tick);
}

TEST(ProtocolInputRoundTrip) {
    net::MessageCodec codec;
    net::MsgInput in{99u, static_cast<uint8_t>(0x15), 0.5f, -0.25f};

    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Input, 4, in);
    CHECK(enc.Ok());
    core::Result<net::DecodedMessage> dec = codec.Decode(enc.Value());
    CHECK(dec.Ok());
    CHECK_EQ(dec.Value().header.msgId, static_cast<uint8_t>(net::MsgType::Input));
    CHECK_EQ(dec.Value().header.seq, 4u);
    CHECK(Holds<net::MsgInput>(dec.Value()));
    const net::MsgInput& out = std::get<net::MsgInput>(dec.Value().payload);
    CHECK_EQ(out.seq, in.seq);
    CHECK_EQ(out.buttons, in.buttons);
    CHECK_NEAR(out.moveX, in.moveX, 1e-5);
    CHECK_NEAR(out.moveY, in.moveY, 1e-5);
}

TEST(ProtocolSnapshotRoundTrip) {
    net::MessageCodec codec;
    net::MsgSnapshot in;
    in.tick = 4242u;
    in.entities = {
        {0x1111111111111111ull, 1.5f, -2.5f, 3.25f, 0.75f},
        {0x2222222222222222ull, 0.0f, 0.0f, 0.0f, 0.0f},
        {0xFFFFFFFFFFFFFFFFull, -100.0f, 100.0f, 0.001f, 3.14159f},
    };
    in.entityCount = static_cast<uint32_t>(in.entities.size());

    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Snapshot, 9, in);
    CHECK(enc.Ok());
    core::Result<net::DecodedMessage> dec = codec.Decode(enc.Value());
    CHECK(dec.Ok());
    CHECK_EQ(dec.Value().header.msgId, static_cast<uint8_t>(net::MsgType::Snapshot));
    CHECK_EQ(dec.Value().header.seq, 9u);
    CHECK(Holds<net::MsgSnapshot>(dec.Value()));
    const net::MsgSnapshot& out = std::get<net::MsgSnapshot>(dec.Value().payload);
    CHECK_EQ(out.tick, in.tick);
    CHECK_EQ(out.entityCount, in.entityCount);
    CHECK_EQ(out.entities.size(), in.entities.size());
    for (size_t i = 0; i < in.entities.size(); ++i) {
        CHECK_EQ(out.entities[i].id, in.entities[i].id);
        CHECK_NEAR(out.entities[i].x, in.entities[i].x, 1e-5);
        CHECK_NEAR(out.entities[i].y, in.entities[i].y, 1e-5);
        CHECK_NEAR(out.entities[i].z, in.entities[i].z, 1e-5);
        CHECK_NEAR(out.entities[i].yaw, in.entities[i].yaw, 1e-5);
    }
}

TEST(ProtocolEmptySnapshot) {
    net::MessageCodec codec;
    net::MsgSnapshot in;
    in.tick = 7u;
    in.entityCount = 0u;

    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Snapshot, 11, in);
    CHECK(enc.Ok());
    core::Result<net::DecodedMessage> dec = codec.Decode(enc.Value());
    CHECK(dec.Ok());
    CHECK(Holds<net::MsgSnapshot>(dec.Value()));
    const net::MsgSnapshot& out = std::get<net::MsgSnapshot>(dec.Value().payload);
    CHECK_EQ(out.tick, 7u);
    CHECK_EQ(out.entityCount, 0u);
    CHECK_EQ(out.entities.size(), 0u);
}

TEST(ProtocolSpawnRoundTrip) {
    net::MessageCodec codec;
    net::MsgSpawn in{0xABCDEF0123456789ull, "player", 1.0f, 2.0f, 3.0f};

    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Spawn, 2, in);
    CHECK(enc.Ok());
    core::Result<net::DecodedMessage> dec = codec.Decode(enc.Value());
    CHECK(dec.Ok());
    CHECK_EQ(dec.Value().header.msgId, static_cast<uint8_t>(net::MsgType::Spawn));
    CHECK(Holds<net::MsgSpawn>(dec.Value()));
    const net::MsgSpawn& out = std::get<net::MsgSpawn>(dec.Value().payload);
    CHECK_EQ(out.entityId, in.entityId);
    CHECK_EQ(out.kind, in.kind);
    CHECK_NEAR(out.x, in.x, 1e-5);
    CHECK_NEAR(out.y, in.y, 1e-5);
    CHECK_NEAR(out.z, in.z, 1e-5);
}

TEST(ProtocolDespawnRoundTrip) {
    net::MessageCodec codec;
    net::MsgDespawn in{0xDEADBEEFCAFEBABEull};

    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Despawn, 5, in);
    CHECK(enc.Ok());
    core::Result<net::DecodedMessage> dec = codec.Decode(enc.Value());
    CHECK(dec.Ok());
    CHECK_EQ(dec.Value().header.msgId, static_cast<uint8_t>(net::MsgType::Despawn));
    CHECK(Holds<net::MsgDespawn>(dec.Value()));
    const net::MsgDespawn& out = std::get<net::MsgDespawn>(dec.Value().payload);
    CHECK_EQ(out.entityId, in.entityId);
}

TEST(ProtocolPingPongRoundTrip) {
    net::MessageCodec codec;
    net::MsgPing ping{1234567890123ull};
    net::MsgPong pong{111111ull, 222222ull};

    core::Result<std::vector<uint8_t>> encPing =
        codec.Encode(net::MsgType::Ping, 6, ping);
    CHECK(encPing.Ok());
    core::Result<net::DecodedMessage> decPing = codec.Decode(encPing.Value());
    CHECK(decPing.Ok());
    CHECK(Holds<net::MsgPing>(decPing.Value()));
    CHECK_EQ(std::get<net::MsgPing>(decPing.Value().payload).sendTime, ping.sendTime);

    core::Result<std::vector<uint8_t>> encPong =
        codec.Encode(net::MsgType::Pong, 6, pong);
    CHECK(encPong.Ok());
    core::Result<net::DecodedMessage> decPong = codec.Decode(encPong.Value());
    CHECK(decPong.Ok());
    CHECK(Holds<net::MsgPong>(decPong.Value()));
    const net::MsgPong& out = std::get<net::MsgPong>(decPong.Value().payload);
    CHECK_EQ(out.sendTime, pong.sendTime);
    CHECK_EQ(out.receiveTime, pong.receiveTime);
}

TEST(ProtocolUnknownMsgId) {
    net::MessageCodec codec;
    core::Result<std::vector<uint8_t>> frame = codec.EncodeFrame(99, 1, {});
    CHECK(frame.Ok());
    core::Result<net::DecodedMessage> dec = codec.Decode(frame.Value());
    CHECK(!dec.Ok());
}

TEST(ProtocolCorruptedPayload) {
    net::MessageCodec codec;
    net::MsgInput in{5u, static_cast<uint8_t>(1), 1.0f, 2.0f};
    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Input, 3, in);
    CHECK(enc.Ok());
    std::vector<uint8_t> bytes = enc.Value();
    bytes[bytes.size() / 2] ^= 0xFF;
    core::Result<net::DecodedMessage> dec = codec.Decode(bytes);
    CHECK(!dec.Ok());
}

TEST(ProtocolTruncatedBuffer) {
    net::MessageCodec codec;
    net::MsgJoin in{"hello", 1u};
    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Join, 1, in);
    CHECK(enc.Ok());
    std::vector<uint8_t> bytes = enc.Value();
    bytes.resize(bytes.size() - 3);
    CHECK(!codec.Decode(bytes).Ok());
    CHECK(!codec.Decode(bytes.data(), 1).Ok());
    CHECK(!codec.Decode(nullptr, 0).Ok());
}

TEST(ProtocolBadVersion) {
    net::MessageCodec codec;
    core::Serializer s;
    s.WriteVersion(net::kProtocolVersion + 1);
    s.WriteU8(static_cast<uint8_t>(net::MsgType::Join));
    s.WriteU16(1);
    s.WriteString("x");
    s.WriteU32(1);
    CHECK(!codec.Decode(s.Data()).Ok());
}

TEST(ProtocolOversizedEntityCount) {
    net::MessageCodec codec;
    std::vector<uint8_t> payload;
    AppendU32(payload, 0);      // tick
    AppendU32(payload, 999999); // entity count over kMaxSnapshotEntities
    core::Result<std::vector<uint8_t>> frame =
        codec.EncodeFrame(static_cast<uint8_t>(net::MsgType::Snapshot), 1, payload);
    CHECK(frame.Ok());
    CHECK(!codec.Decode(frame.Value()).Ok());
}

TEST(ProtocolOversizedString) {
    net::MessageCodec codec;
    std::vector<uint8_t> payload;
    AppendU32(payload, 1000); // name length over kMaxStringBytes
    for (int i = 0; i < 1000; ++i) payload.push_back(static_cast<uint8_t>('x'));
    core::Result<std::vector<uint8_t>> frame =
        codec.EncodeFrame(static_cast<uint8_t>(net::MsgType::Join), 1, payload);
    CHECK(frame.Ok());
    CHECK(!codec.Decode(frame.Value()).Ok());
}

TEST(ProtocolTrailingBytes) {
    net::MessageCodec codec;
    net::MsgDespawn in{7ull};
    core::Result<std::vector<uint8_t>> enc =
        codec.Encode(net::MsgType::Despawn, 1, in);
    CHECK(enc.Ok());
    std::vector<uint8_t> bytes = enc.Value();
    bytes.push_back(0x00); // junk after the message body
    // Re-stamp the CRC (over bytes[8..]) so the frame is otherwise valid: the
    // decode must then reject it for trailing bytes, not CRC corruption.
    uint32_t crc = core::Crc32(bytes.data() + core::kHeaderBytes, bytes.size() - core::kHeaderBytes);
    bytes[4] = static_cast<uint8_t>(crc >> 24);
    bytes[5] = static_cast<uint8_t>(crc >> 16);
    bytes[6] = static_cast<uint8_t>(crc >> 8);
    bytes[7] = static_cast<uint8_t>(crc);
    core::Result<net::DecodedMessage> dec = codec.Decode(bytes);
    CHECK(!dec.Ok());
}

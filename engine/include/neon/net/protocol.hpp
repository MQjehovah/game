#pragma once

// Versioned message framework for NeonEngine networking.
//
// Frames reuse the T1.2 core::Serializer framing, so the wire layout inherits
// its big-endian encoding, magic number and CRC32:
//
//   [ magic : u32 ]  core::kSerializedMagic ("NEON"), checked on decode
//   [ crc   : u32 ]  CRC32 over everything below (version + msgId + seq + payload)
//   [ version: u32 ]  protocol version (kProtocolVersion), checked on decode
//   [ msgId : u8  ]  message type id (MsgType)
//   [ seq   : u16 ]  transport sequence number (ordering / acks, T6.2)
//   [ payload...  ]  per-type body, written/read by each message's codec pair
//
// Decode validates magic, version and CRC and bounds-checks every field
// (truncated reads, oversized strings, oversized entity counts), so hostile or
// corrupt input yields a clean core::Result error rather than undefined
// behaviour.

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "neon/core/result.hpp"
#include "neon/core/serialize.hpp"

namespace neon::net {

// Protocol version this codec speaks. Bump on any incompatible wire change.
// v1 -> v2: added the transport-level Ack message (T6.2 reliable layer).
inline constexpr uint8_t kProtocolVersion = 2;

// Hard caps applied on decode to hostile input. Strings longer than
// kMaxStringBytes and snapshots with more than kMaxSnapshotEntities entries
// are rejected as malformed.
inline constexpr size_t kMaxStringBytes = 256;
inline constexpr uint32_t kMaxSnapshotEntities = 4096;

// Message set for v2. Join/Welcome/Input/Snapshot/Spawn/Despawn drive the
// lobby and replication flow; Ping/Pong carry RTT timestamps; Ack is emitted
// by the T6.2 reliable transport (it never reaches application logic).
enum class MsgType : uint8_t {
    Join = 1,
    Welcome = 2,
    Input = 3,
    Snapshot = 4,
    Spawn = 5,
    Despawn = 6,
    Ping = 7,
    Pong = 8,
    Ack = 9,
};

// Logical header of a decoded frame. The magic + CRC halves of the wire header
// are validated by the core::Serializer framing and not stored here;
// version/msgId/seq are owned by the codec.
struct MessageHeader {
    uint8_t version = 0;
    uint8_t msgId = 0;
    uint16_t seq = 0;
};

// Client join request: requested display name + client engine version.
struct MsgJoin {
    std::string name;
    uint32_t version = 0;
    void Write(core::Serializer& s) const;
    static core::Result<MsgJoin> Read(core::Deserializer& d);
};

// Server acceptance: assigned client id + current simulation tick.
struct MsgWelcome {
    uint64_t clientId = 0;
    uint32_t tick = 0;
    void Write(core::Serializer& s) const;
    static core::Result<MsgWelcome> Read(core::Deserializer& d);
};

// Client input: input sequence number, button bitmask, move axes.
struct MsgInput {
    uint32_t seq = 0;
    uint8_t buttons = 0;
    float moveX = 0.0f;
    float moveY = 0.0f;
    void Write(core::Serializer& s) const;
    static core::Result<MsgInput> Read(core::Deserializer& d);
};

// One replicated entity state inside a snapshot.
struct SnapshotEntity {
    uint64_t id = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
};

// Server world snapshot: tick + replicated entity states.
struct MsgSnapshot {
    uint32_t tick = 0;
    uint32_t entityCount = 0; // entities.size(), carried on the wire verbatim
    std::vector<SnapshotEntity> entities;
    void Write(core::Serializer& s) const;
    static core::Result<MsgSnapshot> Read(core::Deserializer& d);
};

// Spawn an entity of a given kind at a position.
struct MsgSpawn {
    uint64_t entityId = 0;
    std::string kind;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    void Write(core::Serializer& s) const;
    static core::Result<MsgSpawn> Read(core::Deserializer& d);
};

// Despawn an entity.
struct MsgDespawn {
    uint64_t entityId = 0;
    void Write(core::Serializer& s) const;
    static core::Result<MsgDespawn> Read(core::Deserializer& d);
};

// Ping: sender timestamp, used for RTT.
struct MsgPing {
    uint64_t sendTime = 0;
    void Write(core::Serializer& s) const;
    static core::Result<MsgPing> Read(core::Deserializer& d);
};

// Pong: echo of the ping timestamp + when it was received.
struct MsgPong {
    uint64_t sendTime = 0;
    uint64_t receiveTime = 0;
    void Write(core::Serializer& s) const;
    static core::Result<MsgPong> Read(core::Deserializer& d);
};

// Transport acknowledgement (T6.2 reliable layer): sliding-window ack.
// ackSeq is the highest contiguously-delivered sequence number; ackBits is a
// bitmap for the following 32 seqs (bit i covers seq ackSeq+1+i), so
// out-of-order arrivals beyond the gap are acknowledged too and the sender
// stops retransmitting them. The ack frame's own header seq is unused by the
// reliable layer; acks are idempotent and may be dropped/reordered safely.
struct MsgAck {
    uint16_t ackSeq = 0;
    uint32_t ackBits = 0;
    void Write(core::Serializer& s) const;
    static core::Result<MsgAck> Read(core::Deserializer& d);
};

// Type-erased payload of any message; alternative order matches MsgType ids.
using Payload = std::variant<MsgJoin, MsgWelcome, MsgInput, MsgSnapshot, MsgSpawn,
                             MsgDespawn, MsgPing, MsgPong, MsgAck>;

// A fully decoded and validated frame.
struct DecodedMessage {
    MessageHeader header;
    Payload payload;
};

// Encodes/decodes frames. The codec is stateless and thread-safe for use
// from any transport worker.
class MessageCodec {
public:
    MessageCodec() = default;

    // Serializes a typed message into a complete, CRC-valid frame.
    core::Result<std::vector<uint8_t>> Encode(MsgType type, uint16_t seq,
                                              const Payload& payload);

    // Low-level frame builder: wraps a raw payload under an arbitrary msgId
    // with a valid magic/version/CRC. Used by tests to craft hostile input and
    // by the transport layer when it needs a custom frame.
    core::Result<std::vector<uint8_t>> EncodeFrame(uint8_t msgId, uint16_t seq,
                                                   const std::vector<uint8_t>& payload);

    // Decodes and validates a frame. Returns Err on bad magic/version/CRC,
    // unknown msgId, trailing bytes, or any field violating bounds.
    core::Result<DecodedMessage> Decode(const uint8_t* data, size_t size);
    core::Result<DecodedMessage> Decode(const std::vector<uint8_t>& data);
};

} // namespace neon::net

#pragma once

// Reliable channel on top of the T6.1 codec frames + T6.2 UDP transport.
//
// Model: SYNCHRONOUS, game-loop driven. The channel holds no threads; the app
// calls Tick(nowMs) every frame with its monotonic clock (a fake clock in
// tests) and feeds received datagrams into OnDatagram(). Send() hands a
// length-prefixed datagram to the outbound hook immediately and buffers it for
// retransmission until an ack covers it.
//
// Sequencing + acks: each direction has its own u16 sequence space. Frames are
// length-prefixed on the wire as [u16 length][codec frame], one datagram per
// frame (v1), capped at ReliableConfig::maxFrameBytes (~MTU safety).
//
//   Sender  : Send() stamps the next seq, buffers the datagram, emits it.
//             OnAck() slides the window (drops seqs <= ackSeq, plus the bitmap
//             entries from MsgAck). Tick() retransmits frames older than
//             retransmitMs and fails the channel (timeout) when nothing was
//             acked within timeoutMs.
//   Receiver: OnDatagram() decodes and reorders by seq; out-of-order frames
//             are buffered up to windowSize and delivered strictly in order via
//             the deliver callback; duplicates/stale seqs are dropped. Tick()
//             emits throttled sliding-window acks (MsgAck).
//
// The ack scheme is cumulative + selective: MsgAck{ackSeq, ackBits} where
// ackSeq is the highest contiguous seq and bit i of ackBits covers seq
// ackSeq+1+i. Acks are idempotent, so ack loss/reorder is harmless (the sender
// retransmits and the receiver dedups).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "neon/core/result.hpp"
#include "neon/net/protocol.hpp"

namespace neon::net {

// Per-channel reliability tuning. Times are in milliseconds and driven by the
// app's clock (deterministic fake clocks in tests).
struct ReliableConfig {
    uint16_t windowSize = 64;      // max in-flight unacked frames
    uint16_t ackBits = 32;         // MsgAck bitmap entries (<= 32)
    uint64_t retransmitMs = 200;   // resend frames unacked for this long
    uint64_t timeoutMs = 3000;     // fail the channel if a frame is unacked this long
    uint64_t ackIntervalMs = 50;   // min gap between acks emitted by Tick
    uint16_t maxFrameBytes = 1200; // reject larger frames (MTU cap)
};

// Largest datagram the transport emits: u16 length prefix + max frame bytes.
inline constexpr size_t kMaxDatagramBytes = 2 + 1200;

// Length-prefixes a complete codec frame as a transport datagram:
// [u16 length][frame bytes]. Also used to decode incoming datagrams.
std::vector<uint8_t> EncodeDatagram(const std::vector<uint8_t>& frame);

// One channel carries a full duplex link: a sender half (outgoing seq space,
// unacked buffer, retransmission) and a receiver half (incoming reorder
// buffer, dedup, ack generation). v1 tests drive one channel per socket end.
class ReliableChannel {
public:
    // Emits a complete length-prefixed datagram (set by the app to the socket).
    using OutboundFn = std::function<void(const std::vector<uint8_t>&)>;
    // Delivers a decoded message, strictly in sequence (set by the app).
    using DeliverFn = std::function<void(const DecodedMessage&)>;
    // Fired once when a frame times out (channel enters a failed state).
    using OnTimeoutFn = std::function<void()>;

    ReliableChannel();
    explicit ReliableChannel(const ReliableConfig& config);

    // Wiring. Outbound must be set before Send()/Tick() transmit.
    void SetOutbound(OutboundFn fn);
    void SetDeliver(DeliverFn fn);
    void SetTimeout(OnTimeoutFn fn);

    // ---- sender side -----------------------------------------------------
    // Encodes msgId+payload, assigns the next seq, buffers for retransmit and
    // emits the datagram immediately. Err when the frame exceeds maxFrameBytes
    // or the send window is full (pump + retry later).
    core::Status Send(uint8_t msgId, const std::vector<uint8_t>& payload);
    // Applies a sliding-window ack: drops everything <= ackSeq and the bitmap
    // entries. Call from OnDatagram (automatic) or directly for tests.
    void OnAck(uint16_t ackSeq, uint32_t ackBits);
    uint16_t NextSeq() const { return nextSend_; }
    size_t PendingFrames() const { return sent_.size(); }
    size_t PendingBytes() const;

    // ---- receiver side ---------------------------------------------------
    // Feeds one raw length-prefixed datagram (as produced by EncodeDatagram /
    // Send). Decodes, reorders, dedups and delivers in order; acks are handled
    // internally. Malformed/garbage datagrams are dropped.
    void OnDatagram(const uint8_t* data, size_t size);
    uint16_t NextExpected() const { return nextExpected_; }
    size_t DeliveredCount() const { return deliveredCount_; }

    // ---- common ----------------------------------------------------------
    // Advance the fake/real clock: retransmit due frames, fire timeouts and
    // emit throttled acks. Call once per game loop tick with nowMs monotonic.
    void Tick(uint64_t nowMs);
    void Reset();
    bool Connected() const { return !timedOut_; }
    bool TimedOut() const { return timedOut_; }
    const ReliableConfig& Config() const { return config_; }

private:
    struct OutFrame {
        std::vector<uint8_t> datagram;
        uint64_t firstSentMs = 0; // set on first Tick after Send()
        uint64_t lastSentMs = 0;
        uint32_t retransmits = 0;
    };
    struct HeldFrame {
        DecodedMessage message;
    };

    void Deliver(DecodedMessage&& msg);
    void OnFrame(DecodedMessage msg);
    void EmitAck(uint64_t nowMs);
    bool InWindow(uint16_t seq) const;
    static bool SeqLess(uint16_t a, uint16_t b);
    static bool SeqLe(uint16_t a, uint16_t b);

    ReliableConfig config_;
    MessageCodec codec_;
    OutboundFn outbound_;
    DeliverFn deliver_;
    OnTimeoutFn timeoutFn_;

    // sender state
    uint16_t nextSend_ = 0;
    std::unordered_map<uint16_t, OutFrame> sent_;

    // receiver state
    uint16_t nextExpected_ = 0;
    std::unordered_map<uint16_t, HeldFrame> received_;
    bool ackPending_ = false;
    uint64_t lastAckMs_ = 0;
    size_t deliveredCount_ = 0;
    bool timedOut_ = false;
};

} // namespace neon::net

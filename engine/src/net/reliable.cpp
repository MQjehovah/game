#include "neon/net/reliable.hpp"

#include <utility>

namespace neon::net {

std::vector<uint8_t> EncodeDatagram(const std::vector<uint8_t>& frame) {
    std::vector<uint8_t> datagram;
    datagram.reserve(frame.size() + 2);
    uint16_t len = static_cast<uint16_t>(frame.size());
    datagram.push_back(static_cast<uint8_t>(len >> 8));
    datagram.push_back(static_cast<uint8_t>(len & 0xFFu));
    datagram.insert(datagram.end(), frame.begin(), frame.end());
    return datagram;
}

ReliableChannel::ReliableChannel() = default;

ReliableChannel::ReliableChannel(const ReliableConfig& config) : config_(config) {
    if (config_.ackBits > 32) config_.ackBits = 32;
}

void ReliableChannel::SetOutbound(OutboundFn fn) { outbound_ = std::move(fn); }
void ReliableChannel::SetDeliver(DeliverFn fn) { deliver_ = std::move(fn); }
void ReliableChannel::SetTimeout(OnTimeoutFn fn) { timeoutFn_ = std::move(fn); }

size_t ReliableChannel::PendingBytes() const {
    size_t total = 0;
    for (const auto& kv : sent_) total += kv.second.datagram.size();
    return total;
}

// Wrapped (mod-65536) comparison within a half-range span. True when a is
// "before" b in the circular sequence space, i.e. (a - b) reinterpreted as a
// signed 16-bit value is negative. Valid for spans < 32768.
bool ReliableChannel::SeqLess(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(static_cast<uint16_t>(a - b)) < 0;
}

bool ReliableChannel::SeqLe(uint16_t a, uint16_t b) { return a == b || SeqLess(a, b); }

bool ReliableChannel::InWindow(uint16_t seq) const {
    // True when seq is in [nextExpected_, nextExpected_ + windowSize) in the
    // wrapped sequence space: not stale and not so far ahead it cannot be
    // buffered.
    if (SeqLess(seq, nextExpected_)) return false;
    uint16_t upper = static_cast<uint16_t>(nextExpected_ + config_.windowSize);
    return SeqLess(seq, upper);
}

core::Status ReliableChannel::Send(uint8_t msgId, const std::vector<uint8_t>& payload) {
    if (!outbound_)
        return core::Status::Err("net: reliable Send before SetOutbound");
    if (sent_.size() >= config_.windowSize)
        return core::Status::Err("net: reliable send window full");

    core::Result<std::vector<uint8_t>> frame = codec_.EncodeFrame(msgId, nextSend_, payload);
    if (!frame.Ok()) return core::Status::Err(frame.Error());
    if (frame.Value().size() > config_.maxFrameBytes)
        return core::Status::Err("net: reliable frame exceeds maxFrameBytes");

    uint16_t seq = nextSend_;
    nextSend_ = static_cast<uint16_t>(nextSend_ + 1);

    OutFrame out;
    out.datagram = EncodeDatagram(frame.Value());
    outbound_(out.datagram);
    sent_.emplace(seq, std::move(out));
    return core::Status::Ok(true);
}

void ReliableChannel::OnAck(uint16_t ackSeq, uint32_t ackBits) {
    for (auto it = sent_.begin(); it != sent_.end();) {
        uint16_t s = it->first;
        bool acked = SeqLe(s, ackSeq);
        if (!acked) {
            // Bit i of ackBits covers seq ackSeq+1+i; rel = s - (ackSeq+1)
            // wraps naturally in u16, so this stays correct across the wrap.
            uint16_t rel = static_cast<uint16_t>(s - static_cast<uint16_t>(ackSeq + 1));
            if (rel < config_.ackBits && (ackBits & (1u << rel)) != 0) acked = true;
        }
        if (acked)
            it = sent_.erase(it);
        else
            ++it;
    }
}

void ReliableChannel::OnDatagram(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 2) return; // malformed datagram: drop
    uint16_t len = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
    if (static_cast<size_t>(len) != size - 2) return; // length prefix mismatch: drop

    core::Result<DecodedMessage> dec = codec_.Decode(data + 2, len);
    if (!dec.Ok()) return; // bad magic/version/CRC/body: drop
    DecodedMessage msg = std::move(dec.Value());

    if (msg.header.msgId == static_cast<uint8_t>(MsgType::Ack)) {
        const MsgAck& ack = std::get<MsgAck>(msg.payload);
        OnAck(ack.ackSeq, ack.ackBits);
        return;
    }
    OnFrame(std::move(msg));
}

void ReliableChannel::OnFrame(DecodedMessage msg) {
    uint16_t seq = msg.header.seq;

    if (seq == nextExpected_) {
        // Contiguous: deliver now, then drain anything that was buffered behind
        // this one, so delivery is always strictly in sequence.
        Deliver(std::move(msg));
        uint16_t n = static_cast<uint16_t>(nextExpected_ + 1);
        auto it = received_.find(n);
        while (it != received_.end()) {
            DecodedMessage m = std::move(it->second.message);
            received_.erase(it);
            Deliver(std::move(m));
            n = static_cast<uint16_t>(n + 1);
            it = received_.find(n);
        }
        nextExpected_ = n;
        ackPending_ = true;
        return;
    }

    // Out of order: buffer if it is a plausible future frame, otherwise drop
    // (stale = already delivered, duplicate, or outside the reorder window).
    if (!InWindow(seq) || received_.count(seq) != 0) return;
    received_.emplace(seq, HeldFrame{std::move(msg)});
    ackPending_ = true;
}

void ReliableChannel::Deliver(DecodedMessage&& msg) {
    ++deliveredCount_;
    if (deliver_) deliver_(msg);
}

void ReliableChannel::EmitAck(uint64_t nowMs) {
    if (!outbound_) return;

    // ackSeq = highest contiguous delivered = nextExpected_ - 1 (wraps in u16).
    uint16_t ackSeq = static_cast<uint16_t>(nextExpected_ - 1);
    uint32_t ackBits = 0;
    for (uint16_t i = 0; i < config_.ackBits; ++i) {
        uint16_t s = static_cast<uint16_t>(nextExpected_ + i);
        if (received_.count(s) != 0) ackBits |= (1u << i);
    }

    MsgAck ack{ackSeq, ackBits};
    core::Result<std::vector<uint8_t>> frame = codec_.Encode(MsgType::Ack, 0, ack);
    if (!frame.Ok()) return;
    outbound_(EncodeDatagram(frame.Value()));
    lastAckMs_ = nowMs;
    ackPending_ = false;
}

void ReliableChannel::Tick(uint64_t nowMs) {
    if (timedOut_) return; // failed channel: receiver still ingests, nothing sent

    // --- sender: timestamp new frames, retransmit due ones, detect timeout ---
    for (auto& kv : sent_) {
        OutFrame& f = kv.second;
        if (f.firstSentMs == 0) {
            // Sent between ticks: clock it now; do not retransmit this tick.
            f.firstSentMs = nowMs;
            f.lastSentMs = nowMs;
            continue;
        }
        if (nowMs - f.lastSentMs >= config_.retransmitMs && outbound_) {
            outbound_(f.datagram);
            f.lastSentMs = nowMs;
            ++f.retransmits;
        }
        if (nowMs - f.firstSentMs >= config_.timeoutMs) {
            timedOut_ = true;
            if (timeoutFn_) timeoutFn_();
            return;
        }
    }

    // --- receiver: throttled sliding-window ack ---
    if (ackPending_ && nowMs - lastAckMs_ >= config_.ackIntervalMs) EmitAck(nowMs);
}

void ReliableChannel::Reset() {
    nextSend_ = 0;
    nextExpected_ = 0;
    sent_.clear();
    received_.clear();
    ackPending_ = false;
    lastAckMs_ = 0;
    deliveredCount_ = 0;
    timedOut_ = false;
}

} // namespace neon::net

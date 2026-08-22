#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

// Test-suite 6.2: UDP transport + reliable layer.
// ---------------------------------------------------------------------------
// Deterministic loopback tests: two real non-blocking net::UdpSockets on 127.0.0.1
// (ephemeral ports) carry length-prefixed frames; a fake clock (a plain
// uint64_t driven by the test) replaces wall time, so loss/reorder/duplicate
// injection reproduces exactly every run. The reliable channel is pumped
// synchronously (Tick + socket drain), never from a thread.

namespace {

// Big-endian u64, i.e. the codec payload of MsgPing{value}.
std::vector<uint8_t> PingPayload(uint64_t value) {
    std::vector<uint8_t> p(8);
    for (int i = 0; i < 8; ++i)
        p[static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (56 - 8 * i));
    return p;
}

// Fault injection on one direction of a loopback link. Sits between a channel's
// outbound hook and the real socket, so drops/dups/reorders happen "at the
// socket layer". Each emitted datagram gets a 1-based index `n`; dropSet/dupSet
// are the indices to drop / send twice. Retransmits carry new, larger indices,
// so once the original 1..N emissions have passed, every retransmit goes
// through — convergence is guaranteed and deterministic.
struct LinkFaults {
    net::UdpSocket* sock = nullptr;     // socket that carries this direction
    bool swapPairs = false;        // swap each adjacent pair of datagrams
    std::vector<uint64_t> dropSet; // emission indices dropped once (sorted)
    std::vector<uint64_t> dupSet;  // emission indices sent twice (sorted)
    bool dropAll = false;          // drop everything (used to starve acks)
    uint64_t n = 0;                // emissions processed this direction
    std::vector<uint8_t> held;     // swapPairs: first datagram of the pair

    void EmitOne(const std::vector<uint8_t>& bytes) {
        ++n;
        if (dropAll) return;
        if (std::binary_search(dropSet.begin(), dropSet.end(), n)) return;
        sock->Send(bytes.data(), bytes.size());
        if (std::binary_search(dupSet.begin(), dupSet.end(), n)) sock->Send(bytes.data(), bytes.size());
    }

    void Transmit(const std::vector<uint8_t>& bytes) {
        if (swapPairs) {
            if (held.empty()) {
                held = bytes; // delay one datagram...
                return;
            }
            EmitOne(bytes); // ...emit second-then-first (adjacent swap)
            EmitOne(held);
            held.clear();
            return;
        }
        EmitOne(bytes);
    }

    void Flush() {
        if (!held.empty()) {
            EmitOne(held);
            held.clear();
        }
    }
};

std::vector<uint64_t> Multiples(int step, int limit) {
    std::vector<uint64_t> out;
    for (int i = step; i <= limit; i += step) out.push_back(static_cast<uint64_t>(i));
    return out;
}

// Drains both sockets and ticks both channels once. `a` is the client socket
// (data out, acks in), `b` the server socket (data in, acks out).
void Pump(net::UdpSocket& a, net::UdpSocket& b, net::ReliableChannel& client,
          net::ReliableChannel& server, uint64_t now) {
    uint8_t buf[2048];
    for (;;) {
        core::Result<size_t> r = b.Recv(buf, sizeof(buf));
        if (!r.Ok() || r.Value() == 0) break;
        server.OnDatagram(buf, r.Value());
    }
    for (;;) {
        core::Result<size_t> r = a.Recv(buf, sizeof(buf));
        if (!r.Ok() || r.Value() == 0) break;
        client.OnDatagram(buf, r.Value());
    }
    client.Tick(now);
    server.Tick(now);
}

// Runs `numFrames` reliable Ping frames from client to server over the two
// loopback sockets, applying the given faults, and asserts every frame arrives
// exactly once, strictly in order (0..numFrames-1). Deterministic + fast.
void RunLoopback(int numFrames, LinkFaults& toServer, LinkFaults& toClient) {
    core::Result<net::UdpSocket> sockARes = net::UdpSocket::Create();
    core::Result<net::UdpSocket> sockBRes = net::UdpSocket::Create();
    CHECK(sockARes.Ok());
    CHECK(sockBRes.Ok());
    net::UdpSocket sockA = std::move(sockARes.Value());
    net::UdpSocket sockB = std::move(sockBRes.Value());
    CHECK(sockA.BindLoopback(0).Ok());
    CHECK(sockB.BindLoopback(0).Ok());
    CHECK(sockA.SetPeer(net::NetAddress{"127.0.0.1", sockB.Port()}).Ok());
    CHECK(sockB.SetPeer(net::NetAddress{"127.0.0.1", sockA.Port()}).Ok());
    toServer.sock = &sockA;
    toClient.sock = &sockB;

    net::ReliableConfig cfg;
    net::ReliableChannel client(cfg), server(cfg);

    std::vector<uint16_t> delivered;
    std::vector<uint64_t> payloads;
    server.SetDeliver([&](const net::DecodedMessage& m) {
        delivered.push_back(m.header.seq);
        if (std::holds_alternative<net::MsgPing>(m.payload))
            payloads.push_back(std::get<net::MsgPing>(m.payload).sendTime);
    });
    client.SetOutbound([&](const std::vector<uint8_t>& bytes) { toServer.Transmit(bytes); });
    server.SetOutbound([&](const std::vector<uint8_t>& bytes) { toClient.Transmit(bytes); });

    uint64_t now = 0;
    int sent = 0;
    const int kMaxIters = 200000;
    int iters = 0;
    while (sent < numFrames || delivered.size() < static_cast<size_t>(numFrames)) {
        if (++iters > kMaxIters) break;
        while (sent < numFrames && client.PendingFrames() < cfg.windowSize) {
            core::Status s =
                client.Send(static_cast<uint8_t>(net::MsgType::Ping),
                            PingPayload(static_cast<uint64_t>(sent)));
            if (!s.Ok()) break;
            ++sent;
        }
        now += 10;
        Pump(sockA, sockB, client, server, now);
    }
    toServer.Flush();
    toClient.Flush();
    // Settle: the server's ack is throttled (ackIntervalMs), so keep pumping
    // until the last ack round-trips and the send window fully closes.
    int settle = 0;
    while (client.PendingFrames() > 0 && ++settle < 2000) {
        now += 10;
        Pump(sockA, sockB, client, server, now);
    }

    CHECK_EQ(delivered.size(), static_cast<size_t>(numFrames));
    CHECK_EQ(payloads.size(), static_cast<size_t>(numFrames));
    for (int i = 0; i < numFrames && i < static_cast<int>(delivered.size()); ++i) {
        CHECK_EQ(delivered[static_cast<size_t>(i)], static_cast<uint16_t>(i));
        CHECK_EQ(payloads[static_cast<size_t>(i)], static_cast<uint64_t>(i));
    }
    // Both endpoints stay healthy and the send window drains fully.
    CHECK(client.Connected());
    CHECK(server.Connected());
    CHECK_EQ(client.PendingFrames(), 0u);
}

} // namespace

// Step 1: raw socket loopback — create/bind/send/recv on 127.0.0.1, plus the
// server-side RecvFrom -> SetPeer reply flow and non-blocking empty recv.
TEST(ReliableSocketLoopback) {
    core::Result<net::UdpSocket> aRes = net::UdpSocket::Create();
    core::Result<net::UdpSocket> bRes = net::UdpSocket::Create();
    CHECK(aRes.Ok());
    CHECK(bRes.Ok());
    net::UdpSocket a = std::move(aRes.Value());
    net::UdpSocket b = std::move(bRes.Value());
    CHECK(a.BindLoopback(0).Ok());
    CHECK(b.BindLoopback(0).Ok());
    CHECK(a.Port() != 0u); // bound to an OS-assigned ephemeral port
    CHECK(b.Port() != 0u);
    CHECK(a.SetPeer(net::NetAddress{"127.0.0.1", b.Port()}).Ok());
    CHECK(b.SetPeer(net::NetAddress{"127.0.0.1", a.Port()}).Ok());

    // Non-blocking recv on an idle socket must be Ok(0), never block or Err.
    uint8_t buf[64];
    core::Result<size_t> empty = a.Recv(buf, sizeof(buf));
    CHECK(empty.Ok());
    CHECK_EQ(empty.Value(), 0u);

    // a -> b
    const char* hello = "neon-udp";
    core::Result<size_t> sent = a.Send(reinterpret_cast<const uint8_t*>(hello), 8);
    CHECK(sent.Ok());
    CHECK_EQ(sent.Value(), 8u);

    // Server learns the sender address from the datagram (recvfrom).
    core::Result<net::RecvPacket> pkt = b.RecvFrom(buf, sizeof(buf));
    CHECK(pkt.Ok());
    CHECK_EQ(pkt.Value().size, 8u);
    CHECK_EQ(pkt.Value().from.port, a.Port());
    CHECK_EQ(pkt.Value().from.host, std::string("127.0.0.1"));
    CHECK_EQ(std::memcmp(buf, hello, 8), 0);

    // Server replies to the learned peer.
    CHECK(b.SetPeer(pkt.Value().from).Ok());
    core::Result<size_t> reply = b.Send(reinterpret_cast<const uint8_t*>("pong"), 4);
    CHECK(reply.Ok());
    core::Result<size_t> got = a.Recv(buf, sizeof(buf));
    CHECK(got.Ok());
    CHECK_EQ(got.Value(), 4u);
    CHECK_EQ(std::memcmp(buf, "pong", 4), 0);

    a.Close();
    b.Close();
    CHECK(!a.Valid());
    CHECK(!b.Valid());
}

// Step 2: reliable delivery of 1000 frames over loopback with no faults.
TEST(ReliableLoopbackNoLoss) {
    LinkFaults toServer, toClient;
    RunLoopback(1000, toServer, toClient);
}

// Loss: every 3rd original datagram is dropped at the socket layer;
// retransmission recovers all of them, delivered exactly once in order.
TEST(ReliableLoopbackLoss) {
    LinkFaults toServer, toClient;
    toServer.dropSet = Multiples(3, 1000);
    RunLoopback(1000, toServer, toClient);
}

// Reorder: adjacent datagram pairs arrive swapped; the receiver reorders them.
TEST(ReliableLoopbackReorder) {
    LinkFaults toServer, toClient;
    toServer.swapPairs = true;
    RunLoopback(1000, toServer, toClient);
}

// Duplicate: every 10th datagram is sent twice; the receiver dedups.
TEST(ReliableLoopbackDup) {
    LinkFaults toServer, toClient;
    toServer.dupSet = Multiples(10, 1000);
    RunLoopback(1000, toServer, toClient);
}

// Heavy mix: loss + reorder + duplicate all at once.
TEST(ReliableLoopbackLossReorderDup) {
    LinkFaults toServer, toClient;
    toServer.dropSet = Multiples(7, 1000);
    toServer.dupSet = Multiples(13, 1000);
    toServer.swapPairs = true;
    RunLoopback(1000, toServer, toClient);
}

// Window slide: acks arrive -> the sender's unacked buffer shrinks.
TEST(ReliableWindowSlides) {
    core::Result<net::UdpSocket> sockARes = net::UdpSocket::Create();
    core::Result<net::UdpSocket> sockBRes = net::UdpSocket::Create();
    CHECK(sockARes.Ok());
    CHECK(sockBRes.Ok());
    net::UdpSocket sockA = std::move(sockARes.Value());
    net::UdpSocket sockB = std::move(sockBRes.Value());
    CHECK(sockA.BindLoopback(0).Ok());
    CHECK(sockB.BindLoopback(0).Ok());
    CHECK(sockA.SetPeer(net::NetAddress{"127.0.0.1", sockB.Port()}).Ok());
    CHECK(sockB.SetPeer(net::NetAddress{"127.0.0.1", sockA.Port()}).Ok());

    LinkFaults toServer, toClient;
    toServer.sock = &sockA;
    toClient.sock = &sockB;

    net::ReliableChannel client, server;
    server.SetDeliver([](const net::DecodedMessage&) {});
    client.SetOutbound([&](const std::vector<uint8_t>& bytes) { toServer.Transmit(bytes); });
    server.SetOutbound([&](const std::vector<uint8_t>& bytes) { toClient.Transmit(bytes); });

    uint64_t now = 0;
    for (int i = 0; i < 8; ++i)
        CHECK(client.Send(static_cast<uint8_t>(net::MsgType::Ping),
                          PingPayload(static_cast<uint64_t>(i))).Ok());
    CHECK_EQ(client.PendingFrames(), 8u);

    // 60ms advances beat ackIntervalMs (50) so the receiver's ack fires; the
    // second pump carries the ack back to the client.
    now += 60;
    Pump(sockA, sockB, client, server, now);
    now += 60;
    Pump(sockA, sockB, client, server, now);

    CHECK_EQ(server.DeliveredCount(), 8u);
    CHECK_EQ(client.PendingFrames(), 0u);
    CHECK_EQ(client.NextSeq(), 8u);
    CHECK_EQ(server.NextExpected(), 8u);
}

// Sender flow control: the window caps in-flight frames; Send() errors when
// full and OnAck slides the window (cumulative + selective bitmap).
TEST(ReliableSendWindow) {
    net::ReliableConfig cfg;
    cfg.windowSize = 4;
    net::ReliableChannel c(cfg);
    size_t emitted = 0;
    c.SetOutbound([&](const std::vector<uint8_t>&) { ++emitted; });

    for (int i = 0; i < 4; ++i)
        CHECK(c.Send(static_cast<uint8_t>(net::MsgType::Ping),
                     PingPayload(static_cast<uint64_t>(i))).Ok());
    CHECK(!c.Send(static_cast<uint8_t>(net::MsgType::Ping), PingPayload(4)).Ok()); // window full
    CHECK_EQ(c.PendingFrames(), 4u);

    c.OnAck(3, 0); // cumulative: everything <= 3 is gone
    CHECK_EQ(c.PendingFrames(), 0u);
    CHECK(c.Send(static_cast<uint8_t>(net::MsgType::Ping), PingPayload(4)).Ok()); // seq 4

    // Selective bitmap: seq 5 is buffered; ack covers seq 4 + bit0 -> seq 5.
    CHECK(c.Send(static_cast<uint8_t>(net::MsgType::Ping), PingPayload(5)).Ok()); // seq 5
    CHECK_EQ(c.PendingFrames(), 2u);
    c.OnAck(4, 1u); // ackSeq=4, bit0 => seq ackSeq+1 = 5
    CHECK_EQ(c.PendingFrames(), 0u);
    CHECK_EQ(emitted, 6u); // each Send transmitted immediately, nothing more
}

// Timeout: acks are starved (dropAll on the return path) so a frame is never
// acknowledged; the channel fires its timeout callback and reports disconnected.
TEST(ReliableTimeout) {
    core::Result<net::UdpSocket> sockARes = net::UdpSocket::Create();
    core::Result<net::UdpSocket> sockBRes = net::UdpSocket::Create();
    CHECK(sockARes.Ok());
    CHECK(sockBRes.Ok());
    net::UdpSocket sockA = std::move(sockARes.Value());
    net::UdpSocket sockB = std::move(sockBRes.Value());
    CHECK(sockA.BindLoopback(0).Ok());
    CHECK(sockB.BindLoopback(0).Ok());
    CHECK(sockA.SetPeer(net::NetAddress{"127.0.0.1", sockB.Port()}).Ok());
    CHECK(sockB.SetPeer(net::NetAddress{"127.0.0.1", sockA.Port()}).Ok());

    LinkFaults toServer, toClient;
    toServer.sock = &sockA;
    toClient.sock = &sockB;
    toClient.dropAll = true; // acks never reach the client

    net::ReliableConfig cfg;
    cfg.retransmitMs = 100;
    cfg.timeoutMs = 1000;
    net::ReliableChannel client(cfg), server(cfg);
    bool fired = false;
    client.SetTimeout([&]() { fired = true; });
    client.SetOutbound([&](const std::vector<uint8_t>& bytes) { toServer.Transmit(bytes); });
    server.SetOutbound([&](const std::vector<uint8_t>& bytes) { toClient.Transmit(bytes); });

    CHECK(client.Send(static_cast<uint8_t>(net::MsgType::Ping), PingPayload(1)).Ok());

    uint64_t now = 0;
    for (int i = 0; i < 400 && !client.TimedOut(); ++i) {
        now += 10;
        uint8_t buf[2048];
        for (;;) {
            core::Result<size_t> r = sockB.Recv(buf, sizeof(buf));
            if (!r.Ok() || r.Value() == 0) break;
            server.OnDatagram(buf, r.Value()); // data still flows to the receiver
        }
        client.Tick(now);
        server.Tick(now);
    }

    CHECK(client.TimedOut());
    CHECK(!client.Connected());
    CHECK(fired);
    // The receiver half was unaffected: the frame arrived exactly once.
    CHECK_EQ(server.DeliveredCount(), 1u);
}

// Sequence wrap: the u16 sequence number crosses 65535 -> 0 without losing
// ordering. The receiver is driven directly (no sockets) for speed.
TEST(ReliableSeqWrap) {
    net::MessageCodec codec;
    net::ReliableChannel recv;
    std::vector<uint16_t> delivered;
    recv.SetDeliver([&](const net::DecodedMessage& m) { delivered.push_back(m.header.seq); });

    auto feed = [&](uint16_t seq) {
        core::Result<std::vector<uint8_t>> frame =
            codec.Encode(net::MsgType::Ping, seq, net::MsgPing{static_cast<uint64_t>(seq)});
        CHECK(frame.Ok());
        std::vector<uint8_t> datagram = net::EncodeDatagram(frame.Value());
        recv.OnDatagram(datagram.data(), datagram.size());
    };

    // Deliver seqs 0..65534 so nextExpected_ lands right before the wrap.
    for (uint32_t i = 0; i <= 65534; ++i) feed(static_cast<uint16_t>(i));
    CHECK_EQ(recv.NextExpected(), 65535u);
    CHECK_EQ(recv.DeliveredCount(), 65535u);

    // Out-of-order arrivals across the boundary are delivered in wrapped order:
    // 65535, then 0, then 1; duplicates and stale frames are dropped.
    feed(1);     // held (not contiguous yet)
    feed(0);     // held behind 65535
    feed(65535); // delivers 65535, 0, 1
    feed(65535); // duplicate: dropped
    feed(2);     // contiguous next
    feed(1);     // stale duplicate: dropped

    CHECK_EQ(recv.NextExpected(), 3u);
    CHECK_EQ(delivered.size(), 65535u + 4u);
    CHECK_EQ(delivered[65534u], 65534u);
    CHECK_EQ(delivered[65535u], 65535u);
    CHECK_EQ(delivered[65536u], 0u);
    CHECK_EQ(delivered[65537u], 1u);
    CHECK_EQ(delivered[65538u], 2u);
}

// MTU cap: a frame larger than maxFrameBytes is rejected by the sender.
TEST(ReliableFrameTooLarge) {
    net::ReliableChannel c;
    c.SetOutbound([](const std::vector<uint8_t>&) {});
    std::vector<uint8_t> big(net::ReliableConfig{}.maxFrameBytes + 1, 0xAB);
    CHECK(!c.Send(static_cast<uint8_t>(net::MsgType::Ping), big).Ok());
}

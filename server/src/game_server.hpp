#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "neon/core/result.hpp"
#include "neon/ecs/world.hpp"
#include "neon/net/protocol.hpp"
#include "neon/net/reliable.hpp"
#include "neon/net/socket.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/script/gamevars.hpp"
#include "net_input.hpp"

namespace neon::server {

// Serializes a protocol message into the raw body bytes that
// ReliableChannel::Send expects (the transport adds its own frame header,
// protocol version and sequence number). Uses the message's Write() codec pair
// so the wire layout is exactly what the T6.1 codec produces.
template <typename T>
std::vector<uint8_t> EncodeBody(const T& msg) {
    core::Serializer s;
    msg.Write(s);
    const std::vector<uint8_t>& data = s.Data();
    return std::vector<uint8_t>(data.begin() + core::kHeaderBytes, data.end());
}

// Strict key ordering for std::map<NetAddress, ...> (host, then port).
struct NetAddrLess {
    bool operator()(const net::NetAddress& a, const net::NetAddress& b) const {
        if (a.host != b.host) return a.host < b.host;
        return a.port < b.port;
    }
};

// Headless authoritative server (T6.3): owns a sim-only GameRuntime and
// replicates it over UDP to connected clients.
//
// Loop model: the host calls Step(nowMs) with a monotonic clock (wall time in
// neon_server, a deterministic fake clock in tests). Each Step pumps inbound
// datagrams, runs the 60Hz fixed-step simulation via an accumulator, and
// broadcasts MsgSnapshot (entity positions) to every client. No threads, no
// window, no GL/audio — pure simulation + UDP.
//
// Client flow: the first MsgJoin from an unknown address creates a Client
// (ReliableChannel wired to that address) and answers MsgWelcome{clientId,
// tick}. Subsequent MsgInput/MsgPing from that address update the client's
// last input / get a pong. A client with no packets for clientTimeoutMs (or
// whose reliable channel times out) is disconnected.
//
// INPUT MODEL (v1, single-controller): the FIRST client to join is the "input
// controller" — its MsgInput feeds the NetInput that the runtime's scripts
// read via InputAxis/InputKey. Additional clients join and receive snapshots
// but their inputs are ignored until a multi-input model exists (the demo has
// exactly one player). If the controller disconnects, the next client is
// promoted.
//
// DETERMINISM: the runtime uses the deterministic Lua sandbox (T2.4) seeded by
// Config::rngSeed, the fixed tick drives all script/BT/physics updates, and
// the snapshot/entity iteration order is stable — identical inputs + identical
// Step(nowMs) sequences produce identical world state and snapshot streams.
class GameServer {
public:
    struct Config {
        uint16_t port = 26000;
        bool loopback = false;           // bind 127.0.0.1 instead of 0.0.0.0 (tests)
        std::string sceneJsonPath;       // scene JSON file to load (used when sceneJson is empty)
        std::string sceneJson;           // inline scene JSON (overrides sceneJsonPath)
        std::string scriptBaseDir;       // base dir for scripts/behaviors/prefabs
        std::string assetBaseDir;        // asset root (unused headless; parity with player)
        uint64_t rngSeed = 20260821u;    // fixed: the sim is reproducible
        uint64_t clientTimeoutMs = 5000; // disconnect a client silent this long
        uint32_t snapshotEveryTicks = 1; // broadcast a snapshot every N fixed ticks
        int maxClients = 64;
    };

    bool Start(const Config& cfg);
    void Step(uint64_t nowMs);
    void Shutdown();

    bool Running() const { return running_; }
    uint16_t Port() const; // bound local UDP port (0 until Start)
    uint32_t ClientCount() const { return static_cast<uint32_t>(clients_.size()); }
    uint32_t CurrentTick() const { return tick_; }
    uint64_t ControllerClientId() const;
    script::GameVars& GameVars() { return runtime_.GameVars(); }
    ecs::World& World() { return runtime_.World(); }

private:
    struct Client {
        net::NetAddress addr;
        net::ReliableChannel chan;
        uint64_t clientId = 0;
        uint64_t lastSeenMs = 0;
        net::MsgInput lastInput; // v1: stored per client, only the controller's is applied
        bool hasInput = false;
        uint32_t dropLogCount = 0; // throttles the deferred-snapshot log
    };

    void PumpNetwork(uint64_t nowMs);
    void OnClientMessage(const net::NetAddress& addr, const net::DecodedMessage& msg);
    void HandleJoin(const net::NetAddress& addr, const net::MsgJoin& join);
    void AdmitClient(const net::NetAddress& addr, const net::MsgJoin& join);
    void HandleInput(const net::NetAddress& addr, const net::MsgInput& input);
    void HandlePing(const net::NetAddress& addr, const net::MsgPing& ping);
    void SendWelcome(Client& c);
    void SendPong(Client& c, uint64_t sendTime);
    void SendDespawn(Client& c, uint64_t entityId);
    void ApplyControllerInput();
    void BroadcastSnapshot();
    void TickChannels(uint64_t nowMs);
    void DropTimedOutClients(uint64_t nowMs);
    void RemoveClient(const net::NetAddress& addr);
    uint64_t EntityKey(ecs::Entity e) const;

    Config cfg_;
    net::UdpSocket sock_;
    net::MessageCodec codec_; // decodes datagrams from unknown senders (join path)
    scene::GameRuntime runtime_;
    NetInput controllerInput_; // wired into the runtime; fed by the controller client
    std::map<net::NetAddress, Client, NetAddrLess> clients_;
    std::vector<net::NetAddress> pendingRemovals_; // channel-timeout disconnects
    net::NetAddress controllerAddr_;
    uint64_t nextClientId_ = 0;
    uint32_t tick_ = 0;
    double accumulator_ = 0.0;
    uint64_t lastStepMs_ = 0;
    uint64_t nowMs_ = 0;
    std::set<uint64_t> lastSnapshotIds_; // entity ids of the last broadcast (despawn diff)
    bool running_ = false;
};

} // namespace neon::server

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "neon/core/result.hpp"
#include "neon/ecs/world.hpp"
#include "neon/net/protocol.hpp"
#include "neon/net/reliable.hpp"
#include "neon/net/rpc.hpp"
#include "neon/net/socket.hpp"
#include "neon/scene/game_runtime.hpp"
#include "neon/script/gamevars.hpp"
#include "aoi.hpp"
#include "net_input.hpp"
#include "scripted_input.hpp"

namespace neon::server {

// Thin alias onto the canonical shared encoder (neon::net::EncodeBody). Kept
// for call-site readability; the implementation lives in the engine so the
// client player and the tests can never drift from the server's wire layout.
template <typename T>
std::vector<uint8_t> EncodeBody(const T& msg) {
    return net::EncodeBody(msg);
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
// datagrams, runs at most ONE 60Hz fixed simulation step (the accumulator
// residual carries over and drains on later calls), and broadcasts
// MsgSnapshot (entity positions) to every client. Callers that count ticks
// (e.g. neon_server's --ticks N) get an exact count: a Step never produces a
// second tick from the accumulator residual. No threads, no window, no
// GL/audio — pure simulation + UDP.
//
// SNAPSHOT SIZE CAP: the reliable transport caps every frame at
// Config().maxFrameBytes (~1200 bytes), so a full-list snapshot fits roughly
// 48 entities before ReliableChannel::Send refuses it. Such snapshots are
// dropped (counted in SnapshotTooBig(), logged at Warn, throttled) — a client
// simply stays on its last received state.
//
// AOI (T6.5): snapshots are per-client and contain only the entities inside
// the client's interest set — the (2r+1)^2 cells around its controlled entity.
// Entities entering the set are announced with MsgSpawn, entities leaving with
// MsgDespawn (diffed against the client's last interest set); the controlled
// entity is always included. This keeps per-client snapshots small even in
// worlds much larger than the ~48-entity frame cap.
//
// Client flow: the first MsgJoin from an unknown address creates a Client
// (ReliableChannel wired to that address) and answers MsgWelcome{clientId,
// tick}. Subsequent MsgInput/MsgPing from that address update the client's
// last input / get a pong. A client with no packets for clientTimeoutMs (or
// whose reliable channel times out) is disconnected.
//
// INPUT MODEL (v2, multi-player): every client owns a NetInput fed by ITS
// MsgInput. A scene opts in by defining on_player_join(clientId): the server
// calls it when a client is admitted, the script spawns that client's player
// and calls BindPlayerToClient(player, clientId), and the runtime then routes
// the client's input to that entity's script via per-entity input resolution.
// Scenes without on_player_join keep the v1 single-controller fallback: the
// first client's input drives the shared NetInput every script reads, and the
// next client is promoted if the controller disconnects.
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
        // AOI (T6.5): each client's snapshot contains only the entities in the
        // (2*aoiRadiusCells+1)^2 cells centered on its controlled entity, in a
        // grid of aoiCellSize world units. Defaults replicate a 96x96-unit
        // area around the player (the demo scene); scenes with entities spread
        // wider only replicate the nearby ones.
        float aoiCellSize = AoiGrid::kDefaultCellSize;
        int aoiRadiusCells = 1;
    };

    bool Start(const Config& cfg);
    void Step(uint64_t nowMs);
    void Shutdown();

    bool Running() const { return running_; }
    uint16_t Port() const; // bound local UDP port (0 until Start)
    uint32_t ClientCount() const { return static_cast<uint32_t>(clients_.size()); }
    uint32_t CurrentTick() const { return tick_; }
    uint64_t ControllerClientId() const;
    // Total anonymous accounts assigned so far (T6.6). A monotonically
    // increasing counter: every accepted MsgLogin bumps it, so it is also the
    // next account id (0 before any login).
    uint64_t AccountCount() const { return nextAccountId_; }
    // The stable entity key every client's AOI is centered on: the script
    // entity of kind "player" if the scene spawned one, else the first script
    // (CTransformBind) entity, else 0 (clients then focus on the world origin).
    uint64_t ControlledEntityKey();
    script::GameVars& GameVars() { return runtime_.GameVars(); }
    ecs::World& World() { return runtime_.World(); }

    // Snapshot replication stats (admin/tests). SnapshotTooBig counts
    // snapshots dropped because they exceeded the ~1200-byte frame cap (see
    // the loop-model comment); SnapshotDrops counts snapshots the reliable
    // channel refused for other reasons (e.g. a client's send window full).
    uint64_t SnapshotTooBig() const { return snapshotTooBig_; }
    uint64_t SnapshotDrops() const { return snapshotDrops_; }

    // T6.7 test/demo injection: when non-empty, the controller input comes from
    // this fixed scripted sequence instead of a socket client (the
    // deterministic-acceptance path; no clients are needed). The input whose
    // tick equals the current fixed step drives that step. Ignored once empty
    // (default: the normal v1 client-controller path).
    void SetScriptedInputs(std::vector<ScriptedInput> inputs) {
        scriptedInputs_ = std::move(inputs);
    }

private:
    struct Client {
        net::NetAddress addr;
        net::ReliableChannel chan;
        uint64_t clientId = 0;
        uint64_t accountId = 0; // v0 anonymous account (T6.6); 0 = not logged in
        uint64_t lastSeenMs = 0;
        net::MsgInput lastInput;
        NetInput input; // per-client input state fed by THIS client's MsgInput
        bool hasInput = false;
        // AOI: the entity ids replicated to this client's last snapshot.
        // Spawn/despawn diffs are computed against this set, so each client
        // gets exactly the entities it has not seen yet / has lost.
        std::set<uint64_t> lastInterest;
        std::string room;  // P2-4: room membership ("" = lobby)
        uint32_t dropLogCount = 0; // throttles the deferred-snapshot log
    };

    void PumpNetwork(uint64_t nowMs);
    void OnClientMessage(const net::NetAddress& addr, const net::DecodedMessage& msg);
    void HandleJoin(const net::NetAddress& addr, const net::MsgJoin& join);
    void AdmitClient(const net::NetAddress& addr, const std::string& name,
                     uint32_t version);
    void HandleLogin(const net::NetAddress& addr, const net::MsgLogin& login);
    void HandleInput(const net::NetAddress& addr, const net::MsgInput& input);
    void HandlePing(const net::NetAddress& addr, const net::MsgPing& ping);
    // P2-4 production RPC: dispatch + room management over MsgRpc.
    void SetupRpc();
    void HandleRpc(const net::NetAddress& addr, const net::MsgRpc& rpc);
    void SendRpc(Client& c, const std::string& name, const std::string& argsJson);
    void BroadcastRoom(const std::string& room, const std::string& name,
                       const std::string& argsJson);
    void SendWelcome(Client& c);
    void SendLoginOk(Client& c);
    void SendCharList(Client& c);
    void SendPong(Client& c, uint64_t sendTime);
    void SendDespawn(Client& c, uint64_t entityId);
    void ApplyControllerInput();
    void BroadcastSnapshot();
    void TickChannels(uint64_t nowMs);
    void DropTimedOutClients(uint64_t nowMs);
    void RemoveClient(const net::NetAddress& addr);
    uint64_t EntityKey(ecs::Entity e) const;
    // The NetInput of the client with `clientId` (nullptr when disconnected).
    NetInput* ClientInputById(uint64_t clientId);

    Config cfg_;
    net::UdpSocket sock_;
    net::MessageCodec codec_; // decodes datagrams from unknown senders (join path)
    scene::GameRuntime runtime_;
    NetInput controllerInput_; // wired into the runtime; fed by the controller client
    std::vector<ScriptedInput> scriptedInputs_; // T6.7 scripted-controller path
    std::map<net::NetAddress, Client, NetAddrLess> clients_;
    net::RpcDispatcher rpc_;
    // Multi-player ownership: stable entity key -> client id (set by the
    // scene's BindPlayerToClient inside on_player_join). Drives per-entity
    // input routing and per-client AOI focus.
    std::unordered_map<uint64_t, uint64_t> entityClientIds_;
    std::vector<net::NetAddress> pendingRemovals_; // channel-timeout disconnects
    net::NetAddress controllerAddr_;
    uint64_t nextClientId_ = 0;
    uint64_t nextAccountId_ = 0;
    // v0 anonymous account registry (T6.6): accountId -> the client address it
    // is bound to. A future real auth flow can replace the accept-with-counter
    // with a credential lookup while keeping this map as the session table.
    std::map<uint64_t, net::NetAddress> accountToClient_;
    uint32_t tick_ = 0;
    double accumulator_ = 0.0;
    uint64_t lastStepMs_ = 0;
    uint64_t nowMs_ = 0;
    AoiGrid grid_;                 // AOI cell index rebuilt every broadcast
    uint64_t snapshotTooBig_ = 0;  // snapshots dropped for exceeding the frame cap
    uint64_t snapshotDrops_ = 0;   // snapshots dropped for other send failures
    bool running_ = false;
};

} // namespace neon::server

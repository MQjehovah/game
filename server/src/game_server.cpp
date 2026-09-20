#include "game_server.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include "neon/core/log.hpp"
#include "neon/physics/jolt_world.hpp"
#include "neon/script/bindings.hpp"

namespace neon::server {
namespace {

constexpr double kFixedDt = 1.0 / 60.0; // seconds per fixed simulation step
constexpr size_t kRecvBuf = 4096;       // >= 2 + maxFrameBytes

// Rotation around the Y axis from a quaternion (matches Quat::FromEuler's
// yaw/pitch/roll convention); replicated entities carry heading as yaw.
float YawOf(const math::Quat& q) {
    return std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

// NetAddress has no operator==; compare host+port explicitly.
bool SameAddr(const net::NetAddress& a, const net::NetAddress& b) {
    return a.host == b.host && a.port == b.port;
}

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    in.seekg(0, std::ios::beg);
    out.assign(static_cast<size_t>(n > 0 ? n : 0), 0);
    if (n > 0) in.read(reinterpret_cast<char*>(out.data()), n);
    return !in.bad();
}

} // namespace

bool GameServer::Start(const Config& cfg) {
    cfg_ = cfg;
    if (cfg_.clientTimeoutMs == 0) cfg_.clientTimeoutMs = 5000;
    if (cfg_.snapshotEveryTicks == 0) cfg_.snapshotEveryTicks = 1;
    SetupRpc();
    // S1: one default match owns all per-simulation state.
    match_ = std::make_unique<Match>();

    // Scene source: inline JSON wins, else a game.pack (VFS, virtual scene
    // path), else the scene file on disk.
    std::string sceneJson = cfg_.sceneJson;
    bool packMode = false;
    if (sceneJson.empty() && !cfg_.packPath.empty()) {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(cfg_.packPath, bytes)) {
            NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error,
                         "server: cannot read pack '%s'", cfg_.packPath.c_str());
            return false;
        }
        auto vfs = std::make_shared<io::MountStack>();
        vfs->Mount(std::make_shared<io::PackFileSystem>(std::move(bytes)));
        std::string scenePath = cfg_.sceneJsonPath; // virtual path (optional)
        if (scenePath.empty()) {
            auto g = vfs->ReadFile("game.json");
            if (g.Ok()) {
                std::string perr;
                core::Json root = core::Json::Parse(
                    std::string(g.Value().begin(), g.Value().end()), &perr);
                if (const core::Json* s = root.Get("startScene")) scenePath = s->GetString();
            }
        }
        if (scenePath.empty()) {
            NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error,
                         "server: pack has no game.json startScene and no --scene given");
            return false;
        }
        auto sres = vfs->ReadFile(scenePath);
        if (!sres.Ok()) {
            NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error,
                         "server: pack missing scene '%s'", scenePath.c_str());
            return false;
        }
        sceneJson.assign(sres.Value().begin(), sres.Value().end());
        match_->packVfs = vfs; // outlive the runtime
        packMode = true;
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                     "server: pack '%s' served via VFS (scene '%s')", cfg_.packPath.c_str(),
                     scenePath.c_str());
    } else if (sceneJson.empty() && !cfg_.sceneJsonPath.empty()) {
        if (!ReadFile(cfg_.sceneJsonPath, sceneJson)) {
            NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error,
                         "server: cannot read scene '%s'", cfg_.sceneJsonPath.c_str());
            return false;
        }
    }
    if (sceneJson.empty()) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error,
                     "server: no scene given (set sceneJson or sceneJsonPath)");
        return false;
    }

    core::Result<net::UdpSocket> sock = net::UdpSocket::Create();
    if (!sock.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error, "server: %s",
                     sock.Error().c_str());
        return false;
    }
    sock_ = std::move(sock.Value());
    core::Status bind =
        cfg_.loopback ? sock_.BindLoopback(cfg_.port) : sock_.Bind(cfg_.port);
    if (!bind.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error, "server: %s",
                     bind.Error().c_str());
        sock_.Close();
        return false;
    }

    // Headless runtime: no renderer/audio/assets, just scripts + BT + physics.
    sceneJson_ = sceneJson;
    packMode_ = packMode;
    if (!InitMatch(*match_, sceneJson, packMode)) {
        sock_.Close();
        return false;
    }

    running_ = true;
    lastStepMs_ = 0;
    nowMs_ = 0;
    nextClientId_ = 0;
    nextAccountId_ = 0;
    accountToClient_.clear();
    controllerAddr_ = {};
    clients_.clear();
    pendingRemovals_.clear();
    roomMatches_.clear();
    startedRooms_.clear();
    NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                 "server: listening on %s:%u (%zu entities, %zu scripts, %zu trees)",
                 cfg_.loopback ? "127.0.0.1" : "0.0.0.0", Port(), match_->runtime.EntityCount(),
                 match_->runtime.ScriptCount(), match_->runtime.BehaviorTreeCount());
    return true;
}

// Kernel + runtime + script hooks for one match. Shared by the default match
// and each per-room match.
bool GameServer::InitMatch(Match& m, const std::string& sceneJson, bool packMode) {
    m.kernel = std::make_unique<kernel::Kernel>();
    if (cfg_.physicsBackend.rfind("plugin:", 0) != 0) {
#ifdef NEON_ENABLE_JOLT
        if (cfg_.physicsBackend == "jolt")
            m.kernel->Add(std::make_unique<modules::PhysicsModule>(
                std::make_unique<physics::JoltWorld>()));
        else
#endif
            m.kernel->Add(std::make_unique<modules::PhysicsModule>(
                std::make_unique<physics::World>()));
    }
    if (auto lua = script::CreateLuaHost())
        m.kernel->Add(std::make_unique<modules::ScriptModule>(std::move(lua)));
    m.kernel->Init();

    // A room match shares the default match's pack VFS (read-only).
    if (packMode && !m.packVfs && match_) m.packVfs = match_->packVfs;

    scene::GameRuntimeConfig rcfg;
    rcfg.assets = nullptr;
    rcfg.headless = true;
    rcfg.services = &m.kernel->Services();
    rcfg.scriptBaseDir = packMode ? std::string() : cfg_.scriptBaseDir;
    rcfg.assetBaseDir = packMode ? std::string() : cfg_.assetBaseDir;
    if (packMode) rcfg.fileSystem = m.packVfs.get();
    rcfg.rngSeed = cfg_.rngSeed;
    rcfg.input = &m.controllerInput;
    rcfg.physicsBackend = cfg_.physicsBackend;
    core::Status st = m.runtime.Start(sceneJson, rcfg);
    if (!st.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error, "server: %s",
                     st.Error().c_str());
        return false;
    }

    // Input routing: scripts read the input of the client that owns their
    // entity (bound via BindPlayerToClient inside on_player_join/on_match_start);
    // unbound entities fall back to this match's shared controller input.
    script::ScriptContext& ctx = m.runtime.ScriptContext();
    ctx.inputForEntity = [this, &m](ecs::Entity e) -> platform::IInput* {
        const auto it = m.entityClientIds.find(EntityKey(e));
        if (it != m.entityClientIds.end()) {
            if (NetInput* in = ClientInputById(it->second)) return in;
        }
        return &m.controllerInput;
    };
    ctx.bindPlayerToClient = [this, &m](ecs::Entity e, double clientId) {
        if (!e.IsValid()) return;
        const uint64_t id = static_cast<uint64_t>(clientId);
        if (ClientInputById(id) != nullptr) m.entityClientIds[EntityKey(e)] = id;
    };
    // Script Rpc(name, args): broadcast to this match's clients only.
    ctx.rpcCall = [this, &m](const std::string& name, const std::string& argsJson) {
        for (auto& kv : clients_)
            if (MatchForClient(kv.second) == &m) SendRpc(kv.second, name, argsJson);
    };

    m.grid.SetCellSize(cfg_.aoiCellSize);
    m.grid.Clear();
    m.entityClientIds.clear();
    m.tick = 0;
    m.accumulator = 0.0;
    m.snapshotTooBig = 0;
    m.snapshotDrops = 0;
    return true;
}

void GameServer::Step(uint64_t nowMs) {
    if (!running_) return;
    nowMs_ = nowMs;

    // 1) Ingest everything the network delivered since the last Step.
    PumpNetwork(nowMs_);
    // 2) Reliable channels: retransmit due frames, emit acks, fire timeouts.
    TickChannels(nowMs_);
    // 3) Fixed-step simulation. The accumulator tracks elapsed time exactly like
    // core::Application, but Step runs AT MOST ONE fixed tick per call: callers
    // (neon_server's loop) count tick consumptions, so `--ticks N` stops at
    // exactly N — the accumulator residual never produces a second tick inside
    // a single Step (no overshoot). The leftover accumulates and drains one
    // tick per later call, so wall-clock pacing still catches up.
    const double dtSeconds = static_cast<double>(nowMs_ - lastStepMs_) / 1000.0;
    lastStepMs_ = nowMs_;
    auto stepOne = [&](Match& m) {
        m.accumulator += dtSeconds;
        if (m.accumulator < kFixedDt) return;
        m.accumulator -= kFixedDt;
        ApplyControllerInput(m);
        // G3-4 lag compensation: rewind hit tests by the most latent live
        // client's one-way latency (per match).
        uint64_t maxRttMs = 0;
        for (const auto& kv : clients_)
            if (MatchForClient(kv.second) == &m) maxRttMs = std::max(maxRttMs, kv.second.rttMs);
        m.runtime.SetAutoLagComp(LagTicksForRtt(maxRttMs));
        m.runtime.Tick(static_cast<float>(kFixedDt));
        ++m.tick;
        if (m.tick % cfg_.snapshotEveryTicks == 0) BroadcastSnapshot(m);
        m.controllerInput.EndFrame(); // advance edges for the next tick
    };
    if (match_) stepOne(*match_);
    for (auto& kv : roomMatches_) stepOne(*kv.second);
    // 4) Disconnect stale clients (inactivity + reliable-channel timeouts).
    DropTimedOutClients(nowMs_);
    // 5) Reap empty room matches (release their runtime/AOI); the room can be
    //    started again later.
    for (auto it = roomMatches_.begin(); it != roomMatches_.end();) {
        if (ClientsInRoom(it->first).empty()) {
            it->second->runtime.Stop();
            startedRooms_.erase(it->first);
            it = roomMatches_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameServer::PumpNetwork(uint64_t nowMs) {
    uint8_t buf[kRecvBuf];
    for (;;) {
        core::Result<net::RecvPacket> r = sock_.RecvFrom(buf, sizeof(buf));
        if (!r.Ok() || r.Value().size == 0) break;
        const net::NetAddress& from = r.Value().from;
        const size_t size = r.Value().size;

        auto it = clients_.find(from);
        if (it != clients_.end()) {
            it->second.lastSeenMs = nowMs;
            it->second.chan.OnDatagram(buf, size);
            continue;
        }

        // Unknown sender: a valid MsgJoin (T6.3 transport join) or MsgLogin
        // (T6.6 account step) may create a client; anything else is dropped
        // (spoofing / garbage / pre-join messages). The datagram is then fed
        // through the new channel's OnDatagram so its seq-space advances with
        // the channel — subsequent client frames align.
        if (size < 2) continue;
        const uint16_t len = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
        if (static_cast<size_t>(len) != size - 2) continue;
        core::Result<net::DecodedMessage> dec = codec_.Decode(buf + 2, len);
        if (!dec.Ok()) continue;
        const uint8_t msgId = dec.Value().header.msgId;
        if (msgId == static_cast<uint8_t>(net::MsgType::Join)) {
            const net::MsgJoin& join = std::get<net::MsgJoin>(dec.Value().payload);
            AdmitClient(from, join.name, join.version);
            auto admitted = clients_.find(from);
            if (admitted != clients_.end()) {
                admitted->second.lastSeenMs = nowMs;
                admitted->second.chan.OnDatagram(buf, size); // delivers Join -> welcome
            }
        } else if (msgId == static_cast<uint8_t>(net::MsgType::Login)) {
            const net::MsgLogin& login = std::get<net::MsgLogin>(dec.Value().payload);
            AdmitClient(from, login.name, login.clientVersion);
            auto admitted = clients_.find(from);
            if (admitted != clients_.end()) {
                admitted->second.lastSeenMs = nowMs;
                admitted->second.chan.OnDatagram(buf, size); // delivers Login -> LoginOk
            }
        } else {
            NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                         "server: dropping non-join/login datagram from unknown %s:%u",
                         from.host.c_str(), from.port);
        }
    }
}

void GameServer::OnClientMessage(const net::NetAddress& addr,
                                 const net::DecodedMessage& msg) {
    switch (static_cast<net::MsgType>(msg.header.msgId)) {
        case net::MsgType::Join:
            if (auto* m = std::get_if<net::MsgJoin>(&msg.payload)) HandleJoin(addr, *m);
            break;
        case net::MsgType::Login:
            if (auto* m = std::get_if<net::MsgLogin>(&msg.payload)) HandleLogin(addr, *m);
            break;
        case net::MsgType::Input:
            if (auto* m = std::get_if<net::MsgInput>(&msg.payload)) HandleInput(addr, *m);
            break;
        case net::MsgType::Ping:
            if (auto* m = std::get_if<net::MsgPing>(&msg.payload)) HandlePing(addr, *m);
            break;
        case net::MsgType::Rpc:
            if (auto* m = std::get_if<net::MsgRpc>(&msg.payload)) HandleRpc(addr, *m);
            break;
        case net::MsgType::Welcome:
        case net::MsgType::Snapshot:
        case net::MsgType::Spawn:
        case net::MsgType::Despawn:
        case net::MsgType::LoginOk:
        case net::MsgType::CharList:
        case net::MsgType::Pong:
        case net::MsgType::Ack:
            break; // server-authoritative: client replication messages are ignored
    }
}

void GameServer::HandleJoin(const net::NetAddress& addr, const net::MsgJoin& join) {
    auto it = clients_.find(addr);
    if (it == clients_.end()) {
        // A Join that reached us outside the normal admit-then-deliver path
        // (e.g. a re-created client after a race): admit it before welcoming.
        AdmitClient(addr, join.name, join.version);
        it = clients_.find(addr);
    }
    if (it != clients_.end()) {
        it->second.lastSeenMs = nowMs_;
        SendWelcome(it->second); // idempotent: re-joins get a fresh welcome
    }
}

// v0 anonymous login (T6.6): accepts any non-empty name, assigns a fresh
// account id (a plain counter; a real auth would replace the accept with a
// credential lookup) and answers MsgLoginOk + the placeholder MsgCharList.
// An empty name is rejected: no account is created and nothing is sent back.
void GameServer::HandleLogin(const net::NetAddress& addr, const net::MsgLogin& login) {
    auto it = clients_.find(addr);
    if (it == clients_.end()) {
        // A Login that reached us outside the normal admit-then-deliver path
        // (e.g. a re-created client after a race): admit it before logging in.
        AdmitClient(addr, login.name, login.clientVersion);
        it = clients_.find(addr);
    }
    if (it == clients_.end()) return; // e.g. server full

    Client& c = it->second;
    // P2-4 anti-cheat: banned names cannot log in.
    if (bannedNames_.count(login.name) != 0) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: login from %s:%u refused (banned name)", addr.host.c_str(),
                     addr.port);
        // Defer: this runs inside the client's ReliableChannel deliver
        // callback (OnClientMessage -> HandleLogin), so destroying the Client
        // now would free the channel mid-OnDatagram (use-after-free; crashes
        // on MSVC, "works" by luck on libstdc++). DropTimedOutClients drains
        // pendingRemovals_ at the end of the same Step.
        pendingRemovals_.push_back(addr);
        return;
    }
    c.lastSeenMs = nowMs_;
    if (login.name.empty()) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: login from client %llu (%s:%u) rejected (empty name)",
                     static_cast<unsigned long long>(c.clientId), addr.host.c_str(),
                     addr.port);
        return;
    }

    if (c.accountId == 0) {
        c.accountId = ++nextAccountId_;
        accountToClient_[c.accountId] = addr;
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                     "server: client %llu logged in as anonymous account id=%llu "
                     "name='%s' clientVersion=%u",
                     static_cast<unsigned long long>(c.clientId),
                     static_cast<unsigned long long>(c.accountId), login.name.c_str(),
                     login.clientVersion);
    }
    // Idempotent: a re-login re-asserts the session (same account id).
    SendLoginOk(c);
    SendCharList(c);
}

// Creates the Client for a joining address (channel wiring, id assignment,
// v1 controller election). Does NOT welcome — the join/login datagram is
// delivered through the channel right after, which sequences the channel and
// triggers HandleJoin/HandleLogin.
void GameServer::AdmitClient(const net::NetAddress& addr, const std::string& name,
                             uint32_t version) {
    if (clients_.count(addr) != 0) return;
    // P2-4 anti-cheat: banned names are refused at the door.
    if (bannedNames_.count(name) != 0) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: join from %s:%u refused (banned name)", addr.host.c_str(),
                     addr.port);
        return;
    }
    if (clients_.size() >= static_cast<size_t>(cfg_.maxClients)) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: client %s:%u rejected (server full)", addr.host.c_str(),
                     addr.port);
        return;
    }

    Client c;
    c.addr = addr;
    c.name = name;
    c.clientId = ++nextClientId_;
    c.lastSeenMs = nowMs_;
    c.chan.SetOutbound([this, addr](const std::vector<uint8_t>& bytes) {
        if (!sock_.Valid()) return;
        sock_.SetPeer(addr);
        sock_.Send(bytes.data(), bytes.size());
    });
    c.chan.SetDeliver([this, addr](const net::DecodedMessage& msg) {
        OnClientMessage(addr, msg);
    });
    c.chan.SetTimeout([this, addr]() {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: client %s:%u reliable channel timed out",
                     addr.host.c_str(), addr.port);
        pendingRemovals_.push_back(addr);
    });

    auto res = clients_.emplace(addr, std::move(c));
    Client& client = res.first->second;
    NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                 "server: client id=%llu name='%s' version=%u admitted from %s:%u",
                 static_cast<unsigned long long>(client.clientId), name.c_str(),
                 version, addr.host.c_str(), addr.port);

    // v1 input model: the first joiner drives the scene's player script.
    if (!controllerAddr_.Valid()) {
        controllerAddr_ = addr;
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                     "server: client id=%llu is the input controller (v1 single-client model)",
                     static_cast<unsigned long long>(client.clientId));
    }

    // Multi-player (v2): a scene that defines on_player_join(clientId) spawns
    // and binds a player for this client right here (the script calls
    // BindPlayerToClient inside the handler).
    if (match_->runtime.HasScriptFunction("on_player_join")) {
        match_->runtime.CallScriptFunction(
            "on_player_join", {script::Value::Num(static_cast<double>(client.clientId))});
    }
    // Runtime plugins receive the same join event (Plugin.On("player_join")).
    match_->runtime.DispatchPluginEvent(
        "player_join", {script::Value::Num(static_cast<double>(client.clientId))});
}

void GameServer::HandleInput(const net::NetAddress& addr, const net::MsgInput& input) {
    auto it = clients_.find(addr);
    if (it == clients_.end()) return;
    Client& c = it->second;
    // P2-4 anti-cheat: input-rate limiting. A client flooding inputs faster
    // than the fixed tick can consume them is throttled; repeated violations
    // get the client kicked + banned.
    if (c.inputWindowStartMs == 0) c.inputWindowStartMs = nowMs_;
    if (nowMs_ - c.inputWindowStartMs >= 1000) {
        c.inputWindowStartMs = nowMs_;
        c.inputsThisWindow = 0;
    }
    const uint32_t maxPerSec = cfg_.maxInputsPerSecond > 0 ? cfg_.maxInputsPerSecond : 120;
    if (c.inputsThisWindow >= maxPerSec) {
        ++c.violations;
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: client %llu input flood (violation %u/%u)",
                     static_cast<unsigned long long>(c.clientId), c.violations,
                     cfg_.maxViolations);
        if (cfg_.maxViolations > 0 && c.violations >= cfg_.maxViolations) {
            bannedClientIds_.insert(c.clientId);
            if (!c.name.empty()) bannedNames_.insert(c.name);
            NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                         "server: client %llu banned for input flooding",
                         static_cast<unsigned long long>(c.clientId));
            // Deferred (see HandleLogin): this runs inside the channel's
            // deliver callback; removing the client here frees the channel
            // while OnDatagram is still executing on it.
            pendingRemovals_.push_back(addr);
        }
        return;  // drop the excess input
    }
    ++c.inputsThisWindow;
    c.lastInput.seq = input.seq;
    c.lastInput.buttons = input.buttons;
    c.lastInput.moveX = std::max(-1.0f, std::min(1.0f, input.moveX));
    c.lastInput.moveY = std::max(-1.0f, std::min(1.0f, input.moveY));
    c.hasInput = true;
    // v1: only the controller client's input reaches the sim (applied at the
    // start of the next fixed tick).
}

void GameServer::HandlePing(const net::NetAddress& addr, const net::MsgPing& ping) {
    auto it = clients_.find(addr);
    if (it == clients_.end()) return;
    // G3-4: measure the round trip (client stamps sendTime at send) and keep
    // it on the client for lag-compensated hit tests. Sanity-clamped: a
    // spoofed/stale stamp is ignored rather than rewinding 16+ seconds.
    if (nowMs_ > ping.sendTime && nowMs_ - ping.sendTime < 60000u)
        it->second.rttMs = nowMs_ - ping.sendTime;
    SendPong(it->second, ping.sendTime);
}

void GameServer::SendWelcome(Client& c) {
    net::MsgWelcome welcome{c.clientId, match_->tick};
        core::Status st = c.chan.Send(static_cast<uint8_t>(net::MsgType::Welcome),
                                  net::EncodeBody(welcome));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: welcome to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::SendLoginOk(Client& c) {
    net::MsgLoginOk ok{c.accountId, match_->tick};
    core::Status st = c.chan.Send(static_cast<uint8_t>(net::MsgType::LoginOk),
                                  net::EncodeBody(ok));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: loginOk to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

// Placeholder character select (T6.6): a fixed single-character roster.
// v0 ships exactly one character ("主角"); a real character system would build
// the roster from saved character data keyed by accountId.
void GameServer::SendCharList(Client& c) {
    net::MsgCharList list;
    list.characters.push_back({1u, "主角"});
    list.count = static_cast<uint32_t>(list.characters.size());
    core::Status st = c.chan.Send(static_cast<uint8_t>(net::MsgType::CharList),
                                  net::EncodeBody(list));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: charList to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::SendPong(Client& c, uint64_t sendTime) {
    net::MsgPong pong{sendTime, nowMs_};
    core::Status st =
        c.chan.Send(static_cast<uint8_t>(net::MsgType::Pong), net::EncodeBody(pong));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                     "server: pong to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

// P2-4 production RPC: registers the built-in room handlers. Game code can
// register more via RpcDispatcher (exposed below) in Start.
void GameServer::SetupRpc() {
    rpc_ = net::RpcDispatcher();
    // ---- Lobby: a room holds up to 2 players; a 1-player room can start vs AI.
    auto putStr = [](core::Json& o, const char* k, const std::string& v) {
        core::Json j;
        j.type_ = core::Json::Type::String;
        j.string_ = v;
        o.object_[k] = std::move(j);
    };
    auto putNum = [](core::Json& o, const char* k, double v) {
        core::Json j;
        j.type_ = core::Json::Type::Number;
        j.number_ = v;
        o.object_[k] = std::move(j);
    };
    auto putBool = [](core::Json& o, const char* k, bool v) {
        core::Json j;
        j.type_ = core::Json::Type::Bool;
        j.bool_ = v;
        o.object_[k] = std::move(j);
    };
    // Host = the earliest-joined (smallest clientId) member of the room.
    auto roomHostOf = [this](const std::string& code) -> uint64_t {
        uint64_t host = 0;
        for (auto& kv : clients_)
            if (kv.second.room == code &&
                (host == 0 || kv.second.clientId < host))
                host = kv.second.clientId;
        return host;
    };
    rpc_.Register("room.create", [this, putStr, putNum](uint64_t clientId,
                                                        const std::string&) {
        std::string code;
        for (int i = 0; i < 200; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "R%04d", 1000 + (std::rand() % 9000));
            if (ClientsInRoom(buf).empty()) {
                code = buf;
                break;
            }
        }
        if (code.empty()) code = "R0001";
        if (Client* c = ClientById(clientId)) c->room = code;
        NEON_LOG_INFO("server: room.create client=%llu -> %s",
                      static_cast<unsigned long long>(clientId), code.c_str());
        core::Json reply;
        reply.type_ = core::Json::Type::Object;
        putStr(reply, "room", code);
        putNum(reply, "players", 1);
        putNum(reply, "max", 2);
        putNum(reply, "host", static_cast<double>(clientId));
        BroadcastRoom(code, "lobby", core::JsonWriter::Write(reply));
        return std::optional<std::pair<std::string, std::string>>(
            std::make_pair(std::string("room.joined"), core::JsonWriter::Write(reply)));
    });
    rpc_.Register("room.join", [this, putStr, putNum, putBool, roomHostOf](
                                   uint64_t clientId, const std::string& argsJson) {
        std::string code;
        std::string perr;
        core::Json args = core::Json::Parse(argsJson, &perr);
        if (const core::Json* r = args.Get("room")) code = r->GetString();
        if (code.empty()) code = "lobby";
        auto members = ClientsInRoom(code);
        // Joining an empty/new room creates it (implicit create, legacy
        // behavior); an existing room is capped at 2 and closed once started.
        const bool ok = members.size() < 2 && startedRooms_.count(code) == 0;
        core::Json reply;
        reply.type_ = core::Json::Type::Object;
        putBool(reply, "ok", ok);
        if (ok) {
            if (Client* c = ClientById(clientId)) c->room = code;
            putStr(reply, "room", code);
            putNum(reply, "players", static_cast<double>(ClientsInRoom(code).size()));
            putNum(reply, "max", 2);
            putNum(reply, "host", static_cast<double>(roomHostOf(code)));
        } else {
            putStr(reply, "error", "房间不存在/已满/已开始");
        }
        BroadcastRoom(code, "lobby", core::JsonWriter::Write(reply));
        return std::optional<std::pair<std::string, std::string>>(
            std::make_pair(std::string("room.joined"), core::JsonWriter::Write(reply)));
    });
    rpc_.Register("room.start", [this, roomHostOf](uint64_t clientId, const std::string&) {
        std::string code;
        if (Client* c = ClientById(clientId)) code = c->room;
        // Only the room's host may start (1 player -> vs AI, 2 -> 1v1).
        if (!code.empty() && roomHostOf(code) == clientId) StartRoomMatch(code);
        return std::optional<std::pair<std::string, std::string>>{};
    });
    // room.kick (host only): drop every other member from the room.
    rpc_.Register("room.kick", [this, roomHostOf, putNum](uint64_t clientId,
                                                          const std::string&) {
        Client* me = ClientById(clientId);
        if (me == nullptr) return std::optional<std::pair<std::string, std::string>>{};
        const std::string code = me->room;
        if (code.empty() || roomHostOf(code) != clientId)
            return std::optional<std::pair<std::string, std::string>>{};
        for (Client* m : ClientsInRoom(code)) {
            if (m->clientId == clientId) continue;
            m->room.clear();
            SendRpc(*m, "room.left", "{}");
        }
        core::Json reply;
        reply.type_ = core::Json::Type::Object;
        putNum(reply, "players", static_cast<double>(ClientsInRoom(code).size()));
        putNum(reply, "max", 2);
        putNum(reply, "host", static_cast<double>(clientId));
        BroadcastRoom(code, "lobby", core::JsonWriter::Write(reply));
        return std::optional<std::pair<std::string, std::string>>{};
    });
    rpc_.Register("room.leave", [this](uint64_t clientId, const std::string&) {
        std::string code;
        if (Client* c = ClientById(clientId)) {
            code = c->room;
            c->room.clear();
        }
        if (!code.empty()) BroadcastRoom(code, "lobby", "{}");
        return std::optional<std::pair<std::string, std::string>>(
            std::make_pair(std::string("room.left"), std::string("{}")));
    });
    rpc_.Register("room.list", [this, putNum, putBool](uint64_t, const std::string&) {
        std::map<std::string, int> counts;
        for (auto& kv : clients_)
            if (!kv.second.room.empty()) counts[kv.second.room]++;
        core::Json reply;
        reply.type_ = core::Json::Type::Object;
        core::Json arr;
        arr.type_ = core::Json::Type::Array;
        for (auto& kv : counts) {
            core::Json o;
            o.type_ = core::Json::Type::Object;
            core::Json r;
            r.type_ = core::Json::Type::String;
            r.string_ = kv.first;
            o.object_["room"] = std::move(r);
            putNum(o, "players", kv.second);
            putNum(o, "max", 2);
            putBool(o, "started", startedRooms_.count(kv.first) != 0);
            arr.array_.push_back(std::move(o));
        }
        reply.object_["rooms"] = std::move(arr);
        return std::optional<std::pair<std::string, std::string>>(
            std::make_pair(std::string("room.list"), core::JsonWriter::Write(reply)));
    });
    // room.broadcast: relays {message} as room.chat to the sender's room.
    rpc_.Register("room.broadcast", [this](uint64_t clientId, const std::string& argsJson) {
        std::string room;
        std::string message;
        std::string perr;
        core::Json args = core::Json::Parse(argsJson, &perr);
        if (const core::Json* m = args.Get("message")) message = m->GetString();
        for (const auto& kv : clients_) {
            if (kv.second.clientId == clientId) {
                room = kv.second.room;
                break;
            }
        }
        if (!room.empty()) {
            core::Json chat;
            chat.type_ = core::Json::Type::Object;
            core::Json from;
            from.type_ = core::Json::Type::Number;
            from.number_ = static_cast<double>(clientId);
            chat.object_["from"] = from;
            core::Json mj;
            mj.type_ = core::Json::Type::String;
            mj.string_ = message;
            chat.object_["message"] = std::move(mj);
            BroadcastRoom(room, "room.chat", core::JsonWriter::Write(chat));
        }
        return std::optional<std::pair<std::string, std::string>>();
    });
    // Gameplay commands: route a client's MOBA command RPC into the runtime's
    // command queue; authoritative scripts drain it with NetCommand().
    rpc_.Register("moba_cmd", [this](uint64_t clientId, const std::string& argsJson) {
        if (Client* c = ClientById(clientId)) {
            if (Match* mc = MatchForClient(*c))
                mc->runtime.PushNetCommand(clientId, "moba_cmd", argsJson);
        }
        return std::optional<std::pair<std::string, std::string>>{};
    });

    // P2-4 anti-cheat admin RPCs (placeholder: any connected client may use
    // them; a real deployment gates this behind an auth/admin role).
    rpc_.Register("admin.kick", [this](uint64_t, const std::string& argsJson) {
        std::string perr;
        core::Json args = core::Json::Parse(argsJson, &perr);
        const uint64_t target = args.Get("clientId")
                                    ? static_cast<uint64_t>(args.Get("clientId")->GetNumber())
                                    : 0;
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if (it->second.clientId == target) {
                NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                             "server: admin kicked client %llu",
                             static_cast<unsigned long long>(target));
                // Deferred (see HandleLogin): admin RPCs are processed inside
                // the sender's channel deliver callback.
                pendingRemovals_.push_back(it->first);
                break;
            }
        }
        return std::optional<std::pair<std::string, std::string>>();
    });
    rpc_.Register("admin.ban", [this](uint64_t, const std::string& argsJson) {
        std::string perr;
        core::Json args = core::Json::Parse(argsJson, &perr);
        const uint64_t target = args.Get("clientId")
                                    ? static_cast<uint64_t>(args.Get("clientId")->GetNumber())
                                    : 0;
        if (const core::Json* name = args.Get("name")) {
            if (name->IsString() && !name->GetString().empty())
                bannedNames_.insert(name->GetString());
        }
        if (target != 0) bannedClientIds_.insert(target);
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if (it->second.clientId == target) {
                NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                             "server: admin banned client %llu",
                             static_cast<unsigned long long>(target));
                pendingRemovals_.push_back(it->first);
                break;
            }
        }
        return std::optional<std::pair<std::string, std::string>>();
    });
}

std::vector<GameServer::Client*> GameServer::ClientsInRoom(const std::string& room) {
    std::vector<Client*> out;
    if (room.empty()) return out;
    for (auto& kv : clients_)
        if (kv.second.room == room) out.push_back(&kv.second);
    return out;
}

GameServer::Client* GameServer::ClientById(uint64_t id) {
    for (auto& kv : clients_)
        if (kv.second.clientId == id) return &kv.second;
    return nullptr;
}

void GameServer::StartRoomMatch(const std::string& room) {
    if (startedRooms_.count(room) != 0) return;
    auto members = ClientsInRoom(room);
    if (members.empty()) return;
    // Create a dedicated Match for this room (own runtime/physics/AOI).
    auto owned = std::make_unique<Match>();
    owned->room = room;
    if (!InitMatch(*owned, sceneJson_, packMode_)) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error,
                     "server: room '%s' match failed to start", room.c_str());
        return;
    }
    Match* mp = owned.get();
    roomMatches_[room] = std::move(owned); // visible to MatchForClient before start
    startedRooms_.insert(room);
    const uint64_t blue = members[0]->clientId;
    const uint64_t red = members.size() >= 2 ? members[1]->clientId : 0; // 0 = AI
    mp->runtime.CallScriptFunction(
        "on_match_start", {script::Value::Num(static_cast<double>(blue)),
                           script::Value::Num(static_cast<double>(red))});
    for (size_t i = 0; i < members.size(); ++i) {
        core::Json o;
        o.type_ = core::Json::Type::Object;
        core::Json s;
        s.type_ = core::Json::Type::String;
        s.string_ = (i == 0) ? "blue" : (i == 1 ? "red" : "spec");
        o.object_["side"] = std::move(s);
        SendRpc(*members[i], "match.start", core::JsonWriter::Write(o));
    }
    NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                 "server: match started room '%s' blue=%llu red=%llu", room.c_str(),
                 static_cast<unsigned long long>(blue), static_cast<unsigned long long>(red));
}

void GameServer::HandleRpc(const net::NetAddress& addr, const net::MsgRpc& rpc) {
    auto it = clients_.find(addr);
    if (it == clients_.end()) return;
    // room.leave is handled specially so the sender's own room clears first.
    if (rpc.name == "room.leave") {
        it->second.room.clear();
        SendRpc(it->second, "room.left", "{}");
        return;
    }
    std::optional<std::pair<std::string, std::string>> reply;
    if (!rpc_.Dispatch(it->second.clientId, rpc.name, rpc.argsJson, &reply)) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: unhandled rpc '%s' from client %llu", rpc.name.c_str(),
                     static_cast<unsigned long long>(it->second.clientId));
        return;
    }
    if (reply) SendRpc(it->second, reply->first, reply->second);
}

void GameServer::SendRpc(Client& c, const std::string& name, const std::string& argsJson) {
    net::MsgRpc rpc{name, argsJson};
    core::Status st =
        c.chan.Send(static_cast<uint8_t>(net::MsgType::Rpc), net::EncodeBody(rpc));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                     "server: rpc '%s' to client %llu deferred (%s)", name.c_str(),
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::BroadcastRoom(const std::string& room, const std::string& name,
                               const std::string& argsJson) {
    for (auto& kv : clients_) {
        if (kv.second.room == room) SendRpc(kv.second, name, argsJson);
    }
}

void GameServer::SendDespawn(Client& c, uint64_t entityId) {
    net::MsgDespawn despawn{entityId};
    core::Status st =
        c.chan.Send(static_cast<uint8_t>(net::MsgType::Despawn), net::EncodeBody(despawn));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                     "server: despawn to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::ApplyControllerInput(Match& m) {
    // Scripted-controller path (T6.7 determinism acceptance): when a scripted
    // sequence is installed, the input keyed to the CURRENT fixed step drives
    // the sim directly — no socket client involved.
    if (!m.scriptedInputs.empty()) {
        const net::MsgInput* in = InputForTick(m.scriptedInputs, m.tick);
        if (in)
            m.controllerInput.SetInput(in->buttons, in->moveX, in->moveY);
        else
            m.controllerInput.SetInput(0, 0.0f, 0.0f);
        return;
    }

    // Multi-player (v2): drive every THIS match's client's NetInput from ITS
    // latest MsgInput.
    for (auto& kv : clients_) {
        Client& c = kv.second;
        if (MatchForClient(c) != &m) continue;
        if (c.hasInput)
            c.input.SetInput(c.lastInput.buttons, c.lastInput.moveX, c.lastInput.moveY);
        else
            c.input.SetInput(0, 0.0f, 0.0f);
    }

    // v1 fallback for scenes without on_player_join: the controller client's
    // input drives the shared NetInput every unbound script reads.
    m.controllerInput.SetInput(0, 0.0f, 0.0f);
    auto it = clients_.find(controllerAddr_);
    if (it != clients_.end() && MatchForClient(it->second) == &m && it->second.hasInput) {
        const net::MsgInput& in = it->second.lastInput;
        m.controllerInput.SetInput(in.buttons, in.moveX, in.moveY);
    }
}

// The entity every client's AOI is centered on: the script entity of kind
// "player" if the scene spawned one, else the first script (CTransformBind)
// entity, else 0 (no controlled entity -> clients focus on the world origin).
// Matches how the client resolves its controlled entity (the first
// CTransformBind it finds); preferring the "player" kind keeps a scene with
// several script entities centered on the actual playable one.
uint64_t GameServer::ControlledEntityKey() { return ControlledEntityKey(*match_); }

uint64_t GameServer::ControlledEntityKey(Match& m) {
    ecs::World& world = m.runtime.World();
    uint64_t fallback = 0;
    auto view = world.ViewAll<script::CTransformBind>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = world.EntityAt<script::CTransformBind>(i);
        const uint64_t key = EntityKey(e);
        const auto it = m.runtime.ScriptContext().entityKinds.find(e);
        if (it != m.runtime.ScriptContext().entityKinds.end() && it->second == "player")
            return key;
        if (fallback == 0) fallback = key;
    }
    return fallback;
}

GameServer::Match* GameServer::MatchForClient(const Client& c) {
    if (!c.room.empty()) {
        auto it = roomMatches_.find(c.room);
        if (it != roomMatches_.end()) return it->second.get();
    }
    return match_.get();
}

void GameServer::BroadcastSnapshot(Match& m) {
    ecs::World& world = m.runtime.World();

    // One replicated entity: stable id, transform and the kind used for
    // MsgSpawn. Scene entities carry their name as the kind (SceneName), script
    // entities carry the Spawn("kind") recorded in ScriptContext::entityKinds;
    // the default is "box" when neither source provides one.
    struct Item {
        uint64_t id = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float yaw = 0.0f;
        std::string kind;
    };

    std::vector<Item> items;
    std::set<uint64_t> seen;
    auto add = [&](ecs::Entity e, const math::Vec3& pos, const math::Quat& rot,
                   const std::string& kind) {
        const uint64_t key = EntityKey(e);
        if (seen.count(key) != 0) return;
        seen.insert(key);
        Item it;
        it.id = key;
        it.x = pos.x;
        it.y = pos.y;
        it.z = pos.z;
        it.yaw = YawOf(rot);
        it.kind = kind.empty() ? "box" : kind;
        items.push_back(std::move(it));
    };
    {
        auto view = world.ViewAll<scene::SceneTransform>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = world.EntityAt<scene::SceneTransform>(i);
            const scene::SceneTransform* t = world.Get<scene::SceneTransform>(e);
            if (!t) continue;
            std::string kind;
            if (const scene::SceneName* n = world.Get<scene::SceneName>(e)) kind = n->name;
            add(e, t->pos, t->rot, kind);
        }
    }
    {
        auto view = world.ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = world.EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = world.Get<script::CTransformBind>(e);
            if (!t) continue;
            std::string kind;
            const auto it = m.runtime.ScriptContext().entityKinds.find(e);
            if (it != m.runtime.ScriptContext().entityKinds.end()) kind = it->second;
            add(e, t->pos, t->rot, kind);
        }
    }

    // Rebuild the AOI index from the same positions the snapshot uses, so the
    // grid and the snapshot can never disagree about an entity's cell.
    std::vector<AoiGrid::Entry> entries;
    entries.reserve(items.size());
    for (const Item& it : items) entries.push_back({it.id, it.x, it.z});
    m.grid.SetCellSize(cfg_.aoiCellSize);
    m.grid.Update(entries);

    // The fallback focus (v1: a single playable player): the controlled
    // entity's position, or the world origin when the scene has no script
    // entity to center on. Multi-player clients override it with their own
    // bound player below.
    const uint64_t controlledKey = ControlledEntityKey(m);
    math::Vec3 focus{0.0f, 0.0f, 0.0f};
    for (const Item& it : items)
        if (it.id == controlledKey) {
            focus = {it.x, it.y, it.z};
            break;
        }

    // id -> items index for O(1) assembly (the old per-interest linear scan
    // was O(interest x entities) per client per tick). Built once per
    // snapshot; entity counts are bounded by the scene's replicated set.
    std::unordered_map<uint64_t, size_t> itemIndex;
    itemIndex.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) itemIndex.emplace(items[i].id, i);
    const auto itemById = [&](uint64_t id) -> const Item* {
        const auto it = itemIndex.find(id);
        return it == itemIndex.end() ? nullptr : &items[it->second];
    };

    for (auto& kv : clients_) {
        Client& c = kv.second;
        if (c.chan.TimedOut()) continue;
        if (MatchForClient(c) != &m) continue; // this client belongs to another match

        // Per-client AOI focus: the client's OWN bound player when it has one
        // (multi-player), else the shared controlled entity / world origin.
        math::Vec3 clientFocus = focus;
        uint64_t clientBound = 0;
        for (const auto& eit : m.entityClientIds)
            if (eit.second == c.clientId) {
                clientBound = eit.first;
                break;
            }
        if (clientBound != 0) {
            if (const Item* it = itemById(clientBound))
                clientFocus = {it->x, it->y, it->z};
        }
        std::vector<uint64_t> interest =
            m.grid.InterestSet(clientFocus.x, clientFocus.z, cfg_.aoiRadiusCells);
        const uint64_t alwaysVisible =
            clientBound != 0 ? clientBound : controlledKey;
        if (alwaysVisible != 0 &&
            std::find(interest.begin(), interest.end(), alwaysVisible) == interest.end()) {
            interest.push_back(alwaysVisible);
        }

        // Spawn/despawn diff against the client's previous interest set: ids
        // that entered are announced with MsgSpawn (id + kind + position),
        // ids that left with MsgDespawn. The first snapshot spawns the whole
        // interest set (lastInterest starts empty).
        const std::set<uint64_t> interestSet(interest.begin(), interest.end());
        std::vector<uint64_t> spawned;
        std::vector<uint64_t> despawned;
        spawned.reserve(interest.size());
        for (uint64_t id : interest)
            if (c.lastInterest.count(id) == 0) spawned.push_back(id);
        for (uint64_t id : c.lastInterest)
            if (interestSet.count(id) == 0) despawned.push_back(id);
        c.lastInterest = interestSet;

        // Build the per-client snapshot from exactly the interest set, in the
        // same order InterestSet returned it (deterministic).
        net::MsgSnapshot snap;
        snap.tick = m.tick;
        snap.entities.reserve(interest.size());
        for (uint64_t id : interest) {
            const Item* it = itemById(id);
            if (!it) continue;
            net::SnapshotEntity se;
            se.id = it->id;
            se.x = it->x;
            se.y = it->y;
            se.z = it->z;
            se.yaw = it->yaw;
            snap.entities.push_back(se);
        }
        snap.entityCount = static_cast<uint32_t>(snap.entities.size());

        for (uint64_t id : spawned) {
            const Item* it = itemById(id);
            if (!it) continue;
            net::MsgSpawn spawn{it->id, it->kind, it->x, it->y, it->z};
            core::Status st =
                c.chan.Send(static_cast<uint8_t>(net::MsgType::Spawn), net::EncodeBody(spawn));
            if (!st.Ok())
                NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                             "server: spawn of entity %llu to client %llu deferred (%s)",
                             static_cast<unsigned long long>(id),
                             static_cast<unsigned long long>(c.clientId), st.Error().c_str());
        }
        for (uint64_t id : despawned) SendDespawn(c, id);

        // B13: fragment oversized snapshots instead of dropping them. Each part
        // fits the reliable channel's maxFrameBytes (header ~19 bytes on top of
        // the payload); the client reassembles parts by tick (parts arrive in
        // order over the reliable channel).
        constexpr size_t kPartHeaderBytes = 19; // magic+crc+version+msgId+seq+part fields
        const size_t maxPayload =
            c.chan.Config().maxFrameBytes > kPartHeaderBytes
                ? c.chan.Config().maxFrameBytes - kPartHeaderBytes
                : c.chan.Config().maxFrameBytes;
        // Per-entity wire size (u64 id + 4x f32 = 24) + the 16 bytes of fixed
        // MsgSnapshot header (tick/part/partCount/entityCount) per part.
        constexpr size_t kEntityBytes = 24;
        constexpr size_t kSnapshotHeaderBytes = 16;
        const size_t maxEntitiesPerPart =
            maxPayload > kSnapshotHeaderBytes ? (maxPayload - kSnapshotHeaderBytes) / kEntityBytes
                                              : 1;
        const size_t totalEntities = snap.entities.size();
        if (totalEntities <= maxEntitiesPerPart) {
            const std::vector<uint8_t> body = net::EncodeBody(snap);
            const size_t frameBytes = body.size() + 15;
            if (frameBytes > c.chan.Config().maxFrameBytes) {
                // Still too big (shouldn't happen given the entity math above);
                // fall through to the fragmentation path.
                (void)0;
            } else {
                core::Status st =
                    c.chan.Send(static_cast<uint8_t>(net::MsgType::Snapshot), body);
                if (!st.Ok()) {
                    // Throttled: a client that never acks fills the window and would
                    // otherwise log once per tick until it is disconnected.
                    ++m.snapshotDrops;
                    if (++c.dropLogCount % 60 == 1)
                        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                                     "server: snapshot to client %llu deferred (%s)",
                                     static_cast<unsigned long long>(c.clientId),
                                     st.Error().c_str());
                } else {
                    c.lastSnapshotTick = snap.tick;
                }
                continue;
            }
        }
        // Fragmentation path: split the entities into N parts, each under the
        // frame cap.
        {
            const uint32_t partCount = static_cast<uint32_t>(
                (totalEntities + maxEntitiesPerPart - 1) / maxEntitiesPerPart);
            for (uint32_t p = 0; p < partCount; ++p) {
                net::MsgSnapshot part;
                part.tick = snap.tick;
                part.partIndex = p;
                part.partCount = partCount;
                const size_t begin = static_cast<size_t>(p) * maxEntitiesPerPart;
                const size_t end = std::min(begin + maxEntitiesPerPart, totalEntities);
                part.entities.assign(snap.entities.begin() + static_cast<ptrdiff_t>(begin),
                                     snap.entities.begin() + static_cast<ptrdiff_t>(end));
                part.entityCount = static_cast<uint32_t>(part.entities.size());
                const std::vector<uint8_t> partBody = net::EncodeBody(part);
                core::Status st =
                    c.chan.Send(static_cast<uint8_t>(net::MsgType::Snapshot), partBody);
                if (!st.Ok()) {
                    ++m.snapshotDrops;
                    if (++c.dropLogCount % 60 == 1)
                        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                                     "server: snapshot part %u/%u to client %llu deferred (%s)",
                                     p + 1, partCount,
                                     static_cast<unsigned long long>(c.clientId),
                                     st.Error().c_str());
                    break; // window full: stop emitting parts this tick
                }
            }
            c.lastSnapshotTick = snap.tick;
            continue;
        }
    }
    // P2-4 anti-cheat: broadcast a deterministic world checksum ~1 Hz (NOT
    // every tick — that flooded the reliable window and timed clients out).
    if (m.tick % 60 == 0) {
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.id < b.id; });
        uint64_t hash = 1469598103934665603ull;  // FNV-1a 64
        for (const Item& it : items) {
            hash ^= it.id;
            hash *= 1099511628211ull;
            const float* f = &it.x;
            for (int k = 0; k < 4; ++k) {
                uint32_t bits;
                std::memcpy(&bits, &f[k], sizeof(bits));
                hash ^= bits;
                hash *= 1099511628211ull;
            }
            for (char ch : it.kind) {
                hash ^= static_cast<uint8_t>(ch);
                hash *= 1099511628211ull;
            }
        }
        core::Json msg;
        msg.type_ = core::Json::Type::Object;
        core::Json t;
        t.type_ = core::Json::Type::Number;
        t.number_ = m.tick;
        msg.object_["tick"] = t;
        core::Json h;
        // A11: a uint64 FNV-1a hash loses its high bits through a JSON double
        // (53-bit mantissa), so the client's comparison was meaningless.
        // Hex string, same trick pack_manifest.json already uses.
        h.type_ = core::Json::Type::String;
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%016llx",
                          static_cast<unsigned long long>(hash));
            h.string_ = buf;
        }
        msg.object_["hash"] = h;
        for (auto& kv : clients_)
            if (MatchForClient(kv.second) == &m)
                SendRpc(kv.second, "world.hash", core::JsonWriter::Write(msg));
    }
}

void GameServer::TickChannels(uint64_t nowMs) {
    for (auto& kv : clients_) kv.second.chan.Tick(nowMs);
}

void GameServer::DropTimedOutClients(uint64_t nowMs) {
    std::vector<net::NetAddress> inactive;
    for (auto& kv : clients_) {
        if (nowMs - kv.second.lastSeenMs > cfg_.clientTimeoutMs) inactive.push_back(kv.first);
    }
    for (const net::NetAddress& addr : inactive) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                     "server: client %s:%u timed out (inactive for %llu ms)",
                     addr.host.c_str(), addr.port,
                     static_cast<unsigned long long>(nowMs - clients_.at(addr).lastSeenMs));
        RemoveClient(addr);
    }
    for (const net::NetAddress& addr : pendingRemovals_) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                     "server: client %s:%u disconnected (queued removal)",
                     addr.host.c_str(), addr.port);
        RemoveClient(addr);
    }
    pendingRemovals_.clear();
}

void GameServer::RemoveClient(const net::NetAddress& addr) {
    auto it = clients_.find(addr);
    if (it == clients_.end()) return;
    // P2-4: leaving a client drops its room membership (room.chat etc. stop
    // reaching it).
    it->second.room.clear();
    const uint64_t id = it->second.clientId;
    if (it->second.accountId != 0) accountToClient_.erase(it->second.accountId);
    clients_.erase(it);
    // Multi-player: drop every entity this client owned in ANY match.
    auto dropOwned = [&](Match& m) {
        for (auto eit = m.entityClientIds.begin(); eit != m.entityClientIds.end();) {
            if (eit->second == id)
                eit = m.entityClientIds.erase(eit);
            else
                ++eit;
        }
    };
    if (match_) dropOwned(*match_);
    for (auto& kv : roomMatches_) dropOwned(*kv.second);
    NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                 "server: client %llu disconnected (%u remaining)",
                 static_cast<unsigned long long>(id), ClientCount());
    // Promote the next client to controller (v1 input model).
    if (SameAddr(addr, controllerAddr_)) {
        controllerAddr_ = {};
        if (!clients_.empty()) controllerAddr_ = clients_.begin()->first;
    }
}

void GameServer::Shutdown() {
    if (!running_) return;
    NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                 "server: shutting down after tick %u (%u clients)", match_->tick, ClientCount());
    running_ = false;
    clients_.clear();
    match_->runtime.Stop();
    if (match_->kernel) {
        match_->kernel->Shutdown();  // microkernel: tear down the physics/script modules
        match_->kernel.reset();
    }
    match_.reset();
    for (auto& kv : roomMatches_) {
        if (kv.second->kernel) {
            kv.second->runtime.Stop();
            kv.second->kernel->Shutdown();
            kv.second->kernel.reset();
        }
    }
    roomMatches_.clear();
    startedRooms_.clear();
    sock_.Close();
}

uint16_t GameServer::Port() const { return sock_.Port(); }

uint64_t GameServer::ControllerClientId() const {
    auto it = clients_.find(controllerAddr_);
    return it == clients_.end() ? 0 : it->second.clientId;
}

NetInput* GameServer::ClientInputById(uint64_t clientId) {
    for (auto& kv : clients_)
        if (kv.second.clientId == clientId) return &kv.second.input;
    return nullptr;
}

uint32_t GameServer::LagTicksForRtt(uint64_t rttMs) const {
    // One-way latency = half the round trip; 60Hz ticks per millisecond.
    const uint64_t halfRtt = rttMs / 2;
    const uint32_t ticks = static_cast<uint32_t>((halfRtt * 60u) / 1000u);
    return std::min(ticks, scene::GameRuntime::kLagCompHistoryTicks);
}

uint64_t GameServer::EntityKey(ecs::Entity e) const {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace neon::server

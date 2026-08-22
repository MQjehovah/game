#include "game_server.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include "neon/core/log.hpp"
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

} // namespace

bool GameServer::Start(const Config& cfg) {
    cfg_ = cfg;
    if (cfg_.clientTimeoutMs == 0) cfg_.clientTimeoutMs = 5000;
    if (cfg_.snapshotEveryTicks == 0) cfg_.snapshotEveryTicks = 1;

    // Scene source: inline JSON wins, else the scene file.
    std::string sceneJson = cfg_.sceneJson;
    if (sceneJson.empty() && !cfg_.sceneJsonPath.empty()) {
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
    scene::GameRuntimeConfig rcfg;
    rcfg.assets = nullptr;
    rcfg.headless = true;
    rcfg.scriptBaseDir = cfg_.scriptBaseDir;
    rcfg.assetBaseDir = cfg_.assetBaseDir;
    rcfg.rngSeed = cfg_.rngSeed;
    rcfg.input = &controllerInput_;
    core::Status st = runtime_.Start(sceneJson, rcfg);
    if (!st.Ok()) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Error, "server: %s",
                     st.Error().c_str());
        sock_.Close();
        return false;
    }

    running_ = true;
    tick_ = 0;
    accumulator_ = 0.0;
    lastStepMs_ = 0;
    nowMs_ = 0;
    nextClientId_ = 0;
    controllerAddr_ = {};
    clients_.clear();
    pendingRemovals_.clear();
    lastSnapshotIds_.clear();
    NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                 "server: listening on %s:%u (%zu entities, %zu scripts, %zu trees)",
                 cfg_.loopback ? "127.0.0.1" : "0.0.0.0", Port(), runtime_.EntityCount(),
                 runtime_.ScriptCount(), runtime_.BehaviorTreeCount());
    return true;
}

void GameServer::Step(uint64_t nowMs) {
    if (!running_) return;
    nowMs_ = nowMs;

    // 1) Ingest everything the network delivered since the last Step.
    PumpNetwork(nowMs_);
    // 2) Reliable channels: retransmit due frames, emit acks, fire timeouts.
    TickChannels(nowMs_);
    // 3) Fixed-step simulation (accumulator, exactly like core::Application).
    accumulator_ += static_cast<double>(nowMs_ - lastStepMs_) / 1000.0;
    lastStepMs_ = nowMs_;
    while (accumulator_ >= kFixedDt) {
        ApplyControllerInput();
        runtime_.Tick(static_cast<float>(kFixedDt));
        ++tick_;
        if (tick_ % cfg_.snapshotEveryTicks == 0) BroadcastSnapshot();
        controllerInput_.EndFrame(); // advance edges for the next tick
        accumulator_ -= kFixedDt;
    }
    // 4) Disconnect stale clients (inactivity + reliable-channel timeouts).
    DropTimedOutClients(nowMs_);
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

        // Unknown sender: only a valid MsgJoin may create a client. Anything
        // else is dropped (spoofing / garbage / pre-join messages). The join
        // is then fed through the new channel's OnDatagram so its seq-space
        // advances with the channel — subsequent client frames align.
        if (size < 2) continue;
        const uint16_t len = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
        if (static_cast<size_t>(len) != size - 2) continue;
        core::Result<net::DecodedMessage> dec = codec_.Decode(buf + 2, len);
        if (!dec.Ok()) continue;
        if (dec.Value().header.msgId == static_cast<uint8_t>(net::MsgType::Join)) {
            const net::MsgJoin& join = std::get<net::MsgJoin>(dec.Value().payload);
            AdmitClient(from, join);
            auto admitted = clients_.find(from);
            if (admitted != clients_.end()) {
                admitted->second.lastSeenMs = nowMs;
                admitted->second.chan.OnDatagram(buf, size); // delivers Join -> welcome
            }
        } else {
            NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                         "server: dropping non-join datagram from unknown %s:%u",
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
        case net::MsgType::Input:
            if (auto* m = std::get_if<net::MsgInput>(&msg.payload)) HandleInput(addr, *m);
            break;
        case net::MsgType::Ping:
            if (auto* m = std::get_if<net::MsgPing>(&msg.payload)) HandlePing(addr, *m);
            break;
        case net::MsgType::Welcome:
        case net::MsgType::Snapshot:
        case net::MsgType::Spawn:
        case net::MsgType::Despawn:
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
        AdmitClient(addr, join);
        it = clients_.find(addr);
    }
    if (it != clients_.end()) {
        it->second.lastSeenMs = nowMs_;
        SendWelcome(it->second); // idempotent: re-joins get a fresh welcome
    }
}

// Creates the Client for a joining address (channel wiring, id assignment,
// v1 controller election). Does NOT welcome — the join datagram is delivered
// through the channel right after, which sequences the channel and triggers
// HandleJoin -> SendWelcome.
void GameServer::AdmitClient(const net::NetAddress& addr, const net::MsgJoin& join) {
    if (clients_.count(addr) != 0) return;
    if (clients_.size() >= static_cast<size_t>(cfg_.maxClients)) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: join from %s:%u rejected (server full)", addr.host.c_str(),
                     addr.port);
        return;
    }

    Client c;
    c.addr = addr;
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
                 "server: client id=%llu name='%s' version=%u joined from %s:%u",
                 static_cast<unsigned long long>(client.clientId), join.name.c_str(),
                 join.version, addr.host.c_str(), addr.port);

    // v1 input model: the first joiner drives the scene's player script.
    if (!controllerAddr_.Valid()) {
        controllerAddr_ = addr;
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Info,
                     "server: client id=%llu is the input controller (v1 single-client model)",
                     static_cast<unsigned long long>(client.clientId));
    }
}

void GameServer::HandleInput(const net::NetAddress& addr, const net::MsgInput& input) {
    auto it = clients_.find(addr);
    if (it == clients_.end()) return;
    Client& c = it->second;
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
    SendPong(it->second, ping.sendTime);
}

void GameServer::SendWelcome(Client& c) {
    net::MsgWelcome welcome{c.clientId, tick_};
    core::Status st = c.chan.Send(static_cast<uint8_t>(net::MsgType::Welcome),
                                  EncodeBody(welcome));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: welcome to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::SendPong(Client& c, uint64_t sendTime) {
    net::MsgPong pong{sendTime, nowMs_};
    core::Status st =
        c.chan.Send(static_cast<uint8_t>(net::MsgType::Pong), EncodeBody(pong));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                     "server: pong to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::SendDespawn(Client& c, uint64_t entityId) {
    net::MsgDespawn despawn{entityId};
    core::Status st =
        c.chan.Send(static_cast<uint8_t>(net::MsgType::Despawn), EncodeBody(despawn));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                     "server: despawn to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::ApplyControllerInput() {
    auto it = clients_.find(controllerAddr_);
    if (it == clients_.end() || !it->second.hasInput) {
        controllerInput_.SetInput(0, 0.0f, 0.0f);
        return;
    }
    const net::MsgInput& in = it->second.lastInput;
    controllerInput_.SetInput(in.buttons, in.moveX, in.moveY);
}

void GameServer::BroadcastSnapshot() {
    ecs::World& world = runtime_.World();

    // Collect positions from both position-bearing component kinds: scene
    // entities carry SceneTransform (from the data-driven scene), script
    // entities carry CTransformBind (Spawn/SetPosition). Entity ids are the
    // stable (id<<32)|generation key so id reuse across generations never
    // aliases.
    std::vector<net::SnapshotEntity> ents;
    std::set<uint64_t> seen;
    auto add = [&](ecs::Entity e, const math::Vec3& pos, const math::Quat& rot) {
        const uint64_t key = EntityKey(e);
        if (seen.count(key) != 0) return;
        seen.insert(key);
        net::SnapshotEntity se;
        se.id = key;
        se.x = pos.x;
        se.y = pos.y;
        se.z = pos.z;
        se.yaw = YawOf(rot);
        ents.push_back(se);
    };
    {
        auto view = world.ViewAll<scene::SceneTransform>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = world.EntityAt<scene::SceneTransform>(i);
            const scene::SceneTransform* t = world.Get<scene::SceneTransform>(e);
            if (t) add(e, t->pos, t->rot);
        }
    }
    {
        auto view = world.ViewAll<script::CTransformBind>();
        for (size_t i = 0; i < view.Size(); ++i) {
            ecs::Entity e = world.EntityAt<script::CTransformBind>(i);
            const script::CTransformBind* t = world.Get<script::CTransformBind>(e);
            if (t) add(e, t->pos, math::Quat::Identity());
        }
    }

    // Entities that left the snapshot since the last broadcast get MsgDespawn.
    std::vector<uint64_t> despawned;
    for (uint64_t id : lastSnapshotIds_)
        if (seen.count(id) == 0) despawned.push_back(id);
    lastSnapshotIds_ = std::move(seen);

    net::MsgSnapshot snap;
    snap.tick = tick_;
    snap.entityCount = static_cast<uint32_t>(ents.size());
    snap.entities = std::move(ents);
    const std::vector<uint8_t> body = EncodeBody(snap);

    for (auto& kv : clients_) {
        Client& c = kv.second;
        if (c.chan.TimedOut()) continue;
        for (uint64_t id : despawned) SendDespawn(c, id);
        core::Status st =
            c.chan.Send(static_cast<uint8_t>(net::MsgType::Snapshot), body);
        if (!st.Ok()) {
            // Throttled: a client that never acks fills the window and would
            // otherwise log once per tick until it is disconnected.
            if (++c.dropLogCount % 60 == 1)
                NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                             "server: snapshot to client %llu deferred (%s)",
                             static_cast<unsigned long long>(c.clientId),
                             st.Error().c_str());
        } else {
            c.dropLogCount = 0;
        }
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
                     "server: client %s:%u disconnected (reliable timeout)",
                     addr.host.c_str(), addr.port);
        RemoveClient(addr);
    }
    pendingRemovals_.clear();
}

void GameServer::RemoveClient(const net::NetAddress& addr) {
    auto it = clients_.find(addr);
    if (it == clients_.end()) return;
    const uint64_t id = it->second.clientId;
    clients_.erase(it);
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
                 "server: shutting down after tick %u (%u clients)", tick_, ClientCount());
    running_ = false;
    clients_.clear();
    runtime_.Stop();
    sock_.Close();
}

uint16_t GameServer::Port() const { return sock_.Port(); }

uint64_t GameServer::ControllerClientId() const {
    auto it = clients_.find(controllerAddr_);
    return it == clients_.end() ? 0 : it->second.clientId;
}

uint64_t GameServer::EntityKey(ecs::Entity e) const {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace neon::server

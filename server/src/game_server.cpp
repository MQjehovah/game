#include "game_server.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <unordered_map>

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

    // Multi-player input routing: scripts read the input of the client that
    // owns their entity (bound via BindPlayerToClient inside on_player_join);
    // unbound entities fall back to the v1 shared controller input.
    script::ScriptContext& ctx = runtime_.ScriptContext();
    ctx.inputForEntity = [this](ecs::Entity e) -> platform::IInput* {
        const auto it = entityClientIds_.find(EntityKey(e));
        if (it != entityClientIds_.end()) {
            if (NetInput* in = ClientInputById(it->second)) return in;
        }
        return &controllerInput_;
    };
    ctx.bindPlayerToClient = [this](ecs::Entity e, double clientId) {
        if (!e.IsValid()) return;
        const uint64_t id = static_cast<uint64_t>(clientId);
        if (ClientInputById(id) != nullptr) {
            entityClientIds_[EntityKey(e)] = id;
            NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                         "server: entity %llu bound to client %llu",
                         static_cast<unsigned long long>(EntityKey(e)),
                         static_cast<unsigned long long>(id));
        }
    };

    running_ = true;
    tick_ = 0;
    accumulator_ = 0.0;
    lastStepMs_ = 0;
    nowMs_ = 0;
    nextClientId_ = 0;
    nextAccountId_ = 0;
    accountToClient_.clear();
    controllerAddr_ = {};
    clients_.clear();
    pendingRemovals_.clear();
    grid_.SetCellSize(cfg_.aoiCellSize);
    grid_.Clear();
    entityClientIds_.clear();
    snapshotTooBig_ = 0;
    snapshotDrops_ = 0;
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
    // 3) Fixed-step simulation. The accumulator tracks elapsed time exactly like
    // core::Application, but Step runs AT MOST ONE fixed tick per call: callers
    // (neon_server's loop) count tick consumptions, so `--ticks N` stops at
    // exactly N — the accumulator residual never produces a second tick inside
    // a single Step (no overshoot). The leftover accumulates and drains one
    // tick per later call, so wall-clock pacing still catches up.
    accumulator_ += static_cast<double>(nowMs_ - lastStepMs_) / 1000.0;
    lastStepMs_ = nowMs_;
    if (accumulator_ >= kFixedDt) {
        accumulator_ -= kFixedDt;
        ApplyControllerInput();
        runtime_.Tick(static_cast<float>(kFixedDt));
        ++tick_;
        if (tick_ % cfg_.snapshotEveryTicks == 0) BroadcastSnapshot();
        controllerInput_.EndFrame(); // advance edges for the next tick
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
    if (clients_.size() >= static_cast<size_t>(cfg_.maxClients)) {
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: client %s:%u rejected (server full)", addr.host.c_str(),
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
    if (runtime_.HasScriptFunction("on_player_join")) {
        runtime_.CallScriptFunction(
            "on_player_join", {script::Value::Num(static_cast<double>(client.clientId))});
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
                                  net::EncodeBody(welcome));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                     "server: welcome to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::SendLoginOk(Client& c) {
    net::MsgLoginOk ok{c.accountId, tick_};
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

void GameServer::SendDespawn(Client& c, uint64_t entityId) {
    net::MsgDespawn despawn{entityId};
    core::Status st =
        c.chan.Send(static_cast<uint8_t>(net::MsgType::Despawn), net::EncodeBody(despawn));
    if (!st.Ok())
        NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Debug,
                     "server: despawn to client %llu deferred (%s)",
                     static_cast<unsigned long long>(c.clientId), st.Error().c_str());
}

void GameServer::ApplyControllerInput() {
    // Scripted-controller path (T6.7 determinism acceptance): when a scripted
    // sequence is installed, the input keyed to the CURRENT fixed step drives
    // the sim directly — no socket client involved, so a headless run can be
    // compared bit-exactly against a client prediction fed the same sequence.
    if (!scriptedInputs_.empty()) {
        const net::MsgInput* in = InputForTick(scriptedInputs_, tick_);
        if (in)
            controllerInput_.SetInput(in->buttons, in->moveX, in->moveY);
        else
            controllerInput_.SetInput(0, 0.0f, 0.0f);
        return;
    }

    // Multi-player (v2): drive every client's own NetInput from ITS latest
    // MsgInput. Entities bound via BindPlayerToClient read their owner's
    // input through the runtime's per-entity resolver.
    for (auto& kv : clients_) {
        Client& c = kv.second;
        if (c.hasInput)
            c.input.SetInput(c.lastInput.buttons, c.lastInput.moveX, c.lastInput.moveY);
        else
            c.input.SetInput(0, 0.0f, 0.0f);
    }

    // v1 fallback for scenes without on_player_join: the controller client's
    // input drives the shared NetInput every unbound script reads.
    controllerInput_.SetInput(0, 0.0f, 0.0f);
    auto it = clients_.find(controllerAddr_);
    if (it != clients_.end() && it->second.hasInput) {
        const net::MsgInput& in = it->second.lastInput;
        controllerInput_.SetInput(in.buttons, in.moveX, in.moveY);
    }
}

// The entity every client's AOI is centered on: the script entity of kind
// "player" if the scene spawned one, else the first script (CTransformBind)
// entity, else 0 (no controlled entity -> clients focus on the world origin).
// Matches how the client resolves its controlled entity (the first
// CTransformBind it finds); preferring the "player" kind keeps a scene with
// several script entities centered on the actual playable one.
uint64_t GameServer::ControlledEntityKey() {
    ecs::World& world = runtime_.World();
    uint64_t fallback = 0;
    auto view = world.ViewAll<script::CTransformBind>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = world.EntityAt<script::CTransformBind>(i);
        const uint64_t key = EntityKey(e);
        const auto it = runtime_.ScriptContext().entityKinds.find(e);
        if (it != runtime_.ScriptContext().entityKinds.end() && it->second == "player")
            return key;
        if (fallback == 0) fallback = key;
    }
    return fallback;
}

void GameServer::BroadcastSnapshot() {
    ecs::World& world = runtime_.World();

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
            const auto it = runtime_.ScriptContext().entityKinds.find(e);
            if (it != runtime_.ScriptContext().entityKinds.end()) kind = it->second;
            add(e, t->pos, t->rot, kind);
        }
    }

    // Rebuild the AOI index from the same positions the snapshot uses, so the
    // grid and the snapshot can never disagree about an entity's cell.
    std::vector<AoiGrid::Entry> entries;
    entries.reserve(items.size());
    for (const Item& it : items) entries.push_back({it.id, it.x, it.z});
    grid_.SetCellSize(cfg_.aoiCellSize);
    grid_.Update(entries);

    // The fallback focus (v1: a single playable player): the controlled
    // entity's position, or the world origin when the scene has no script
    // entity to center on. Multi-player clients override it with their own
    // bound player below.
    const uint64_t controlledKey = ControlledEntityKey();
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

        // Per-client AOI focus: the client's OWN bound player when it has one
        // (multi-player), else the shared controlled entity / world origin.
        math::Vec3 clientFocus = focus;
        uint64_t clientBound = 0;
        for (const auto& eit : entityClientIds_)
            if (eit.second == c.clientId) {
                clientBound = eit.first;
                break;
            }
        if (clientBound != 0) {
            if (const Item* it = itemById(clientBound))
                clientFocus = {it->x, it->y, it->z};
        }
        std::vector<uint64_t> interest =
            grid_.InterestSet(clientFocus.x, clientFocus.z, cfg_.aoiRadiusCells);
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
        snap.tick = tick_;
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
        const std::vector<uint8_t> body = net::EncodeBody(snap);

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

        // The reliable transport caps every frame at Config().maxFrameBytes
        // (~1200; the codec adds 8 magic+CRC + 4 version + 1 msgId + 2 seq
        // bytes on top of the payload). AOI keeps the interest set small, but
        // a dense neighborhood (or a large radius) can still exceed the cap —
        // keep the guard.
        const size_t frameBytes = body.size() + 15;
        if (frameBytes > c.chan.Config().maxFrameBytes) {
            // The snapshot cannot fit in one frame: the client would silently
            // freeze on the previous state. Count it and log at Warn (throttled
            // so a too-big scene does not spam every tick).
            ++snapshotTooBig_;
            if (++c.dropLogCount % 60 == 1)
                NEON_LOG_CAT(core::LogCategory::Net, core::LogLevel::Warn,
                             "server: snapshot with %zu entities exceeds the %u-byte frame "
                             "cap for client %llu; dropped",
                             snap.entityCount, c.chan.Config().maxFrameBytes,
                             static_cast<unsigned long long>(c.clientId));
            continue;
        }
        core::Status st =
            c.chan.Send(static_cast<uint8_t>(net::MsgType::Snapshot), body);
        if (!st.Ok()) {
            // Throttled: a client that never acks fills the window and would
            // otherwise log once per tick until it is disconnected.
            ++snapshotDrops_;
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
    if (it->second.accountId != 0) accountToClient_.erase(it->second.accountId);
    clients_.erase(it);
    // Multi-player: drop every entity this client owned.
    for (auto eit = entityClientIds_.begin(); eit != entityClientIds_.end();) {
        if (eit->second == id)
            eit = entityClientIds_.erase(eit);
        else
            ++eit;
    }
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

NetInput* GameServer::ClientInputById(uint64_t clientId) {
    for (auto& kv : clients_)
        if (kv.second.clientId == clientId) return &kv.second.input;
    return nullptr;
}

uint64_t GameServer::EntityKey(ecs::Entity e) const {
    return (static_cast<uint64_t>(e.id) << 32) | static_cast<uint64_t>(e.generation);
}

} // namespace neon::server

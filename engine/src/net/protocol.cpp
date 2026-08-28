#include "neon/net/protocol.hpp"

#include <utility>

namespace neon::net {
namespace {

// Maps a Payload variant alternative to its wire msgId. Keep in sync with the
// MsgType enum and the Payload alternative order in protocol.hpp.
MsgType TypeOf(const Payload& p) {
    switch (p.index()) {
        case 0: return MsgType::Join;
        case 1: return MsgType::Welcome;
        case 2: return MsgType::Input;
        case 3: return MsgType::Snapshot;
        case 4: return MsgType::Spawn;
        case 5: return MsgType::Despawn;
        case 6: return MsgType::Ping;
        case 7: return MsgType::Pong;
        case 8: return MsgType::Ack;
        case 9: return MsgType::Login;
        case 10: return MsgType::LoginOk;
        case 11: return MsgType::CharList;
        case 12: return MsgType::Rpc;
        default: return MsgType::CharList;
    }
}

core::Result<SnapshotEntity> ReadEntity(core::Deserializer& d) {
    SnapshotEntity e;
    core::Result<uint64_t> id = d.ReadU64();
    if (!id.Ok()) return core::Result<SnapshotEntity>::Err("net: snapshot entity id truncated");
    e.id = id.Value();
    float* fields[4] = {&e.x, &e.y, &e.z, &e.yaw};
    for (float* f : fields) {
        core::Result<float> v = d.ReadF32();
        if (!v.Ok()) return core::Result<SnapshotEntity>::Err("net: snapshot entity field truncated");
        *f = v.Value();
    }
    return core::Result<SnapshotEntity>::Ok(e);
}

} // namespace

void MsgJoin::Write(core::Serializer& s) const {
    s.WriteString(name);
    s.WriteU32(version);
}

core::Result<MsgJoin> MsgJoin::Read(core::Deserializer& d) {
    MsgJoin m;
    core::Result<std::string> name = d.ReadString();
    if (!name.Ok()) return core::Result<MsgJoin>::Err("net: join name truncated");
    if (name.Value().size() > kMaxStringBytes)
        return core::Result<MsgJoin>::Err("net: join name exceeds " +
                                          std::to_string(kMaxStringBytes) + " bytes");
    m.name = std::move(name.Value());
    core::Result<uint32_t> version = d.ReadU32();
    if (!version.Ok()) return core::Result<MsgJoin>::Err("net: join version truncated");
    m.version = version.Value();
    return core::Result<MsgJoin>::Ok(std::move(m));
}

void MsgWelcome::Write(core::Serializer& s) const {
    s.WriteU64(clientId);
    s.WriteU32(tick);
}

core::Result<MsgWelcome> MsgWelcome::Read(core::Deserializer& d) {
    MsgWelcome m;
    core::Result<uint64_t> clientId = d.ReadU64();
    if (!clientId.Ok()) return core::Result<MsgWelcome>::Err("net: welcome clientId truncated");
    m.clientId = clientId.Value();
    core::Result<uint32_t> tick = d.ReadU32();
    if (!tick.Ok()) return core::Result<MsgWelcome>::Err("net: welcome tick truncated");
    m.tick = tick.Value();
    return core::Result<MsgWelcome>::Ok(std::move(m));
}

void MsgInput::Write(core::Serializer& s) const {
    s.WriteU32(seq);
    s.WriteU8(buttons);
    s.WriteF32(moveX);
    s.WriteF32(moveY);
}

core::Result<MsgInput> MsgInput::Read(core::Deserializer& d) {
    MsgInput m;
    core::Result<uint32_t> seq = d.ReadU32();
    if (!seq.Ok()) return core::Result<MsgInput>::Err("net: input seq truncated");
    m.seq = seq.Value();
    core::Result<uint8_t> buttons = d.ReadU8();
    if (!buttons.Ok()) return core::Result<MsgInput>::Err("net: input buttons truncated");
    m.buttons = buttons.Value();
    core::Result<float> moveX = d.ReadF32();
    if (!moveX.Ok()) return core::Result<MsgInput>::Err("net: input moveX truncated");
    m.moveX = moveX.Value();
    core::Result<float> moveY = d.ReadF32();
    if (!moveY.Ok()) return core::Result<MsgInput>::Err("net: input moveY truncated");
    m.moveY = moveY.Value();
    return core::Result<MsgInput>::Ok(std::move(m));
}

void MsgSnapshot::Write(core::Serializer& s) const {
    s.WriteU32(tick);
    s.WriteU32(partIndex);
    s.WriteU32(partCount);
    s.WriteU32(static_cast<uint32_t>(entities.size()));
    for (const SnapshotEntity& e : entities) {
        s.WriteU64(e.id);
        s.WriteF32(e.x);
        s.WriteF32(e.y);
        s.WriteF32(e.z);
        s.WriteF32(e.yaw);
    }
}

core::Result<MsgSnapshot> MsgSnapshot::Read(core::Deserializer& d) {
    MsgSnapshot m;
    core::Result<uint32_t> tick = d.ReadU32();
    if (!tick.Ok()) return core::Result<MsgSnapshot>::Err("net: snapshot tick truncated");
    m.tick = tick.Value();
    core::Result<uint32_t> partIndex = d.ReadU32();
    if (!partIndex.Ok())
        return core::Result<MsgSnapshot>::Err("net: snapshot part index truncated");
    m.partIndex = partIndex.Value();
    core::Result<uint32_t> partCount = d.ReadU32();
    if (!partCount.Ok())
        return core::Result<MsgSnapshot>::Err("net: snapshot part count truncated");
    m.partCount = partCount.Value();
    if (m.partCount == 0 || m.partIndex >= m.partCount)
        return core::Result<MsgSnapshot>::Err("net: snapshot part index out of range");
    core::Result<uint32_t> count = d.ReadU32();
    if (!count.Ok()) return core::Result<MsgSnapshot>::Err("net: snapshot entity count truncated");
    if (count.Value() > kMaxSnapshotEntities)
        return core::Result<MsgSnapshot>::Err(
            "net: snapshot entity count " + std::to_string(count.Value()) +
            " exceeds limit " + std::to_string(kMaxSnapshotEntities));
    m.entityCount = count.Value();
    m.entities.reserve(m.entityCount);
    for (uint32_t i = 0; i < m.entityCount; ++i) {
        core::Result<SnapshotEntity> e = ReadEntity(d);
        if (!e.Ok()) return core::Result<MsgSnapshot>::Err(e.Error());
        m.entities.push_back(e.Value());
    }
    return core::Result<MsgSnapshot>::Ok(std::move(m));
}

void MsgSpawn::Write(core::Serializer& s) const {
    s.WriteU64(entityId);
    s.WriteString(kind);
    s.WriteF32(x);
    s.WriteF32(y);
    s.WriteF32(z);
}

core::Result<MsgSpawn> MsgSpawn::Read(core::Deserializer& d) {
    MsgSpawn m;
    core::Result<uint64_t> entityId = d.ReadU64();
    if (!entityId.Ok()) return core::Result<MsgSpawn>::Err("net: spawn entityId truncated");
    m.entityId = entityId.Value();
    core::Result<std::string> kind = d.ReadString();
    if (!kind.Ok()) return core::Result<MsgSpawn>::Err("net: spawn kind truncated");
    if (kind.Value().size() > kMaxStringBytes)
        return core::Result<MsgSpawn>::Err("net: spawn kind exceeds " +
                                           std::to_string(kMaxStringBytes) + " bytes");
    m.kind = std::move(kind.Value());
    float* pos[3] = {&m.x, &m.y, &m.z};
    for (float* f : pos) {
        core::Result<float> v = d.ReadF32();
        if (!v.Ok()) return core::Result<MsgSpawn>::Err("net: spawn position truncated");
        *f = v.Value();
    }
    return core::Result<MsgSpawn>::Ok(std::move(m));
}

void MsgDespawn::Write(core::Serializer& s) const { s.WriteU64(entityId); }

core::Result<MsgDespawn> MsgDespawn::Read(core::Deserializer& d) {
    MsgDespawn m;
    core::Result<uint64_t> entityId = d.ReadU64();
    if (!entityId.Ok()) return core::Result<MsgDespawn>::Err("net: despawn entityId truncated");
    m.entityId = entityId.Value();
    return core::Result<MsgDespawn>::Ok(std::move(m));
}

void MsgPing::Write(core::Serializer& s) const { s.WriteU64(sendTime); }

core::Result<MsgPing> MsgPing::Read(core::Deserializer& d) {
    MsgPing m;
    core::Result<uint64_t> sendTime = d.ReadU64();
    if (!sendTime.Ok()) return core::Result<MsgPing>::Err("net: ping sendTime truncated");
    m.sendTime = sendTime.Value();
    return core::Result<MsgPing>::Ok(std::move(m));
}

void MsgPong::Write(core::Serializer& s) const {
    s.WriteU64(sendTime);
    s.WriteU64(receiveTime);
}

core::Result<MsgPong> MsgPong::Read(core::Deserializer& d) {
    MsgPong m;
    core::Result<uint64_t> sendTime = d.ReadU64();
    if (!sendTime.Ok()) return core::Result<MsgPong>::Err("net: pong sendTime truncated");
    m.sendTime = sendTime.Value();
    core::Result<uint64_t> receiveTime = d.ReadU64();
    if (!receiveTime.Ok()) return core::Result<MsgPong>::Err("net: pong receiveTime truncated");
    m.receiveTime = receiveTime.Value();
    return core::Result<MsgPong>::Ok(std::move(m));
}

void MsgAck::Write(core::Serializer& s) const {
    s.WriteU16(ackSeq);
    s.WriteU32(ackBits);
}

core::Result<MsgAck> MsgAck::Read(core::Deserializer& d) {
    MsgAck m;
    core::Result<uint16_t> ackSeq = d.ReadU16();
    if (!ackSeq.Ok()) return core::Result<MsgAck>::Err("net: ack ackSeq truncated");
    m.ackSeq = ackSeq.Value();
    core::Result<uint32_t> ackBits = d.ReadU32();
    if (!ackBits.Ok()) return core::Result<MsgAck>::Err("net: ack ackBits truncated");
    m.ackBits = ackBits.Value();
    return core::Result<MsgAck>::Ok(std::move(m));
}

void MsgLogin::Write(core::Serializer& s) const {
    s.WriteString(name);
    s.WriteU32(clientVersion);
}

core::Result<MsgLogin> MsgLogin::Read(core::Deserializer& d) {
    MsgLogin m;
    core::Result<std::string> name = d.ReadString();
    if (!name.Ok()) return core::Result<MsgLogin>::Err("net: login name truncated");
    if (name.Value().size() > kMaxStringBytes)
        return core::Result<MsgLogin>::Err("net: login name exceeds " +
                                           std::to_string(kMaxStringBytes) + " bytes");
    m.name = std::move(name.Value());
    core::Result<uint32_t> clientVersion = d.ReadU32();
    if (!clientVersion.Ok())
        return core::Result<MsgLogin>::Err("net: login clientVersion truncated");
    m.clientVersion = clientVersion.Value();
    return core::Result<MsgLogin>::Ok(std::move(m));
}

void MsgLoginOk::Write(core::Serializer& s) const {
    s.WriteU64(accountId);
    s.WriteU32(tick);
}

core::Result<MsgLoginOk> MsgLoginOk::Read(core::Deserializer& d) {
    MsgLoginOk m;
    core::Result<uint64_t> accountId = d.ReadU64();
    if (!accountId.Ok()) return core::Result<MsgLoginOk>::Err("net: loginOk accountId truncated");
    m.accountId = accountId.Value();
    core::Result<uint32_t> tick = d.ReadU32();
    if (!tick.Ok()) return core::Result<MsgLoginOk>::Err("net: loginOk tick truncated");
    m.tick = tick.Value();
    return core::Result<MsgLoginOk>::Ok(std::move(m));
}

void MsgCharInfo::Write(core::Serializer& s) const {
    s.WriteU64(id);
    s.WriteString(name);
}

core::Result<MsgCharInfo> MsgCharInfo::Read(core::Deserializer& d) {
    MsgCharInfo m;
    core::Result<uint64_t> id = d.ReadU64();
    if (!id.Ok()) return core::Result<MsgCharInfo>::Err("net: charInfo id truncated");
    m.id = id.Value();
    core::Result<std::string> name = d.ReadString();
    if (!name.Ok()) return core::Result<MsgCharInfo>::Err("net: charInfo name truncated");
    if (name.Value().size() > kMaxStringBytes)
        return core::Result<MsgCharInfo>::Err("net: charInfo name exceeds " +
                                              std::to_string(kMaxStringBytes) + " bytes");
    m.name = std::move(name.Value());
    return core::Result<MsgCharInfo>::Ok(std::move(m));
}

void MsgCharList::Write(core::Serializer& s) const {
    s.WriteU32(static_cast<uint32_t>(characters.size()));
    for (const MsgCharInfo& c : characters) {
        s.WriteU64(c.id);
        s.WriteString(c.name);
    }
}

core::Result<MsgCharList> MsgCharList::Read(core::Deserializer& d) {
    MsgCharList m;
    core::Result<uint32_t> count = d.ReadU32();
    if (!count.Ok()) return core::Result<MsgCharList>::Err("net: charList count truncated");
    if (count.Value() > kMaxCharacters)
        return core::Result<MsgCharList>::Err(
            "net: charList count " + std::to_string(count.Value()) +
            " exceeds limit " + std::to_string(kMaxCharacters));
    m.count = count.Value();
    m.characters.reserve(m.count);
    for (uint32_t i = 0; i < m.count; ++i) {
        core::Result<MsgCharInfo> c = MsgCharInfo::Read(d);
        if (!c.Ok()) return core::Result<MsgCharList>::Err(c.Error());
        m.characters.push_back(c.Value());
    }
    return core::Result<MsgCharList>::Ok(std::move(m));
}

void MsgRpc::Write(core::Serializer& s) const {
    s.WriteString(name);
    s.WriteString(argsJson);
}

core::Result<MsgRpc> MsgRpc::Read(core::Deserializer& d) {
    MsgRpc out;
    core::Result<std::string> name = d.ReadString();
    if (!name.Ok()) return core::Result<MsgRpc>::Err("net: rpc name " + name.Error());
    core::Result<std::string> args = d.ReadString();
    if (!args.Ok()) return core::Result<MsgRpc>::Err("net: rpc args " + args.Error());
    if (name.Value().size() > kMaxStringBytes)
        return core::Result<MsgRpc>::Err("net: rpc name exceeds " +
                                         std::to_string(kMaxStringBytes) + " bytes");
    if (args.Value().size() > kMaxStringBytes)
        return core::Result<MsgRpc>::Err("net: rpc args exceed " +
                                         std::to_string(kMaxStringBytes) + " bytes");
    out.name = std::move(name.Value());
    out.argsJson = std::move(args.Value());
    if (out.name.empty()) return core::Result<MsgRpc>::Err("net: rpc name must not be empty");
    return core::Result<MsgRpc>::Ok(std::move(out));
}

core::Result<std::vector<uint8_t>> MessageCodec::Encode(MsgType type, uint16_t seq,
                                                        const Payload& payload) {
    if (TypeOf(payload) != type)
        return core::Result<std::vector<uint8_t>>::Err(
            "net: encode msg type/id mismatch");
    core::Serializer s;
    s.WriteVersion(kProtocolVersion);
    s.WriteU8(static_cast<uint8_t>(type));
    s.WriteU16(seq);
    std::visit([&s](const auto& m) { m.Write(s); }, payload);
    return core::Result<std::vector<uint8_t>>::Ok(s.Data());
}

core::Result<std::vector<uint8_t>> MessageCodec::EncodeFrame(
    uint8_t msgId, uint16_t seq, const std::vector<uint8_t>& payload) {
    core::Serializer s;
    s.WriteVersion(kProtocolVersion);
    s.WriteU8(msgId);
    s.WriteU16(seq);
    for (uint8_t b : payload) s.WriteU8(b);
    return core::Result<std::vector<uint8_t>>::Ok(s.Data());
}

core::Result<DecodedMessage> MessageCodec::Decode(const std::vector<uint8_t>& data) {
    return Decode(data.data(), data.size());
}

core::Result<DecodedMessage> MessageCodec::Decode(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0)
        return core::Result<DecodedMessage>::Err("net: decode of empty buffer");

    std::vector<uint8_t> buf(data, data + size);
    core::Deserializer d(buf);
    if (!d.Validate())
        return core::Result<DecodedMessage>::Err("net: bad magic or crc");
    core::Result<uint32_t> version = d.ReadVersion(kProtocolVersion);
    if (!version.Ok())
        return core::Result<DecodedMessage>::Err(
            "net: unsupported protocol version or truncated frame");
    core::Result<uint8_t> msgId = d.ReadU8();
    if (!msgId.Ok())
        return core::Result<DecodedMessage>::Err("net: missing message id");
    core::Result<uint16_t> seq = d.ReadU16();
    if (!seq.Ok())
        return core::Result<DecodedMessage>::Err("net: missing sequence number");

    DecodedMessage out;
    out.header.version = kProtocolVersion;
    out.header.msgId = msgId.Value();
    out.header.seq = seq.Value();

    switch (static_cast<MsgType>(msgId.Value())) {
        case MsgType::Join: {
            core::Result<MsgJoin> m = MsgJoin::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Welcome: {
            core::Result<MsgWelcome> m = MsgWelcome::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Input: {
            core::Result<MsgInput> m = MsgInput::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Snapshot: {
            core::Result<MsgSnapshot> m = MsgSnapshot::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Spawn: {
            core::Result<MsgSpawn> m = MsgSpawn::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Despawn: {
            core::Result<MsgDespawn> m = MsgDespawn::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Ping: {
            core::Result<MsgPing> m = MsgPing::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Pong: {
            core::Result<MsgPong> m = MsgPong::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Ack: {
            core::Result<MsgAck> m = MsgAck::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Login: {
            core::Result<MsgLogin> m = MsgLogin::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::LoginOk: {
            core::Result<MsgLoginOk> m = MsgLoginOk::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::CharList: {
            core::Result<MsgCharList> m = MsgCharList::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        case MsgType::Rpc: {
            core::Result<MsgRpc> m = MsgRpc::Read(d);
            if (!m.Ok()) return core::Result<DecodedMessage>::Err(m.Error());
            out.payload = std::move(m.Value());
            break;
        }
        default:
            return core::Result<DecodedMessage>::Err(
                "net: unknown message id " + std::to_string(msgId.Value()));
    }

    if (d.Remaining() != 0)
        return core::Result<DecodedMessage>::Err("net: trailing bytes in frame");
    return core::Result<DecodedMessage>::Ok(std::move(out));
}

} // namespace neon::net

#include "neon/core/serialize.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <utility>

namespace neon::core {
namespace {

// Standard CRC-32 (IEEE 802.3), reflected polynomial 0xEDB88320.
const uint32_t kCrcPoly = 0xEDB88320u;

// 4-byte length prefix used for strings and byte buffers.
constexpr size_t kLenBytes = 4;

// Table-driven CRC-32 lookup table. Initialized once at first use by the
// compiler (thread-safe C++11 magic static); const, so no mutable global state.
const std::array<uint32_t, 256> kCrcTable = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (kCrcPoly ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}();

} // namespace

uint32_t Crc32(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = kCrcTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

Serializer::Serializer() {
    // Reserve header space (magic + crc). Payload starts at offset 8.
    buf_.resize(kHeaderBytes);
}

void Serializer::WriteRaw(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + size);
}

void Serializer::WriteU8(uint8_t v) { WriteRaw(&v, 1); }

void Serializer::WriteU16(uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xFFu)};
    WriteRaw(b, sizeof(b));
}

void Serializer::WriteU32(uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                    static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xFFu)};
    WriteRaw(b, sizeof(b));
}

void Serializer::WriteI32(int32_t v) { WriteU32(static_cast<uint32_t>(v)); }

void Serializer::WriteU64(uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
    WriteRaw(b, sizeof(b));
}

void Serializer::WriteF32(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    WriteU32(bits);
}

void Serializer::WriteF64(double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    WriteU64(bits);
}

void Serializer::WriteString(const std::string& str) {
    WriteU32(static_cast<uint32_t>(str.size()));
    if (!str.empty()) WriteRaw(str.data(), str.size());
}

void Serializer::WriteBytes(const std::vector<uint8_t>& bytes) {
    WriteU32(static_cast<uint32_t>(bytes.size()));
    if (!bytes.empty()) WriteRaw(bytes.data(), bytes.size());
}

void Serializer::WriteVersion(uint32_t version) {
    // Contract: must be the first write. In debug builds assert that nothing
    // else has been written yet so we never silently overwrite payload data.
    assert(buf_.size() == kHeaderBytes);
    uint8_t b[4] = {static_cast<uint8_t>(version >> 24), static_cast<uint8_t>(version >> 16),
                    static_cast<uint8_t>(version >> 8), static_cast<uint8_t>(version & 0xFFu)};
    WriteRaw(b, kLenBytes);
}

void Serializer::Finalize() {
    if (finalized_) return;
    if (buf_.size() < kHeaderBytes + kLenBytes)
        buf_.resize(kHeaderBytes + kLenBytes); // ensure version slot exists
    const uint8_t* payload = buf_.data() + kHeaderBytes;
    uint32_t crc = Crc32(payload, buf_.size() - kHeaderBytes);
    uint8_t b[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16),
                    static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc & 0xFFu)};
    std::memcpy(&buf_[4], b, 4);
    uint8_t m[4] = {static_cast<uint8_t>(kSerializedMagic >> 24),
                    static_cast<uint8_t>(kSerializedMagic >> 16),
                    static_cast<uint8_t>(kSerializedMagic >> 8),
                    static_cast<uint8_t>(kSerializedMagic & 0xFFu)};
    std::memcpy(&buf_[0], m, 4);
    finalized_ = true;
}

const std::vector<uint8_t>& Serializer::Data() const {
    // First access stamps magic + CRC, so a buffer handed to a reader is always
    // complete and validated. Idempotent.
    const_cast<Serializer*>(this)->Finalize();
    return buf_;
}

Deserializer::Deserializer(const std::vector<uint8_t>& buffer) : buf_(buffer) {}

bool Deserializer::Validate() const {
    if (buf_.size() < kHeaderBytes + kLenBytes) return false;
    uint32_t magic = (static_cast<uint32_t>(buf_[0]) << 24) |
                     (static_cast<uint32_t>(buf_[1]) << 16) |
                     (static_cast<uint32_t>(buf_[2]) << 8) |
                     static_cast<uint32_t>(buf_[3]);
    if (magic != kSerializedMagic) return false;
    uint32_t stored = (static_cast<uint32_t>(buf_[4]) << 24) |
                      (static_cast<uint32_t>(buf_[5]) << 16) |
                      (static_cast<uint32_t>(buf_[6]) << 8) |
                      static_cast<uint32_t>(buf_[7]);
    uint32_t actual = Crc32(buf_.data() + kHeaderBytes, buf_.size() - kHeaderBytes);
    return stored == actual;
}

core::Result<uint32_t> Deserializer::ReadVersion() {
    if (buf_.size() < kHeaderBytes + kLenBytes)
        return core::Result<uint32_t>::Err("serialize: buffer too short for header");
    pos_ = 0; // version lives at the start of the payload
    return ReadU32();
}

core::Result<uint32_t> Deserializer::ReadVersion(uint32_t expected) {
    core::Result<uint32_t> v = ReadVersion();
    if (!v.Ok()) return v;
    if (v.Value() != expected)
        return core::Result<uint32_t>::Err("serialize: version mismatch (got " +
                                           std::to_string(v.Value()) + ", expected " +
                                           std::to_string(expected) + ")");
    return v;
}

bool Deserializer::ReadRaw(void* out, size_t size) {
    if (size > Remaining()) return false;
    std::memcpy(out, buf_.data() + kHeaderBytes + pos_, size);
    pos_ += size;
    return true;
}

core::Result<uint8_t> Deserializer::ReadU8() {
    uint8_t v = 0;
    if (!ReadRaw(&v, 1)) return core::Result<uint8_t>::Err("serialize: truncated u8");
    return core::Result<uint8_t>::Ok(v);
}

core::Result<uint16_t> Deserializer::ReadU16() {
    uint8_t b[2];
    if (!ReadRaw(b, sizeof(b))) return core::Result<uint16_t>::Err("serialize: truncated u16");
    uint16_t v = (static_cast<uint16_t>(b[0]) << 8) | static_cast<uint16_t>(b[1]);
    return core::Result<uint16_t>::Ok(v);
}

core::Result<uint32_t> Deserializer::ReadU32() {
    uint8_t b[4];
    if (!ReadRaw(b, sizeof(b))) return core::Result<uint32_t>::Err("serialize: truncated u32");
    uint32_t v = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                 (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
    return core::Result<uint32_t>::Ok(v);
}

core::Result<uint64_t> Deserializer::ReadU64() {
    uint8_t b[8];
    if (!ReadRaw(b, sizeof(b))) return core::Result<uint64_t>::Err("serialize: truncated u64");
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint64_t>(b[i]);
    return core::Result<uint64_t>::Ok(v);
}

core::Result<int32_t> Deserializer::ReadI32() {
    core::Result<uint32_t> v = ReadU32();
    if (!v.Ok()) return core::Result<int32_t>::Err(v.Error());
    return core::Result<int32_t>::Ok(static_cast<int32_t>(v.Value()));
}

core::Result<float> Deserializer::ReadF32() {
    core::Result<uint32_t> v = ReadU32();
    if (!v.Ok()) return core::Result<float>::Err(v.Error());
    uint32_t bits = v.Value();
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return core::Result<float>::Ok(f);
}

core::Result<double> Deserializer::ReadF64() {
    core::Result<uint64_t> v = ReadU64();
    if (!v.Ok()) return core::Result<double>::Err(v.Error());
    uint64_t bits = v.Value();
    double d = 0.0;
    std::memcpy(&d, &bits, sizeof(d));
    return core::Result<double>::Ok(d);
}

core::Result<std::string> Deserializer::ReadString() {
    core::Result<uint32_t> len = ReadU32();
    if (!len.Ok()) return core::Result<std::string>::Err(len.Error());
    if (len.Value() > Remaining())
        return core::Result<std::string>::Err("serialize: string length exceeds buffer");
    std::string out(reinterpret_cast<const char*>(buf_.data()) + kHeaderBytes + pos_,
                    static_cast<size_t>(len.Value()));
    pos_ += len.Value();
    return core::Result<std::string>::Ok(std::move(out));
}

core::Result<std::vector<uint8_t>> Deserializer::ReadBytes() {
    core::Result<uint32_t> len = ReadU32();
    if (!len.Ok()) return core::Result<std::vector<uint8_t>>::Err(len.Error());
    if (len.Value() > Remaining())
        return core::Result<std::vector<uint8_t>>::Err("serialize: bytes length exceeds buffer");
    std::vector<uint8_t> out(buf_.data() + kHeaderBytes + pos_,
                             buf_.data() + kHeaderBytes + pos_ + len.Value());
    pos_ += len.Value();
    return core::Result<std::vector<uint8_t>>::Ok(std::move(out));
}

size_t Deserializer::Remaining() const {
    size_t payloadBytes = buf_.size() > kHeaderBytes ? buf_.size() - kHeaderBytes : 0;
    return pos_ <= payloadBytes ? payloadBytes - pos_ : 0;
}

} // namespace neon::core

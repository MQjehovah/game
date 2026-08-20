#pragma once

// Versioned binary serialization for stable cross-platform wire/disk formats.
//
// Layout (all multi-byte integers and floats big-endian / network byte order):
//   [ magic  : u32 ]  kSerializedMagic (fixed)
//   [ crc    : u32 ]  CRC32 over everything below (version + payload)
//   [ version: u32 ]  written via WriteVersion / checked via ReadVersion
//   [ payload...    ]
//
// Strings, byte buffers and counted arrays are length-prefixed with a u32
// byte/entry count written in big-endian.
//
// Errors are reported via core::Result; expected failure paths (wrong version,
// truncated input, corrupted data) never throw.

#include <cstdint>
#include <string>
#include <vector>

#include "neon/core/result.hpp"

namespace neon::core {

// Fixed magic number that must appear at the start of a serialized buffer.
// 0x4E454F4E = "NEON".
inline constexpr uint32_t kSerializedMagic = 0x4E454F4Eu;

// Bytes reserved for the magic + CRC header; the version field and all
// payload reads start right after it.
inline constexpr size_t kHeaderBytes = 8;

// Standard CRC-32 (poly 0xEDB88320), table-driven. Public so the same checksum
// can be reused for file containers / asset hashes.
uint32_t Crc32(const void* data, size_t size);

// Streaming writer. Append-only; call Data() to obtain the final bytes.
class Serializer {
public:
    Serializer();

    // Big-endian primitive writes. No error reporting needed: the backing
    // vector never fails except on OOM.
    void WriteU8(uint8_t v);
    void WriteU16(uint16_t v);
    void WriteU32(uint32_t v);
    void WriteU64(uint64_t v);
    void WriteI32(int32_t v) { WriteU32(static_cast<uint32_t>(v)); }
    void WriteF32(float v);
    void WriteF64(double v);

    // Length-prefixed (u32 byte count) payloads.
    void WriteString(const std::string& str);
    void WriteBytes(const std::vector<uint8_t>& bytes);

    // Marks the current write position as the format version, so an older
    // reader can reject buffers it does not understand.
    void WriteVersion(uint32_t version);

    // Stamps the header (magic + CRC32) after all payload writes are done.
    // Data() calls this automatically on first access, so it only needs to be
    // called explicitly when the raw header bytes are inspected beforehand.
    void Finalize();

    const std::vector<uint8_t>& Data() const;
    size_t Size() const { return buf_.size(); }

private:
    void WriteRaw(const void* data, size_t size);

    std::vector<uint8_t> buf_;
    bool finalized_ = false;
};

// Reading wrapper over an existing byte buffer with a forward cursor. Reading
// past the end returns Err instead of reading garbage.
class Deserializer {
public:
    explicit Deserializer(const std::vector<uint8_t>& buffer);

    // Verifies magic and CRC32 over the payload. Returns false on any mismatch.
    // Call before reading when integrity matters; cheap (single linear pass).
    bool Validate() const;

    // Reads the version field and compares it against `expected`. Returns Err
    // if the buffer carries a different version (or is malformed/truncated).
    core::Result<uint32_t> ReadVersion(uint32_t expected);

    // Reads the version field without comparing. Useful when the caller wants
    // to branch on the version itself.
    core::Result<uint32_t> ReadVersion();

    core::Result<uint8_t> ReadU8();
    core::Result<uint16_t> ReadU16();
    core::Result<uint32_t> ReadU32();
    core::Result<uint64_t> ReadU64();
    core::Result<int32_t> ReadI32();
    core::Result<float> ReadF32();
    core::Result<double> ReadF64();
    core::Result<std::string> ReadString();
    core::Result<std::vector<uint8_t>> ReadBytes();

    // Number of unread payload bytes (excludes the fixed header).
    size_t Remaining() const;
    size_t Size() const { return buf_.size(); }

private:
    bool ReadRaw(void* out, size_t size);

    const std::vector<uint8_t>& buf_;
    size_t pos_ = 0;
};

} // namespace neon::core

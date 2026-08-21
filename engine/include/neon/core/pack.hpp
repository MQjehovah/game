#pragma once

// Game asset container: one byte stream that carries named byte blobs plus an
// index of their locations. Written by PackWriter, read on demand by
// PackReader (only the index is parsed up front; data blocks are pulled and
// verified per entry).
//
// Layout (all multi-byte integers big-endian, matching core::Serializer):
//   [ magic     : 8 bytes ]  "NEONPACK"
//   [ version   : u32     ]  kPackVersion
//   [ indexCrc  : u32     ]  CRC32 over the index below (fileCount + entries)
//   [ fileCount : u32     ]
//   [ entries, sorted by path:
//       pathLen : u32
//       path    : bytes (UTF-8, opaque to the pack)
//       offset  : u64     byte offset of the data block from start of pack
//       compSize: u32     bytes stored on disk
//       rawSize : u32     bytes after decompression
//       crc32   : u32     CRC32 of the raw (uncompressed) bytes
//       method  : u8      0 = store; other values reserved for future codecs
//   ]
//   [ data blocks... ]        referenced by entry offsets
//
// Paths are opaque strings (never touched by the real filesystem), so UTF-8
// keys round-trip on any platform. Errors are reported via core::Result or the
// Valid()/Error() accessors; nothing throws.

#include <cstdint>
#include <string>
#include <vector>

#include "neon/core/result.hpp"

namespace neon::core {

// Pack format version. Bump (and branch on) when the layout changes.
inline constexpr uint32_t kPackVersion = 1;

// Bytes reserved for the fixed header: magic(8) + version(4) + indexCrc(4).
inline constexpr size_t kPackHeaderBytes = 16;

// Compression method byte values.
inline constexpr uint8_t kPackCompressionStore = 0; // raw bytes, uncompressed

// Collects named byte blobs and serializes them into a single pack stream.
class PackWriter {
public:
    PackWriter() = default;

    // Registers an entry under `virtualPath` (an arbitrary string key stored
    // verbatim). Returns Err and ignores the entry when the path is empty,
    // duplicates an existing path, or when the path/blob exceeds UINT32_MAX
    // bytes (the per-entry size fields are u32). Never throws.
    core::Status AddFile(const std::string& virtualPath, const std::vector<uint8_t>& data);

    // Serializes the pack: paths are sorted, the index CRC is stamped, and each
    // entry's data block is appended in that order. Deterministic for the same
    // input set. Entries are currently stored uncompressed (kPackCompressionStore).
    std::vector<uint8_t> Build() const;

private:
    struct Entry {
        std::string path;
        std::vector<uint8_t> data;
    };
    std::vector<Entry> entries_;
};

// Reads a pack stream. The constructor parses and validates the header plus the
// full index (magic, version, index CRC, block bounds); the referenced buffer
// must outlive this reader. Data blocks are only read/verified on demand.
class PackReader {
public:
    explicit PackReader(const std::vector<uint8_t>& bytes);

    bool Valid() const;
    const std::string& Error() const;

    size_t FileCount() const;
    bool Has(const std::string& virtualPath) const;

    // Sorted list of entry paths.
    std::vector<std::string> Enumerate() const;

    // Reads a block on demand. Returns Err for unknown paths, corrupted blocks
    // (per-entry CRC mismatch), unsupported compression methods or malformed
    // offset/size fields.
    core::Result<std::vector<uint8_t>> Read(const std::string& virtualPath) const;

private:
    struct Entry {
        std::string path;
        uint64_t offset = 0;
        uint32_t compSize = 0;
        uint32_t rawSize = 0;
        uint32_t crc32 = 0;
        uint8_t method = 0;
    };

    bool ParseIndex();

    const std::vector<uint8_t>& buf_;
    std::vector<Entry> entries_;
    std::string error_;
};

// Expands every entry of `reader` under `destDir`: virtual paths use forward
// slashes and map onto real subdirectories, which are created on demand. The
// destination directory itself is created when missing. Returns Err on the
// first failed entry (an invalid reader, unsafe/traversal path, or write
// failure); entries already written are left on disk for the caller to clean
// up. Used by the neon_game player to turn a store-only pack into a disk tree
// the AssetManager/GameRuntime can read.
core::Status Unpack(const PackReader& reader, const std::string& destDir);

} // namespace neon::core

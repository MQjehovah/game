#pragma once

#include <memory>
#include <string>
#include <vector>

#include "neon/core/pack.hpp"
#include "neon/core/result.hpp"

namespace neon::io {

// Read-only virtual file system (G7-1). Virtual paths are forward-slash,
// traversal-safe keys; implementations resolve them to their backing store.
// DiskFileSystem serves a root directory, PackFileSystem serves a game.pack
// container without unpacking, and MountStack overlays several layers so Mod
// directories can override base-pack content (later mounts win).
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    virtual bool Exists(const std::string& path) const = 0;
    // Full file contents; Err when the path is missing or unreadable.
    virtual core::Result<std::vector<uint8_t>> ReadFile(const std::string& path) const = 0;
    // Modification time in seconds (0 when missing / not applicable); used by
    // the asset hot-reload watcher.
    virtual uint64_t FileMTime(const std::string& path) const = 0;
    // Sorted virtual paths under `dir` (recursive when true; "" = whole tree).
    // Empty when the directory does not exist.
    virtual std::vector<std::string> ListFiles(const std::string& dir, bool recursive) const = 0;
};

// Normalizes a virtual path: backslashes to forward slashes, collapses "."
// and "..", rejects escapes above the root (returns false), and trims leading
// '/'. All IFileSystem implementations apply this to incoming paths.
bool NormalizeVirtualPath(const std::string& path, std::string& out);

// A directory on the real filesystem, mounted at its root. Resolved paths are
// guaranteed to stay under the root (traversal is rejected, not escaped).
class DiskFileSystem : public IFileSystem {
public:
    explicit DiskFileSystem(std::string rootDir);
    std::string Root() const { return root_; }

    bool Exists(const std::string& path) const override;
    core::Result<std::vector<uint8_t>> ReadFile(const std::string& path) const override;
    uint64_t FileMTime(const std::string& path) const override;
    std::vector<std::string> ListFiles(const std::string& dir, bool recursive) const override;

private:
    // Absolute path under root_ for a normalized virtual path ("" on escape).
    std::string Resolve(const std::string& path) const;
    void ListDir(const std::string& absDir, const std::string& prefix, bool recursive,
                 std::vector<std::string>& out) const;

    std::string root_;
};

// A game.pack container served as a read-only file system (no unpacking).
// Owns the pack bytes, so the instance can outlive the original buffer.
class PackFileSystem : public IFileSystem {
public:
    explicit PackFileSystem(std::vector<uint8_t> packBytes);
    const core::PackReader& Reader() const { return reader_; }

    bool Exists(const std::string& path) const override;
    core::Result<std::vector<uint8_t>> ReadFile(const std::string& path) const override;
    uint64_t FileMTime(const std::string& path) const override;
    std::vector<std::string> ListFiles(const std::string& dir, bool recursive) const override;

private:
    std::vector<uint8_t> bytes_;
    core::PackReader reader_;
};

// Layered read-only file system. Mount() pushes a layer to the FRONT
// (highest priority), so later mounts override earlier ones (Mod layers
// mounted after the base pack win). Read/Exists/FileMTime stop at the first
// layer that owns the path; ListFiles unions every layer (deduped, sorted).
class MountStack : public IFileSystem {
public:
    void Mount(std::shared_ptr<IFileSystem> layer);
    void Unmount(const IFileSystem* layer);
    void Clear();
    size_t LayerCount() const { return layers_.size(); }
    // The highest-priority layer that owns `path` (nullptr when none).
    const IFileSystem* OwnerOf(const std::string& path) const;

    bool Exists(const std::string& path) const override;
    core::Result<std::vector<uint8_t>> ReadFile(const std::string& path) const override;
    uint64_t FileMTime(const std::string& path) const override;
    std::vector<std::string> ListFiles(const std::string& dir, bool recursive) const override;

private:
    // Front = highest priority (last mounted).
    std::vector<std::shared_ptr<IFileSystem>> layers_;
};

} // namespace neon::io

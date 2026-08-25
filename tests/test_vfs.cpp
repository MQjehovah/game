#include <string>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"
#include "test_backend.hpp"

using namespace neon;

namespace {

std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

std::string AsString(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }

#if defined(_WIN32)
#include <direct.h>
bool Mkdir(const std::string& p) { return ::_mkdir(p.c_str()) == 0; }
#else
#include <sys/stat.h>
bool Mkdir(const std::string& p) { return ::mkdir(p.c_str(), 0777) == 0; }
#endif

} // namespace

// Path normalization: separators, "."/".." collapsing, escape rejection.
TEST(VfsNormalizePath) {
    std::string out;
    CHECK(io::NormalizeVirtualPath("a/b/c.png", out));
    CHECK_EQ(out, std::string("a/b/c.png"));
    CHECK(io::NormalizeVirtualPath("a\\b\\c.png", out));
    CHECK_EQ(out, std::string("a/b/c.png"));
    CHECK(io::NormalizeVirtualPath("./a/../b/./c", out));
    CHECK_EQ(out, std::string("b/c"));
    CHECK(io::NormalizeVirtualPath("/assets/tex.png", out)); // leading slash trimmed
    CHECK_EQ(out, std::string("assets/tex.png"));
    CHECK(!io::NormalizeVirtualPath("../secret.txt", out)); // escape rejected
    CHECK(!io::NormalizeVirtualPath("a/../../secret.txt", out));
    CHECK(!io::NormalizeVirtualPath("", out));
}

// A directory mounted as a file system: reads, existence, listing, and the
// traversal guard.
TEST(VfsDiskFileSystem) {
    test::TempDir tmp;
    const std::string root = tmp.Str();
    CHECK(test::WriteFileAll(root + "/hello.txt", "hello"));
    CHECK(Mkdir(root + "/dir"));
    CHECK(test::WriteFileAll(root + "/dir/nested.txt", "nested"));

    io::DiskFileSystem fs(root);
    CHECK(fs.Exists("hello.txt"));
    CHECK(!fs.Exists("missing.txt"));
    const auto r = fs.ReadFile("hello.txt");
    CHECK(r.Ok());
    CHECK_EQ(AsString(r.Value()), std::string("hello"));
    CHECK(fs.FileMTime("hello.txt") != 0);
    CHECK_EQ(fs.FileMTime("missing.txt"), 0u);

    const std::vector<std::string> all = fs.ListFiles("", true);
    CHECK(all.size() == 2u);
    CHECK_EQ(all[0], std::string("dir/nested.txt"));
    CHECK_EQ(all[1], std::string("hello.txt"));
    const std::vector<std::string> top = fs.ListFiles("", false);
    CHECK_EQ(top.size(), 1u);
    CHECK_EQ(top[0], std::string("hello.txt"));

    // Traversal attempts are rejected, never escaped.
    CHECK(!fs.Exists("../escape.txt"));
    const auto bad = fs.ReadFile("../escape.txt");
    CHECK(!bad.Ok());
}

// A game.pack served as a file system without unpacking.
TEST(VfsPackFileSystem) {
    core::PackWriter writer;
    CHECK(writer.AddFile("game.json", Bytes("{\"startScene\":\"scenes/main.json\"}")).Ok());
    CHECK(writer.AddFile("scenes/main.json", Bytes("{}")).Ok());
    CHECK(writer.AddFile("assets/tex.png", Bytes("png")).Ok());
    const std::vector<uint8_t> bytes = writer.Build();

    io::PackFileSystem fs(bytes);
    CHECK(fs.Exists("game.json"));
    CHECK(fs.Exists("assets/tex.png"));
    CHECK(!fs.Exists("nope"));
    const auto r = fs.ReadFile("assets/tex.png");
    CHECK(r.Ok());
    CHECK_EQ(AsString(r.Value()), std::string("png"));
    CHECK(!fs.ReadFile("missing").Ok());
    CHECK_EQ(fs.FileMTime("game.json"), 0u); // immutable container

    const std::vector<std::string> scenes = fs.ListFiles("scenes", false);
    CHECK_EQ(scenes.size(), 1u);
    CHECK_EQ(scenes[0], std::string("scenes/main.json"));
    const std::vector<std::string> all = fs.ListFiles("", true);
    CHECK_EQ(all.size(), 3u);
}

// Mount stack: later mounts override earlier ones (Mod over pack), and files
// only in the base layer still resolve.
TEST(VfsMountStackOverrides) {
    core::PackWriter writer;
    CHECK(writer.AddFile("game.json", Bytes("base game")).Ok());
    CHECK(writer.AddFile("scripts/ai.lua", Bytes("base ai")).Ok());
    CHECK(writer.AddFile("assets/tex.png", Bytes("base png")).Ok());
    const std::vector<uint8_t> packBytes = writer.Build();
    auto pack = std::make_shared<io::PackFileSystem>(packBytes);

    test::TempDir tmp;
    const std::string mod = tmp.Str();
    CHECK(Mkdir(mod + "/assets"));
    CHECK(Mkdir(mod + "/scripts"));
    CHECK(test::WriteFileAll(mod + "/assets/tex.png", "mod png"));
    CHECK(test::WriteFileAll(mod + "/scripts/mod.lua", "mod script"));
    auto modFs = std::make_shared<io::DiskFileSystem>(mod);

    io::MountStack vfs;
    vfs.Mount(pack);   // base
    vfs.Mount(modFs);  // mod wins
    CHECK_EQ(vfs.LayerCount(), 2u);

    const auto tex = vfs.ReadFile("assets/tex.png");
    CHECK(tex.Ok());
    CHECK_EQ(AsString(tex.Value()), std::string("mod png")); // overridden
    const auto ai = vfs.ReadFile("scripts/ai.lua");
    CHECK(ai.Ok());
    CHECK_EQ(AsString(ai.Value()), std::string("base ai")); // base only
    CHECK(!vfs.ReadFile("scripts/nope.lua").Ok());
    CHECK(vfs.OwnerOf("assets/tex.png") == modFs.get());
    CHECK(vfs.OwnerOf("scripts/ai.lua") == pack.get());

    // Unmounting the mod layer restores the base content.
    vfs.Unmount(modFs.get());
    const auto base = vfs.ReadFile("assets/tex.png");
    CHECK(base.Ok());
    CHECK_EQ(AsString(base.Value()), std::string("base png"));
}

// ListFiles unions all layers, deduped and sorted.
TEST(VfsMountStackListUnion) {
    core::PackWriter writer;
    CHECK(writer.AddFile("a.txt", Bytes("1")).Ok());
    CHECK(writer.AddFile("shared.txt", Bytes("base")).Ok());
    const std::vector<uint8_t> packBytes = writer.Build();
    auto pack = std::make_shared<io::PackFileSystem>(packBytes);

    test::TempDir tmp;
    const std::string mod = tmp.Str();
    CHECK(test::WriteFileAll(mod + "/shared.txt", "mod"));
    CHECK(test::WriteFileAll(mod + "/b.txt", "2"));
    auto modFs = std::make_shared<io::DiskFileSystem>(mod);

    io::MountStack vfs;
    vfs.Mount(pack);
    vfs.Mount(modFs);
    const std::vector<std::string> files = vfs.ListFiles("", true);
    CHECK_EQ(files.size(), 3u);
    CHECK_EQ(files[0], std::string("a.txt"));
    CHECK_EQ(files[1], std::string("b.txt"));
    CHECK_EQ(files[2], std::string("shared.txt"));
}

// Production path (G7-1): the AssetManager reads assets through the mount
// stack (pack container + Mod overlay) when one is installed.
TEST(VfsAssetManagerReadsThroughMountStack) {
    std::string pngText, objText;
    CHECK(test::ReadFileAll("projects/pvz/assets/sprites/sun.png", pngText));
    objText = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

    core::PackWriter writer;
    CHECK(writer.AddFile("assets/sprites/sun.png", Bytes(pngText)).Ok());
    CHECK(writer.AddFile("assets/crate.obj", Bytes(objText)).Ok());
    const std::vector<uint8_t> packBytes = writer.Build();
    auto pack = std::make_shared<io::PackFileSystem>(packBytes);

    // Mod layer overrides the base texture with a different real PNG.
    test::TempDir tmp;
    const std::string mod = tmp.Str();
    CHECK(Mkdir(mod + "/assets"));
    CHECK(Mkdir(mod + "/assets/sprites"));
    std::string modPng;
    CHECK(test::ReadFileAll("projects/pvz/assets/sprites/pea.png", modPng));
    CHECK(test::WriteFileAll(mod + "/assets/sprites/sun.png", modPng));
    auto modFs = std::make_shared<io::DiskFileSystem>(mod);

    io::MountStack vfs;
    vfs.Mount(pack);
    vfs.Mount(modFs);

    test::HeadlessAssetFixture fixture;
    fixture.assets.SetFileSystem(&vfs);
    CHECK(vfs.OwnerOf("assets/sprites/sun.png") == modFs.get());
    const gfx::Texture tex = fixture.assets.LoadTexture("assets/sprites/sun.png");
    CHECK(tex.Valid());
    // The OBJ exists only in the pack layer.
    const gfx::Mesh mesh = fixture.assets.LoadMeshOBJ("assets/crate.obj");
    CHECK(mesh.Valid());
    // A path in neither layer fails cleanly.
    CHECK(!fixture.assets.LoadTexture("assets/sprites/missing.png").Valid());
}

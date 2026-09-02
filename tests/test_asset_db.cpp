#include <filesystem>
#include <string>

#include "neon/neon.hpp"
#include "neon/assets/asset_db.hpp"
#include "helpers.hpp"

using namespace neon;

// G5-4-4(项3), central-store revision: asset identity lives ONLY in the single
// .asset_db.json (path -> guid + content hash). Same path keeps its GUID; a
// new path with identical bytes to a vanished one is a MOVE and inherits the
// GUID, so scene path references get rewritten.
TEST(AssetDbStableGuids) {
    test::TempDir tmp;
    const std::string root = tmp.Str();
    std::filesystem::create_directories(root + "/assets");
    CHECK(test::WriteFileAll(root + "/game.json", "{}"));
    CHECK(test::WriteFileAll(root + "/assets/a.png", "aaaa"));
    CHECK(test::WriteFileAll(root + "/assets/b.png", "bbbb"));

    const assets::AssetDatabase db1 = assets::AssetDatabase::Build(root);
    CHECK_EQ(db1.Entries().size(), 3u); // game.json + 2 textures (no sidecar entries)
    const std::string guidA = db1.GuidFor("assets/a.png");
    const std::string guidB = db1.GuidFor("assets/b.png");
    CHECK(!guidA.empty());
    CHECK(!guidB.empty());
    CHECK(guidA != guidB);
    CHECK_EQ(db1.PathFor(guidA), "assets/a.png");

    // B: ResolveAssetRef is GUID-first with path fallback — a known GUID maps to
    // its path, a path (or unknown GUID) is returned literally.
    CHECK_EQ(db1.ResolveAssetRef(guidA), "assets/a.png");
    CHECK_EQ(db1.ResolveAssetRef("assets/b.png"), "assets/b.png");      // literal path
    CHECK_EQ(db1.ResolveAssetRef("ab12cd34ef56ab12"), "ab12cd34ef56ab12"); // unknown GUID -> literal
    CHECK(!assets::AssetDatabase::IsGuidToken("assets/a.png"));
    CHECK(assets::AssetDatabase::IsGuidToken(guidA));

    // No sidecar files: identity lives in the database only.
    std::string metaProbe;
    CHECK(!test::ReadFileAll(root + "/assets/a.png.meta", metaProbe));

    // Round-trip through the snapshot preserves identity + hashes.
    const assets::AssetDatabase db2 = assets::AssetDatabase::FromJson(db1.ToJson());
    CHECK_EQ(db2.GuidFor("assets/a.png"), guidA);

    // A rescan against the previous snapshot reuses the persisted GUIDs.
    const assets::AssetDatabase db3 = assets::AssetDatabase::Build(root, db2);
    CHECK_EQ(db3.GuidFor("assets/a.png"), guidA);
    CHECK_EQ(db3.GuidFor("assets/b.png"), guidB);
}

TEST(AssetDbDetectMovesAndRewrite) {
    test::TempDir root;
    const std::string dir = root.Str();
    std::filesystem::create_directories(dir + "/assets");
    CHECK(test::WriteFileAll(dir + "/assets/old.png", "xxxx"));
    CHECK(test::WriteFileAll(dir + "/assets/keep.png", "yyyy"));
    const assets::AssetDatabase dbBefore = assets::AssetDatabase::Build(dir);
    const std::string guidOld = dbBefore.GuidFor("assets/old.png");
    const std::string guidKeep = dbBefore.GuidFor("assets/keep.png");
    CHECK(!guidOld.empty());
    CHECK(!guidKeep.empty());

    // Physically move the asset — the matching content hash lets the new path
    // inherit the old entry's GUID (no sidecar needed).
    std::error_code ec;
    std::filesystem::rename(dir + "/assets/old.png", dir + "/assets/renamed.png", ec);
    CHECK(!ec);
    const assets::AssetDatabase dbAfter = assets::AssetDatabase::Build(dir, dbBefore);

    const std::vector<assets::AssetMove> moves = assets::DetectAssetMoves(dbBefore, dbAfter);
    CHECK_EQ(moves.size(), 1u);
    CHECK_EQ(moves[0].oldPath, "assets/old.png");
    CHECK_EQ(moves[0].newPath, "assets/renamed.png");
    CHECK_EQ(dbAfter.GuidFor("assets/renamed.png"), guidOld); // identity survived
    CHECK_EQ(dbAfter.GuidFor("assets/keep.png"), guidKeep);

    // A scene referencing the old path is rewritten to the new one.
    const std::string scene =
        "{\"entities\":[{\"mesh\":{\"meshKey\":\"gltf:assets/old.png\"},"
        "\"textures\":{\"albedoTex\":\"assets/old.png\"}}]}";
    const std::string rewritten = assets::RewriteJsonReferences(scene, moves);
    CHECK(rewritten.find("assets/renamed.png") != std::string::npos);
    CHECK(rewritten.find("assets/old.png") == std::string::npos);
}

TEST(AssetDbEditAndDelete) {
    test::TempDir root;
    const std::string dir = root.Str();
    std::filesystem::create_directories(dir + "/assets");
    CHECK(test::WriteFileAll(dir + "/assets/a.png", "aaaa"));
    CHECK(test::WriteFileAll(dir + "/assets/gone.png", "gone"));
    const assets::AssetDatabase dbBefore = assets::AssetDatabase::Build(dir);
    const std::string guidA = dbBefore.GuidFor("assets/a.png");

    // Editing a file in place keeps its identity (path is the primary key).
    CHECK(test::WriteFileAll(dir + "/assets/a.png", "aaaa-edited"));
    // Deleting an asset just drops its entry (nothing inherits its GUID).
    std::error_code ec;
    std::filesystem::remove(dir + "/assets/gone.png", ec);
    CHECK(!ec);
    const assets::AssetDatabase dbAfter = assets::AssetDatabase::Build(dir, dbBefore);

    CHECK_EQ(dbAfter.GuidFor("assets/a.png"), guidA);
    CHECK_EQ(dbAfter.Entries().size(),
             static_cast<size_t>(dbBefore.Entries().size() - 1));
    CHECK(dbAfter.PathFor(dbBefore.GuidFor("assets/gone.png")).empty());
}

TEST(AssetDbLegacyMetaAdopted) {
    test::TempDir root;
    const std::string dir = root.Str();
    std::filesystem::create_directories(dir + "/assets");
    CHECK(test::WriteFileAll(dir + "/assets/a.png", "aaaa"));
    // A pre-centralization project carries sidecar metas; their GUID is
    // adopted once and the redundant sidecar is reported for deletion.
    CHECK(test::WriteFileAll(dir + "/assets/a.png.meta", "deadbeefdeadbeef\n"));

    const assets::AssetDatabase db = assets::AssetDatabase::Build(dir);
    CHECK_EQ(db.GuidFor("assets/a.png"), std::string("deadbeefdeadbeef"));

    std::vector<std::string> adopted;
    const assets::AssetDatabase db2 = assets::AssetDatabase::Build(dir, db, &adopted);
    CHECK(adopted.empty()); // nothing left to adopt after the first scan
    CHECK_EQ(db2.GuidFor("assets/a.png"), std::string("deadbeefdeadbeef"));
}

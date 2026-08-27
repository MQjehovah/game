#include <filesystem>
#include <string>

#include "neon/neon.hpp"
#include "neon/assets/asset_db.hpp"
#include "helpers.hpp"

using namespace neon;

// G5-4-4(项3): asset GUID database — a .meta file carries a stable GUID that
// physically travels with the asset, so a rename is detected and scene path
// references get rewritten.
TEST(AssetDbStableGuids) {
    test::TempDir tmp;
    const std::string root = tmp.Str();
    std::filesystem::create_directories(root + "/assets");
    CHECK(test::WriteFileAll(root + "/game.json", "{}"));
    CHECK(test::WriteFileAll(root + "/assets/a.png", "aaaa"));
    CHECK(test::WriteFileAll(root + "/assets/b.png", "bbbb"));

    assets::AssetDatabase db1 = assets::AssetDatabase::Build(root);
    CHECK_EQ(db1.Entries().size(), 3u); // game.json + 2 textures (no .meta entries)
    const std::string guidA = db1.GuidFor("assets/a.png");
    const std::string guidB = db1.GuidFor("assets/b.png");
    CHECK(!guidA.empty());
    CHECK(!guidB.empty());
    CHECK(guidA != guidB);
    CHECK_EQ(db1.PathFor(guidA), "assets/a.png");

    // The .meta file now exists next to the asset and carries the GUID.
    std::string metaText;
    CHECK(test::ReadFileAll(root + "/assets/a.png.meta", metaText));
    CHECK(metaText.find(guidA) != std::string::npos);

    // A fresh scan reuses the persisted GUID (no new .meta written).
    const assets::AssetDatabase db2 = assets::AssetDatabase::Build(root);
    CHECK_EQ(db2.GuidFor("assets/a.png"), guidA);
    CHECK_EQ(db2.GuidFor("assets/b.png"), guidB);
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

    // Physically move the asset WITH its .meta — the GUID travels.
    std::error_code ec;
    std::filesystem::rename(dir + "/assets/old.png", dir + "/assets/renamed.png", ec);
    CHECK(!ec);
    ec.clear();
    std::filesystem::rename(dir + "/assets/old.png.meta", dir + "/assets/renamed.png.meta", ec);
    CHECK(!ec);
    const assets::AssetDatabase dbAfter = assets::AssetDatabase::Build(dir);

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

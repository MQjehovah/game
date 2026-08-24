#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

namespace {

math::AABB MakeBox(core::Rng& rng, float spread, float halfSize) {
    const float x = rng.Range(-spread, spread);
    const float y = rng.Range(-spread, spread);
    const float z = rng.Range(-spread, spread);
    const float h = rng.Range(0.1f, halfSize);
    return {math::Vec3{x - h, y - h, z - h}, math::Vec3{x + h, y + h, z + h}};
}

std::vector<uint32_t> BruteAABB(const std::vector<math::AABB>& boxes, const math::AABB& q) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < boxes.size(); ++i)
        if (boxes[i].Intersects(q)) out.push_back(static_cast<uint32_t>(i));
    return out;
}

std::vector<uint32_t> BruteFrustum(const std::vector<math::AABB>& boxes,
                                   const math::Frustum& f) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < boxes.size(); ++i)
        if (f.Intersects(boxes[i])) out.push_back(static_cast<uint32_t>(i));
    return out;
}

std::vector<uint32_t> BruteRay(const std::vector<math::AABB>& boxes, const math::Ray& ray,
                               float maxDist) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < boxes.size(); ++i) {
        float t = 0.0f;
        if (math::IntersectRayAABB(ray, boxes[i], t) && t <= maxDist)
            out.push_back(static_cast<uint32_t>(i));
    }
    return out;
}

std::vector<uint32_t> QueryToVector(const math::Bvh& bvh, const math::AABB& q) {
    std::vector<uint32_t> out;
    bvh.QueryAABB(q, [&](math::Bvh::Id id) { out.push_back(id); });
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<uint32_t> FrustumToVector(const math::Bvh& bvh, const math::Frustum& f) {
    std::vector<uint32_t> out;
    bvh.QueryFrustum(f, [&](math::Bvh::Id id) { out.push_back(id); });
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<uint32_t> RayToVector(const math::Bvh& bvh, const math::Ray& ray, float maxDist) {
    std::vector<uint32_t> out;
    bvh.QueryRay(ray, maxDist, [&](math::Bvh::Id id) { out.push_back(id); });
    std::sort(out.begin(), out.end());
    return out;
}

math::Frustum TestFrustum(const math::Vec3& pos, const math::Vec3& target, float aspect) {
    gfx::Camera cam;
    cam.position = pos;
    cam.target = target;
    cam.up = {0, 1, 0};
    cam.fovY = 60.0f * math::kDegToRad;
    cam.nearPlane = 0.1f;
    cam.farPlane = 200.0f;
    return math::Frustum::FromViewProjection(cam.ViewProjection(aspect));
}

} // namespace

// Basic insert + AABB query over a small known set.
TEST(BvhBasicQuery) {
    math::Bvh bvh;
    bvh.Insert(7, {{0, 0, 0}, {2, 2, 2}});
    bvh.Insert(3, {{10, 10, 10}, {12, 12, 12}});
    bvh.Insert(9, {{-5, -5, -5}, {-3, -3, -3}});

    CHECK_EQ(bvh.Size(), 3u);
    CHECK(bvh.Contains(7));
    CHECK(!bvh.Contains(1));

    std::vector<uint32_t> hit = QueryToVector(bvh, {{1, 1, 1}, {11, 11, 11}});
    CHECK_EQ(hit.size(), 2u);
    CHECK_EQ(hit[0], 3u);
    CHECK_EQ(hit[1], 7u);

    // Duplicate insert is a no-op; Update moves the box; Remove drops it.
    bvh.Insert(7, {{0, 0, 0}, {2, 2, 2}});
    CHECK_EQ(bvh.Size(), 3u);
    bvh.Update(7, {{20, 20, 20}, {22, 22, 22}});
    hit = QueryToVector(bvh, {{0, 0, 0}, {5, 5, 5}});
    CHECK(hit.empty());
    hit = QueryToVector(bvh, {{19, 19, 19}, {23, 23, 23}});
    CHECK_EQ(hit.size(), 1u);
    CHECK_EQ(hit[0], 7u);
    bvh.Remove(7);
    CHECK_EQ(bvh.Size(), 2u);
    CHECK(!bvh.Contains(7));
    bvh.Clear();
    CHECK(bvh.Empty());
}

// Random insert + AABB queries must match brute force exactly.
TEST(BvhMatchesBruteForceAabb) {
    core::Rng rng(42);
    math::Bvh bvh;
    std::vector<math::AABB> boxes;
    for (int i = 0; i < 500; ++i) {
        boxes.push_back(MakeBox(rng, 100.0f, 3.0f));
        bvh.Insert(static_cast<uint32_t>(i), boxes.back());
    }
    CHECK_EQ(bvh.Size(), 500u);

    for (int q = 0; q < 200; ++q) {
        const math::AABB query = MakeBox(rng, 80.0f, 12.0f);
        const std::vector<uint32_t> expected = BruteAABB(boxes, query);
        const std::vector<uint32_t> actual = QueryToVector(bvh, query);
        CHECK_EQ(actual.size(), expected.size());
        if (actual.size() == expected.size())
            for (size_t i = 0; i < actual.size(); ++i) CHECK_EQ(actual[i], expected[i]);
    }
}

// Remove + Update (moves) must keep queries identical to brute force.
TEST(BvhRemoveAndUpdateMatchBruteForce) {
    core::Rng rng(7);
    math::Bvh bvh;
    std::vector<math::AABB> boxes;
    std::vector<bool> alive;
    // Keep the moved boxes in a parallel copy so brute force uses the same
    // state the BVH saw after Update.
    std::vector<math::AABB> liveBoxes;
    std::vector<uint32_t> liveIds;
    for (int i = 0; i < 400; ++i) {
        boxes.push_back(MakeBox(rng, 100.0f, 3.0f));
        alive.push_back(true);
        bvh.Insert(static_cast<uint32_t>(i), boxes.back());
        liveBoxes.push_back(boxes.back());
        liveIds.push_back(static_cast<uint32_t>(i));
    }

    // Remove ~30% of the ids (keep the brute-force copy in sync).
    for (int i = 0; i < 400; ++i) {
        if (rng.Bool(0.3f)) {
            bvh.Remove(static_cast<uint32_t>(i));
            alive[i] = false;
        }
    }
    // Move ~30% of the survivors to new spots.
    for (int i = 0; i < 400; ++i) {
        if (alive[i] && rng.Bool(0.3f)) {
            boxes[static_cast<size_t>(i)] = MakeBox(rng, 100.0f, 3.0f);
            bvh.Update(static_cast<uint32_t>(i), boxes[static_cast<size_t>(i)]);
        }
    }
    // Rebuild the brute-force side from the surviving (possibly moved) boxes.
    liveBoxes.clear();
    liveIds.clear();
    for (int i = 0; i < 400; ++i) {
        if (alive[i]) {
            liveBoxes.push_back(boxes[static_cast<size_t>(i)]);
            liveIds.push_back(static_cast<uint32_t>(i));
        }
    }
    CHECK_EQ(bvh.Size(), liveBoxes.size());

    for (int q = 0; q < 150; ++q) {
        const math::AABB query = MakeBox(rng, 80.0f, 12.0f);
        std::vector<uint32_t> expected;
        for (size_t i = 0; i < liveBoxes.size(); ++i)
            if (liveBoxes[i].Intersects(query)) expected.push_back(liveIds[i]);
        std::sort(expected.begin(), expected.end());
        const std::vector<uint32_t> actual = QueryToVector(bvh, query);
        CHECK_EQ(actual.size(), expected.size());
        if (actual.size() == expected.size())
            for (size_t i = 0; i < actual.size(); ++i) CHECK_EQ(actual[i], expected[i]);
    }
}

// Frustum queries must match brute force (the renderer's own culling test).
TEST(BvhFrustumQueryMatchesBruteForce) {
    core::Rng rng(99);
    math::Bvh bvh;
    std::vector<math::AABB> boxes;
    for (int i = 0; i < 400; ++i) {
        boxes.push_back(MakeBox(rng, 60.0f, 2.0f));
        bvh.Insert(static_cast<uint32_t>(i), boxes.back());
    }

    const math::Frustum frustum = TestFrustum({0, 5, -30}, {0, 1, 0}, 16.0f / 9.0f);
    const std::vector<uint32_t> expected = BruteFrustum(boxes, frustum);
    const std::vector<uint32_t> actual = FrustumToVector(bvh, frustum);
    CHECK_EQ(actual.size(), expected.size());
    if (actual.size() == expected.size())
        for (size_t i = 0; i < actual.size(); ++i) CHECK_EQ(actual[i], expected[i]);
}

// Ray queries must match brute force.
TEST(BvhRayQueryMatchesBruteForce) {
    core::Rng rng(1234);
    math::Bvh bvh;
    std::vector<math::AABB> boxes;
    for (int i = 0; i < 400; ++i) {
        boxes.push_back(MakeBox(rng, 80.0f, 2.0f));
        bvh.Insert(static_cast<uint32_t>(i), boxes.back());
    }

    for (int q = 0; q < 100; ++q) {
        math::Ray ray;
        ray.origin = rng.OnUnitSphere() * 200.0f;
        ray.dir = rng.OnUnitSphere();
        const float maxDist = rng.Range(20.0f, 150.0f);
        const std::vector<uint32_t> expected = BruteRay(boxes, ray, maxDist);
        const std::vector<uint32_t> actual = RayToVector(bvh, ray, maxDist);
        CHECK_EQ(actual.size(), expected.size());
        if (actual.size() == expected.size())
            for (size_t i = 0; i < actual.size(); ++i) CHECK_EQ(actual[i], expected[i]);
    }
}

// G1-2 acceptance: 10k dynamic entities, frustum culling correctness + a
// timing comparison against brute force (informational; not a hard assert,
// timings are environment-dependent).
TEST(BvhTenThousandCullingBench) {
    core::Rng rng(20260821);
    constexpr int kCount = 10000;
    math::Bvh bvh;
    std::vector<math::AABB> boxes;
    boxes.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        boxes.push_back(MakeBox(rng, 500.0f, 2.0f));
        bvh.Insert(static_cast<uint32_t>(i), boxes.back());
    }
    CHECK_EQ(bvh.Size(), static_cast<size_t>(kCount));

    const math::Frustum frustum = TestFrustum({0, 10, -50}, {0, 0, 0}, 16.0f / 9.0f);
    const std::vector<uint32_t> expected = BruteFrustum(boxes, frustum);
    const std::vector<uint32_t> actual = FrustumToVector(bvh, frustum);
    CHECK_EQ(actual.size(), expected.size());

    // Warm both paths, then measure 200 queries each.
    const auto t0 = std::chrono::steady_clock::now();
    size_t bruteHits = 0;
    for (int q = 0; q < 200; ++q) bruteHits += BruteFrustum(boxes, frustum).size();
    const auto t1 = std::chrono::steady_clock::now();
    size_t bvhHits = 0;
    for (int q = 0; q < 200; ++q) {
        const std::vector<uint32_t> v = FrustumToVector(bvh, frustum);
        bvhHits += v.size();
    }
    const auto t2 = std::chrono::steady_clock::now();
    CHECK_EQ(bruteHits, bvhHits);

    const double bruteMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double bvhMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("  [bench] 10k frustum x200: brute=%.2fms bvh=%.2fms (visible=%zu)\n", bruteMs,
                bvhMs, expected.size());
}

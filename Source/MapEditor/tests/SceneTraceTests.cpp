// ============================================================================
// SceneTrace — the terrain ray march behind cursorTrace (click -> ground).
//
// cScene::Trace (the real implementation, extracted into SceneTrace.h)
// marches the camera ray in 18-bit fixed point. With 32-bit accumulators
// the right half of a 2^14 map broke: coord*2^18 >= 2^32 wraps, the
// ignore_bounds=false march leaves the map immediately (the click is
// swallowed — units keep an old order), the ignore_bounds=true march hits
// at negative X and the shell clamps it to the map edge (terraform marks
// offset from the cursor). 64-bit accumulators fixed it; this test pins
// the behavior at 2^14 coordinates from a game-like camera.
// ============================================================================

#include "TestHarness.h"

// The real engine implementation (extracted from cScene::Trace).
#include "src/SceneTrace.h"

#include <cmath>
#include <cstdio>

namespace {

const int kMapW = 16384; // LARGEWORLD: 2^14
const int kMapH = 16384;
const int kGround = 40;  // flat world ground height (whole voxels)

/// Normalizes d and returns a hit marching from cam along it.
bool march(const float cam[3], const float dIn[3], bool ignoreBounds,
           float& hx, float& hy, float& hz)
{
    float n = std::sqrt(dIn[0] * dIn[0] + dIn[1] * dIn[1] + dIn[2] * dIn[2]);
    const float d[3] = {dIn[0] / n, dIn[1] / n, dIn[2] / n};
    return scene_trace::traceRay(
        cam[0], cam[1], cam[2],
        cam[0] + d[0], cam[1] + d[1], cam[2] + d[2],
        kMapW, kMapH,
        [](int, int) { return kGround; },
        kGround,
        false, ignoreBounds,
        &hx, &hy, &hz);
}

/// Analytic ground-hit along the ray: t where z = ground.
void analyticHit(const float cam[3], const float dIn[3], float& x, float& y)
{
    float n = std::sqrt(dIn[0] * dIn[0] + dIn[1] * dIn[1] + dIn[2] * dIn[2]);
    const float d[3] = {dIn[0] / n, dIn[1] / n, dIn[2] / n};
    const float t = (kGround - cam[2]) / d[2];
    x = cam[0] + d[0] * t;
    y = cam[1] + d[1] * t;
}

bool checkPick(const char* label, float camX, float camY)
{
    // A typical RTS camera: high above the picked spot, looking down.
    const float cam[3] = {camX, camY, 900.0f};
    const float d[3] = {0.15f, -0.1f, -1.0f};

    float hx = 0, hy = 0, hz = 0;
    TEST_REQUIRE(march(cam, d, false, hx, hy, hz),
                 "%s: the trace found no ground (click would be swallowed).",
                 label);
    float ax = 0, ay = 0;
    analyticHit(cam, d, ax, ay);
    TEST_REQUIRE(std::fabs(hx - ax) <= 3.0f && std::fabs(hy - ay) <= 3.0f,
                 "%s: hit (%.1f, %.1f), analytic (%.1f, %.1f) — off by more "
                 "than 3 cells.",
                 label, hx, hy, ax, ay);
    TEST_REQUIRE(hz == kGround, "%s: hit height %g, expected %d.", label, hz,
                 kGround);
    std::printf("[PASS] %s: hit (%.1f, %.1f) vs analytic (%.1f, %.1f).\n",
                label, hx, hy, ax, ay);

    // The region-edit path (ignore_bounds=true) must land at the same spot.
    float gx = 0, gy = 0, gz = 0;
    TEST_REQUIRE(march(cam, d, true, gx, gy, gz),
                 "%s: the ignore_bounds trace found no ground.", label);
    TEST_REQUIRE(std::fabs(gx - ax) <= 3.0f && std::fabs(gy - ay) <= 3.0f,
                 "%s: ignore_bounds hit (%.1f, %.1f) off from (%.1f, %.1f).",
                 label, gx, gy, ax, ay);
    return true;
}

bool runSceneTraceTests()
{
    // Left half (worked even with the old 32-bit accumulators).
    TEST_REQUIRE(checkPick("left-middle", 3000.0f, 8000.0f),
                 "the left-middle pick failed.");
    // Right-middle of a 2^14 map — the old int32 wrap lived here.
    TEST_REQUIRE(checkPick("right-middle", 12000.0f, 8000.0f),
                 "the right-middle pick failed.");
    // Deep right edge.
    TEST_REQUIRE(checkPick("far-right", 15800.0f, 3000.0f),
                 "the far-right pick failed.");

    // Tall terrain near the 8191-voxel cap: the z accumulator used to sit
    // at the int32 boundary.
    {
        const float cam[3] = {9000.0f, 9000.0f, 9500.0f};
        const float d[3] = {0.1f, 0.05f, -1.0f};
        float hx = 0, hy = 0, hz = 0;
        auto tallHeight = [](int, int) { return 8000; };
        float ax = 0, ay = 0;
        {
            float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float t = (8000.0f - cam[2]) / (d[2] / n);
            ax = cam[0] + (d[0] / n) * t;
            ay = cam[1] + (d[1] / n) * t;
        }
        const bool hit = scene_trace::traceRay(
            cam[0], cam[1], cam[2], cam[0] + d[0], cam[1] + d[1], cam[2] + d[2],
            kMapW, kMapH, tallHeight, 8000, false, false,
            &hx, &hy, &hz);
        TEST_REQUIRE(hit, "the tall-terrain trace found no ground.");
        TEST_REQUIRE(std::fabs(hx - ax) <= 3.0f && std::fabs(hy - ay) <= 3.0f,
                     "tall-terrain hit (%.1f, %.1f) off from (%.1f, %.1f).",
                     hx, hy, ax, ay);
        std::printf("[PASS] tall terrain (8000 voxels) picks correctly.\n");
    }
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Scene ray trace at 2^14 (click picking)", 116,
                         runSceneTraceTests);

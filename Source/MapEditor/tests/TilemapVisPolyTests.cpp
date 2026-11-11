// ============================================================================
// TilemapVisPoly — the terrain visibility rasterizer at large tile grids.
//
// fillVisPoly (TileMapVisPoly.h, the REAL engine implementation) rasterizes
// the camera's visibility polygon into the tile grid, one byte per 64x64
// tile. On maps above 8192 per side the grid exceeds the old hardcoded
// 128x128 stack buffers (256x256 tiles at 2^14 — the user's TileMap.cpp:20
// assert and the silent Release crash; 512x512 at 2^15). The buffers are
// dynamic now; this test pins the behavior at both scales:
//   * canary rows around the buffer stay untouched (no out-of-bounds
//     writes — the Release crash mode);
//   * completeness: cells well inside the polygon are filled;
//   * soundness: no filled cell's center lies outside the polygon by more
//     than a one-cell tolerance (scanline boundary semantics).
// ============================================================================

#include "TestHarness.h"

// The real engine implementation (extracted from TileMap.cpp).
#include "tilemap/TileMapVisPoly.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

/// Signed distance-ish point-in-polygon test (ray cast), good for any simple
/// polygon; boundary points count as inside.
bool pointInPolygon(const std::vector<Vect2f>& poly, float x, float y)
{
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const float xi = poly[i].x, yi = poly[i].y;
        const float xj = poly[j].x, yj = poly[j].y;
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

/// Runs one polygon on a W x H tile grid with canary rows above and below.
bool checkGrid(int W, int H, const std::vector<Vect2f>& poly,
               const char* label)
{
    // Guard bands before and after the exact-size buffer (the engine writes
    // with a W stride, so side padding is impossible — but the row range is
    // exactly what the old Release crash violated via the stack arrays).
    const std::size_t guard = 64 * 1024;
    std::vector<std::uint8_t> storage(guard * 2 +
                                      static_cast<std::size_t>(W) * H);
    std::fill(storage.begin(), storage.end(), static_cast<std::uint8_t>(0xCC));
    std::uint8_t* buf = storage.data() + guard;

    fillVisPoly(buf, poly, W, H);

    // 1. Guard bands must stay 0xCC — no writes outside the W x H buffer.
    for (std::size_t i = 0; i < guard; ++i) {
        TEST_REQUIRE(storage[i] == 0xCC,
                     "%s: write before the %dx%d buffer (offset -%zu).",
                     label, W, H, guard - i);
        TEST_REQUIRE(storage[guard + static_cast<std::size_t>(W) * H + i] ==
                         0xCC,
                     "%s: write past the %dx%d buffer (offset +%zu).",
                     label, W, H, i);
    }

    // 2. Completeness: a cell whose corners (center +/- 0.5 on both axes)
    //    are all inside the polygon is strictly interior — the scanline
    //    fill must cover it regardless of boundary semantics.
    int completeChecked = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            bool deep = true;
            for (float dy = -0.5f; dy <= 0.5f && deep; dy += 1.0f) {
                for (float dx = -0.5f; dx <= 0.5f && deep; dx += 1.0f) {
                    if (!pointInPolygon(poly, x + 0.5f + dx, y + 0.5f + dy)) {
                        deep = false;
                    }
                }
            }
            if (deep) {
                ++completeChecked;
                TEST_REQUIRE(buf[y * W + x] == 1,
                             "%s: strictly-inside cell (%d,%d) is not filled.",
                             label, x, y);
            }
        }
    }
    TEST_REQUIRE(completeChecked > 0, "%s: the polygon interior covered no cells.",
                 label);

    // 3. Soundness: no filled cell center more than one cell outside.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (buf[y * W + x] != 1) continue;
            if (pointInPolygon(poly, x + 0.5f, y + 0.5f)) continue;
            bool nearBoundary = false;
            for (float dx = -1.0f; dx <= 1.0f && !nearBoundary; dx += 1.0f) {
                for (float dy = -1.0f; dy <= 1.0f && !nearBoundary; dy += 1.0f) {
                    if (pointInPolygon(poly, x + 0.5f + dx, y + 0.5f + dy)) {
                        nearBoundary = true;
                    }
                }
            }
            TEST_REQUIRE(nearBoundary,
                         "%s: filled cell (%d,%d) is far outside the polygon.",
                         label, x, y);
        }
    }
    return true;
}

bool runTilemapVisPolyTests()
{
    // 2^15: a 32768-cell map is 512 tiles per side; the visibility polygon
    // is the camera frustum over the tile grid (a wide quad in tile units).
    {
        const int W = 512, H = 512;
        const std::vector<Vect2f> quad = {
            {20, 20}, {500, 8}, {511, 480}, {10, 500}};
        TEST_REQUIRE(checkGrid(W, H, quad, "2^15 quad"),
                     "the 2^15 quad failed.");
        // A polygon spanning the whole grid (top-down camera at map centre).
        const std::vector<Vect2f> full = {
            {0, 0}, {512, 0}, {512, 512}, {0, 512}};
        TEST_REQUIRE(checkGrid(W, H, full, "2^15 full"),
                     "the full-grid polygon at 2^15 failed.");
        std::printf("[PASS] fillVisPoly at the 2^15 tile grid (512x512).\n");
    }
    // 2^14: 256 tiles per side — the exact grid that asserted/crashed before.
    {
        const int W = 256, H = 256;
        const std::vector<Vect2f> quad = {
            {5, 10}, {250, 5}, {240, 250}, {8, 240}};
        TEST_REQUIRE(checkGrid(W, H, quad, "2^14 quad"),
                     "the 2^14 quad failed.");
        std::printf("[PASS] fillVisPoly at the 2^14 tile grid (256x256).\n");
    }
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Tilemap visMap rasterizer (2^14/2^15 grids)", 115,
                         runTilemapVisPolyTests);

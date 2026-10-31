// ============================================================================
// T3.1 — Brush mask geometry (BrushMasks).
//
// The ring rounding must match Terra's radius tables exactly (the brush
// stamps have to cover the same cells the game's circle tools affect), so
// the tests cross-check the analytic masks against maxRad[]/xRad[]/yRad[]
// built by vrtMap::landPrepare(). Square masks, bbox sizes and the
// Bresenham line union are checked shape-side.
//
// Runs after the world round-trip (order 20), so landPrepare() has run and
// the radius tables are available.
// ============================================================================

#include "TestHarness.h"

#include "BrushTool.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using MapEditor::BrushMask;
using MapEditor::BrushMasks;

/// Convert a mask into a set of cell-offset pairs relative to its bbox.
std::set<std::pair<int, int>> maskCells(const BrushMask& mask)
{
    std::set<std::pair<int, int>> cells;
    for (int dy = 0; dy < mask.sy; ++dy) {
        for (int dx = 0; dx < mask.sx; ++dx) {
            if (mask.influence[static_cast<std::size_t>(dy) * mask.sx + dx]) {
                cells.insert({dx + mask.x, dy + mask.y});
            }
        }
    }
    return cells;
}

/// Same, but the offsets are relative to a given stamp centre (for masks
/// built at an arbitrary absolute position, e.g. the line test).
std::set<std::pair<int, int>>
maskCellsAround(const BrushMask& mask, int centreX, int centreY)
{
    std::set<std::pair<int, int>> cells;
    for (int dy = 0; dy < mask.sy; ++dy) {
        for (int dx = 0; dx < mask.sx; ++dx) {
            if (mask.influence[static_cast<std::size_t>(dy) * mask.sx + dx]) {
                cells.insert({dx + mask.x - centreX, dy + mask.y - centreY});
            }
        }
    }
    return cells;
}

bool runBrushMaskTests()
{
    // ------------------------------------------------------------------
    // 1.1: the bbox of the symmetric shapes is 2r+1 around the centre.
    // ------------------------------------------------------------------
    for (int radius : {0, 1, 8, 64}) {
        const BrushMask circle = BrushMasks::filledCircle(radius);
        const BrushMask outline = BrushMasks::circleOutline(radius);
        const BrushMask square = BrushMasks::filledSquare(radius);
        const BrushMask squareOutline = BrushMasks::squareOutline(radius);
        TEST_REQUIRE(circle.sx == 2 * radius + 1 && circle.sy == 2 * radius + 1 &&
                     circle.x == -radius && circle.y == -radius,
                     "filledCircle bbox is not 2r+1 centred (radius %d).", radius);
        TEST_REQUIRE(outline.sx == circle.sx && outline.x == circle.x,
                     "circleOutline bbox does not match the filled one.");
        TEST_REQUIRE(square.sx == 2 * radius + 1 && square.x == -radius,
                     "filledSquare bbox is not 2r+1 centred (radius %d).", radius);
        TEST_REQUIRE(squareOutline.sx == square.sx, "squareOutline bbox mismatch.");
    }
    std::printf("[PASS] symmetric masks are 2r+1 and centred on the stamp.\n");

    // ------------------------------------------------------------------
    // 1.2: circle masks match Terra's radius tables. landPrepare() has run
    // in earlier tests, so the tables exist; ring r of the tables holds
    // exactly the CircleOutline offsets (ring membership = truncated sqrt).
    // ------------------------------------------------------------------
    TEST_REQUIRE(xRad[0] != nullptr,
                 "the Terra radius tables were not built by any earlier test.");
    for (int radius : {0, 1, 2, 7, 33, 210}) {
        // full disc: rings 0..radius, the same set influenceDZ stamps with.
        std::size_t tableTotal = 0;
        std::set<std::pair<int, int>> tableDisc;
        for (int ring = 0; ring <= radius; ++ring) {
            const int* ringX = xRad[ring];
            const int* ringY = yRad[ring];
            for (int k = 0; k < maxRad[ring]; ++k) {
                tableDisc.insert({ringX[k], ringY[k]});
            }
            tableTotal += static_cast<std::size_t>(maxRad[ring]);
        }
        TEST_REQUIRE(tableDisc.size() == tableTotal,
                     "Terra's rings overlap; cross-check would be meaningless.");

        const std::set<std::pair<int, int>> filled = maskCells(BrushMasks::filledCircle(radius));
        // Multi-set balance: table counts may repeat the same ring offsets at
        // most once per table row, so compare membership directions both ways.
        TEST_REQUIRE(tableDisc == filled,
                     "filledCircle(radius=%d) differs from Terra's rings 0..%d.",
                     radius, radius);

        const std::set<std::pair<int, int>> outline =
            maskCells(BrushMasks::circleOutline(radius));
        const int* ringX = xRad[radius];
        const int* ringY = yRad[radius];
        std::set<std::pair<int, int>> tableRing;
        for (int k = 0; k < maxRad[radius]; ++k) {
            tableRing.insert({ringX[k], ringY[k]});
        }
        TEST_REQUIRE(outline == tableRing,
                     "circleOutline(radius=%d) differs from Terra ring %d.",
                     radius, radius);
    }
    std::printf("[PASS] circle masks coincide with Terra's radius tables.\n");

    // ------------------------------------------------------------------
    // 1.2b: radii beyond Terra's tables. The editor cap is
    // kMaxBrushRadius = 512, but the tables end at MAX_RADIUS_CIRCLEARR,
    // so the membership is checked against the analytic ring rule
    // (truncated sqrt) the tables themselves are built from.
    // ------------------------------------------------------------------
    {
        constexpr int big = MapEditor::kMaxBrushRadius;
        static_assert(big > MAX_RADIUS_CIRCLEARR,
                      "the editor cap must exceed Terra's table length.");
        const std::set<std::pair<int, int>> filled = maskCells(BrushMasks::filledCircle(big));
        const std::set<std::pair<int, int>> outline =
            maskCells(BrushMasks::circleOutline(big));

        TEST_REQUIRE(filled.size() > outline.size(),
                     "filledCircle(512) must be larger than its rim.");
        // Disc membership: the truncated-sqrt ring decides, exactly like
        // the table construction in vrtMap::landPrepare().
        const auto inDisc = [&filled](int x, int y) {
            return filled.count({x, y}) != 0;
        };
        TEST_REQUIRE(inDisc(0, 0) && inDisc(big, 0) && inDisc(-big, 0) &&
                         inDisc(0, big) && inDisc(0, -big),
                     "filledCircle(512) misses its axis tips.");
        TEST_REQUIRE(inDisc(362, 362), // ring 511 (sqrt(2)*362 truncates to 511)
                     "filledCircle(512) misses an interior ring-511 cell.");
        TEST_REQUIRE(inDisc(508, 63),  // ring 511
                     "filledCircle(512) misses an interior ring-511 cell.");
        TEST_REQUIRE(!inDisc(480, 197), // ring 518
                     "filledCircle(512) takes a cell beyond the radius.");
        // Rim: ring exactly 512 — the outline of thickness 1.
        const auto inRim = [&outline](int x, int y) {
            return outline.count({x, y}) != 0;
        };
        TEST_REQUIRE(inRim(big, 0) && inRim(0, -big),
                     "circleOutline(512) misses its ring-512 axis cells.");
        TEST_REQUIRE(inRim(508, 64), // 508^2 + 64^2 = 262160, ring 512
                     "circleOutline(512) misses a ring-512 cell.");
        TEST_REQUIRE(!inRim(0, 0) && !inRim(508, 63),
                     "circleOutline(512) takes cells below its ring.");
    }
    std::printf("[PASS] radius-512 masks follow the analytic ring rule.\n");

    // ------------------------------------------------------------------
    // 1.3: square masks cover the full area / only the border.
    // ------------------------------------------------------------------
    const int r = 3;
    const std::set<std::pair<int, int>> filled = maskCells(BrushMasks::filledSquare(r));
    TEST_REQUIRE(filled.size() == static_cast<std::size_t>(2 * r + 1) * (2 * r + 1),
                 "filledSquare must cover the whole (2r+1)^2 area.");
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            TEST_REQUIRE(filled.count({x, y}),
                         "filledSquare misses cell (%d,%d).", x, y);
        }
    }
    const std::set<std::pair<int, int>> border = maskCells(BrushMasks::squareOutline(r));
    TEST_REQUIRE(border.size() == static_cast<std::size_t>(8) * r, // 4 sides of (2r+1) minus the four doubled corners
                 "squareOutline holds exactly 8r border cells, got %zu.",
                 border.size());
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            const bool isBorder =
                std::max(std::abs(x), std::abs(y)) == r;
            TEST_REQUIRE(border.count({x, y}) == (isBorder ? 1u : 0u),
                         "squareOutline mismatch at (%d,%d).", x, y);
        }
    }
    std::printf("[PASS] square masks cover the area / the border exactly.\n");

    // ------------------------------------------------------------------
    // 1.4: the line stamp includes both endpoints, keeps the stamp width,
    // is connected and counts every cell once (no duplicated influence).
    // ------------------------------------------------------------------
    {
        const int stampRadius = 2;
        const BrushMask line = BrushMasks::lineStamp(-10, -4, 40, 17, stampRadius);
        TEST_REQUIRE(!line.empty(), "lineStamp produced an empty mask.");
        TEST_REQUIRE(line.x <= -10 - stampRadius && line.y <= -4 - stampRadius,
                     "line bbox does not cover the start stamp.");
        TEST_REQUIRE(line.x + line.sx >
                         40 + stampRadius && line.y + line.sy > 17 + stampRadius,
                     "line bbox does not cover the end stamp.");

        // Every stamped cell holds exactly 255 — no overlaps accumulate.
        const std::set<std::pair<int, int>> cells = maskCells(line);
        const std::set<std::pair<int, int>> startStamp =
            maskCellsAround(line, -10, -4);
        const std::set<std::pair<int, int>> endStamp =
            maskCellsAround(line, 40, 17);
        TEST_REQUIRE(startStamp.count({0, 0}) && endStamp.count({0, 0}),
                     "the line endpoints must be inside the stamp.");

        // Connectivity: every mask cell has at least one other mask cell
        // within the stamp radius (no skipped steps along the path).
        for (const auto& [cx, cy] : cells) {
            bool connected = false;
            for (int j = -stampRadius; j <= stampRadius && !connected; ++j) {
                for (int i = -stampRadius; i <= stampRadius; ++i) {
                    if ((i != 0 || j != 0) && cells.count({cx + i, cy + j})) {
                        connected = true;
                        break;
                    }
                }
            }
            TEST_REQUIRE(connected,
                         "line is not connected around (%d,%d).", cx, cy);
        }

        // Degenerate line = a single stamp equivalent to filledCircle(r),
        // compared in coordinates around the line centre (5,5).
        const std::set<std::pair<int, int>> dot = maskCellsAround(
            BrushMasks::lineStamp(5, 5, 5, 5, stampRadius), 5, 5);
        const std::set<std::pair<int, int>> stamp =
            maskCells(BrushMasks::filledCircle(stampRadius));
        if (dot != stamp) {
            for (const auto& c : dot) {
                if (!stamp.count(c)) {
                    std::printf("  dot-only cell: (%d,%d)\n", c.first, c.second);
                }
            }
            for (const auto& c : stamp) {
                if (!dot.count(c)) {
                    std::printf("  stamp-only cell: (%d,%d)\n", c.first, c.second);
                }
            }
        }
        TEST_REQUIRE(dot == stamp,
                     "a zero-length line must degenerate to a single stamp "
                     "(dot=%zu stamp=%zu).",
                     dot.size(), stamp.size());
    }
    std::printf("[PASS] line stamps are endpoint-covered, connected and "
                "overlap-free.\n");

    // ------------------------------------------------------------------
    // 1.5: byShape() routing and the Line guard.
    // ------------------------------------------------------------------
    TEST_REQUIRE(maskCells(BrushMasks::byShape(MapEditor::BrushShape::FilledSquare, 2)).size()
                     == 25,
                 "byShape must route FilledSquare.");
    TEST_REQUIRE(BrushMasks::byShape(MapEditor::BrushShape::Line, 4).empty(),
                 "Line has no endpoints — byShape must produce an empty mask.");
    std::printf("[PASS] byShape() routes the shapes, Line stays endpoint-bound.\n");

    // ------------------------------------------------------------------
    // 1.6: regular-polygon geometry. Vertices sit on the radius circle at
    // angleOffset + i*(360/sides) (0 = +x, growing towards +y); the filled
    // mask is the even-odd fill, the outline is the inset boundary band.
    // ------------------------------------------------------------------
    {
        // Vertex math: a radius-32 square polygon at offset 0 has its
        // vertices on the axes; at 45 degrees they sit on the diagonals.
        const auto v0 = BrushMasks::regularPolygonVertices(32, 4, 0.0);
        TEST_REQUIRE(v0.size() == 4, "a 4-sided polygon must have 4 vertices.");
        TEST_REQUIRE(std::lround(v0[0].first) == 32 && std::lround(v0[0].second) == 0 &&
                         std::lround(v0[1].first) == 0 && std::lround(v0[1].second) == 32 &&
                         std::lround(v0[2].first) == -32 && std::lround(v0[2].second) == 0 &&
                         std::lround(v0[3].first) == 0 && std::lround(v0[3].second) == -32,
                     "square-polygon vertices must sit on the axes at offset 0.");
        const auto v45 = BrushMasks::regularPolygonVertices(32, 4, 45.0);
        TEST_REQUIRE(std::lround(v45[0].first) == 23 && std::lround(v45[0].second) == 23,
                     "45° offset must rotate the first vertex to the diagonal.");
        // Sides clamp: 2 or 100 both produce a valid polygon.
        TEST_REQUIRE(BrushMasks::regularPolygonVertices(8, 2, 0.0).size() == 3,
                     "sides below 3 must clamp to 3.");
        TEST_REQUIRE(BrushMasks::regularPolygonVertices(8, 100, 0.0).size() ==
                         static_cast<std::size_t>(MapEditor::kMaxPolygonSides),
                     "sides above 20 must clamp to 20.");

        // Filled diamond (4 sides, offset 0, radius 32): axis tips in, the
        // corners of the bbox out, interior/exterior by the L1 norm.
        const BrushMask diamond = BrushMasks::filledPolygon(32, 4, 0.0);
        TEST_REQUIRE(diamond.sx == 65 && diamond.x == -32,
                     "polygon masks keep the (2r+1)^2 centred bbox.");
        const std::set<std::pair<int, int>> diamondCells = maskCells(diamond);
        const auto has = [&diamondCells](int x, int y) {
            return diamondCells.count({x, y}) != 0;
        };
        TEST_REQUIRE(has(0, 0) && has(32, 0) && has(0, 32) && has(-32, 0) &&
                         has(0, -32),
                     "the filled diamond misses its centre or axis tips.");
        TEST_REQUIRE(has(15, 15) && !has(17, 17),
                     "diamond interior/exterior cells mismatch the L1 norm.");
        TEST_REQUIRE(!has(32, 32) && !has(32, -32),
                     "the filled diamond takes bbox corners outside the polygon.");

        // Rotation: a 45° offset turns the diamond into an axis-aligned
        // square on the diagonals (vertex rounding keeps both fills valid).
        const std::set<std::pair<int, int>> rotated =
            maskCells(BrushMasks::filledPolygon(32, 4, 45.0));
        TEST_REQUIRE(rotated.count({23, 23}) && rotated.count({-23, 23}) &&
                         rotated.count({23, -23}),
                     "the 45°-rotated square misses its diagonal tips.");
        TEST_REQUIRE(rotated.count({23, 0}) && !rotated.count({24, 24}),
                     "the rotated square fill/boundary cells mismatch.");
        TEST_REQUIRE(!rotated.count({32, 0}),
                     "the rotated square must not reach the old axis tip.");

        // Outline: the one-cell band hugs the boundary (axis tips in, centre
        // out), thicker bands grow inward and never leave the footprint.
        const std::set<std::pair<int, int>> rim = maskCells(
            BrushMasks::polygonOutline(32, 4, 0.0, 1));
        TEST_REQUIRE(rim.count({32, 0}) && rim.count({0, 32}) && !rim.count({0, 0}),
                     "the one-cell polygon rim misses the boundary or takes the centre.");
        TEST_REQUIRE(std::all_of(rim.begin(), rim.end(),
                                 [](const std::pair<int, int>& c) {
                                     return std::abs(c.first) <= 32 &&
                                            std::abs(c.second) <= 32;
                                 }),
                     "the polygon rim leaves the radius footprint.");
        const std::set<std::pair<int, int>> wide = maskCells(
            BrushMasks::polygonOutline(32, 4, 0.0, 6));
        TEST_REQUIRE(wide.size() > rim.size(),
                     "a thicker polygon band must cover more cells.");
        TEST_REQUIRE(!wide.count({0, 0}),
                     "thickness 6 must not reach the diamond centre (sd = 32/sqrt(2) ~= 22.6).");
        TEST_REQUIRE(wide.count({28, 0}),
                     "thickness 6 must include the sd = 4*sqrt(2) ~= 5.7 cell (28,0).");
        // Inset rule: the band of every thickness stays inside the fill.
        TEST_REQUIRE(std::all_of(wide.begin(), wide.end(),
                                 [&diamondCells](const std::pair<int, int>& c) {
                                     // The fringe straddles the ideal edge by
                                     // up to one cell — allow the boundary
                                     // ring but never a full cell beyond it.
                                     return diamondCells.count(c) ||
                                            diamondCells.count({c.first - 1, c.second}) ||
                                            diamondCells.count({c.first + 1, c.second}) ||
                                            diamondCells.count({c.first, c.second - 1}) ||
                                            diamondCells.count({c.first, c.second + 1});
                                 }),
                     "the polygon band must stay within one cell of the fill.");

        // byShape routes the polygon shapes with their params.
        TEST_REQUIRE(maskCells(BrushMasks::byShape(
                         MapEditor::BrushShape::FilledPolygon, 8, 1, 5, 10.0))
                         .size() > 0,
                     "byShape must route FilledPolygon.");
        TEST_REQUIRE(maskCells(BrushMasks::byShape(
                         MapEditor::BrushShape::PolygonOutline, 8, 3, 5, 10.0))
                         .size() > 0,
                     "byShape must route PolygonOutline.");
    }
    std::printf("[PASS] regular-polygon geometry: vertices, fill, inset band.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush masks (T3.1)", 56, runBrushMaskTests);

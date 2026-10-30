// ============================================================================
// Brush thickness — the stroke width of the UNFILLED shapes.
//
// BrushParams::thickness makes the circle/square outline a band of N cells
// instead of the original one-cell ring, and the Line shape a stroke of N
// cells instead of the full-radius stamp. The band is always INSET: the outer
// edge stays at the shape's radius, so a stamp can never edit outside the
// footprint the shape has always had. thickness 0 keeps the pre-thickness
// behaviour (one-cell ring/border, full-stamp line) — the default.
//
// Checked here (Qt-free, no world needed for the geometry, one loaded world
// for the application):
//   1. outline bands are concentric and inset, and thickness 0/1 reproduce the
//      original one-cell ring exactly (backward compatibility);
//   2. every band cell lies inside the filled shape of the same radius, so no
//      stamp reaches outside the footprint;
//   3. the Line stroke radius is (thickness-1)/2 clamped to the brush radius,
//      0 = the full stamp;
//   4. clampThickness/maxThickness hold for wild values;
//   5. a stamp with a band edits exactly the band cells of the mask.
//
// Runs after the world round-trip (its temporary world copy is reused).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMask;
using MapEditor::BrushMasks;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushShape;
using MapEditor::BrushTool;

/// Mask cells as (dx,dy) offsets relative to the stamp centre.
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

/// Mask cells as (dx,dy) offsets relative to a given stamp centre (for
/// absolute masks like lineStamp built at an arbitrary position).
std::set<std::pair<int, int>> maskCellsAround(const BrushMask& mask,
                                              int centreX, int centreY)
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

/// Load a private copy of the temporary test world into its own content root.
bool prepareBrushTestSession(const char* subdirName,
                             MapEditor::EditorSession& session)
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    if (ctx.tempRoot.empty() || !fs::exists(ctx.tempRoot) ||
        ctx.worldName.empty()) {
        return false;
    }
    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / subdirName;
    std::error_code ec;
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(
        ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
    fs::copy_file(ctx.gameDir / "Perimeter.ini",
                  loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(
        ctx.tempWorldDir,
        loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName));

    if (!session.setGameDir(loadRoot.u8string())) {
        return false;
    }
    if (!session.scanWorlds() || session.worldCount() < 1) {
        return false;
    }
    return session.loadWorld(0);
}

// ---------------------------------------------------------------------------
// Test 87: the band geometry (pure, no world)
// ---------------------------------------------------------------------------

bool runBrushThicknessGeometryTests()
{
    // ------------------------------------------------------------------
    // 1: thickness 0 and 1 reproduce the original one-cell ring exactly.
    // ------------------------------------------------------------------
    for (int radius : {1, 4, 12, 33}) {
        const std::set<std::pair<int, int>> auto0 =
            maskCells(BrushMasks::circleOutline(radius, 0));
        const std::set<std::pair<int, int>> auto1 =
            maskCells(BrushMasks::circleOutline(radius, 1));
        TEST_REQUIRE(auto0 == auto1,
                     "circleOutline(radius=%d): thickness 0 and 1 must be the "
                     "same one-cell ring.",
                     radius);

        const std::set<std::pair<int, int>> square0 =
            maskCells(BrushMasks::squareOutline(radius, 0));
        const std::set<std::pair<int, int>> square1 =
            maskCells(BrushMasks::squareOutline(radius, 1));
        TEST_REQUIRE(square0 == square1,
                     "squareOutline(radius=%d): thickness 0 and 1 must be the "
                     "same one-cell border.",
                     radius);

        // The one-cell border of a square is exactly 8*radius cells.
        TEST_REQUIRE(square1.size() == static_cast<std::size_t>(8 * radius),
                     "squareOutline(radius=%d, thickness=1): %zu cells, "
                     "expected %d.",
                     radius, square1.size(), 8 * radius);
    }
    std::printf("[PASS] thickness 0/1 keep the original one-cell ring.\n");

    // ------------------------------------------------------------------
    // 2: a thicker band is concentric, inset and strictly growing.
    // ------------------------------------------------------------------
    {
        const int radius = 10;
        std::set<std::pair<int, int>> previous;
        for (int thickness : {1, 3, 5, 8}) {
            const std::set<std::pair<int, int>> band =
                maskCells(BrushMasks::circleOutline(radius, thickness));
            TEST_REQUIRE(!band.empty(),
                         "circleOutline band (radius=%d, thickness=%d) is empty.",
                         radius, thickness);

            // Every band cell lies within the disc: ring <= radius (the
            // footprint is never exceeded).
            for (const auto& [dx, dy] : band) {
                const int ring = BrushMasks::circularRing(dx, dy);
                TEST_REQUIRE(ring <= radius,
                             "circle band cell (%d,%d) has ring %d > radius %d.",
                             dx, dy, ring, radius);
            }

            // The band's inner boundary: the smallest ring present is
            // radius-thickness+1 (clamped at 0).
            int minRing = radius;
            for (const auto& [dx, dy] : band) {
                minRing = std::min(minRing, BrushMasks::circularRing(dx, dy));
            }
            const int expectedInner = std::max(0, radius - thickness + 1);
            TEST_REQUIRE(minRing == expectedInner,
                         "circle band (radius=%d, thickness=%d): inner ring %d, "
                         "expected %d.",
                         radius, thickness, minRing, expectedInner);

            // Monotone growth: a thicker band strictly contains the thinner.
            if (!previous.empty()) {
                for (const auto& c : previous) {
                    TEST_REQUIRE(band.count(c),
                                 "thickness %d lost the cell (%d,%d) of the "
                                 "thinner band.",
                                 thickness, c.first, c.second);
                }
                TEST_REQUIRE(band.size() > previous.size(),
                             "thickness %d did not grow the band (%zu cells).",
                             thickness, band.size());
            }
            previous = band;
        }

        // Square: the same inset property on the Chebyshev edge.
        for (int thickness : {2, 4, 9}) {
            const std::set<std::pair<int, int>> band =
                maskCells(BrushMasks::squareOutline(radius, thickness));
            const int inner = std::max(0, radius - thickness + 1);
            for (const auto& [dx, dy] : band) {
                const int edge = std::max(std::abs(dx), std::abs(dy));
                TEST_REQUIRE(edge <= radius && edge >= inner,
                             "square band cell (%d,%d): edge %d outside "
                             "[%d,%d].",
                             dx, dy, edge, inner, radius);
            }
        }
        std::printf("[PASS] a thicker band is concentric, inset and growing.\n");
    }

    // ------------------------------------------------------------------
    // 3: a band as thick as the footprint covers the whole filled shape.
    // ------------------------------------------------------------------
    {
        const int radius = 7;
        TEST_REQUIRE(maskCells(BrushMasks::circleOutline(radius, 2 * radius + 1)) ==
                         maskCells(BrushMasks::filledCircle(radius)),
                     "a full-extent circle band must equal the filled disc.");
        TEST_REQUIRE(maskCells(BrushMasks::squareOutline(radius, 2 * radius + 1)) ==
                         maskCells(BrushMasks::filledSquare(radius)),
                     "a full-extent square band must equal the filled square.");
    }

    // ------------------------------------------------------------------
    // 4: the Line stroke radius — inset into the brush footprint.
    // ------------------------------------------------------------------
    {
        TEST_REQUIRE(BrushTool::lineStrokeRadius(8, 0) == 8,
                     "thickness 0 must keep the full radius-8 line stamp.");
        TEST_REQUIRE(BrushTool::lineStrokeRadius(8, 1) == 0,
                     "thickness 1 must be the one-cell Bresenham path.");
        TEST_REQUIRE(BrushTool::lineStrokeRadius(8, 3) == 1,
                     "thickness 3 must stamp a radius-1 disc.");
        TEST_REQUIRE(BrushTool::lineStrokeRadius(8, 5) == 2,
                     "thickness 5 must stamp a radius-2 disc.");
        // Never exceeds the brush radius.
        TEST_REQUIRE(BrushTool::lineStrokeRadius(2, 1000) == 2,
                     "the line stroke must be clamped to the brush radius.");
        TEST_REQUIRE(BrushTool::lineStrokeRadius(0, 7) == 0,
                     "a radius-0 brush must never grow the stroke.");

        // The line mask of thickness 1 is exactly the Bresenham path (a
        // radius-0 stamp), and it lies inside the thickness-0 mask.
        const std::set<std::pair<int, int>> thin =
            maskCells(BrushMasks::lineStamp(-9, 4, 13, 4,
                                            BrushTool::lineStrokeRadius(4, 1)));
        const std::set<std::pair<int, int>> wide =
            maskCells(BrushMasks::lineStamp(-9, 4, 13, 4,
                                            BrushTool::lineStrokeRadius(4, 0)));
        TEST_REQUIRE(thin.size() == 23,
                     "a one-cell horizontal line from -9 to 13 holds 23 cells, "
                     "got %zu.",
                     thin.size());
        for (const auto& c : thin) {
            TEST_REQUIRE(wide.count(c),
                         "the thin line lost cell (%d,%d) of the wide stamp.",
                         c.first, c.second);
        }
    }
    std::printf("[PASS] the Line stroke radius is inset into the footprint.\n");

    // ------------------------------------------------------------------
    // 5: clamps hold for wild values.
    // ------------------------------------------------------------------
    {
        TEST_REQUIRE(BrushTool::maxThickness(0) == 1,
                     "a radius-0 stamp has a 1-cell extent.");
        TEST_REQUIRE(BrushTool::maxThickness(5) == 11,
                     "maxThickness(5) must be 2*5+1.");
        TEST_REQUIRE(BrushTool::clampThickness(-100, 5) == 0,
                     "a negative thickness must clamp to 0 (auto).");
        TEST_REQUIRE(BrushTool::clampThickness(10000, 5) == 11,
                     "a wild thickness must clamp to the stamp extent.");
        TEST_REQUIRE(BrushTool::clampThickness(3, 5) == 3,
                     "a valid thickness must pass through.");

        // byShape() honours the parameter for the unfilled shapes and ignores
        // it for the filled ones.
        TEST_REQUIRE(BrushMasks::byShape(BrushShape::CircleOutline, 6, 3).sx ==
                         BrushMasks::filledCircle(6).sx,
                     "byShape must keep the bbox of the outline shape.");
        TEST_REQUIRE(maskCells(BrushMasks::byShape(BrushShape::FilledCircle, 6, 3)) ==
                         maskCells(BrushMasks::filledCircle(6)),
                     "thickness must not change a filled shape.");
        TEST_REQUIRE(maskCells(BrushMasks::byShape(BrushShape::SquareOutline, 6, 1)) ==
                         maskCells(BrushMasks::squareOutline(6)),
                     "byShape must route the square outline.");

        TEST_REQUIRE(BrushMasks::isUnfilledShape(BrushShape::CircleOutline) &&
                         BrushMasks::isUnfilledShape(BrushShape::SquareOutline) &&
                         BrushMasks::isUnfilledShape(BrushShape::Line),
                     "the three outline shapes must report unfilled.");
        TEST_REQUIRE(!BrushMasks::isUnfilledShape(BrushShape::FilledCircle) &&
                         !BrushMasks::isUnfilledShape(BrushShape::FilledSquare),
                     "the filled shapes must not report unfilled.");
    }
    std::printf("[PASS] thickness clamps and byShape() routing hold.\n");

    return true;
}

MAP_EDITOR_REGISTER_TEST("Brush thickness geometry", 87,
                         runBrushThicknessGeometryTests);

// ---------------------------------------------------------------------------
// Test 88: a banded stamp edits exactly the band cells
// ---------------------------------------------------------------------------

bool runBrushThicknessApplyTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_thickness", session);
    TEST_REQUIRE(loaded,
                 "the thickness test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the thickness test.");

    // A flat-enough area with headroom for a raise of one stamp.
    const int radius = 12;
    const int margin = radius + 6;
    int cx = 0;
    int cy = 0;
    bool found = false;
    for (int gy = margin; gy < mapH - margin && !found; gy += 24) {
        for (int gx = margin; gx < mapW - margin && !found; gx += 24) {
            int localMax = 0;
            for (int dy = -margin; dy <= margin && found == false; ++dy) {
                for (int dx = -margin; dx <= margin; ++dx) {
                    localMax = std::max(
                        localMax, static_cast<int>(vMap.SGetAlt(gx + dx, gy + dy)));
                }
            }
            if (localMax + 2048 <= MAX_VX_HEIGHT) {
                cx = gx;
                cy = gy;
                found = true;
            }
        }
    }
    TEST_REQUIRE(found, "no suitable stamp area for the thickness test.");

    auto readHeight = [](int x, int y) {
        return static_cast<int>(vMap.SGetAlt(x, y));
    };

    BrushTool brush(session);
    BrushParams params;
    params.shape = BrushShape::CircleOutline;
    params.radius = radius;
    params.effect = BrushEffect::ChangeHeight;
    params.mode = BrushMode::Geo;
    params.direction = 1;
    params.strength = 4;   // delta = 4 voxels at full influence
    const int delta = BrushTool::changeHeightDelta(params, 255);

    // ------------------------------------------------------------------
    // 1: a radius-12 outline with thickness 4 raises exactly the band and
    //    leaves the inside of the disc untouched.
    // ------------------------------------------------------------------
    {
        params.thickness = 4;
        const BrushMask band = BrushMasks::circleOutline(radius, params.thickness);
        const std::set<std::pair<int, int>> bandCells = maskCells(band);

        std::vector<int> before(static_cast<std::size_t>(2 * margin + 1) *
                                (2 * margin + 1));
        for (int dy = -margin; dy <= margin; ++dy) {
            for (int dx = -margin; dx <= margin; ++dx) {
                before[static_cast<std::size_t>(dy + margin) *
                           (2 * margin + 1) + (dx + margin)] =
                    readHeight(cx + dx, cy + dy);
            }
        }

        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the banded outline stamp was refused.");

        int raised = 0;
        for (int dy = -margin; dy <= margin; ++dy) {
            for (int dx = -margin; dx <= margin; ++dx) {
                const int oldH = before[static_cast<std::size_t>(dy + margin) *
                                            (2 * margin + 1) + (dx + margin)];
                const int newH = readHeight(cx + dx, cy + dy);
                const bool inBand = bandCells.count({dx, dy}) != 0;
                if (inBand) {
                    ++raised;
                    TEST_REQUIRE(newH == oldH + delta,
                                 "band cell (%d,%d): %d -> %d, expected +%d.",
                                 dx, dy, oldH, newH, delta);
                } else {
                    TEST_REQUIRE(newH == oldH,
                                 "cell (%d,%d) outside the band changed: "
                                 "%d -> %d.",
                                 dx, dy, oldH, newH);
                }
            }
        }
        TEST_REQUIRE(raised > 0, "the band covered no cells.");
        std::printf("[PASS] the radius-%d/thickness-4 band raised %d cells and "
                    "nothing else.\n",
                    radius, raised);

        TEST_REQUIRE(session.undo(), "the banded stamp undo failed.");
        for (int dy = -margin; dy <= margin; ++dy) {
            for (int dx = -margin; dx <= margin; ++dx) {
                const int oldH = before[static_cast<std::size_t>(dy + margin) *
                                            (2 * margin + 1) + (dx + margin)];
                TEST_REQUIRE(readHeight(cx + dx, cy + dy) == oldH,
                             "undo left cell (%d,%d) modified.", dx, dy);
            }
        }
    }

    // ------------------------------------------------------------------
    // 2: a Line stroke of thickness 5 stays inside the radius-12 footprint
    //    of the same line.
    // ------------------------------------------------------------------
    {
        params.shape = BrushShape::Line;
        params.thickness = 5;
        const int x0 = cx - radius;
        const int y0 = cy;
        const int x1 = cx + radius;
        const int y1 = cy;

        const int thinR = BrushTool::lineStrokeRadius(radius, params.thickness);
        TEST_REQUIRE(thinR == 2,
                     "a thickness-5 stroke of a radius-12 brush must stamp "
                     "radius 2, got %d.",
                     thinR);

        const int wideR = BrushTool::lineStrokeRadius(radius, 0);
        const std::set<std::pair<int, int>> thin =
            maskCellsAround(BrushMasks::lineStamp(x0, y0, x1, y1, thinR), cx, cy);
        const std::set<std::pair<int, int>> wide =
            maskCellsAround(BrushMasks::lineStamp(x0, y0, x1, y1, wideR), cx, cy);
        for (const auto& c : thin) {
            TEST_REQUIRE(wide.count(c),
                         "the thin stroke left the full-stamp footprint at "
                         "(%d,%d).",
                         c.first, c.second);
        }

        std::vector<int> before(static_cast<std::size_t>(2 * margin + 1) *
                                (2 * margin + 1));
        for (int dy = -margin; dy <= margin; ++dy) {
            for (int dx = -margin; dx <= margin; ++dx) {
                before[static_cast<std::size_t>(dy + margin) *
                           (2 * margin + 1) + (dx + margin)] =
                    readHeight(cx + dx, cy + dy);
            }
        }

        TEST_REQUIRE(brush.applyLine(x0, y0, x1, y1, params),
                     "the thin line stroke was refused.");

        for (int dy = -margin; dy <= margin; ++dy) {
            for (int dx = -margin; dx <= margin; ++dx) {
                const int oldH = before[static_cast<std::size_t>(dy + margin) *
                                            (2 * margin + 1) + (dx + margin)];
                const int newH = readHeight(cx + dx, cy + dy);
                const bool inStroke = thin.count({dx, dy}) != 0;
                if (inStroke) {
                    TEST_REQUIRE(newH == oldH + delta,
                                 "stroke cell (%d,%d): %d -> %d, expected +%d.",
                                 dx, dy, oldH, newH, delta);
                } else {
                    TEST_REQUIRE(newH == oldH,
                                 "cell (%d,%d) outside the stroke changed.",
                                 dx, dy);
                }
            }
        }
        std::printf("[PASS] the thickness-5 line stroke stays inside the "
                    "radius-%d footprint.\n",
                    radius);

        TEST_REQUIRE(session.undo(), "the line stroke undo failed.");
    }

    // ------------------------------------------------------------------
    // 3: clampedParams keeps thickness inside the footprint.
    // ------------------------------------------------------------------
    {
        BrushParams wild = params;
        wild.shape = BrushShape::SquareOutline;
        wild.radius = 6;
        wild.thickness = 5000;
        brush.setParams(wild);
        TEST_REQUIRE(brush.params().thickness == BrushTool::maxThickness(6),
                     "setParams must clamp the thickness to the stamp extent "
                     "(got %d, expected %d).",
                     brush.params().thickness, BrushTool::maxThickness(6));

        brush.setParams(wild);
        wild.thickness = -50;
        brush.setParams(wild);
        TEST_REQUIRE(brush.params().thickness == 0,
                     "a negative thickness must clamp to 0 (auto), got %d.",
                     brush.params().thickness);
    }
    std::printf("[PASS] setParams clamps the thickness into the footprint.\n");

    return true;
}

MAP_EDITOR_REGISTER_TEST("Brush thickness band application", 88,
                         runBrushThicknessApplyTests);

} // namespace

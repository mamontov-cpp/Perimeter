// ============================================================================
// Regular-polygon brush stamps (FilledPolygon / PolygonOutline).
//
// The polygon shapes add two BrushParams: the vertex count polygonSides
// (3..20) and the first-vertex angle polygonAngleDeg (0..360, wrapping). The
// brush radius stays the stamp half-size — for the polygon shapes it is the
// circumradius of the vertex circle, exactly like the circle brush uses it.
//
// Checked here (one loaded world, the thickness-test setup pattern):
//   1. a filled-polygon raise edits exactly the cells of the generated mask
//      (no more, no less) and undoes byte-exact;
//   2. a polygon-outline raise with a stroke width edits exactly the inset
//      band cells of the generated mask;
//   3. setParams clamps polygonSides into [3,20] and wraps the angle into
//      [0,360).
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

bool runPolygonBrushApplyTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_polygon", session);
    TEST_REQUIRE(loaded,
                 "the polygon test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the polygon test.");

    // A flat-enough area with headroom for a raise of one stamp.
    const int radius = 16;
    const int margin = radius + 4;
    int cx = 0;
    int cy = 0;
    bool found = false;
    for (int gy = margin; gy < mapH - margin && !found; gy += 24) {
        for (int gx = margin; gx < mapW - margin && !found; gx += 24) {
            int localMax = 0;
            for (int dy = -margin; dy <= margin; ++dy) {
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
    TEST_REQUIRE(found, "no suitable stamp area for the polygon test.");

    auto readHeight = [](int x, int y) {
        return static_cast<int>(vMap.SGetAlt(x, y));
    };

    BrushTool brush(session);
    BrushParams params;
    params.radius = radius;
    params.effect = BrushEffect::ChangeHeight;
    params.mode = BrushMode::Geo;
    params.direction = 1;
    params.strength = 4;   // delta = 4 voxels at full influence
    params.polygonSides = 5;
    params.polygonAngleDeg = 10.0;
    const int delta = BrushTool::changeHeightDelta(params, 255);

    auto checkStampAgainstMask = [&](const BrushMask& mask, const char* what,
                                     int& raisedOut) -> bool {
        const std::set<std::pair<int, int>> maskSet = maskCells(mask);
        std::vector<int> before(
            static_cast<std::size_t>(2 * margin + 1) * (2 * margin + 1));
        for (int dy = -margin; dy <= margin; ++dy) {
            for (int dx = -margin; dx <= margin; ++dx) {
                before[static_cast<std::size_t>(dy + margin) * (2 * margin + 1) +
                       (dx + margin)] = readHeight(cx + dx, cy + dy);
            }
        }

        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the %s stamp was refused.", what);

        int raised = 0;
        for (int dy = -margin; dy <= margin; ++dy) {
            for (int dx = -margin; dx <= margin; ++dx) {
                const int oldH =
                    before[static_cast<std::size_t>(dy + margin) *
                               (2 * margin + 1) +
                           (dx + margin)];
                const int newH = readHeight(cx + dx, cy + dy);
                const bool inMask = maskSet.count({dx, dy}) != 0;
                if (inMask) {
                    ++raised;
                    TEST_REQUIRE(newH == oldH + delta,
                                 "%s: mask cell (%d,%d): %d -> %d, expected "
                                 "+%d.",
                                 what, dx, dy, oldH, newH, delta);
                } else {
                    TEST_REQUIRE(newH == oldH,
                                 "%s: cell (%d,%d) outside the mask changed: "
                                 "%d -> %d.",
                                 what, dx, dy, oldH, newH);
                }
            }
        }
        TEST_REQUIRE(raised == static_cast<int>(maskSet.size()),
                     "%s: %d mask cells changed, the mask has %zu.",
                     what, raised, maskSet.size());
        TEST_REQUIRE(session.undo(), "the %s stamp undo failed.", what);
        for (int dy = -margin; dy <= margin; ++dy) {
            for (int dx = -margin; dx <= margin; ++dx) {
                const int oldH =
                    before[static_cast<std::size_t>(dy + margin) *
                               (2 * margin + 1) +
                           (dx + margin)];
                TEST_REQUIRE(readHeight(cx + dx, cy + dy) == oldH,
                             "%s: undo left cell (%d,%d) modified.", what, dx,
                             dy);
            }
        }
        raisedOut = raised;
        return true;
    };

    // ------------------------------------------------------------------
    // 1: the filled pentagon raises exactly the generated mask cells.
    // ------------------------------------------------------------------
    params.shape = BrushShape::FilledPolygon;
    params.thickness = 0;
    const BrushMask pentagon =
        BrushMasks::filledPolygon(radius, params.polygonSides,
                                  params.polygonAngleDeg);
    TEST_REQUIRE(!pentagon.empty() && pentagon.sx == 2 * radius + 1,
                 "the pentagon mask must keep the centred (2r+1) bbox.");
    int raisedCount = 0;
    checkStampAgainstMask(pentagon, "filled polygon", raisedCount);
    const int filledRaised = raisedCount;
    std::printf("[PASS] the filled pentagon raised %d cells and nothing "
                "else, undo restores.\n",
                filledRaised);

    // ------------------------------------------------------------------
    // 2: the pentagon outline with stroke width 4 raises exactly the band.
    // ------------------------------------------------------------------
    params.shape = BrushShape::PolygonOutline;
    params.thickness = 4;
    const BrushMask band =
        BrushMasks::polygonOutline(radius, params.polygonSides,
                                   params.polygonAngleDeg, params.thickness);
    TEST_REQUIRE(!band.empty(), "the polygon band mask must not be empty.");
    checkStampAgainstMask(band, "polygon outline", raisedCount);
    const int bandRaised = raisedCount;
    TEST_REQUIRE(bandRaised < filledRaised,
                 "the outline band (%d) must raise fewer cells than the fill "
                 "(%d).",
                 bandRaised, filledRaised);
    std::printf("[PASS] the polygon outline band raised %d cells and nothing "
                "else, undo restores.\n",
                bandRaised);

    // ------------------------------------------------------------------
    // 3: the polygon params clamp like every other brush parameter.
    // ------------------------------------------------------------------
    BrushParams wild = params;
    wild.polygonSides = 100;
    wild.polygonAngleDeg = 725.0;
    brush.setParams(wild);
    TEST_REQUIRE(brush.params().polygonSides == MapEditor::kMaxPolygonSides,
                 "polygonSides must clamp to %d, got %d.",
                 MapEditor::kMaxPolygonSides, brush.params().polygonSides);
    TEST_REQUIRE(brush.params().polygonAngleDeg == 5.0,
                 "725° must wrap to 5°, got %.1f.",
                 brush.params().polygonAngleDeg);

    wild.polygonSides = 1;
    wild.polygonAngleDeg = -90.0;
    brush.setParams(wild);
    TEST_REQUIRE(brush.params().polygonSides == MapEditor::kMinPolygonSides,
                 "polygonSides must clamp to %d, got %d.",
                 MapEditor::kMinPolygonSides, brush.params().polygonSides);
    TEST_REQUIRE(brush.params().polygonAngleDeg == 270.0,
                 "-90° must wrap to 270°, got %.1f.",
                 brush.params().polygonAngleDeg);
    std::printf("[PASS] polygonSides/polygonAngleDeg clamp and wrap.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush regular polygon stamps", 89,
                         runPolygonBrushApplyTests);

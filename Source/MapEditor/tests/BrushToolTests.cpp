// ============================================================================
// T3.1 — BrushTool application path on a loaded world.
//
// Exercises the full pipeline "mask → recordUndoArea → individualToolzer
// (T2TE_CHANGING_TERRAIN_HEIGHT) → recalcArea2Grid → regRender":
//
//   * a raise stamp changes exactly the mask cells by strength<<VX_FRACTION,
//     everything else (bbox corners, one-cell margins) stays untouched;
//   * every vertex of SGetAlt math the game's own toolzer performs
//     (fraction bits, MAX/MIN clamping, SurBuf re-derivation) holds;
//   * undo restores the stamp bbox byte-exact, redo re-applies;
//   * a line application is ONE undo entry (disjoint lines undo step-wise);
//   * unsupported effects (T3.2+ entry points) and None are refused without
//     touching the undo stack.
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushShape;

/// Heights across a small square region (SGetAlt reads whole<<VX_FRACTION).
struct RegionHeights
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<int> heights;

    static RegionHeights read(int x, int y, int w, int h)
    {
        RegionHeights region;
        region.x = x;
        region.y = y;
        region.w = w;
        region.h = h;
        region.heights.reserve(static_cast<std::size_t>(w) * h);
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) {
                region.heights.push_back(
                    static_cast<int>(vMap.SGetAlt(x + i, y + j)));
            }
        }
        return region;
    }

    int& atRef(int px, int py)
    {
        return heights[static_cast<std::size_t>(py - y) * w + (px - x)];
    }
    int at(int px, int py) const
    {
        return heights[static_cast<std::size_t>(py - y) * w + (px - x)];
    }

    bool equals(const RegionHeights& other) const
    {
        return x == other.x && y == other.y && w == other.w && h == other.h &&
               heights == other.heights;
    }
};

/// Byte-exact four-buffer snapshot of a region (for the undo check).
struct RegionBytes
{
    std::vector<std::uint8_t> geo;
    std::vector<std::uint8_t> dam;
    std::vector<std::uint8_t> atr;
    std::vector<std::uint8_t> sur;

    static RegionBytes read(int x, int y, int w, int h)
    {
        RegionBytes region;
        const std::size_t cells = static_cast<std::size_t>(w) * h;
        region.geo.resize(cells);
        region.dam.resize(cells);
        region.atr.resize(cells);
        region.sur.resize(cells);
        std::size_t cnt = 0;
        for (int j = 0; j < h; ++j) {
            const int row = (y + j) * static_cast<int>(vMap.H_SIZE) + x;
            for (int i = 0; i < w; ++i, ++cnt) {
                const int off = row + i;
                region.geo[cnt] = vMap.VxGBuf[off];
                region.dam[cnt] = vMap.VxDBuf[off];
                region.atr[cnt] = vMap.AtrBuf[off];
                region.sur[cnt] = vMap.SurBuf[off];
            }
        }
        return region;
    }

    bool equals(const RegionBytes& other) const
    {
        return geo == other.geo && dam == other.dam && atr == other.atr &&
               sur == other.sur;
    }
};

bool runBrushToolTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Self-contained content root: the temporary world copy plus the shared
    // resources; the real game folder stays read-only.
    const fs::path loadRoot = ctx.tempRoot / "load_root";
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the brush-test content root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx",
            loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini",
                      loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.tempWorldDir,
            loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName));
    }

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    // Stamp region: find a location where the stamp bbox has plenty of head
    // room below MAX_VX_HEIGHT, so a raise never clamps inside Terra (the
    // "+expectedDelta" assertions would be invalid otherwise).
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the brush test.");
    const int region = 32; // bbox checked around every stamp (radius <= 6..8)
    TEST_REQUIRE(region * 2 < mapW && region * 2 < mapH,
                 "the world is too small for the checked region.");

    int cx = mapW / 2;
    int cy = mapH / 2;
    {
        const int heightCeiling = MAX_VX_HEIGHT - 2048;
        bool found = false;
        for (int step = 128; step < mapH && !found; step += 128) {
            for (int gy = region; gy < mapH - region && !found; gy += step) {
                for (int gx = region; gx < mapW - region && !found; gx += step) {
                    int localMax = 0;
                    for (int dy = -8; dy <= 8; ++dy) {
                        for (int dx = -8; dx <= 8; ++dx) {
                            localMax = std::max(
                                localMax,
                                static_cast<int>(vMap.SGetAlt(gx + dx, gy + dy)));
                        }
                    }
                    if (localMax <= heightCeiling) {
                        cx = gx;
                        cy = gy;
                        found = true;
                    }
                }
            }
        }
        TEST_REQUIRE(found,
                     "no suitably low-terrain cell for the raise test was found.");
        std::printf("Brush test region: (%d, %d)\n", cx, cy);
    }

    // ------------------------------------------------------------------
    // 2.1: a raise stamp moves exactly the mask cells by strength<<VX_FRACTION
    // (fraction bits, SurBuf re-derivation and MAX clamping follow Terra's own
    // tVoxSet), all other cells of the bbox+1 region stay untouched.
    // ------------------------------------------------------------------
    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = 4;
    params.strength = 4;    // delta = 4 << VX_FRACTION = 128 per full cell
    params.mode = BrushMode::Geo;
    params.effect = BrushEffect::ChangeHeight;
    params.direction = 1;
    TEST_REQUIRE(brush.maxRadius() == MapEditor::kMaxBrushRadius,
                 "maxRadius must be the editor cap kMaxBrushRadius.");

    const RegionHeights before =
        RegionHeights::read(cx - region, cy - region, 2 * region, 2 * region);
    const RegionBytes beforeBytes = RegionBytes::read(
        cx - region, cy - region, 2 * region, 2 * region);

    // A raise in this test must not hit MAX_VX_HEIGHT clamping inside Terra,
    // otherwise the "+expectedDelta" assertions would be invalid.
    {
        int maxHeight = 0;
        for (int dy = -6; dy <= 6; ++dy) {
            for (int dx = -6; dx <= 6; ++dx) {
                maxHeight = std::max(maxHeight, before.at(cx + dx, cy + dy));
            }
        }
        TEST_REQUIRE(maxHeight + 2000 <= MAX_VX_HEIGHT,
                     "the test region terrain is too tall for the raise test.");
    }

    TEST_REQUIRE(!session.canUndo(),
                 "a fresh session must not be undoable before the stamp.");

    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the raise stamp was refused (world loaded, effect supported).");
    TEST_REQUIRE(session.canUndo(), "the stamp must enter the undo stack.");

    const int expectedDelta =
        MapEditor::BrushTool::changeHeightDelta(params, 255);
    TEST_REQUIRE(expectedDelta == (4 << VX_FRACTION),
                 "changeHeightDelta(full) must equal strength<<VX_FRACTION.");

    const RegionHeights after = RegionHeights::read(cx - region, cy - region,
                                                    2 * region, 2 * region);
    const MapEditor::BrushMask stamp =
        MapEditor::BrushMasks::filledCircle(params.radius);
    int maskCellsHit = 0;
    for (int j = 0; j < 2 * region; ++j) {
        for (int i = 0; i < 2 * region; ++i) {
            const int px = cx - region + i;
            const int py = cy - region + j;
            const int dx = px - cx;
            const int dy = py - cy;
            const int maskInfluence =
                (std::abs(dx) <= params.radius && std::abs(dy) <= params.radius &&
                 dx >= stamp.x && dy >= stamp.y &&
                 dx < stamp.x + stamp.sx && dy < stamp.y + stamp.sy)
                    ? stamp.influence[static_cast<std::size_t>(dy - stamp.y) *
                                          stamp.sx +
                                      (dx - stamp.x)]
                    : 0;
            if (maskInfluence) {
                ++maskCellsHit;
                TEST_REQUIRE(after.at(px, py) == before.at(px, py) + expectedDelta,
                             "mask cell (%d,%d) must grow by %d, got %d -> %d.",
                             dx, dy, expectedDelta, before.at(px, py),
                             after.at(px, py));
            } else {
                TEST_REQUIRE(after.at(px, py) == before.at(px, py),
                             "non-mask cell (%d,%d) changed unexpectedly.",
                             dx, dy);
            }
        }
    }
    TEST_REQUIRE(maskCellsHit > 0, "the stamp mask covered no cells.");
    std::printf("[PASS] the raise stamp moved exactly %d mask cells by %d "
                "(1/32 m units).\n",
                maskCellsHit, expectedDelta);

    // ------------------------------------------------------------------
    // 2.2: undo restores the bbox byte-exact, redo re-applies it.
    // ------------------------------------------------------------------
    session.undo();
    const RegionBytes restored = RegionBytes::read(cx - region, cy - region,
                                                   2 * region, 2 * region);
    TEST_REQUIRE(restored.equals(beforeBytes),
                 "undo() did not restore the stamp region byte-exact.");
    TEST_REQUIRE(!session.canUndo(),
                 "the single stamp's undo must empty the undo side.");
    session.redo();
    const RegionHeights reApplied = RegionHeights::read(
        cx - region, cy - region, 2 * region, 2 * region);
    TEST_REQUIRE(reApplied.equals(after),
                 "redo() did not re-apply the stamp.");
    std::printf("[PASS] undo/redo round-trips the stamp byte-exact.\n");

    // ------------------------------------------------------------------
    // 2.3: the lower stamp follows the same route downwards (lowered cells
    // demote to the geo layer exactly like the game toolzer does).
    // ------------------------------------------------------------------
    params.direction = -1;
    const RegionHeights beforeLower = RegionHeights::read(
        cx - region, cy - region, 2 * region, 2 * region);
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the lower stamp was refused.");
    const RegionHeights afterLower = RegionHeights::read(
        cx - region, cy - region, 2 * region, 2 * region);
    for (int dy = -params.radius; dy <= params.radius; ++dy) {
        for (int dx = -params.radius; dx <= params.radius; ++dx) {
            const int influence =
                stamp.influence[static_cast<std::size_t>(dy - stamp.y) *
                                    stamp.sx +
                                (dx - stamp.x)];
            if (!influence) {
                continue;
            }
            const int oldHeight = beforeLower.at(cx + dx, cy + dy);
            const int expectedNew =
                std::max(0, oldHeight - expectedDelta); // MIN_VX_HEIGHT floor
            TEST_REQUIRE(afterLower.at(cx + dx, cy + dy) == expectedNew,
                         "lower delta at (%d,%d): expected %d, got %d.",
                         dx, dy, expectedNew, afterLower.at(cx + dx, cy + dy));
        }
    }
    std::printf("[PASS] the lower stamp routes through the same toolzer "
                "(MIN clamp honoured).\n");
    params.direction = 1;

    // ------------------------------------------------------------------
    // 2.4: the None effect is refused without touching the undo stack.
    // (The T3.4/T3.5 entry points routed their effects since this check was
    // written — every BrushEffect except None is supported now, and the
    // T4.2 pick is covered in BrushPickColorTests.)
    // ------------------------------------------------------------------
    TEST_REQUIRE(MapEditor::BrushTool::effectSupported(BrushEffect::SetColor),
                  "SetColor must be supported in T3.3.");
    const bool undoableBeforeRefusal = session.canUndo();
    BrushParams refused = params;
    refused.effect = BrushEffect::None;
    TEST_REQUIRE(!brush.applyStamp(cx, cy, refused),
                  "the None effect must be refused.");
    TEST_REQUIRE(session.canUndo() == undoableBeforeRefusal,
                  "refused stamps must not push undo entries.");
    std::printf("[PASS] the None effect is refused cleanly.\n");

    // ------------------------------------------------------------------
    // 2.5: a line application is a single undo entry; two disjoint lines
    // step back one at a time; endpoints and coverage match the stamp.
    // ------------------------------------------------------------------
    {
        // Back to the raised state of 2.2 (undo of the lower stamp), so the
        // two test lines below act on a known baseline.
        TEST_REQUIRE(session.undo(), "undo of the lower stamp failed.");
        const int lineY = cy + 24;
        const RegionHeights beforeLines = RegionHeights::read(
            cx - region, cy - region, 2 * region, 2 * region);

        BrushParams lineParams = params; // raise, ChangeHeight
        lineParams.radius = 1;
        lineParams.strength = 1;         // delta = 1 << VX_FRACTION

        TEST_REQUIRE(brush.applyLine(cx - 16, lineY, cx + 8, lineY, lineParams),
                     "the first line was refused.");
        TEST_REQUIRE(session.canUndo(), "the line must enter the undo stack.");

        const int lineDelta =
            MapEditor::BrushTool::changeHeightDelta(lineParams, 255);
        TEST_REQUIRE(lineDelta == (1 << VX_FRACTION),
                     "a strength=1 line must raise by 1<<VX_FRACTION.");
        const RegionHeights afterLine1 = RegionHeights::read(
            cx - region, cy - region, 2 * region, 2 * region);
        for (int dx = -region; dx < region; ++dx) {
            // A radius=1 filled-circle stamp covers 3 rows and 3 columns; on
            // the line row itself every path cell (stamp borders included)
            // must be raised exactly once (stamp overlaps do not accumulate).
            const bool onPath = dx >= -16 - 1 && dx <= 8 + 1;
            const int expected = onPath
                ? beforeLines.at(cx + dx, lineY) + lineDelta
                : beforeLines.at(cx + dx, lineY);
            TEST_REQUIRE(afterLine1.at(cx + dx, lineY) == expected,
                         "line cell raise mismatch at dx=%d.", dx);
        }

        // Second, disjoint line: undo must first step back to line 1's state.
        TEST_REQUIRE(brush.applyLine(cx - 4, lineY + 8, cx + 4, lineY + 8,
                                     lineParams),
                     "the second line was refused.");
        TEST_REQUIRE(session.undo(),
                     "the second line must undo alone (per-line entries).");
        const RegionHeights backTo1 = RegionHeights::read(
            cx - region, cy - region, 2 * region, 2 * region);
        TEST_REQUIRE(backTo1.equals(afterLine1),
                     "after one undo, only the second line must be gone.");
        TEST_REQUIRE(session.undo(),
                     "the first line must undo on the next undo step.");
        const RegionHeights backTo0 = RegionHeights::read(
            cx - region, cy - region, 2 * region, 2 * region);
        TEST_REQUIRE(backTo0.equals(beforeLines),
                     "two undos must restore the pristine state.");
        std::printf("[PASS] each line application is one undo entry.\n");
    }

    // ------------------------------------------------------------------
    // 2.6: clamping helpers keep pathological parameter values usable.
    // ------------------------------------------------------------------
    BrushParams wild;
    wild.radius = 5000;
    wild.strength = -7;
    wild.direction = 3;
    TEST_REQUIRE(MapEditor::BrushTool::clampRadius(wild.radius) ==
                     MapEditor::BrushTool::maxRadius(),
                 "the radius must clamp to the editor cap (kMaxBrushRadius).");
    TEST_REQUIRE(MapEditor::BrushTool::maxRadius() == 512,
                 "the editor brush radius cap must be 512 cells.");
    TEST_REQUIRE(MapEditor::BrushTool::clampStrength(wild.strength) == 1,
                 "the strength must clamp into [1,100].");
    wild.strength = 4000;
    TEST_REQUIRE(MapEditor::BrushTool::clampStrength(wild.strength) == 100,
                 "the strength must clamp into [1,100].");
    std::printf("[PASS] radius/strength clamps hold for wild values.\n");

    // ------------------------------------------------------------------
    // 2.7: the radius cap is the editor's own kMaxBrushRadius = 512 (the
    // masks are analytic, Terra's radius-table length does not apply): a
    // radius-512 filled-square stamp touches the whole 1025x1025
    // footprint, nothing outside it, and undoes byte-exact. The soot
    // effect is used because it is height-independent (a raise could
    // clamp against MAX_VX_HEIGHT on tall worlds like COMPASS).
    // ------------------------------------------------------------------
    TEST_REQUIRE(mapW >= 2 * MapEditor::kMaxBrushRadius + 3 &&
                 mapH >= 2 * MapEditor::kMaxBrushRadius + 3,
                 "the world is too small for the 512-radius stamp test.");
    const int bigCx = mapW / 2;
    const int bigCy = mapH / 2;

    BrushParams bigParams;
    bigParams.shape = BrushShape::FilledSquare;
    bigParams.radius = MapEditor::kMaxBrushRadius;
    bigParams.effect = BrushEffect::PlotSoot;

    constexpr int bigR = 512;
    constexpr int bigSide = 2 * bigR + 3; // footprint + the 1-cell margin
    const RegionBytes bigBeforeBytes = RegionBytes::read(
        bigCx - bigR - 1, bigCy - bigR - 1, bigSide, bigSide);

    TEST_REQUIRE(brush.applyStamp(bigCx, bigCy, bigParams),
                 "the 512-radius stamp was refused.");
    TEST_REQUIRE(session.canUndo(),
                 "the 512-radius stamp must enter the undo stack.");

    // Every changed cell must lie inside the square footprint; at least
    // one cell must change (so the stamp genuinely reached the rim cells).
    std::size_t changedInside = 0;
    std::size_t changedOutside = 0;
    for (int j = 0; j < bigSide; ++j) {
        for (int i = 0; i < bigSide; ++i) {
            const std::size_t idx = static_cast<std::size_t>(j) * bigSide + i;
            const bool changed =
                bigBeforeBytes.geo[idx] != vMap.VxGBuf[(bigCy - bigR - 1 + j) * mapW +
                                                       (bigCx - bigR - 1 + i)] ||
                bigBeforeBytes.dam[idx] != vMap.VxDBuf[(bigCy - bigR - 1 + j) * mapW +
                                                       (bigCx - bigR - 1 + i)] ||
                bigBeforeBytes.atr[idx] != vMap.AtrBuf[(bigCy - bigR - 1 + j) * mapW +
                                                       (bigCx - bigR - 1 + i)] ||
                bigBeforeBytes.sur[idx] != vMap.SurBuf[(bigCy - bigR - 1 + j) * mapW +
                                                       (bigCx - bigR - 1 + i)];
            if (!changed) {
                continue;
            }
            const int dx = i - bigR - 1;
            const int dy = j - bigR - 1;
            if (std::abs(dx) <= bigR && std::abs(dy) <= bigR) {
                ++changedInside;
            } else {
                ++changedOutside;
            }
        }
    }
    TEST_REQUIRE(changedInside > 0,
                 "the 512-radius stamp changed nothing inside its footprint.");
    TEST_REQUIRE(changedOutside == 0,
                 "the 512-radius stamp changed cells outside its footprint.");
    std::printf("[PASS] the 512-radius stamp touches the whole 1025x1025 "
                "footprint (%zu cells) and nothing outside it.\n",
                changedInside);

    TEST_REQUIRE(session.undo(), "the 512-radius stamp must be undoable.");
    const RegionBytes bigRestored = RegionBytes::read(
        bigCx - bigR - 1, bigCy - bigR - 1, bigSide, bigSide);
    TEST_REQUIRE(bigRestored.equals(bigBeforeBytes),
                 "undo() did not restore the 512-radius stamp byte-exact.");
    std::printf("[PASS] undo() restores the 512-radius stamp byte-exact.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("BrushTool (T3.1)", 58, runBrushToolTests);

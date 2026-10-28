// ============================================================================
// T3.2 revision — "Level to Height" (AlignVariableH) is an absolute set.
//
// The brush used to move every cell by the strength-scaled per-stamp delta
// towards the target. On terrain much lower than the chosen height a single
// stamp only grew the surface by `strength` voxels and never reached the
// target; overlapping stamps could also leave different heights inside one
// patch. The brush now moves each cell by the whole remaining distance to the
// target in one stamp (scaled by the mask influence), so:
//
//   1. Dam mode on a geo-only cell seeds the dam and sets its top exactly to
//      the target height;
//   2. Dam mode on an existing low dam sets that dam exactly to the target;
//   3. Dam mode on an existing dam above the target lowers it to the target;
//   4. Geo mode sets the geo top exactly to the target;
//   5. two overlapping radius-64 stamps (centres 3 cells apart) leave the
//      whole union at the target — the maximum is exactly the target height.
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
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushShape;

int mapOffset(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

int topHeight(int x, int y)
{
    return static_cast<int>(vMap.SGetAlt(x, y));
}

void setCell(int x, int y, int geoWhole, int damWhole, int fraction,
             unsigned char surface)
{
    const int off = mapOffset(x, y);
    vMap.VxGBuf[off] = static_cast<unsigned char>(geoWhole);
    vMap.VxDBuf[off] = static_cast<unsigned char>(damWhole);
    vMap.AtrBuf[off] = static_cast<unsigned char>(fraction);
    vMap.SurBuf[off] = surface;
}

/// Fill a square patch with a controlled geo/dam state.
void fillPatch(int cx, int cy, int half, int geoWhole, int damWhole,
               int fraction, unsigned char surface)
{
    for (int y = cy - half; y <= cy + half; ++y) {
        for (int x = cx - half; x <= cx + half; ++x) {
            setCell(x, y, geoWhole, damWhole, fraction, surface);
        }
    }
}

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

/// The whole patch must sit exactly at `target` (1/32 m units) on the layer
/// the mode owns.
bool checkPatchAtTarget(const MapEditor::BrushMask& mask, int stampX,
                        int stampY, int target, int expectedGeo,
                        int expectedDamNonZero, bool checkGeoUnchanged,
                        bool checkSurface)
{
    for (int dy = mask.y; dy < mask.y + mask.sy; ++dy) {
        for (int dx = mask.x; dx < mask.x + mask.sx; ++dx) {
            if (!mask.at(dx - mask.x, dy - mask.y)) {
                continue;
            }
            const int px = stampX + dx;
            const int py = stampY + dy;
            const int off = mapOffset(px, py);

            TEST_REQUIRE(topHeight(px, py) == target,
                         "cell (%d,%d): top height %d, expected %d.",
                         dx, dy, topHeight(px, py), target);
            TEST_REQUIRE((vMap.AtrBuf[off] & VX_FRACTION_MASK) ==
                             (target & VX_FRACTION_MASK),
                         "cell (%d,%d): fraction bits do not match the "
                         "target.", dx, dy);
            if (checkGeoUnchanged) {
                TEST_REQUIRE(vMap.VxGBuf[off] == expectedGeo,
                             "cell (%d,%d): geo %d, expected %d.",
                             dx, dy, vMap.VxGBuf[off], expectedGeo);
            }
            if (expectedDamNonZero) {
                TEST_REQUIRE(vMap.VxDBuf[off] != 0,
                             "cell (%d,%d): the dam layer must exist.",
                             dx, dy);
            }
            if (checkSurface) {
                TEST_REQUIRE(vMap.SurBuf[off] == vMap.veryLightDam,
                             "cell (%d,%d): the seeded dam must keep the "
                             "veryLightDam surface.", dx, dy);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Test 80: absolute "Level to Height" in both modes and overlapping stamps
// ---------------------------------------------------------------------------

bool runBrushSetHeightTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_set_height", session);
    TEST_REQUIRE(loaded,
                 "the set-height test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the set-height test.");

    // Controlled patch in the middle of the map, far from any real terrain
    // feature the test does not own.
    const int cx = mapW / 2;
    const int cy = mapH / 2;
    const int targetLevel = 180;
    const int target = targetLevel << VX_FRACTION;
    const unsigned char geoSurface = 10;
    const unsigned char damSurface = 200;

    MapEditor::BrushTool brush(session);

    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = 8;
    params.strength = 30; // must not limit the absolute set
    params.effect = BrushEffect::AlignVariableH;
    params.heightLevel = targetLevel;

    // ------------------------------------------------------------------
    // 1: Dam mode on geo-only, much lower terrain.
    // ------------------------------------------------------------------
    {
        fillPatch(cx, cy, params.radius + 4, 20, 0, 0, geoSurface);
        params.mode = BrushMode::Dam;

        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the Dam-mode set-height stamp was refused.");

        const MapEditor::BrushMask mask =
            MapEditor::BrushMasks::filledCircle(params.radius);
        TEST_REQUIRE(checkPatchAtTarget(mask, cx, cy, target, 20,
                                        /*expectedDamNonZero=*/1,
                                        /*checkGeoUnchanged=*/true,
                                        /*checkSurface=*/true),
                     "Dam mode on geo-only terrain did not reach the target.");
        std::printf("[PASS] Dam mode sets geo-only terrain to %d voxels.\n",
                    targetLevel);

        TEST_REQUIRE(session.undo(), "the Dam-mode stamp undo failed.");
    }

    // ------------------------------------------------------------------
    // 2: Dam mode on an existing low dam (with a non-zero fraction, the
    //    common real-world case).
    // ------------------------------------------------------------------
    {
        fillPatch(cx, cy, params.radius + 4, 10, 40, 17, damSurface);
        params.mode = BrushMode::Dam;

        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the Dam-mode set-height stamp on a dam was refused.");

        const MapEditor::BrushMask mask =
            MapEditor::BrushMasks::filledCircle(params.radius);
        for (int dy = -params.radius; dy <= params.radius; ++dy) {
            for (int dx = -params.radius; dx <= params.radius; ++dx) {
                if (!mask.at(dx - mask.x, dy - mask.y)) {
                    continue;
                }
                const int off = mapOffset(cx + dx, cy + dy);
                TEST_REQUIRE(topHeight(cx + dx, cy + dy) == target,
                             "existing dam at (%d,%d): top %d, expected %d.",
                             dx, dy, topHeight(cx + dx, cy + dy), target);
                TEST_REQUIRE(vMap.VxDBuf[off] != 0,
                             "existing dam at (%d,%d) disappeared.", dx, dy);
                TEST_REQUIRE(vMap.VxGBuf[off] == 10,
                             "geo under the dam must stay unchanged at "
                             "(%d,%d).", dx, dy);
                TEST_REQUIRE(vMap.SurBuf[off] == damSurface,
                             "the existing dam surface must stay unchanged "
                             "at (%d,%d).", dx, dy);
            }
        }
        std::printf("[PASS] Dam mode sets an existing low dam to %d voxels.\n",
                    targetLevel);

        TEST_REQUIRE(session.undo(), "the existing-dam stamp undo failed.");
    }

    // ------------------------------------------------------------------
    // 2b: Dam mode on an existing dam ABOVE the target — the absolute set
    //     lowers it to the target as well (the patch maximum stays 180).
    // ------------------------------------------------------------------
    {
        fillPatch(cx, cy, params.radius + 4, 10, 220, 0, damSurface);
        params.mode = BrushMode::Dam;

        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the lowering Dam-mode set-height stamp was refused.");

        const MapEditor::BrushMask mask =
            MapEditor::BrushMasks::filledCircle(params.radius);
        for (int dy = -params.radius; dy <= params.radius; ++dy) {
            for (int dx = -params.radius; dx <= params.radius; ++dx) {
                if (!mask.at(dx - mask.x, dy - mask.y)) {
                    continue;
                }
                const int off = mapOffset(cx + dx, cy + dy);
                TEST_REQUIRE(topHeight(cx + dx, cy + dy) == target,
                             "high dam at (%d,%d): top %d, expected %d.",
                             dx, dy, topHeight(cx + dx, cy + dy), target);
                TEST_REQUIRE(vMap.VxDBuf[off] == targetLevel,
                             "high dam at (%d,%d): dam %d, expected %d.",
                             dx, dy, vMap.VxDBuf[off], targetLevel);
                TEST_REQUIRE(vMap.VxGBuf[off] == 10,
                             "geo under the high dam must stay unchanged at "
                             "(%d,%d).", dx, dy);
                TEST_REQUIRE(vMap.SurBuf[off] == damSurface,
                             "the high dam surface must stay unchanged at "
                             "(%d,%d).", dx, dy);
            }
        }
        std::printf("[PASS] Dam mode lowers an above-target dam to %d "
                    "voxels.\n", targetLevel);

        TEST_REQUIRE(session.undo(), "the high-dam stamp undo failed.");
    }

    // ------------------------------------------------------------------
    // 3: Geo mode on geo-only terrain.
    // ------------------------------------------------------------------
    {
        fillPatch(cx, cy, params.radius + 4, 20, 0, 0, geoSurface);
        params.mode = BrushMode::Geo;

        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the Geo-mode set-height stamp was refused.");

        const MapEditor::BrushMask mask =
            MapEditor::BrushMasks::filledCircle(params.radius);
        TEST_REQUIRE(checkPatchAtTarget(mask, cx, cy, target, targetLevel,
                                        /*expectedDamNonZero=*/0,
                                        /*checkGeoUnchanged=*/false,
                                        /*checkSurface=*/false),
                     "Geo mode did not reach the target.");
        for (int dy = -params.radius; dy <= params.radius; ++dy) {
            for (int dx = -params.radius; dx <= params.radius; ++dx) {
                if (!mask.at(dx - mask.x, dy - mask.y)) {
                    continue;
                }
                const int off = mapOffset(cx + dx, cy + dy);
                TEST_REQUIRE(vMap.VxDBuf[off] == 0,
                             "Geo mode must not create a dam at (%d,%d).",
                             dx, dy);
            }
        }
        std::printf("[PASS] Geo mode sets the geo layer to %d voxels.\n",
                    targetLevel);

        TEST_REQUIRE(session.undo(), "the Geo-mode stamp undo failed.");
    }

    // ------------------------------------------------------------------
    // 4: two overlapping radius-64 stamps, centres 3 cells apart.
    // ------------------------------------------------------------------
    {
        const int bigRadius = 64;
        fillPatch(cx, cy, bigRadius + 8, 10, 0, 0, geoSurface);

        BrushParams big = params;
        big.mode = BrushMode::Dam;
        big.radius = bigRadius;
        big.heightLevel = targetLevel;

        const int firstX = cx - 1;
        const int firstY = cy;
        const int secondX = cx + 2;
        const int secondY = cy;

        TEST_REQUIRE(brush.applyStamp(firstX, firstY, big),
                     "the first radius-64 set-height stamp was refused.");
        TEST_REQUIRE(brush.applyStamp(secondX, secondY, big),
                     "the second radius-64 set-height stamp was refused.");

        // The union of the two stamps must be exactly at the target; the
        // maximum over the patch must be the target, never above it.
        int maxTop = 0;
        for (int y = cy - bigRadius - 2; y <= cy + bigRadius + 2; ++y) {
            for (int x = cx - bigRadius - 3; x <= cx + bigRadius + 4; ++x) {
                const int top = topHeight(x, y);
                maxTop = std::max(maxTop, top);
                const bool inFirst =
                    MapEditor::BrushMasks::circularRing(x - firstX,
                                                        y - firstY) <= bigRadius;
                const bool inSecond =
                    MapEditor::BrushMasks::circularRing(x - secondX,
                                                        y - secondY) <= bigRadius;
                if (!inFirst && !inSecond) {
                    continue;
                }
                TEST_REQUIRE(top == target,
                             "overlapping stamps: cell (%d,%d) top %d, "
                             "expected %d.", x, y, top, target);
            }
        }
        TEST_REQUIRE(maxTop == target,
                     "overlapping stamps: maximum height %d, expected %d.",
                     maxTop, target);
        std::printf("[PASS] Two overlapping radius-64 stamps leave the patch "
                    "at %d voxels (max %d).\n", targetLevel, maxTop);

        TEST_REQUIRE(session.undo(), "the second big stamp undo failed.");
        TEST_REQUIRE(session.undo(), "the first big stamp undo failed.");
    }

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Set Height brush reaches the target (T3.2 fix)", 80,
                         runBrushSetHeightTests);

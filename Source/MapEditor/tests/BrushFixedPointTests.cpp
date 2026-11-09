// ============================================================================
// Fixed-point height conversion for the set/raise/lower height brushes.
//
// Terra stores heights in 1/32-voxel fixed point (VX_FRACTION bits in
// AtrBuf); whole voxels in VxG/VxDBuf. The editor brushes used to round
// intermediate heights to WHOLE voxels — sculpting capped at a truncated
// target (setHAppoximation shifts away the fraction), the 3D shape and model
// brushes lround()-ed each per-cell target, and the script/MCP height inputs
// truncated (duk_to_int / integer schema) — producing ripples/stair-stepping
// on generated surfaces.
//
// The fix keeps heights in the 1/32 fixed point end to end (see
// BrushTool::{heightLevelFixedPoint,setExactToolzerTarget}) and converts
// user input ONCE to the nearest representable step. These tests pin the
// behaviour:
//
//   1. Smooth (effect 1.0) over 10.0/11.0 must converge BOTH cells to 10.5
//      (336 fixed) — the old whole-voxel cap flattened to 10.0;
//   2. the scripted setHeight accepts fractional voxels: 10.4 -> 333 fixed
//      (was: truncated to 10 -> 320);
//   3. the scripted raiseHeight accepts fractional deltas: +0.5 voxel;
//   4. a 3D cone (height 3, base 0, radius 8) lands the r/2 cell EXACTLY on
//      1.5 voxels (48 fixed) — the old per-cell lround gave 2.0 (64).
//
// Runs after the world round-trip (its temporary world copy is reused).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ScriptedBrush.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushShape;
using MapEditor::Shape3DKind;

int mapOffset(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

int topFixed(int x, int y)
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

bool loadWorld(MapEditor::EditorSession& session)
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    if (ctx.tempRoot.empty() || !fs::exists(ctx.tempRoot) ||
        ctx.worldName.empty()) {
        return false;
    }
    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "fixedpoint";
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
// Test 1: Smooth converges to the fractional average (exact fixed-point cap).
// ---------------------------------------------------------------------------
bool runSmoothFixedPointTests()
{
    MapEditor::EditorSession session;
    TEST_REQUIRE(loadWorld(session), "world load failed.");
    session.setUndoCoalescingWindowMs(0);
    MapEditor::BrushTool brush(session);

    // 9x9 patch: left half at 10.0, middle column at 10.5, right half at
    // 11.0 (geo, no dam). The symmetric middle column keeps the least-
    // squares plane offset (the Smooth reference is plane.offset, the
    // fitted height at the stamp centre) exactly at the 10.5 average.
    const int cx = 100, cy = 100;
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int whole = dx < 0 ? 10 : (dx > 0 ? 11 : 10);
            const int fraction = dx == 0 ? 16 : 0; // 16/32 = 0.5
            setCell(cx + dx, cy + dy, whole, 0, fraction, 1);
        }
    }

    BrushParams params = brush.params();
    params.mode = BrushMode::Dam;
    params.effect = BrushEffect::Smooth;
    params.shape = BrushShape::FilledSquare;
    params.radius = 4;               // exactly the 9x9 patch
    params.sculptEffect = 1.0;       // reach the reference in one stamp
    TEST_REQUIRE(brush.applyStamp(cx, cy, params), "smooth stamp was a no-op.");

    // The reference is the mask average 10.5 voxels = 336 fixed; every cell
    // must sit exactly there (the old whole-voxel cap pinned them to 10.0).
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            TEST_REQUIRE(topFixed(cx + dx, cy + dy) == 336,
                         "smooth cell (%d,%d) must be 10.5 voxels (336), got %d.",
                         dx, dy, topFixed(cx + dx, cy + dy));
        }
    }
    session.unloadWorld();
    return true;
}

// ---------------------------------------------------------------------------
// Tests 2+3: the script API keeps fractional heights (no int truncation).
// ---------------------------------------------------------------------------
bool runScriptFixedPointTests()
{
    MapEditor::EditorSession session;
    TEST_REQUIRE(loadWorld(session), "world load failed.");
    session.setUndoCoalescingWindowMs(0);
    MapEditor::BrushTool brush(session);

    const int cx = 200, cy = 200;
    for (int dy = -6; dy <= 6; ++dy) {
        for (int dx = -6; dx <= 6; ++dx) {
            setCell(cx + dx, cy + dy, 10, 0, 0, 1);
        }
    }

    BrushParams params = brush.params();

    // setHeight to a fractional voxel height: 10.4 vox -> round(10.4*32)=333.
    std::string error;
    TEST_REQUIRE(MapEditor::runEditorScript(
                     session, brush, params, cx, cy,
                     "setHeight({x: " + std::to_string(cx) +
                         ", y: " + std::to_string(cy) + "}, 10.4, 4, false);",
                     error),
                 "setHeight script failed: %s", error.c_str());
    TEST_REQUIRE(topFixed(cx, cy) == 333,
                 "script setHeight(10.4) must land on 333 (10 + 9/32), got %d.",
                 topFixed(cx, cy));

    // raiseHeight by a fractional delta: +1.5 vox = +48 fixed. (The
    // strength clamp keeps the per-stamp delta at >= 1 voxel — the UI spin
    // floor — so sub-voxel raises are policy-clamped, not truncated.)
    TEST_REQUIRE(MapEditor::runEditorScript(
                     session, brush, params, cx, cy,
                     "raiseHeight({x: " + std::to_string(cx) +
                         ", y: " + std::to_string(cy) + "}, 1.5, 4, false);",
                     error),
                 "raiseHeight script failed: %s", error.c_str());
    TEST_REQUIRE(topFixed(cx, cy) == 381,
                 "script raiseHeight(1.5) must add 48 fixed points, got %d.",
                 topFixed(cx, cy));
    session.unloadWorld();
    return true;
}

// ---------------------------------------------------------------------------
// Test 4: a 3D cone lands the r/2 cell exactly on the fractional profile.
// ---------------------------------------------------------------------------
bool runShape3DFixedPointTests()
{
    MapEditor::EditorSession session;
    TEST_REQUIRE(loadWorld(session), "world load failed.");
    session.setUndoCoalescingWindowMs(0);
    MapEditor::BrushTool brush(session);

    const int cx = 300, cy = 300;
    for (int dy = -9; dy <= 9; ++dy) {
        for (int dx = -9; dx <= 9; ++dx) {
            setCell(cx + dx, cy + dy, 0, 0, 0, 1);
        }
    }

    BrushParams params = brush.params();
    params.mode = BrushMode::Dam;
    params.effect = BrushEffect::ChangeHeight;
    params.direction = 1;
    params.shape = BrushShape::FilledCircle;
    params.radius = 8;
    params.shape3d = Shape3DKind::Cone;
    params.shapeHeight = 3;
    params.heightLevel = 0.0;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params), "cone stamp was a no-op.");

    // Cone profile at half the radius: 1 - 4/8 = 0.5; target 0 + 0.5*3 =
    // 1.5 voxels = 48 fixed (the old per-cell lround gave 2.0 = 64).
    TEST_REQUIRE(topFixed(cx + 4, cy) == 48,
                 "cone cell at r/2 must be exactly 1.5 voxels (48), got %d.",
                 topFixed(cx + 4, cy));
    session.unloadWorld();
    return true;
}

MAP_EDITOR_REGISTER_TEST("Fixed-point heights: smooth fractional target", 96,
                         runSmoothFixedPointTests);
MAP_EDITOR_REGISTER_TEST("Fixed-point heights: script set/raise fractional", 97,
                         runScriptFixedPointTests);
MAP_EDITOR_REGISTER_TEST("Fixed-point heights: 3D cone fractional profile", 98,
                         runShape3DFixedPointTests);

} // namespace

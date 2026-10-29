// ============================================================================
// Zeroplast brush over geo-only ground — one stamp reaches and marks (fix).
//
// "Level to Zeroplast" (Align4ZP) used to take Terra's game-like
// strength-scaled step instead of the whole remaining distance. Over a
// low geo-only patch the step was far below the gap to hZeroPlast, so the
// brush "tried to raise the (seeded) dam" but most of the time never
// reached the level and never marked the cells At_ZEROPLAST — the user had
// to stamp the same place several times. The brush now is an absolute set
// (like "Level to Height"): one stamp takes the whole remaining distance,
// the toolzer caps the overshoot at the target, and every covered cell
// that lands on the level gets the At_ZEROPLAST mark.
//
// This test stamps with strength = 5 (a tiny per-stamp step the old brush
// would have needed ~25 stamps of to climb 40 voxels) and still expects:
//   1. one stamp on a dam-free, 40-voxel-low geo patch sets the top exactly
//      to hZeroPlast, seeds the (very light) dam layer and marks the cells
//      At_ZEROPLAST;
//   2. the geo layer bytes stay untouched (the dam is the driven layer);
//   3. a second stamp is a no-op (the cells are zeroplast now);
//   4. undo restores the pre-stamp patch byte-exact (LIFO, two entries).
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

#include <cstdio>
#include <filesystem>
#include <functional>
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

/// Full state of one cell: the four voxel buffers plus the game height
/// (SGetAlt, 1/32 m units).
struct CellState
{
    unsigned char geo = 0;
    unsigned char dam = 0;
    unsigned char atr = 0;
    unsigned char sur = 0;
    int height = 0;

    static CellState read(int x, int y)
    {
        const int off = mapOffset(x, y);
        CellState cell;
        cell.geo = vMap.VxGBuf[off];
        cell.dam = vMap.VxDBuf[off];
        cell.atr = vMap.AtrBuf[off];
        cell.sur = vMap.SurBuf[off];
        cell.height = static_cast<int>(vMap.SGetAlt(x, y));
        return cell;
    }

    bool equals(const CellState& other) const
    {
        return geo == other.geo && dam == other.dam && atr == other.atr &&
               sur == other.sur && height == other.height;
    }
};

/// Per-cell state of a stamp bbox, row-major ((2r+1) x (2r+1), dy outer).
struct StampState
{
    int radius = 0;
    std::vector<CellState> cells;

    static StampState read(int stampX, int stampY, int r)
    {
        StampState state;
        state.radius = r;
        state.cells.reserve(static_cast<std::size_t>(2 * r + 1) * (2 * r + 1));
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                state.cells.push_back(CellState::read(stampX + dx, stampY + dy));
            }
        }
        return state;
    }

    const CellState& at(int dx, int dy) const
    {
        const int side = 2 * radius + 1;
        return cells[static_cast<std::size_t>(dy + radius) * side +
                     (dx + radius)];
    }
};

bool maskCell(const MapEditor::BrushMask& mask, int dx, int dy)
{
    return mask.influence[static_cast<std::size_t>(dy - mask.y) * mask.sx +
                          (dx - mask.x)] != 0;
}

/// Find a stamp centre whose radius bbox satisfies the predicate on every
/// cell (map-cell coordinates, no torus wrap).
bool findStampArea(int mapW, int mapH, int radius,
                   const std::function<bool(int, int)>& predicate,
                   int& outCx, int& outCy)
{
    const int pad = radius + 8;
    for (int cy = pad; cy < mapH - pad; cy += 24) {
        for (int cx = pad; cx < mapW - pad; cx += 24) {
            bool ok = true;
            for (int dy = -radius; dy <= radius && ok; ++dy) {
                for (int dx = -radius; dx <= radius && ok; ++dx) {
                    ok = predicate(cx + dx, cy + dy);
                }
            }
            if (ok) {
                outCx = cx;
                outCy = cy;
                return true;
            }
        }
    }
    return false;
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

/// Byte-exact comparison of a stamp bbox against a saved state.
bool checkStampEquals(const StampState& want, int stampX, int stampY,
                      const char* what)
{
    for (int dy = -want.radius; dy <= want.radius; ++dy) {
        for (int dx = -want.radius; dx <= want.radius; ++dx) {
            const CellState& w = want.at(dx, dy);
            const CellState got = CellState::read(stampX + dx, stampY + dy);
            TEST_REQUIRE(got.equals(w),
                         "%s: cell (%d,%d) must stay byte-exact: "
                         "want G=%d D=%d A=%02X S=%d H=%d | got G=%d D=%d "
                         "A=%02X S=%d H=%d.",
                         what, dx, dy, w.geo, w.dam, w.atr, w.sur, w.height,
                         got.geo, got.dam, got.atr, got.sur, got.height);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Test 86: one stamp raises a low geo-only patch to the zeroplast level
// ---------------------------------------------------------------------------

bool runBrushZeroplastGeoTests()
{
    MapEditor::EditorSession session;
    const bool loaded =
        prepareBrushTestSession("load_root_zp_geo", session);
    TEST_REQUIRE(loaded,
                 "the zeroplast geo test could not load the temporary "
                 "world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the zeroplast geo test.");

    const int radius = 2;
    const int hZP = static_cast<int>(vMap.hZeroPlast);
    const int target = hZP << VX_FRACTION;
    // The patch sits 40 whole voxels (1280 fraction units) below the
    // zeroplast level — far beyond any tiny strength step.
    const int geoWhole = hZP - 40;
    TEST_REQUIRE(geoWhole >= 1,
                 "the world's hZeroPlast (%d) is too low for the test patch.",
                 hZP);

    int cx = 0;
    int cy = 0;
    const bool found = findStampArea(
        mapW, mapH, radius + 8,
        [](int x, int y) {
            const int off = mapOffset(x, y);
            return vMap.VxDBuf[off] == 0 &&
                   (vMap.AtrBuf[off] & At_ZPMASK) != At_ZEROPLAST;
        },
        cx, cy);
    TEST_REQUIRE(found,
                 "no dam-free, zeroplast-free stamp area was found.");
    std::printf("Zeroplast geo test stamp area around (%d, %d)\n", cx, cy);

    // The patch: dam-free geo 40 voxels under the level, fraction bits of
    // the shared AtrBuf assorted so the cells cannot converge by accident.
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int off = mapOffset(cx + dx, cy + dy);
            vMap.VxGBuf[off] = static_cast<unsigned char>(geoWhole);
            vMap.VxDBuf[off] = 0;
            vMap.AtrBuf[off] = static_cast<unsigned char>(
                (dx + radius) & VX_FRACTION_MASK);
            vMap.SurBuf[off] = vMap.GetGeoType(off, geoWhole << VX_FRACTION);
        }
    }

    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = radius;
    params.strength = 5;        // tiny step — the old brush needed ~25 stamps
    params.mode = BrushMode::Dam;
    params.effect = BrushEffect::Align4ZP;
    params.direction = 1;       // alignment tools ignore the direction

    const MapEditor::BrushMask mask =
        MapEditor::BrushMasks::filledCircle(radius);
    const StampState pre = StampState::read(cx, cy, radius);
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            TEST_REQUIRE(pre.at(dx, dy).height < target,
                         "the pre-stamp patch must sit below the level.");
        }
    }

    // ------------------------------------------------------------------
    // 1: one stamp sets every dam-free cell exactly to hZeroPlast and
    // marks it At_ZEROPLAST (strength = 5 would have stepped it there over
    // ~25 stamps in the old incremental math).
    // ------------------------------------------------------------------
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the Align4ZP stamp over the geo patch was refused.");
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (!maskCell(mask, dx, dy)) {
                continue;
            }
            const int px = cx + dx;
            const int py = cy + dy;
            const CellState before = pre.at(dx, dy);
            const CellState post = CellState::read(px, py);

            TEST_REQUIRE(post.height == target,
                         "top at (%d,%d): expected the zeroplast level "
                         "%d, got %d (started at %d).",
                         dx, dy, target, post.height, before.height);
            TEST_REQUIRE(post.dam == static_cast<unsigned char>(hZP),
                         "the seeded dam layer at (%d,%d) must sit on "
                         "hZeroPlast.", dx, dy);
            TEST_REQUIRE((post.atr & VX_FRACTION_MASK) == 0,
                         "the fraction bits at (%d,%d) must be zero on the "
                         "whole zeroplast level.", dx, dy);
            TEST_REQUIRE((post.atr & At_ZPMASK) == At_ZEROPLAST,
                         "cells that reached the level must be marked "
                         "At_ZEROPLAST at (%d,%d).", dx, dy);
            TEST_REQUIRE(post.geo == before.geo,
                         "the geo layer bytes at (%d,%d) must not change in "
                         "Dam mode.", dx, dy);
            TEST_REQUIRE(post.sur == vMap.veryLightDam,
                         "the seeded dam surface at (%d,%d) must be the "
                         "very light dam material.", dx, dy);
        }
    }
    std::printf("[PASS] Align4ZP raises a low geo patch to hZeroPlast and "
                "marks it zeroplast in one stamp.\n");

    const StampState afterFirst = StampState::read(cx, cy, radius);

    // ------------------------------------------------------------------
    // 2: a second stamp is a no-op — the cells are zeroplast now.
    // ------------------------------------------------------------------
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the second Align4ZP stamp was refused.");
    TEST_REQUIRE(checkStampEquals(afterFirst, cx, cy,
                                  "second zeroplast stamp"),
                 "the second stamp must leave the zeroplast cells untouched.");
    std::printf("[PASS] zeroplast stamp is idempotent.\n");

    // ------------------------------------------------------------------
    // 3: undo (LIFO) restores the first stamp, then the whole patch.
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.undo(), "the second zeroplast stamp undo failed.");
    TEST_REQUIRE(checkStampEquals(afterFirst, cx, cy,
                                  "undo of the second stamp"),
                 "undo must restore the state after the first stamp.");
    TEST_REQUIRE(session.undo(), "the first zeroplast stamp undo failed.");
    TEST_REQUIRE(checkStampEquals(pre, cx, cy, "undo of the first stamp"),
                 "undo must restore the pre-stamp patch byte-exact.");
    TEST_REQUIRE(!session.canUndo(),
                 "the two stamps must empty the undo side.");
    std::printf("[PASS] zeroplast geo stamp undoes byte-exact.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush zeroplast reaches over geo in one stamp", 86,
                         runBrushZeroplastGeoTests);

// ============================================================================
// T3.2 — height brushes: the two alignment tools and the Geo/Dam brush modes.
//
// Test 59 (align):
//   * Align4ZP and AlignVariableH route through Terra's own alignment
//     toolzers; every mask cell levels exactly per the Terra math
//     (raise capped at the target, lower attenuated by the GRIDAT hardness
//     the same integer way tVoxSet does, both capped at the target);
//   * cells that reach the target carry At_LEVELED in the At_ZPMASK bits,
//     cells short of it lose the old attribute bits (Terra overwrites
//     AtrBuf with the fraction);
//   * cells marked At_ZEROPLAST are left byte-exact (game behaviour);
//   * each align application is one undo entry (byte-exact restore).
//
// Test 60 (modes):
//   * Geo mode raises VxGBuf only: VxDBuf stays 0, SurBuf re-derives via
//     GetGeoType, AtrBuf carries the fraction bits;
//   * Dam mode raises a dam-free cell by seeding VxDBuf = geo height and
//     SurBuf = veryLightDam (the T2TE_PLOTTING_VERY_LIGHT_DAM writes) and
//     removes the dam when the surface is lowered to the geo height;
//   * Geo mode over an existing dam is capped by the dam height (dam >= geo);
//   * undo restores the stamp regions byte-exact (LIFO).
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

/// Per-cell pre-state of a stamp bbox, row-major ((2r+1) x (2r+1), dy outer).
struct StampPre
{
    int radius = 0;
    std::vector<CellState> cells;

    static StampPre read(int stampX, int stampY, int r)
    {
        StampPre pre;
        pre.radius = r;
        pre.cells.reserve(static_cast<std::size_t>(2 * r + 1) * (2 * r + 1));
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                pre.cells.push_back(CellState::read(stampX + dx, stampY + dy));
            }
        }
        return pre;
    }

    const CellState& at(int dx, int dy) const
    {
        const int side = 2 * radius + 1;
        return cells[static_cast<std::size_t>(dy + radius) * side +
                     (dx + radius)];
    }
};

/// Byte blocks of the four buffers over a rectangle (for the undo checks).
struct RegionBytes
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<unsigned char> geo;
    std::vector<unsigned char> dam;
    std::vector<unsigned char> atr;
    std::vector<unsigned char> sur;

    static RegionBytes read(int rx, int ry, int rw, int rh)
    {
        RegionBytes region;
        region.x = rx;
        region.y = ry;
        region.w = rw;
        region.h = rh;
        const std::size_t cells = static_cast<std::size_t>(rw) * rh;
        region.geo.resize(cells);
        region.dam.resize(cells);
        region.atr.resize(cells);
        region.sur.resize(cells);
        std::size_t cnt = 0;
        for (int j = 0; j < rh; ++j) {
            for (int i = 0; i < rw; ++i, ++cnt) {
                const int off = mapOffset(rx + i, ry + j);
                region.geo[cnt] = vMap.VxGBuf[off];
                region.dam[cnt] = vMap.VxDBuf[off];
                region.atr[cnt] = vMap.AtrBuf[off];
                region.sur[cnt] = vMap.SurBuf[off];
            }
        }
        return region;
    }
};

/// Terra tVoxSet (tools.h:488-495) hardness attenuation for dV < 0,
/// replicated verbatim for the expected-value math: integer ops in the
/// order `dV = dV * (hardness << hardness) / 24`.
int hardnessAttenuation(int x, int y, int dV)
{
    if (dV >= 0) {
        return dV;
    }
    const int hrd = vMap.GABuf[vMap.offsetBuf2offsetGBuf(mapOffset(x, y))] &
                    GRIDAT_MASK_HARDNESS;
    return (dV * (hrd << hrd)) / 24;
}

/// The full-power per-stamp delta of a brush (influence 255). Magnitude only.
int fullStampDelta(const BrushParams& params)
{
    return MapEditor::BrushTool::heightDeltaMagnitude(params, 255);
}

bool cellIsZeroplast(int x, int y)
{
    return (vMap.AtrBuf[mapOffset(x, y)] & At_ZPMASK) == At_ZEROPLAST;
}

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
/// The caller aborts via TEST_REQUIRE(!ok) when this returns false.
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
// Shared alignment stamp check: every mask cell must follow the exact Terra
// alignment math towards `target` (raise capped at it, hardness-attenuated
// lower capped at it). `skip` marks cells checked byte-exactly instead
// (At_ZEROPLAST cells that Terra refuses to touch).
// ---------------------------------------------------------------------------

bool checkAlignStamp(const MapEditor::BrushMask& mask, int stampX, int stampY,
                     int radius, int magnitude, int target,
                     const StampPre& pre,
                     const std::function<bool(int, int)>& skip = nullptr)
{
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (!maskCell(mask, dx, dy)) {
                continue;
            }
            const int px = stampX + dx;
            const int py = stampY + dy;
            const CellState& before = pre.at(dx, dy);
            const CellState post = CellState::read(px, py);

            if (skip && skip(dx, dy)) {
                // Terra returns early: nothing written at all.
                TEST_REQUIRE(post.equals(before),
                             "a skipped align cell (%d,%d) must stay "
                             "byte-exact.", dx, dy);
                continue;
            }

            // Terra's alignment paths (tools.h:545-631):
            //   dV = +magnitude when below the target      -> capped at it;
            //   dV = -magnitude scaled by GRIDAT hardness  -> capped at it;
            //   exactly at the target                      -> no height change,
            //                                                 At_LEVELED marking.
            // All moving paths re-derive SurBuf via GetGeoType for geo-layer
            // cells and rebuild AtrBuf from the fraction bits. This check
            // runs in a dam-free area, so cells stay on the geo layer.
            int expected = before.height;
            bool changed = false;
            if (before.height < target) {
                expected = std::min(before.height + magnitude, target);
            } else if (before.height > target) {
                const int dV = hardnessAttenuation(px, py, -magnitude);
                if (dV != 0) {
                    expected = std::max(before.height + dV, target);
                }
            }
            changed = expected != before.height;

            if (changed) {
                TEST_REQUIRE(post.height == expected,
                             "align height at stamp offset (%d,%d): expected "
                             "%d, got %d.",
                             dx, dy, expected, post.height);
                TEST_REQUIRE(post.dam == before.dam,
                             "align must not touch VxDBuf in a dam-free "
                             "area.");
                TEST_REQUIRE(post.geo ==
                                 static_cast<unsigned char>(expected >> VX_FRACTION),
                             "align must route through VxGBuf in Geo mode.");
                TEST_REQUIRE(post.sur == vMap.GetGeoType(mapOffset(px, py),
                                                        expected),
                             "align must re-derive SurBuf == GetGeoType().");
                if (expected == target) {
                    TEST_REQUIRE((post.atr & At_ZPMASK) == At_LEVELED,
                                 "cells that reach the target must be marked "
                                 "At_LEVELED.");
                } else {
                    TEST_REQUIRE((post.atr & At_ZPMASK) == 0,
                                 "cells short of the target must lose the "
                                 "At_ZPMASK bits (AtrBuf := fraction only).");
                }
                TEST_REQUIRE((post.atr & VX_FRACTION_MASK) ==
                                 (expected & VX_FRACTION_MASK),
                             "the fraction bits must hold the new height.");
            } else {
                TEST_REQUIRE(post.height == before.height &&
                                 post.dam == before.dam &&
                                 post.geo == before.geo,
                             "a no-op align cell (%d,%d) must keep its "
                             "layers.", dx, dy);
                if (before.height == target) {
                    // tVoxSetAll marks v == hAppr with At_LEVELED even when
                    // the height itself does not move.
                    TEST_REQUIRE((post.atr & At_ZPMASK) == At_LEVELED,
                                 "a cell already at the target must be "
                                 "marked At_LEVELED.");
                }
            }
        }
    }
    return true;
}

/// Byte-exact undo check of a stamp region against a StampPre snapshot.
bool checkUndoRestores(const StampPre& pre, int stampX, int stampY)
{
    const int side = 2 * pre.radius + 1;
    const RegionBytes restored = RegionBytes::read(
        stampX - pre.radius, stampY - pre.radius, side, side);
    std::size_t cnt = 0;
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i, ++cnt) {
            const CellState& want = pre.cells[cnt];
            if (restored.geo[cnt] == want.geo && restored.dam[cnt] == want.dam &&
                restored.atr[cnt] == want.atr && restored.sur[cnt] == want.sur) {
                continue;
            }
            TEST_REQUIRE(false,
                         "undo must restore align cell (%d,%d) byte-exact: "
                         "want G=%d D=%d A=%02X S=%d | got G=%d D=%d A=%02X "
                         "S=%d.",
                         i - pre.radius, j - pre.radius,
                         want.geo, want.dam, want.atr, want.sur,
                         restored.geo[cnt], restored.dam[cnt],
                         restored.atr[cnt], restored.sur[cnt]);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Test 59: the alignment tools (Align4ZP / AlignVariableH)
// ---------------------------------------------------------------------------

bool runBrushAlignTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_align", session);
    TEST_REQUIRE(loaded, "the align test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the align test.");

    const int radius = 4;
    const int targetZP = static_cast<int>(vMap.hZeroPlast) << VX_FRACTION;

    // The AlignVariableH target must differ from the zeroplast height to
    // prove the two tools do not share the same level.
    const int customLevel = MapEditor::BrushTool::clampHeightLevel(
        static_cast<int>(vMap.hZeroPlast) + 5);
    TEST_REQUIRE((customLevel << VX_FRACTION) != targetZP,
                 "the custom align level must differ from the zeroplast one.");

    // One clean (dam-free, zeroplast-free) bbox holding two stamp areas
    // 16 cells apart so the radius-4 stamps never overlap.
    int cx = 0;
    int cy = 0;
    const bool found = findStampArea(
        mapW, mapH, radius + 12,
        [](int x, int y) {
            const int off = mapOffset(x, y);
            return vMap.VxDBuf[off] == 0 && !cellIsZeroplast(x, y);
        },
        cx, cy);
    TEST_REQUIRE(found,
                 "no dam-free, zeroplast-free stamp area was found.");
    std::printf("Align test stamp areas around (%d, %d)\n", cx, cy);

    MapEditor::BrushTool brush(session);

    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = radius;
    params.strength = 100;      // full delta = 3200 = 100 voxels per stamp
    params.mode = BrushMode::Geo;
    params.direction = 1;       // alignment tools must ignore the direction
    params.heightLevel = customLevel;

    const int magnitude = fullStampDelta(params);
    TEST_REQUIRE(magnitude == (100 << VX_FRACTION),
                 "the full stamp delta must equal strength<<VX_FRACTION.");

    const MapEditor::BrushMask mask =
        MapEditor::BrushMasks::filledCircle(radius);

    // ------------------------------------------------------------------
    // 1.1: Align4ZP — raise below the zeroplast level (capped there), keep
    // above it, and leave At_ZEROPLAST cells byte-exact.
    // ------------------------------------------------------------------
    {
        params.effect = BrushEffect::Align4ZP;

        // Mark one non-centre mask cell on the mask edge as zeroplast
        // (fraction bits kept). recordUndoArea() runs later (inside
        // applyStamp), so the marking is part of the snapshot the undo
        // restores — see the undo check below.
        const int zpx = cx + radius;
        const int zpy = cy;
        {
            const int off = mapOffset(zpx, zpy);
            vMap.AtrBuf[off] = static_cast<unsigned char>(
                (vMap.AtrBuf[off] & ~At_ZPMASK) | At_ZEROPLAST);
        }

        const StampPre pre = StampPre::read(cx, cy, radius);
        TEST_REQUIRE((pre.at(radius, 0).atr & At_ZPMASK) == At_ZEROPLAST,
                     "the zeroplast marking did not land on a mask cell.");

        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the Align4ZP stamp was refused.");
        const CellState zpMarked = pre.at(radius, 0);
        const CellState zpAfter = CellState::read(zpx, zpy);
        TEST_REQUIRE(zpAfter.equals(zpMarked),
                     "At_ZEROPLAST cells must be skipped byte-exact.");
        TEST_REQUIRE(checkAlignStamp(
                         mask, cx, cy, radius, magnitude, targetZP, pre,
                         [&](int dx, int dy) {
                             return dx == radius && dy == 0;
                         }),
                     "Align4ZP cell-level math failed.");

        TEST_REQUIRE(session.undo(), "Align4ZP undo failed.");
        TEST_REQUIRE(checkUndoRestores(pre, cx, cy),
                     "the align region must undo byte-exact.");
        TEST_REQUIRE(!session.canUndo(),
                     "the single align stamp must empty the undo side.");
    }
    std::printf("[PASS] Align4ZP caps at hZeroPlast and is one undo entry.\n");

    // ------------------------------------------------------------------
    // 1.2: AlignVariableH — the same math to the explicit custom level,
    // which is deliberately different from hZeroPlast.
    // ------------------------------------------------------------------
    {
        params.effect = BrushEffect::AlignVariableH;
        const int stampX = cx + 16;
        const int stampY = cy;
        const int target = customLevel << VX_FRACTION;

        const StampPre pre = StampPre::read(stampX, stampY, radius);
        TEST_REQUIRE(brush.applyStamp(stampX, stampY, params),
                     "the AlignVariableH stamp was refused.");
        TEST_REQUIRE(checkAlignStamp(mask, stampX, stampY, radius,
                                     magnitude, target, pre),
                     "AlignVariableH cell-level math failed.");
        TEST_REQUIRE(session.undo(), "AlignVariableH undo failed.");
        TEST_REQUIRE(checkUndoRestores(pre, stampX, stampY),
                     "the variable-level region must undo byte-exact.");
    }
    std::printf("[PASS] AlignVariableH levels to the custom height.\n");

    // ------------------------------------------------------------------
    // 1.3: params clamping — an out-of-range level is clamped into
    // [0, MAX_VX_HEIGHT_WHOLE] (the UI spinbox domain).
    // ------------------------------------------------------------------
    TEST_REQUIRE(MapEditor::BrushTool::clampHeightLevel(-10) == 0,
                 "the height level must clamp at 0.");
    TEST_REQUIRE(MapEditor::BrushTool::clampHeightLevel(4000) ==
                     MAX_VX_HEIGHT_WHOLE,
                 "the height level must clamp at MAX_VX_HEIGHT_WHOLE.");
    BrushParams wild;
    wild.heightLevel = -1;
    brush.setParams(wild);
    TEST_REQUIRE(brush.params().heightLevel == 0,
                 "setParams must clamp the height level.");
    std::printf("[PASS] heightLevel clamps hold for wild values.\n");

    return true;
}

// ---------------------------------------------------------------------------
// Test 60: the Geo/Dam brush modes of ChangeHeight
// ---------------------------------------------------------------------------

bool runBrushModeTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_modes", session);
    TEST_REQUIRE(loaded, "the mode test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the mode test.");

    const int radius = 2;
    // Clean area: no dam, no zeroplast, fully diggable (hardness 3, so the
    // lower-stamp math stays exact) and with raise headroom. Must hold three
    // radius-2 stamps at (-8,-8), (+8,0) and (-8,+8) without overlaps.
    int cx = 0;
    int cy = 0;
    const bool found = findStampArea(
        mapW, mapH, radius + 12,
        [](int x, int y) {
            if (vMap.VxDBuf[mapOffset(x, y)] != 0 || cellIsZeroplast(x, y)) {
                return false;
            }
            const int hrd = vMap.GABuf[vMap.offsetBuf2offsetGBuf(
                mapOffset(x, y))] & GRIDAT_MASK_HARDNESS;
            if (hrd != GRIDAT_MASK_HARDNESS) {
                return false;
            }
            return static_cast<int>(vMap.SGetAlt(x, y)) <=
                   MAX_VX_HEIGHT - 2048;
        },
        cx, cy);
    TEST_REQUIRE(found, "no clean diggable stamp area was found.");
    std::printf("Mode test stamp areas around (%d, %d)\n", cx, cy);

    MapEditor::BrushTool brush(session);

    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = radius;
    params.effect = BrushEffect::ChangeHeight;

    const MapEditor::BrushMask mask =
        MapEditor::BrushMasks::filledCircle(radius);
    const int side = 2 * radius + 1;

    // ------------------------------------------------------------------
    // 2.1: A — Geo mode raise: VxDBuf stays empty, VxGBuf grows by
    // strength voxels, SurBuf re-derives, AtrBuf holds the fraction.
    // ------------------------------------------------------------------
    {
        params.mode = BrushMode::Geo;
        params.direction = 1;
        params.strength = 4;                        // delta = 128 per cell
        const int delta = fullStampDelta(params);
        TEST_REQUIRE(delta == (4 << VX_FRACTION),
                     "the Geo raise delta must equal strength<<VX_FRACTION.");

        const int stampX = cx - 8;
        const int stampY = cy - 8;
        const StampPre pre = StampPre::read(stampX, stampY, radius);
        TEST_REQUIRE(brush.applyStamp(stampX, stampY, params),
                     "the Geo raise stamp was refused.");

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (!maskCell(mask, dx, dy)) {
                    continue;
                }
                const int px = stampX + dx;
                const int py = stampY + dy;
                const CellState& preCell = pre.at(dx, dy);
                const CellState post = CellState::read(px, py);
                const int expected = preCell.height + delta;
                TEST_REQUIRE(post.height == expected,
                             "Geo raise height at (%d,%d): expected %d, "
                             "got %d.", dx, dy, expected, post.height);
                TEST_REQUIRE(post.dam == 0,
                             "Geo mode must never create a dam layer.");
                TEST_REQUIRE(post.geo ==
                                 static_cast<unsigned char>(expected >> VX_FRACTION),
                             "Geo mode must raise through VxGBuf.");
                TEST_REQUIRE(post.sur == vMap.GetGeoType(mapOffset(px, py),
                                                        expected),
                             "Geo mode must re-derive SurBuf == GetGeoType().");
                TEST_REQUIRE((post.atr & VX_FRACTION_MASK) ==
                                 (expected & VX_FRACTION_MASK),
                             "Geo mode must carry the fraction bits.");
            }
        }
        std::printf("[PASS] Geo raise drives VxGBuf only.\n");
    }

    // ------------------------------------------------------------------
    // 2.2: B — Dam mode raise over dam-free cells seeds the dam layer
    // exactly like T2TE_PLOTTING_VERY_LIGHT_DAM, then a strength-100 lower
    // removes it again when the surface falls to the geo height. Both
    // applications undo back-to-back (LIFO).
    // ------------------------------------------------------------------
    {
        params.mode = BrushMode::Dam;
        params.direction = 1;
        params.strength = 4;
        const int delta = fullStampDelta(params);

        const int stampX = cx + 8;
        const int stampY = cy;
        const StampPre preB = StampPre::read(stampX, stampY, radius);
        TEST_REQUIRE(brush.applyStamp(stampX, stampY, params),
                     "the Dam raise stamp was refused.");

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (!maskCell(mask, dx, dy)) {
                    continue;
                }
                const int px = stampX + dx;
                const int py = stampY + dy;
                const CellState& pre = preB.at(dx, dy);
                const CellState post = CellState::read(px, py);
                const int expected = pre.height + delta;
                TEST_REQUIRE(post.height == expected,
                             "Dam raise height at (%d,%d): expected %d, "
                             "got %d.", dx, dy, expected, post.height);
                TEST_REQUIRE(post.geo == pre.geo,
                             "the seeded dam must not touch the geo height.");
                TEST_REQUIRE(post.dam ==
                                 static_cast<unsigned char>(expected >> VX_FRACTION),
                             "the seeded dam must start at the geo height "
                             "plus the raise delta.");
                TEST_REQUIRE(post.sur == vMap.veryLightDam,
                             "the seeded dam surface must be veryLightDam "
                             "(T2TE_PLOTTING_VERY_LIGHT_DAM writes).");
            }
        }
        std::printf("[PASS] Dam raise seeds VxDBuf with veryLightDam.\n");

        // Lower with a full-power stamp: the seeded dam (4 voxels above the
        // geo) falls below the geo height -> dam removal, the geo layer
        // takes over exactly at the fallen surface.
        params.direction = -1;
        params.strength = 100;                      // delta = 3200
        const int lowerDelta = fullStampDelta(params);
        TEST_REQUIRE(brush.applyStamp(stampX, stampY, params),
                     "the Dam lower stamp was refused.");

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (!maskCell(mask, dx, dy)) {
                    continue;
                }
                const int px = stampX + dx;
                const int py = stampY + dy;
                const CellState& pre = preB.at(dx, dy);
                const CellState post = CellState::read(px, py);
                // The exact Terra sequence: seed (height := geo + 128) then
                // the toolzer lower with the hardness-attenuated delta.
                const int afterRaise = pre.height + delta;
                const int dV = hardnessAttenuation(px, py, -lowerDelta);
                const int geoLevel =
                    (static_cast<int>(pre.geo) << VX_FRACTION) |
                    (pre.atr & VX_FRACTION_MASK);
                const int lowered = afterRaise + dV;
                // v <= geoLevel removes the dam; the result then clamps to
                // MIN_VX_HEIGHT (Terra writes v >= 0 after the removal).
                TEST_REQUIRE(lowered <= geoLevel,
                             "the lower stamp must dig through the seeded "
                             "dam at hardness 3.");
                const int expected = std::max(0, lowered);
                TEST_REQUIRE(post.dam == 0,
                             "the dam must be removed when the surface "
                             "falls to the geo height.");
                TEST_REQUIRE(post.height == expected,
                             "Dam lower height at (%d,%d): expected %d, "
                             "got %d.", dx, dy, expected, post.height);
                TEST_REQUIRE(post.sur == vMap.GetGeoType(mapOffset(px, py),
                                                        expected),
                             "the dam removal must re-derive SurBuf from "
                             "the geo layer.");
                TEST_REQUIRE((post.atr & VX_FRACTION_MASK) ==
                                 (expected & VX_FRACTION_MASK),
                             "the fraction bits must hold the new height.");
            }
        }
        std::printf("[PASS] lowering below geo removes the dam layer.\n");

        TEST_REQUIRE(session.undo() && session.undo(),
                     "the dam raise/lower must be two undo entries.");
        TEST_REQUIRE(checkUndoRestores(preB, stampX, stampY),
                     "undo must restore the dam region byte-exact.");
    }

    // ------------------------------------------------------------------
    // 2.3: C — Geo mode over an existing (crafted) dam is capped by the
    // dam height; the dam buffer itself is untouched.
    // ------------------------------------------------------------------
    {
        params.mode = BrushMode::Geo;
        params.direction = 1;
        params.strength = 4;
        const int delta = fullStampDelta(params);

        const int stampX = cx - 8;
        const int stampY = cy + 8;
        const StampPre preC = StampPre::read(stampX, stampY, radius);

        // Craft a dam two voxels above the geo on every mask cell. The
        // crafting happens BEFORE the brush snapshot inside applyStamp, so
        // the undo restores the crafted-dam state — compare against it, not
        // against pre-craft geo-only state.
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (!maskCell(mask, dx, dy)) {
                    continue;
                }
                const int off = mapOffset(stampX + dx, stampY + dy);
                vMap.VxDBuf[off] = static_cast<unsigned char>(
                    vMap.VxGBuf[off] + 2);
            }
        }
        const StampPre crafted = StampPre::read(stampX, stampY, radius);

        TEST_REQUIRE(brush.applyStamp(stampX, stampY, params),
                     "the capped Geo raise stamp was refused.");
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (!maskCell(mask, dx, dy)) {
                    continue;
                }
                const int px = stampX + dx;
                const int py = stampY + dy;
                const CellState& pre = preC.at(dx, dy);
                const CellState post = CellState::read(px, py);
                const int ceiling =
                    ((static_cast<int>(pre.geo) + 2) << VX_FRACTION) |
                    (pre.atr & VX_FRACTION_MASK);
                TEST_REQUIRE(ceiling < pre.height + delta,
                             "the crafted dam must be lower than the "
                             "uncapped raise would go.");
                TEST_REQUIRE(post.height == ceiling,
                             "Geo raise over a dam must cap at the dam "
                             "height: expected %d, got %d.", ceiling,
                             post.height);
                TEST_REQUIRE(post.dam ==
                                 static_cast<unsigned char>(pre.geo + 2),
                             "the dam buffer must stay untouched in Geo "
                             "mode.");
                TEST_REQUIRE(post.sur == vMap.GetGeoType(mapOffset(px, py),
                                                        ceiling),
                             "SurBuf must reflect the capped geo height.");
            }
        }
        std::printf("[PASS] Geo raise is capped by the dam height.\n");

        TEST_REQUIRE(session.undo(),
                     "the capped stamp must be one undo entry.");
        TEST_REQUIRE(checkUndoRestores(crafted, stampX, stampY),
                     "undo must restore the crafted-dam state byte-exact.");
    }

    // The A-stamp entry is still in the history — LIFO holds to the end.
    TEST_REQUIRE(session.canUndo(),
                 "the Geo raise entry must still be in the history.");
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush align (T3.2)", 59, runBrushAlignTests);
MAP_EDITOR_REGISTER_TEST("Brush height modes (T3.2)", 60, runBrushModeTests);

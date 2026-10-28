// ============================================================================
// T3.2 — Dam mode alignment tools on geo cells: dam seeding before alignment.
//
// Test 61: Dam mode "Level to Height" (AlignVariableH) and "Level to
// Zeroplast" (Align4ZP) must seed a dam layer on geo cells before driving
// it — otherwise Terra's toolzer takes the geo branch (VD==0) and the brush
// moves the ground instead of building a dam at the chosen height.
//
// The check follows the exact Terra alignment math:
//   after seedDamLayer, VxDBuf = VxGBuf (dam at geo height);
//   the toolzer drives the dam layer:
//     AlignVariableH — absolute set: damTop = target in one stamp;
//     Align4ZP       — game-like step:
//                      damTop = min(geoTop + magnitude, target);
//   VxGBuf stays unchanged (the geo layer is owned by Geo mode);
//   SurBuf = veryLightDam (the seed writes the dam material);
//   undo restores the pre-stamp geo-only state byte-exact.
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

/// Apply a Dam-mode alignment stamp over geo cells and verify the seeded dam.
///
/// For every mask cell the expected result follows Terra's exact sequence:
///   1. seedDamLayer sets VxDBuf := VxGBuf, SurBuf := veryLightDam;
///   2. the toolzer (VD != 0 branch) drives the dam layer:
///        AlignVariableH — damTop = target (absolute set);
///        Align4ZP       — damTop = min(geoTop + magnitude, target) when
///                         geoTop < target, otherwise damTop = geoTop;
///   3. VxGBuf is unchanged (Geo mode owns it);
///   4. AtrBuf := damTop & VX_FRACTION_MASK (fraction bits of the dam top).
///
/// `isAlign4ZP` selects the At_ZEROPLAST marking of the cells that reach the
/// target; the absolute AlignVariableH toolzer writes only the fraction bits.
bool checkDamSeededStamp(const MapEditor::BrushMask& mask, int stampX,
                         int stampY, int radius, int magnitude, int target,
                         const StampPre& pre, bool isAlign4ZP)
{
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int px = stampX + dx;
            const int py = stampY + dy;
            const int off = mapOffset(px, py);
            const CellState& before = pre.at(dx, dy);
            const CellState post = CellState::read(px, py);

            TEST_REQUIRE(post.dam != 0,
                         "dam must be seeded on geo cell (%d,%d) in Dam mode.",
                         dx, dy);

            const int geoTop = before.height;
            // seedDamLayer: if VxGBuf == 0, the dam starts at voxel 1 (bump
            // of 1 << VX_FRACTION above geoTop), because VxDBuf == 0 means
            // "no dam" and a zero-height cell cannot carry one.
            const int effectiveTop = (before.geo == 0)
                ? (geoTop + (1 << VX_FRACTION))
                : geoTop;
            // AlignVariableH ("Level to Height") is an absolute set: one
            // stamp moves the seeded dam by the whole remaining distance.
            // Align4ZP keeps the game-like strength-scaled step.
            const int expectedDamTop = isAlign4ZP
                ? ((effectiveTop < target)
                       ? std::min(effectiveTop + magnitude, target)
                       : effectiveTop)
                : target;

            const int actualDamTop =
                (static_cast<int>(post.dam) << VX_FRACTION) +
                (post.atr & VX_FRACTION_MASK);

            TEST_REQUIRE(actualDamTop == expectedDamTop,
                         "dam top at (%d,%d): expected %d, got %d "
                         "(geoTop=%d magnitude=%d target=%d).",
                         dx, dy, expectedDamTop, actualDamTop,
                         geoTop, magnitude, target);

            TEST_REQUIRE(post.geo == before.geo,
                         "geo layer must not change in Dam mode at (%d,%d).",
                         dx, dy);

            TEST_REQUIRE(post.sur == vMap.veryLightDam,
                         "dam surface at (%d,%d) must be veryLightDam "
                         "(seedDamLayer writes).",
                         dx, dy);

            if (expectedDamTop == target && isAlign4ZP) {
                TEST_REQUIRE((post.atr & At_ZPMASK) == At_ZEROPLAST,
                             "cells at target must be At_ZEROPLAST "
                             "(Align4ZP) at (%d,%d).", dx, dy);
            } else {
                // AlignVariableH's Terra toolzer writes only the fraction
                // bits when it moves; it never sets At_LEVELED.
                TEST_REQUIRE((post.atr & At_ZPMASK) == 0,
                             "moving cells must have zero At_ZPMASK bits "
                             "(AtrBuf := fraction only) at (%d,%d).",
                             dx, dy);
            }
        }
    }
    return true;
}

/// Verify undo restores the geo-only pre-stamp state.
bool checkUndoRestores(const StampPre& pre, int stampX, int stampY)
{
    std::size_t cnt = 0;
    for (int dy = -pre.radius; dy <= pre.radius; ++dy) {
        for (int dx = -pre.radius; dx <= pre.radius; ++dx, ++cnt) {
            const CellState& want = pre.cells[cnt];
            const CellState got = CellState::read(stampX + dx, stampY + dy);
            TEST_REQUIRE(got.equals(want),
                         "undo must restore cell (%d,%d) byte-exact: "
                         "want G=%d D=%d A=%02X S=%d | got G=%d D=%d "
                         "A=%02X S=%d.",
                         dx, dy, want.geo, want.dam, want.atr, want.sur,
                         got.geo, got.dam, got.atr, got.sur);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Test 61: Dam mode alignment on geo cells (AlignVariableH + Align4ZP)
// ---------------------------------------------------------------------------

bool runBrushAlignDamOnGeoTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_align_dam", session);
    TEST_REQUIRE(loaded,
                 "the align dam on geo test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the align dam on geo test.");

    const int radius = 2;

    int cx = 0;
    int cy = 0;
    const bool found = findStampArea(
        mapW, mapH, radius + 12,
        [](int x, int y) {
            const int off = mapOffset(x, y);
            return vMap.VxDBuf[off] == 0 &&
                   (vMap.AtrBuf[off] & At_ZPMASK) != At_ZEROPLAST;
        },
        cx, cy);
    TEST_REQUIRE(found,
                 "no dam-free, zeroplast-free stamp area was found.");
    std::printf("Align dam on geo test stamp area around (%d, %d)\n", cx, cy);

    MapEditor::BrushTool brush(session);

    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = radius;
    params.strength = 100;      // full delta = 3200 = 100 voxels per stamp
    params.mode = BrushMode::Dam;

    const MapEditor::BrushMask mask =
        MapEditor::BrushMasks::filledCircle(radius);
    const int magnitude = MapEditor::BrushTool::heightDeltaMagnitude(params, 255);

    // ------------------------------------------------------------------
    // 1: AlignVariableH — Dam mode must seed the dam, then drive it to
    // min(geoTop + magnitude, target).
    // ------------------------------------------------------------------
    {
        const int customLevel = MapEditor::BrushTool::clampHeightLevel(
            static_cast<int>(vMap.hZeroPlast) + 5);
        const int target = customLevel << VX_FRACTION;
        params.effect = BrushEffect::AlignVariableH;
        params.heightLevel = customLevel;

        const int stampX = cx + 8;
        const int stampY = cy;
        const StampPre pre = StampPre::read(stampX, stampY, radius);

        TEST_REQUIRE(brush.applyStamp(stampX, stampY, params),
                     "the AlignVariableH stamp in Dam mode was refused.");

        TEST_REQUIRE(checkDamSeededStamp(mask, stampX, stampY, radius,
                                         magnitude, target, pre,
                                         /*isAlign4ZP=*/false),
                     "AlignVariableH Dam mode cell check failed.");
        std::printf("[PASS] AlignVariableH Dam mode seeds the dam and drives "
                    "it towards the target.\n");

        TEST_REQUIRE(session.undo(), "AlignVariableH Dam mode undo failed.");
        TEST_REQUIRE(checkUndoRestores(pre, stampX, stampY),
                     "undo must restore the geo-only state byte-exact.");
        TEST_REQUIRE(!session.canUndo(),
                     "the single stamp must empty the undo side.");
        std::printf("[PASS] AlignVariableH Dam mode undo restores geo-only.\n");
    }

    // ------------------------------------------------------------------
    // 2: Align4ZP — Dam mode must seed the dam, level it to hZeroPlast,
    // and mark At_ZEROPLAST cells that reach the target.
    // ------------------------------------------------------------------
    {
        const int targetZP = static_cast<int>(vMap.hZeroPlast) << VX_FRACTION;
        params.effect = BrushEffect::Align4ZP;

        const int stampX = cx - 8;
        const int stampY = cy;
        const StampPre pre = StampPre::read(stampX, stampY, radius);

        TEST_REQUIRE(brush.applyStamp(stampX, stampY, params),
                     "the Align4ZP stamp in Dam mode was refused.");

        TEST_REQUIRE(checkDamSeededStamp(mask, stampX, stampY, radius,
                                         magnitude, targetZP, pre,
                                         /*isAlign4ZP=*/true),
                     "Align4ZP Dam mode cell check failed.");
        std::printf("[PASS] Align4ZP Dam mode seeds the dam and marks "
                    "At_ZEROPLAST on target cells.\n");

        TEST_REQUIRE(session.undo(), "Align4ZP Dam mode undo failed.");
        TEST_REQUIRE(checkUndoRestores(pre, stampX, stampY),
                     "undo must restore the geo-only state byte-exact.");
        TEST_REQUIRE(!session.canUndo(),
                     "the single stamp must empty the undo side.");
        std::printf("[PASS] Align4ZP Dam mode undo restores geo-only.\n");
    }

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush align dam on geo (T3.2 Dam mode)", 78,
                         runBrushAlignDamOnGeoTests);

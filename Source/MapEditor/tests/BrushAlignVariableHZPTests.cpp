// ============================================================================
// T3.2 — AlignVariableH on zeroplast-marked dam cells (T3.2-ревизия fix).
//
// Problem: Terra's alignment toolzers (T2TE_ALIGNMENT_TERRAIN_VARIABLE_H and
// T2TE_ALIGNMENT_TERRAIN_4ZP) have an early-return gate:
//   `if ((AtrBuf & At_ZPMASK) == At_ZEROPLAST) return 0;`
// (tools.h:546, 590). A cell marked At_ZEROPLAST is silently skipped — the
// dam height never moves.
//
// Fix: when AlignVariableH (Level to Height) in Dam mode targets a height
// different from hZeroPlast, clear the ZP gate bit before calling the toolzer.
// The user-level rule: if the brush target doesn't match zeroplast height,
// the dam should be levelled to the target and the zeroplast mark removed.
//
// Test 79 checks:
//   1. AlignVariableH Dam mode on a zeroplast cell with target < hZeroPlast:
//      dam height drops to target, At_ZEROPLAST cleared.
//   2. AlignVariableH Dam mode on a zeroplast cell with target > hZeroPlast:
//      dam height rises to target, At_ZEROPLAST cleared.
//   3. AlignVariableH Dam mode with target == hZeroPlast: no change, mark kept.
//   4. Undo restores byte-exact in all cases.
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

/// Craft a single cell: geo height, dam height, surface index, zeroplast flag.
void craftCell(int x, int y, int geoVoxels, int damVoxels,
               unsigned char surIndex, bool isZeroPlast)
{
    const int off = mapOffset(x, y);
    vMap.VxGBuf[off] = static_cast<unsigned char>(geoVoxels);
    vMap.VxDBuf[off] = static_cast<unsigned char>(damVoxels);
    vMap.SurBuf[off] = surIndex;
    // Keep fraction bits zero for simplicity
    vMap.AtrBuf[off] = static_cast<unsigned char>(
        isZeroPlast ? At_ZEROPLAST : (vMap.AtrBuf[off] & ~At_ZPMASK));
}

/// Craft a (2r+1) x (2r+1) patch of cells.
void craftPatch(int cx, int cy, int radius, int geoVoxels, int damVoxels,
                unsigned char surIndex, bool isZeroPlast)
{
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            craftCell(cx + dx, cy + dy, geoVoxels, damVoxels, surIndex,
                      isZeroPlast);
        }
    }
}

/// Find a stamp centre with enough room around it (no edge clipping).
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

// ---------------------------------------------------------------------------
// Test 79: AlignVariableH on zeroplast-marked dam cells
// ---------------------------------------------------------------------------

bool runBrushAlignVariableHZPTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_vh_zp", session);
    TEST_REQUIRE(loaded,
                 "the AlignVariableH ZP test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the AlignVariableH ZP test.");

    const int radius = 2;

    // Find a clean area (no existing dam, no zeroplast).
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
    std::printf("AlignVariableH ZP test stamp area around (%d, %d)\n", cx, cy);

    MapEditor::BrushTool brush(session);

    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = radius;
    params.strength = 100; // full delta = 3200 = 100 voxels per stamp
    params.mode = BrushMode::Dam;

    // hZeroPlast from the loaded world.
    const int hZP = static_cast<int>(vMap.hZeroPlast);
    std::printf("hZeroPlast = %d voxels\n", hZP);

    // Craft a zeroplast-marked dam patch: dam at hZeroPlast, geo well below.
    const int geoBase = 30; // 30 voxels — far below hZeroPlast
    const int stampX1 = cx + 8;
    const int stampY1 = cy;
    craftPatch(stampX1, stampY1, radius, geoBase, hZP, vMap.veryLightDam,
               true);

    // =========================================================================
    // 1: AlignVariableH — target BELOW hZeroPlast: dam drops, ZP mark removed.
    // =========================================================================
    {
        const int targetLevel = MapEditor::BrushTool::clampHeightLevel(
            hZP - 20); // 20 voxels below zeroplast
        const int target = targetLevel << VX_FRACTION;
        params.effect = BrushEffect::AlignVariableH;
        params.heightLevel = targetLevel;

        const StampPre pre = StampPre::read(stampX1, stampY1, radius);

        TEST_REQUIRE(brush.applyStamp(stampX1, stampY1, params),
                     "the AlignVariableH stamp was refused.");

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int px = stampX1 + dx;
                const int py = stampY1 + dy;
                const int off = mapOffset(px, py);
                const CellState& before = pre.at(dx, dy);
                const CellState post = CellState::read(px, py);

                // Dam height: AlignVariableH is an absolute set, so the dam
                // drops exactly to the target in one stamp.
                const int expectedDamTop = target;
                const int actualDamTop =
                    (static_cast<int>(post.dam) << VX_FRACTION) +
                    (post.atr & VX_FRACTION_MASK);

                TEST_REQUIRE(actualDamTop == expectedDamTop,
                             "dam top at (%d,%d): expected %d, got %d "
                             "(targetLevel=%d target=%d magnitude=%d).",
                             dx, dy, expectedDamTop, actualDamTop,
                             targetLevel, target, 100 << VX_FRACTION);

                // Dam must still exist (target > geoBase).
                TEST_REQUIRE(post.dam != 0,
                             "dam must still exist at (%d,%d) after lowering.",
                             dx, dy);

                // Geo must not change in Dam mode.
                TEST_REQUIRE(post.geo == before.geo,
                             "geo must not change in Dam mode at (%d,%d).",
                             dx, dy);

                // At_ZEROPLAST must be cleared.
                TEST_REQUIRE((post.atr & At_ZPMASK) != At_ZEROPLAST,
                             "At_ZEROPLAST must be cleared at (%d,%d).", dx,
                             dy);
            }
        }
        std::printf("[PASS] AlignVariableH Dam mode drops dam and clears ZP "
                    "mark.\n");

        // Undo restores byte-exact.
        TEST_REQUIRE(session.undo(), "Undo failed.");
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int px = stampX1 + dx;
                const int py = stampY1 + dy;
                const CellState& want = pre.at(dx, dy);
                const CellState got = CellState::read(px, py);
                TEST_REQUIRE(got.equals(want),
                             "undo must restore cell (%d,%d) byte-exact: "
                             "want G=%d D=%d A=%02X S=%d | got G=%d D=%d "
                             "A=%02X S=%d.",
                             dx, dy, want.geo, want.dam, want.atr, want.sur,
                             got.geo, got.dam, got.atr, got.sur);
            }
        }
        TEST_REQUIRE(!session.canUndo(),
                     "the single stamp must empty the undo side.");
        std::printf("[PASS] Undo restores byte-exact.\n");
    }

    // =========================================================================
    // 2: AlignVariableH — target ABOVE hZeroPlast: dam rises, ZP mark removed.
    // =========================================================================
    {
        const int targetLevel = MapEditor::BrushTool::clampHeightLevel(
            hZP + 15); // 15 voxels above zeroplast
        const int target = targetLevel << VX_FRACTION;
        params.heightLevel = targetLevel;

        // Re-craft the zeroplast patch.
        craftPatch(stampX1, stampY1, radius, geoBase, hZP, vMap.veryLightDam,
                   true);

        const StampPre pre = StampPre::read(stampX1, stampY1, radius);

        TEST_REQUIRE(brush.applyStamp(stampX1, stampY1, params),
                     "the AlignVariableH stamp was refused.");

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int px = stampX1 + dx;
                const int py = stampY1 + dy;
                const CellState& before = pre.at(dx, dy);
                const CellState post = CellState::read(px, py);

                // Dam height: AlignVariableH is an absolute set, so the dam
                // rises exactly to the target in one stamp.
                const int expectedDamTop = target;
                const int actualDamTop =
                    (static_cast<int>(post.dam) << VX_FRACTION) +
                    (post.atr & VX_FRACTION_MASK);

                TEST_REQUIRE(actualDamTop == expectedDamTop,
                             "dam top at (%d,%d): expected %d, got %d "
                             "(targetLevel=%d).",
                             dx, dy, expectedDamTop, actualDamTop,
                             targetLevel);

                // At_ZEROPLAST must be cleared.
                TEST_REQUIRE((post.atr & At_ZPMASK) != At_ZEROPLAST,
                             "At_ZEROPLAST must be cleared at (%d,%d).", dx,
                             dy);
            }
        }
        std::printf("[PASS] AlignVariableH Dam mode raises dam and clears ZP "
                    "mark.\n");

        TEST_REQUIRE(session.undo(), "Undo failed.");
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int px = stampX1 + dx;
                const int py = stampY1 + dy;
                const CellState& want = pre.at(dx, dy);
                const CellState got = CellState::read(px, py);
                TEST_REQUIRE(got.equals(want),
                             "undo must restore cell (%d,%d) byte-exact.", dx,
                             dy);
            }
        }
        std::printf("[PASS] Undo restores byte-exact.\n");
    }

    // =========================================================================
    // 3: AlignVariableH — target == hZeroPlast: no change, ZP mark preserved.
    // =========================================================================
    {
        params.heightLevel = hZP; // target = hZeroPlast

        // Re-craft the zeroplast patch.
        craftPatch(stampX1, stampY1, radius, geoBase, hZP, vMap.veryLightDam,
                   true);

        const StampPre pre = StampPre::read(stampX1, stampY1, radius);

        TEST_REQUIRE(brush.applyStamp(stampX1, stampY1, params),
                     "the AlignVariableH stamp was refused.");

        // The cell is already at target → toolzer's v == hAppr branch sets
        // At_LEVELED (or keeps At_ZEROPLAST if the toolzer gate fires first).
        // With the fix, the gate is cleared before the toolzer, so the
        // toolzer sets At_LEVELED. However, when whole == hZeroPlast the
        // brush height equals zeroplast, so the user-level rule says the mark
        // should be kept. The fix does NOT clear the gate in this case (whole
        // == hZeroPlast), so the toolzer gate fires and the cell stays
        // byte-exact.
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int px = stampX1 + dx;
                const int py = stampY1 + dy;
                const CellState& before = pre.at(dx, dy);
                const CellState post = CellState::read(px, py);

                // Since target == hZeroPlast and the gate is NOT cleared,
                // the toolzer returns early — cell stays byte-exact.
                TEST_REQUIRE(post.equals(before),
                             "cell (%d,%d) must stay byte-exact when target "
                             "== hZeroPlast.",
                             dx, dy);

                // At_ZEROPLAST must be preserved.
                TEST_REQUIRE((post.atr & At_ZPMASK) == At_ZEROPLAST,
                             "At_ZEROPLAST must be preserved at (%d,%d) when "
                             "target == hZeroPlast.",
                             dx, dy);
            }
        }
        std::printf("[PASS] AlignVariableH with target == hZeroPlast preserves "
                    "ZP mark.\n");

        TEST_REQUIRE(session.undo(), "Undo failed.");
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int px = stampX1 + dx;
                const int py = stampY1 + dy;
                const CellState& want = pre.at(dx, dy);
                const CellState got = CellState::read(px, py);
                TEST_REQUIRE(got.equals(want),
                             "undo must restore cell (%d,%d) byte-exact.", dx,
                             dy);
            }
        }
        std::printf("[PASS] Undo restores byte-exact.\n");
    }

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("AlignVariableH on zeroplast cells (T3.2 fix)", 79,
                         runBrushAlignVariableHZPTests);

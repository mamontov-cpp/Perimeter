// ============================================================================
// T3.3 — color brush (SetSurf) and eyedropper.
//
// Test 61 (color brush):
//   * SetColor paints SurBuf at the selected palette index without changing
//     VxGBuf, VxDBuf, or AtrBuf (height remains byte-exact);
//   * every cell under the stamp mask gets the correct SurBuf index;
//   * undo restores the stamp region byte-exact; redo re-applies it;
//   * a line application is one undo entry for SetColor too.
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
using MapEditor::BrushMask;
using MapEditor::BrushMasks;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushShape;

int mapOffset(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

/// Full state of one cell: the four voxel buffers plus the game height.
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
        cell.height = vMap.SGetAlt(off);
        return cell;
    }

    bool buffersEqual(const CellState& o) const
    {
        return geo == o.geo && dam == o.dam && atr == o.atr && sur == o.sur;
    }
};

/// Snapshot a rectangular region of cells (row-major).
struct RegionSnapshot
{
    int originX = 0;
    int originY = 0;
    int width = 0;
    int height = 0;
    std::vector<CellState> cells;

    static RegionSnapshot capture(int x, int y, int w, int h)
    {
        RegionSnapshot snap;
        snap.originX = x;
        snap.originY = y;
        snap.width = w;
        snap.height = h;
        snap.cells.resize(static_cast<std::size_t>(w) * h);
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) {
                snap.cells[static_cast<std::size_t>(j) * w + i] =
                    CellState::read(x + i, y + j);
            }
        }
        return snap;
    }

    const CellState& at(int px, int py) const
    {
        return cells[static_cast<std::size_t>(py - originY) * width +
                     (px - originX)];
    }
};

bool runBrushColorToolTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                  "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                  "earlier tests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Self-contained content root.
    const fs::path loadRoot = ctx.tempRoot / "load_root_color";
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the color-test content root.");
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

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 256 && mapH >= 256,
                  "the world is too small for the color test.");

    // Pick a safe region for the stamp.
    const int cx = mapW / 2;
    const int cy = mapH / 2;
    const int radius = 4;
    const int margin = 2;
    const int half = radius + margin;
    const int regionSide = 2 * half + 1;

    // ------------------------------------------------------------------
    // 61.1: SetColor paints SurBuf without changing height buffers.
    // ------------------------------------------------------------------
    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = radius;
    params.strength = 30;
    params.mode = BrushMode::Geo;
    params.effect = BrushEffect::SetColor;
    params.colorIndex = 42; // arbitrary geo palette index

    TEST_REQUIRE(MapEditor::BrushTool::effectSupported(BrushEffect::SetColor),
                  "SetColor must be supported in T3.3.");

    // Capture the whole region before the stamp.
    const RegionSnapshot before =
        RegionSnapshot::capture(cx - half, cy - half, regionSide, regionSide);

    const bool painted = brush.applyStamp(cx, cy, params);
    TEST_REQUIRE(painted, "the SetColor stamp was refused.");

    // Verify the stamp cells.
    const BrushMask stamp = BrushMasks::filledCircle(radius);
    int maskCellsHit = 0;
    for (int dy = -half; dy <= half; ++dy) {
        for (int dx = -half; dx <= half; ++dx) {
            const int px = cx + dx;
            const int py = cy + dy;
            const CellState& orig = before.at(px, py);
            const CellState cell = CellState::read(px, py);

            const bool inMask =
                (dx >= stamp.x && dy >= stamp.y &&
                 dx < stamp.x + stamp.sx && dy < stamp.y + stamp.sy &&
                 stamp.influence[static_cast<std::size_t>(dy - stamp.y) *
                                    stamp.sx +
                                (dx - stamp.x)] != 0);

            if (inMask) {
                ++maskCellsHit;
                TEST_REQUIRE(cell.sur == 42,
                             "SurBuf at (%d,%d) must be 42, got %d.",
                             px, py, cell.sur);
                // Height buffers must be unchanged.
                TEST_REQUIRE(cell.geo == orig.geo,
                             "VxGBuf at (%d,%d) changed unexpectedly.",
                             px, py);
                TEST_REQUIRE(cell.dam == orig.dam,
                             "VxDBuf at (%d,%d) changed unexpectedly.",
                             px, py);
                TEST_REQUIRE(cell.atr == orig.atr,
                             "AtrBuf at (%d,%d) changed unexpectedly.",
                             px, py);
                TEST_REQUIRE(cell.height == orig.height,
                             "SGetAlt at (%d,%d) changed unexpectedly.",
                             px, py);
            } else {
                // Outside the stamp: all buffers must be unchanged.
                TEST_REQUIRE(cell.buffersEqual(orig),
                             "cell (%d,%d) outside stamp changed.",
                             px, py);
            }
        }
    }
    TEST_REQUIRE(maskCellsHit > 0, "the stamp mask covered no cells.");
    std::printf("[PASS] SetColor painted %d cells to SurBuf=42, height "
                "buffers unchanged.\n",
                maskCellsHit);

    // ------------------------------------------------------------------
    // 61.2: undo restores the stamp region byte-exact, redo re-applies.
    // ------------------------------------------------------------------
    session.undo();
    for (int dy = -half; dy <= half; ++dy) {
        for (int dx = -half; dx <= half; ++dx) {
            const int px = cx + dx;
            const int py = cy + dy;
            const CellState& orig = before.at(px, py);
            const CellState cell = CellState::read(px, py);
            TEST_REQUIRE(cell.buffersEqual(orig) &&
                             cell.height == orig.height,
                         "undo did not restore cell (%d,%d) byte-exact.",
                         px, py);
        }
    }
    TEST_REQUIRE(!session.canUndo(),
                  "the single SetColor stamp's undo must empty the undo side.");

    session.redo();
    const CellState afterRedo = CellState::read(cx, cy);
    TEST_REQUIRE(afterRedo.sur == 42,
                  "redo did not re-apply the SetColor stamp.");
    std::printf("[PASS] undo/redo round-trips SetColor byte-exact.\n");

    // ------------------------------------------------------------------
    // 61.3: line application with SetColor is one undo entry.
    // ------------------------------------------------------------------
    // Undo the redo first to get back to the pristine state.
    session.undo();

    const int lineY = cy;
    BrushParams lineParams = params;
    lineParams.radius = 1;

    // Capture the line region.
    const RegionSnapshot beforeLine =
        RegionSnapshot::capture(cx - 12, lineY - 2, 25, 5);

    TEST_REQUIRE(brush.applyLine(cx - 10, lineY, cx + 10, lineY, lineParams),
                  "the SetColor line was refused.");
    TEST_REQUIRE(session.canUndo(), "the line must enter the undo stack.");

    // Verify line cells got SurBuf=42.
    for (int dx = -10 - 1; dx <= 10 + 1; ++dx) {
        const int px = cx + dx;
        const CellState cell = CellState::read(px, lineY);
        TEST_REQUIRE(cell.sur == 42,
                     "line cell (%d,%d) SurBuf must be 42, got %d.",
                     px, lineY, cell.sur);
    }

    // Undo the line.
    TEST_REQUIRE(session.undo(), "the SetColor line undo failed.");
    const CellState backToPristine = CellState::read(cx, lineY);
    const CellState& origLine = beforeLine.at(cx, lineY);
    TEST_REQUIRE(backToPristine.sur == origLine.sur,
                  "after undo, SurBuf must be back to original (%d, got %d).",
                  origLine.sur, backToPristine.sur);
    std::printf("[PASS] SetColor line application is one undo entry.\n");

    // ------------------------------------------------------------------
    // 61.4: Dam palette index (128..255) works too.
    // ------------------------------------------------------------------
    params.colorIndex = 200; // dam palette index
    const CellState beforeDam = CellState::read(cx, cy);

    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                  "the SetColor stamp with dam index was refused.");
    const CellState afterDam = CellState::read(cx, cy);
    TEST_REQUIRE(afterDam.sur == 200,
                  "SurBuf with dam index must be 200, got %d.",
                  afterDam.sur);
    TEST_REQUIRE(afterDam.geo == beforeDam.geo,
                  "VxGBuf must be unchanged with dam index.");
    TEST_REQUIRE(afterDam.height == beforeDam.height,
                  "height must be unchanged with dam index.");
    std::printf("[PASS] SetColor with dam palette index 200 works.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush color tool (T3.3)", 61, runBrushColorToolTests);

// ============================================================================
// T3.3 — color brush (SetSurf) and eyedropper.
//
// Test 61 (color brush):
//   * SetColor paints SurBuf at the selected palette index without changing
//     VxGBuf, VxDBuf, or AtrBuf (height remains byte-exact);
//   * the paint honours the brush layer (T4.3): Geo mode recolors geo cells
//     only, Dam mode recolors existing dam cells only — it never seeds or
//     removes a dam;
//   * every cell under the stamp mask gets the correct SurBuf index;
//   * undo restores the stamp region byte-exact; redo re-applies it;
//   * a line application is one undo entry for SetColor too.
//
// The stamp/line anchors are searched layer matching patches near the map
// centre; the fixed-centre cells are layered unpredictably on real worlds
// (dam covers roughly half of COMPASS).
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

/// True when every influenced cell of a radius-`radius` filled circle centred
/// at (cx,cy) lies in the requested layer (dam present / absent).
bool patchInLayer(int cx, int cy, int radius, bool wantDam)
{
    const BrushMask stamp = BrushMasks::filledCircle(radius);
    for (int dy = stamp.y; dy < stamp.y + stamp.sy; ++dy) {
        for (int dx = stamp.x; dx < stamp.x + stamp.sx; ++dx) {
            if (stamp.influence[static_cast<std::size_t>(dy - stamp.y) *
                                    stamp.sx +
                                (dx - stamp.x)] == 0) {
                continue;
            }
            const bool isDam = vMap.VxDBuf[mapOffset(cx + dx, cy + dy)] != 0;
            if (isDam != wantDam) {
                return false;
            }
        }
    }
    return true;
}

/// Find the point closest to (cxc,cyc) whose radius-`radius` circle mask lies
/// entirely inside one layer. The interior-only coarse scan keeps the cost
/// bounded while the buffer access stays inside the map borders.
bool findLayerPatch(int cxc, int cyc, int radius, bool wantDam,
                    int& outX, int& outY)
{
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    long best = -1;
    for (int y = 32; y < mapH - 32 - radius; y += 4) {
        for (int x = 32; x < mapW - 32 - radius; x += 4) {
            if (!patchInLayer(x, y, radius, wantDam)) {
                continue;
            }
            const long dist = static_cast<long>(x - cxc) * (x - cxc) +
                              static_cast<long>(y - cyc) * (y - cyc);
            if (best < 0 || dist < best) {
                best = dist;
                outX = x;
                outY = y;
            }
        }
    }
    return best >= 0;
}

/// Find the row closest to `cy` along which the strip (cx-11 .. cx+11) is all
/// geo — the anchor row for the one-undo-entry SetColor line test.
int findGeoLineRow(int cx, int cy)
{
    const int mapH = static_cast<int>(vMap.V_SIZE);
    for (int d = 0; d < mapH - 32; ++d) {
        for (int dir = (d == 0) ? 0 : 1; dir >= -1; --dir) {
            const int y = (d == 0) ? cy : cy + d * dir;
            if (y < 32 || y >= mapH - 32) {
                continue;
            }
            bool allGeo = true;
            for (int dx = -11; dx <= 11 && allGeo; ++dx) {
                if (vMap.VxDBuf[mapOffset(cx + dx, y)]) {
                    allGeo = false;
                }
            }
            if (allGeo) {
                return y;
            }
        }
    }
    return -1;
}

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

    // The stamp anchors start at the map centre; the actual anchors are
    // searched layer matching patches (see the helpers above).
    const int cx = mapW / 2;
    const int cy = mapH / 2;
    const int radius = 4;
    const int margin = 2;
    const int half = radius + margin;
    const int regionSide = 2 * half + 1;

    int geoX = cx;
    int geoY = cy;
    TEST_REQUIRE(findLayerPatch(cx, cy, radius, false, geoX, geoY),
                 "no all-geo patch found for the SetColor tests.");
    const int stampX = geoX;
    const int stampY = geoY;

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
        RegionSnapshot::capture(stampX - half, stampY - half,
                                regionSide, regionSide);

    const bool painted = brush.applyStamp(stampX, stampY, params);
    TEST_REQUIRE(painted, "the SetColor stamp was refused.");

    // Verify the stamp cells.
    const BrushMask stamp = BrushMasks::filledCircle(radius);
    int maskCellsHit = 0;
    for (int dy = -half; dy <= half; ++dy) {
        for (int dx = -half; dx <= half; ++dx) {
            const int px = stampX + dx;
            const int py = stampY + dy;
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
            const int px = stampX + dx;
            const int py = stampY + dy;
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
    const CellState afterRedo = CellState::read(stampX, stampY);
    TEST_REQUIRE(afterRedo.sur == 42,
                  "redo did not re-apply the SetColor stamp.");
    std::printf("[PASS] undo/redo round-trips SetColor byte-exact.\n");

    // ------------------------------------------------------------------
    // 61.3: line application with SetColor is one undo entry.
    // ------------------------------------------------------------------
    // Undo the redo first to get back to the pristine state.
    session.undo();

    // The line needs a fully-geo centre row across the whole mask strip,
    // because Geo mode now skips dam cells.
    const int lineY = findGeoLineRow(cx, cy);
    TEST_REQUIRE(lineY >= 0,
                 "no all-geo row found for the SetColor line test.");
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
    // 61.4: Dam palette index (128..255) recolors existing dam cells and
    // never seeds a dam on geo cells.
    // ------------------------------------------------------------------
    int damX = cx;
    int damY = cy;
    TEST_REQUIRE(findLayerPatch(cx, cy, radius, true, damX, damY),
                 "no all-dam patch found for the SetColor dam test.");

    // First check the Geo mode gate: a geo palette index must not touch a
    // dam cell (the renderer would resolve it through the wrong palette).
    params.mode = BrushMode::Geo;
    params.colorIndex = 42;
    const CellState damBefore = CellState::read(damX, damY);
    TEST_REQUIRE(brush.applyStamp(damX, damY, params),
                 "the Geo-mode SetColor stamp on a dam cell was refused.");
    const CellState damSkip = CellState::read(damX, damY);
    TEST_REQUIRE(damSkip.buffersEqual(damBefore),
                 "Geo-mode SetColor must not touch dam cells.");

    // Now the Dam mode paint on a genuine dam cell.
    params.mode = BrushMode::Dam;
    params.colorIndex = 200; // dam palette index
    TEST_REQUIRE(brush.applyStamp(damX, damY, params),
                 "the SetColor stamp with dam index was refused.");
    const CellState afterDam = CellState::read(damX, damY);
    TEST_REQUIRE(afterDam.sur == 200,
                  "SurBuf with dam index must be 200, got %d.",
                  afterDam.sur);
    TEST_REQUIRE(afterDam.geo == damBefore.geo,
                  "VxGBuf must be unchanged with dam index.");
    TEST_REQUIRE(afterDam.dam == damBefore.dam,
                  "VxDBuf must be unchanged with dam index.");
    TEST_REQUIRE(afterDam.height == damBefore.height,
                  "height must be unchanged with dam index.");

    // Negative again on the other side: Dam mode on a geo cell keeps the
    // SurBuf — the color brush never seeds a new dam (PlotZeroplast and
    // VeryLightDam own dam creation).
    const CellState geoBefore = CellState::read(stampX, stampY);
    TEST_REQUIRE(brush.applyStamp(stampX, stampY, params),
                 "the Dam-mode SetColor stamp on a geo cell was refused.");
    const CellState geoSkip = CellState::read(stampX, stampY);
    TEST_REQUIRE(geoSkip.buffersEqual(geoBefore) &&
                     geoSkip.height == geoBefore.height,
                 "Dam-mode SetColor must not touch geo cells.");
    std::printf("[PASS] SetColor honours the layer: dam index 200 recolors "
                "dam cells, geo cells stay untouched.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush color tool (T3.3)", 61, runBrushColorToolTests);

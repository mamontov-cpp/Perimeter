// ============================================================================
// T4.2 — the eyedropper as a brush type (PickColor).
//
//   * PickColor is a supported BrushEffect and a BrushTool point operation:
//     the "stamp" samples the Geo/Dam pixel (the SurBuf entry — indices
//     0..127 are the geo palette, 128..255 the dam palette) at the click
//     cell into lastPickedColorIndex()/params().colorIndex;
//   * the sample writes no buffer and creates no undo entry (byte-exact
//     region comparison over voxels/attributes/surface + empty redo/undo);
//   * sky/off-map picks return false;
//   * the pick round-trips: painting a cell with a SetColor stamp and picking
//     it later produces exactly the painted index without touching the world;
//   * the Line shape refuses the point-operated PickColor effect.
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

    bool sameRegion(const RegionSnapshot& o) const
    {
        return originX == o.originX && originY == o.originY &&
               width == o.width && height == o.height &&
               std::equal(cells.begin(), cells.end(), o.cells.begin(),
                          [](const CellState& a, const CellState& b) {
                              return a.geo == b.geo && a.dam == b.dam &&
                                     a.atr == b.atr && a.sur == b.sur &&
                                     a.height == b.height;
                          });
    }
};

bool runBrushPickColorTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Self-contained content root.
    const fs::path loadRoot = ctx.tempRoot / "load_root_pick";
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the pick-test content root.");
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
                 "the world is too small for the pick test.");

    const int cx = mapW / 2;
    const int cy = mapH / 2;

    MapEditor::BrushTool brush(session);

    // ------------------------------------------------------------------
    // 63.1: PickColor is a supported point operation of the brush interface.
    // ------------------------------------------------------------------
    TEST_REQUIRE(MapEditor::BrushTool::effectSupported(BrushEffect::PickColor),
                 "PickColor must be a supported brush effect in T4.2.");

    BrushParams pickParams;
    pickParams.effect = BrushEffect::PickColor;
    pickParams.mode = BrushMode::Geo;
    // The pick is a point operation: shape/radius/strength must not matter.
    pickParams.shape = BrushShape::FilledCircle;
    pickParams.radius = 5;
    pickParams.strength = 77;

    const RegionSnapshot before =
        RegionSnapshot::capture(cx - 3, cy - 3, 7, 7);

    TEST_REQUIRE(brush.applyStamp(cx, cy, pickParams),
                 "the PickColor stamp was refused.");
    const unsigned char surAtCell = vMap.SurBuf[mapOffset(cx, cy)];
    TEST_REQUIRE(brush.hasPickedColor(),
                 "the pick did not report a sampled pixel.");
    TEST_REQUIRE(brush.lastPickedColorIndex() == surAtCell,
                 "lastPickedColorIndex must equal SurBuf (%d, got %d).",
                 static_cast<int>(surAtCell),
                 static_cast<int>(brush.lastPickedColorIndex()));
    TEST_REQUIRE(brush.params().colorIndex ==
                     static_cast<int>(brush.lastPickedColorIndex()),
                 "the pick must feed params().colorIndex.");

    // The pick writes nothing and enters no undo entry.
    TEST_REQUIRE(RegionSnapshot::capture(cx - 3, cy - 3, 7, 7)
                     .sameRegion(before),
                 "the PickColor stamp must leave the world byte-exact.");
    TEST_REQUIRE(!session.canUndo(),
                 "the pick must not create an undo entry.");
    std::printf("[PASS] PickColor sampled SurBuf=%d with no world change.\n",
                static_cast<int>(surAtCell));

    // ------------------------------------------------------------------
    // 63.2: sky/off-map picks are refused.
    // ------------------------------------------------------------------
    TEST_REQUIRE(!brush.applyStamp(-1, -1, pickParams),
                 "an off-map pick must be refused.");
    TEST_REQUIRE(!brush.applyStamp(mapW, cy, pickParams),
                 "an x-out-of-range pick must be refused.");
    TEST_REQUIRE(!brush.applyStamp(cx, cy + mapH, pickParams),
                 "a y-out-of-range pick must be refused.");
    std::printf("[PASS] off-map picks are refused.\n");

    // ------------------------------------------------------------------
    // 63.3: the pick round-trips the color brush: paint with SetColor, then
    // pick the painted pixel — the sampled index equals the painted one and
    // params().colorIndex carries it for the next painting stroke.
    // ------------------------------------------------------------------
    const int paintColor = 110;
    BrushParams paintParams = pickParams;
    paintParams.effect = BrushEffect::SetColor;
    paintParams.colorIndex = paintColor;
    paintParams.radius = 1;

    TEST_REQUIRE(brush.applyStamp(cx + 40, cy + 40, paintParams),
                 "the SetColor stamp around the center was refused.");
    TEST_REQUIRE(vMap.SurBuf[mapOffset(cx + 40, cy + 40)] == paintColor,
                 "the SetColor paint must produce SurBuf=%d.", paintColor);

    // Back to the pick: the effect is forced by the caller (the viewport
    // does the same in the Eyedropper mode).
    TEST_REQUIRE(brush.applyStamp(cx + 40, cy + 40, pickParams),
                 "the pick over the painted cell was refused.");
    TEST_REQUIRE(brush.lastPickedColorIndex() == paintColor,
                 "the pick must sample the painted index (%d, got %d).",
                 paintColor, static_cast<int>(brush.lastPickedColorIndex()));
    TEST_REQUIRE(brush.params().colorIndex == paintColor,
                 "the pick must carry the painted index into colorIndex.");

    // ...and the picked index paints again (the eyedropper → color brush
    // round-trip the UI performs after every pick).
    paintParams.colorIndex = brush.params().colorIndex;
    const int px = cx + 60;
    TEST_REQUIRE(brush.applyStamp(px, cy + 40, paintParams),
                 "the follow-up paint with the picked index was refused.");
    TEST_REQUIRE(vMap.SurBuf[mapOffset(px, cy + 40)] == paintColor,
                 "the picked index must paint SurBuf=%d (got %d).",
                 paintColor, static_cast<int>(vMap.SurBuf[mapOffset(px, cy + 40)]));
    std::printf("[PASS] paint → pick → paint round-trip with index %d.\n",
                paintColor);

    // ------------------------------------------------------------------
    // 63.4: the Line shape refuses the point-operated pick.
    // ------------------------------------------------------------------
    const RegionSnapshot beforeLine =
        RegionSnapshot::capture(cx - 12, cy - 2, 25, 5);
    TEST_REQUIRE(!brush.applyLine(cx - 10, cy, cx + 10, cy, pickParams),
                 "the Line shape must refuse the PickColor effect.");
    TEST_REQUIRE(RegionSnapshot::capture(cx - 12, cy - 2, 25, 5)
                     .sameRegion(beforeLine),
                 "a refused line application must leave the world intact.");
    std::printf("[PASS] the Line shape refuses the pick.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush pick color (T4.2)", 63, runBrushPickColorTests);

// ============================================================================
// VoxLayers — internal voxel-layer painting.
//
// Covers the per-column height-range color layers:
//   * paint/lookup/replace/split/merge semantics of VoxColLayers;
//   * brush integration: paintHeight below the surface paints layers
//     [h-depth+1 .. h]; at/above the surface it is the default top paint;
//   * the render rule: the surface shows the layer color when the terrain
//     is cut down to a painted layer, the default otherwise (no
//     interpolation, same shading as the game);
//   * the editor slice view (show colors at a height);
//   * undo/redo of layer painting;
//   * the sidecar round trip (heights16.bin layers section).
//
// Runs on its own content root (order 117).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "WorldFiles.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

const int kPx = 1100;
const int kPy = 1100;

int offAt(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

bool buildLoadRoot(const fs::path& gameDir, const fs::path& savedWorldDir,
                   const fs::path& worldName, const fs::path& loadRoot)
{
    std::error_code ec;
    fs::remove_all(loadRoot, ec);
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(gameDir / "RESOURCE" / "GeoTx",
                                             loadRoot / "RESOURCE" / "GeoTx");
    fs::copy_file(gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);
    MapEditor::Tests::copyDirectoryRecursive(
        savedWorldDir, loadRoot / "RESOURCE" / "Worlds" / worldName);
    return true;
}

bool runVoxLayerTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_voxlayers";
    TEST_REQUIRE(buildLoadRoot(ctx.gameDir, ctx.tempWorldDir,
                               fs::u8path(ctx.worldName), loadRoot),
                 "could not build the load content root.");

    MapEditor::EditorSession session;
    session.setUndoCoalescingWindowMs(0);
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");
    TEST_REQUIRE(session.isWorldLoaded(), "session reports no loaded world.");

    const fs::path sidecarPath =
        session.currentWorldFiles()->worldDir() /
        MapEditor::WorldFiles::Heights16File;

    // A flat dam patch at height 40 (the paint playground).
    for (int j = -6; j <= 6; ++j) {
        for (int i = -6; i <= 6; ++i) {
            const int off = offAt(kPx + i, kPy + j);
            vMap.VxGBuf[off] = 10;
            vMap.VxDBuf[off] = 40;
            vMap.AtrBuf[off] = 0;
            vMap.SurBuf[off] = 5;
        }
    }

    MapEditor::BrushTool brush(session);
    const auto paintAt = [&](int height, int depth, std::uint32_t rgb) {
        MapEditor::BrushParams p;
        p.effect = MapEditor::BrushEffect::SetColor;
        p.mode = MapEditor::BrushMode::Dam;
        p.radius = 2;
        p.colorRgb = rgb;
        p.paintHeight = height;
        p.paintDepth = depth;
        return brush.applyStamp(kPx, kPy, p);
    };
    const std::uint32_t kRed = 0x00FF2040;
    const std::uint32_t kBlue = 0x004060FF;

    // ------------------------------------------------------------------
    // 1: below-surface paint creates a layer [h-depth+1 .. h]; the surface
    //    above the layer keeps the default color.
    // ------------------------------------------------------------------
    TEST_REQUIRE(paintAt(30, 1, kRed), "the layer paint was refused.");
    {
        uint32_t rgb = 0;
        TEST_REQUIRE(vMap.voxLayers.lookup(offAt(kPx, kPy), 30, &rgb) &&
                         rgb == kRed,
                     "layer 30 must carry the exact RGB.");
        TEST_REQUIRE(!vMap.voxLayers.lookup(offAt(kPx, kPy), 31, &rgb),
                     "layer 31 must not exist (depth 1).");
        TEST_REQUIRE(!vMap.voxLayers.lookup(offAt(kPx, kPy), 29, &rgb),
                     "layer 29 must not exist.");
        // Surface at 40 — above the layer: the layer color must NOT show
        // yet (the layer applies only when the surface is inside it).
        const int off0 = offAt(kPx, kPy);
        const unsigned int col = vMap.getColor32(off0);
        const unsigned int light = vMap.RnrBuf[off0] & 0x7F;
        TEST_REQUIRE(col != vrtMap::trueColor32(0xFF000000u | kRed, light),
                     "a surface above the layer must not show the layer "
                     "color.");
    }
    std::printf("[PASS] below-surface paint creates the layer.\n");

    // ------------------------------------------------------------------
    // 2: cutting the terrain down to the layer shows the layer color;
    //    raising back shows the default again.
    // ------------------------------------------------------------------
    {
        // Terraform down: set the surface to exactly 30.
        MapEditor::BrushParams dig;
        dig.effect = MapEditor::BrushEffect::AlignVariableH;
        dig.mode = MapEditor::BrushMode::Dam;
        dig.radius = 2;
        dig.heightLevel = 30;
        TEST_REQUIRE(brush.applyStamp(kPx, kPy, dig), "the cut was refused.");
        const int off = offAt(kPx, kPy);
        TEST_REQUIRE(vMap.SGetAlt(off) >> VX_FRACTION == 30,
                     "the surface must be cut to 30.");
        const unsigned int light = vMap.RnrBuf[off] & 0x7F;
        TEST_REQUIRE(vMap.getColor32(off) ==
                         vrtMap::trueColor32(0xFF000000u | kRed, light),
                     "the new top voxel must show the layer color.");
        std::printf("[PASS] cutting to the layer shows its color.\n");

        // Undo the cut: surface back at 40, default color again.
        TEST_REQUIRE(session.undo(), "the cut undo failed.");
        TEST_REQUIRE(session.undo(), "the paint undo failed.");
        uint32_t undoRgb = 0;
        TEST_REQUIRE(!vMap.voxLayers.lookup(off, 30, &undoRgb),
                     "undo must remove the painted layer.");
        std::printf("[PASS] undo restores the layer state.\n");
    }

    // ------------------------------------------------------------------
    // 3: paint height at/above the surface = the default top paint (no
    //    layer record).
    // ------------------------------------------------------------------
    {
        TEST_REQUIRE(paintAt(500, 1, kBlue),
                     "the above-surface paint was refused.");
        const int off = offAt(kPx, kPy);
        uint32_t rgb = 0;
        TEST_REQUIRE(!vMap.voxLayers.lookup(off, 40, &rgb),
                     "an above-surface paint must not create a layer.");
        TEST_REQUIRE(vMap.SurColBuf != nullptr &&
                         vMap.SurColBuf[off] == (0xFF000000u | kBlue),
                     "the above-surface paint must set the default color.");
        TEST_REQUIRE(session.undo(), "undo 3 failed.");
        TEST_REQUIRE(paintAt(-1, 1, kBlue), "the top paint was refused.");
        TEST_REQUIRE(vMap.SurColBuf[off] == (0xFF000000u | kBlue),
                     "paintHeight -1 must paint the top.");
        TEST_REQUIRE(session.undo(), "undo 3b failed.");
        std::printf("[PASS] top/above-surface paint sets the default color.\n");
    }

    // ------------------------------------------------------------------
    // 4: depth — paintDepth 3 covers [h-2 .. h]; replace semantics.
    // ------------------------------------------------------------------
    {
        TEST_REQUIRE(paintAt(30, 3, kRed), "the depth paint was refused.");
        uint32_t rgb = 0;
        const int off = offAt(kPx, kPy);
        for (int z = 28; z <= 30; ++z) {
            TEST_REQUIRE(vMap.voxLayers.lookup(off, z, &rgb) && rgb == kRed,
                         "layer %d must be red.", z);
        }
        TEST_REQUIRE(!vMap.voxLayers.lookup(off, 27, &rgb),
                     "layer 27 must stay unpainted.");
        // Replace 29 with blue: the range splits/replaces.
        {
            vMap.voxLayers.paint(off, 29, 29, 0x40, 0x60, 0xFF);
            TEST_REQUIRE(vMap.voxLayers.lookup(off, 29, &rgb) &&
                             rgb == kBlue,
                         "layer 29 must become blue.");
            TEST_REQUIRE(vMap.voxLayers.lookup(off, 28, &rgb) && rgb == kRed,
                         "layer 28 stays red.");
            TEST_REQUIRE(vMap.voxLayers.lookup(off, 30, &rgb) && rgb == kRed,
                         "layer 30 stays red.");
        }
        // One undo reverts the whole stamp (the direct replace above is
        // wiped together with it — the snapshot restores the pre-stamp
        // "no layers" state).
        TEST_REQUIRE(session.undo(), "undo 4 failed.");
        TEST_REQUIRE(vMap.voxLayers.col_.empty(),
                     "layers must be empty after the undo.");
        std::printf("[PASS] paint depth and replace semantics.\n");
    }

    // ------------------------------------------------------------------
    // 5: the slice view — getColor32 clipped at the slice height.
    // ------------------------------------------------------------------
    {
        TEST_REQUIRE(paintAt(30, 1, kRed), "the slice paint was refused.");
        session.setViewSliceHeight(30);
        const int off = offAt(kPx, kPy);
        const unsigned int light = vMap.RnrBuf[off] & 0x7F;
        TEST_REQUIRE(vMap.getColor32(off) ==
                         vrtMap::trueColor32(0xFF000000u | kRed, light),
                     "the slice at 30 must show the layer color.");
        session.setViewSliceHeight(-1);
        TEST_REQUIRE(session.viewSliceHeight() == -1,
                     "the slice must switch off.");
        TEST_REQUIRE(session.undo(), "undo 5 failed.");
        std::printf("[PASS] the slice view projects the layer at a height.\n");
    }

    // ------------------------------------------------------------------
    // 6: the sidecar round trip — layers survive save/reload.
    // ------------------------------------------------------------------
    {
        TEST_REQUIRE(paintAt(30, 1, kRed), "the round-trip paint failed.");
        TEST_REQUIRE(session.saveWorld(), "saveWorld() failed.");
        TEST_REQUIRE(fs::exists(sidecarPath), "the sidecar must be written.");
        const std::vector<std::uint8_t> sc =
            MapEditor::Tests::readFileBytes(sidecarPath);
        TEST_REQUIRE(sc.size() >= 20, "the sidecar is too small.");
        const std::uint32_t flags = static_cast<std::uint32_t>(
            sc[16] | (sc[17] << 8) | (sc[18] << 16) | (sc[19] << 24));
        TEST_REQUIRE((flags & 0x2) != 0, "the LAYERS flag must be set.");
        session.unloadWorld();
        TEST_REQUIRE(session.loadWorld(0), "the reload failed.");
        uint32_t rgb = 0;
        TEST_REQUIRE(vMap.voxLayers.lookup(offAt(kPx, kPy), 30, &rgb) &&
                         rgb == kRed,
                     "the layer must survive the sidecar round trip.");
        std::printf("[PASS] the sidecar round-trips the layers.\n");
    }

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Voxel layer painting", 117, runVoxLayerTests);

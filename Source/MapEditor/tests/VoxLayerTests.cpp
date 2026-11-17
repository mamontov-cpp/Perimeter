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

#include "VoxCavity.h"

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

// ---------------------------------------------------------------------------
// 122 — the loaded brush image (useImageMask) stamps the inner-layer paint:
// the pixel alpha gates the cells, the radius is ignored.
// ---------------------------------------------------------------------------

bool runVoxLayerMaskTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_voxlayermask";
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

    // A 3x3 brush image with only the top-left pixel (the stamp centre
    // after translate) opaque.
    MapEditor::BrushMask imageMask;
    imageMask.sx = 3;
    imageMask.sy = 3;
    imageMask.influence.assign(9, 0);
    imageMask.influence[0] = 255;

    MapEditor::BrushTool brush(session);
    brush.setImageMask(imageMask);

    MapEditor::BrushParams p;
    p.effect = MapEditor::BrushEffect::SetColor;
    p.mode = MapEditor::BrushMode::Dam;
    p.radius = 5; // ignored while the image mask owns the stamp
    p.useImageMask = true;
    p.colorRgb = 0x00FF2040;
    p.paintHeight = 30;
    p.paintDepth = 3;

    const int altBefore =
        static_cast<int>(vMap.SGetAlt(offAt(kPx, kPy)));
    TEST_REQUIRE(brush.applyStamp(kPx, kPy, p),
                 "the masked layer paint was refused.");

    uint32_t rgb = 0;
    // Only the mask's opaque centre cell is painted: [28 .. 30].
    TEST_REQUIRE(vMap.voxLayers.lookup(offAt(kPx, kPy), 30, &rgb) &&
                     rgb == 0x00FF2040,
                 "the mask centre must carry the layer at 30.");
    TEST_REQUIRE(vMap.voxLayers.lookup(offAt(kPx, kPy), 28, &rgb) &&
                     rgb == 0x00FF2040,
                 "the depth must span [28 .. 30].");
    TEST_REQUIRE(!vMap.voxLayers.lookup(offAt(kPx, kPy), 27, &rgb),
                 "below the depth nothing is painted.");
    // Transparent mask pixels never stamp, whatever the radius says.
    TEST_REQUIRE(!vMap.voxLayers.lookup(offAt(kPx + 1, kPy), 30, &rgb),
                 "a transparent mask pixel must not stamp.");
    TEST_REQUIRE(!vMap.voxLayers.lookup(offAt(kPx, kPy - 1), 30, &rgb),
                 "a transparent mask pixel must not stamp.");
    // Heights are untouched by the paint.
    TEST_REQUIRE(static_cast<int>(vMap.SGetAlt(offAt(kPx, kPy))) == altBefore,
                 "the masked paint must not change heights.");
    std::printf("[PASS] the image mask stamps the inner-layer paint.\n");

    TEST_REQUIRE(session.undo(), "the masked paint undo failed.");
    TEST_REQUIRE(!vMap.voxLayers.lookup(offAt(kPx, kPy), 30, &rgb) ||
                     rgb != 0x00FF2040,
                 "undo must remove the masked layer paint.");
    std::printf("[PASS] the masked layer paint is one undo entry.\n");

    session.unloadWorld();
    return true;
}

// ---------------------------------------------------------------------------
// 124 — transparent (empty) voxels: painting, the top-strip invariant, the
// raise-solidify rule, see-through color lookup and the load-time strip.
// ---------------------------------------------------------------------------

bool runVoxTransparentTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_voxtransparent";
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

    const fs::path sidecarPath =
        session.currentWorldFiles()->worldDir() /
        MapEditor::WorldFiles::Heights16File;

    // A flat dam patch at height 40 (the playground).
    for (int j = -8; j <= 8; ++j) {
        for (int i = -8; i <= 8; ++i) {
            const int off = offAt(kPx + i, kPy + j);
            vMap.VxGBuf[off] = 10;
            vMap.VxDBuf[off] = 40;
            vMap.AtrBuf[off] = 0;
            vMap.SurBuf[off] = 5;
        }
    }

    MapEditor::BrushTool brush(session);
    const auto paintAt = [&](int height, int depth, std::uint32_t rgb,
                             bool transparent) {
        MapEditor::BrushParams p;
        p.effect = MapEditor::BrushEffect::SetColor;
        p.mode = MapEditor::BrushMode::Dam;
        p.radius = 2;
        p.colorRgb = rgb;
        p.paintHeight = height;
        p.paintDepth = depth;
        p.paintTransparent = transparent;
        return brush.applyStamp(kPx, kPy, p);
    };

    // ------------------------------------------------------------------
    // 1: a transparent range strictly below the surface: it exists, the
    //    surface keeps its height, and the color lookup sees THROUGH it to
    //    the solid voxel below.
    // ------------------------------------------------------------------
    TEST_REQUIRE(paintAt(27, 4, 0x00102030, false),
                 "the colored layer paint was refused.");
    TEST_REQUIRE(paintAt(30, 3, 0, true), "the transparent paint was refused.");
    TEST_REQUIRE(vMap.voxLayers.isTransparent(offAt(kPx, kPy), 29),
                 "voxel 29 must be transparent.");
    TEST_REQUIRE(vMap.voxLayers.isTransparent(offAt(kPx, kPy), 30),
                 "voxel 30 must be transparent.");
    TEST_REQUIRE(!vMap.voxLayers.isTransparent(offAt(kPx, kPy), 31),
                 "voxel 31 (above the range) must not be transparent.");
    TEST_REQUIRE((vMap.SGetAlt(offAt(kPx, kPy)) >> VX_FRACTION) == 40,
                 "an interior transparent range must not change the height.");
    // Slice view at 30: transparent → falls through to the solid voxel
    // below (27, the colored layer), not the surface color.
    session.setViewSliceHeight(30);
    {
        const int off = offAt(kPx, kPy);
        const unsigned int light = vMap.RnrBuf[off] & 0x7F;
        TEST_REQUIRE(vMap.getColor32(off) ==
                         vrtMap::trueColor32(0xFF000000u | 0x00102030u, light),
                     "the slice must see through transparent voxels to the "
                     "solid voxel below.");
    }
    session.setViewSliceHeight(-1);
    std::printf("[PASS] interior transparent voxels keep the height; the "
                "slice sees through them.\n");

    // ------------------------------------------------------------------
    // 2: transparent paint ON the top: the voxels are destroyed and the
    //    height recalculates to the nearest solid voxel.
    // ------------------------------------------------------------------
    TEST_REQUIRE(paintAt(45, 6, 0, true),
                 "the top transparent paint was refused.");
    TEST_REQUIRE((vMap.SGetAlt(offAt(kPx, kPy)) >> VX_FRACTION) == 34,
                 "the top transparent voxels must be stripped: height 40 -> "
                 "34 (voxel 33 is the solid cap of the colored range).");
    TEST_REQUIRE(!vMap.voxLayers.isTransparent(offAt(kPx, kPy), 33),
                 "the new top voxel (33) must be solid.");
    // Regression: the crater floor must NOT render black. The stripped
    // surface (34) still sits inside the stale transparent range
    // [34..39] — the color lookup has to fall through it to the nearest
    // solid voxel below (the colored layer at 27), never to the stored
    // zero rgb of the transparent range.
    {
        const int off = offAt(kPx, kPy);
        const unsigned int light = vMap.RnrBuf[off] & 0x7F;
        TEST_REQUIRE(vMap.getColor32(off) ==
                         vrtMap::trueColor32(0xFF000000u | 0x00102030u,
                                             light),
                     "the stripped crater floor must show the solid voxel "
                     "below, not black.");
        TEST_REQUIRE((vMap.getColor32(off) & 0x00FFFFFFu) != 0,
                     "the crater floor must not be black.");
    }
    std::printf("[PASS] transparent voxels on the top are destroyed with a "
                "height recalc.\n");

    // ------------------------------------------------------------------
    // 3: raising the terrain through a transparent voxel replaces it with
    //    a solid one (the column default color). Fresh spot — the column
    //    above still carries the step-2 ranges.
    // ------------------------------------------------------------------
    const int rx = kPx + 16;
    const int ry = kPy;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            const int off = offAt(rx + i, ry + j);
            vMap.VxGBuf[off] = 10;
            vMap.VxDBuf[off] = 40;
            vMap.AtrBuf[off] = 0;
            vMap.SurBuf[off] = 5;
        }
    }
    {
        MapEditor::BrushParams p;
        p.effect = MapEditor::BrushEffect::SetColor;
        p.mode = MapEditor::BrushMode::Dam;
        p.radius = 2;
        p.paintHeight = 20;
        p.paintDepth = 3;
        p.paintTransparent = true;
        TEST_REQUIRE(brush.applyStamp(rx, ry, p), "the pocket paint refused.");
    }
    TEST_REQUIRE(vMap.voxLayers.isTransparent(offAt(rx, ry), 20),
                 "voxel 20 must be transparent before the raise.");
    {
        MapEditor::BrushParams raise;
        raise.effect = MapEditor::BrushEffect::ChangeHeight;
        raise.mode = MapEditor::BrushMode::Dam;
        raise.direction = 1;
        raise.radius = 2;
        raise.strength = 25;
        TEST_REQUIRE(brush.applyStamp(rx, ry, raise), "the raise refused.");
        TEST_REQUIRE(!vMap.voxLayers.isTransparent(offAt(rx, ry), 20),
                     "raising through a transparent voxel must solidify it.");
        std::uint32_t rgb = 0;
        uint8_t flags = 0;
        TEST_REQUIRE(vMap.voxLayers.lookup(
                         static_cast<std::uint32_t>(offAt(rx, ry)), 20,
                         &rgb, &flags) &&
                         !(flags & VOX_LAYER_TRANSPARENT),
                     "the solidified voxel must be a solid layer entry.");
        TEST_REQUIRE(
            (vMap.SGetAlt(offAt(rx, ry)) >> VX_FRACTION) == 65,
            "the raise must reach 40 + 25, got %d.",
            vMap.SGetAlt(offAt(rx, ry)) >> VX_FRACTION);
    }
    std::printf("[PASS] raising through transparent voxels solidifies them "
                "with the default color.\n");

    // ------------------------------------------------------------------
    // 4: load-time strip — a sidecar carrying transparent voxels ABOVE the
    //    solid top loses them on load (no warnings, immediate destruction).
    //    The column above sits at height 34 with a stale [34..39]
    //    transparent range; pushing the height back to 40 without
    //    normalization mimics a hand-edited save.
    // ------------------------------------------------------------------
    {
        const int off = offAt(kPx, kPy);
        TEST_REQUIRE((vMap.SGetAlt(off) >> VX_FRACTION) == 34,
                     "the pre-condition height must be 34.");
        TEST_REQUIRE(paintAt(30, 4, 0, true), "the strip-setup paint refused.");
        vMap.VxDBuf[off] = 40;
        TEST_REQUIRE(session.saveWorld(), "saveWorld() failed.");
        session.unloadWorld();
        TEST_REQUIRE(session.loadWorld(0), "the reload failed.");
        TEST_REQUIRE((vMap.SGetAlt(offAt(kPx, kPy)) >> VX_FRACTION) == 34,
                     "loading must strip transparent voxels hanging on top "
                     "(40 -> 34).");
        TEST_REQUIRE(!vMap.voxLayers.isTransparent(offAt(kPx, kPy), 33),
                     "the stripped top voxel must be solid.");
    }
    std::printf("[PASS] the sidecar load strips top-hanging transparent "
                "voxels.\n");

    (void)sidecarPath;
    session.unloadWorld();
    return true;
}


// ---------------------------------------------------------------------------
// 125 — the cavity overlay mesher: a plateau column with a transparent
// pocket gets island underside + cavity floor; a solid column emits
// nothing; vertices are never black.
// ---------------------------------------------------------------------------

bool runVoxCavityMesherTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_voxcavitymesh";
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

    // A 5x5 playground: plateau 500, geo 10, transparent pocket [251..300]
    // in the centre column; ring neighbors at 100.
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            const int off = offAt(kPx + i, kPy + j);
            vMap.VxGBuf[off] = 10;
            vMap.VxDBuf[off] = 100;
            vMap.AtrBuf[off] = 0;
            vMap.SurBuf[off] = 5;
        }
    }
    {
        const int off = offAt(kPx, kPy);
        vMap.VxDBuf[off] = 500;
        vMap.voxLayers.paint(static_cast<std::uint32_t>(off), 251, 300, 0, 0,
                             0, /*transparent=*/true);
    }

    // The heightfield renders the base at the display height (251); the
    // overlay renders the island: cap at 500 + underside at 301 + side
    // quads against the lower ring. The floor at 251 must NOT be emitted
    // (the heightfield draws it — re-emitting would z-fight).
    TEST_REQUIRE(vMap.cavityDisplayHeight(offAt(kPx, kPy)) == 251,
                 "the display height must be the base top under the pocket.");
    std::vector<VoxCavityVertex> tris;
    voxCavityAppendColumn(vMap, kPx, kPy, tris);
    TEST_REQUIRE(!tris.empty(),
                 "a cavity column must produce overlay geometry.");
    {
        bool hasCeiling = false, hasCap = false, hasFloorTriangle = false;
        for (const VoxCavityVertex& v : tris) {
            if (v.z == 301.f) {
                hasCeiling = true;
            }
        }
        // The island CAP lives in the merged per-row generator, not the
        // per-column mesher.
        std::vector<VoxCavityVertex> caps;
        voxCavityAppendRowCaps(vMap, kPy, kPx - 2, kPx + 2, caps);
        for (const VoxCavityVertex& v : caps) {
            if (v.z == 500.f) {
                hasCap = true;
            }
        }
        // A full triangle in the z=251 plane would be a floor quad
        // (z-fighting with the heightfield) — edge vertices at 251 (side
        // quads of the base wall) are fine.
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            if (tris[i].z == 251.f && tris[i + 1].z == 251.f &&
                tris[i + 2].z == 251.f) {
                hasFloorTriangle = true;
            }
        }
        TEST_REQUIRE(!hasFloorTriangle,
                     "the floor at the display height belongs to the "
                     "heightfield, not the overlay.");
        TEST_REQUIRE(hasCap,
                     "the island cap (z=500) must come from the row "
                     "merger.");
        TEST_REQUIRE(hasCeiling,
                     "the island underside (z=301) must be emitted.");
    }
    for (const VoxCavityVertex& v : tris) {
        TEST_REQUIRE((v.diffuse & 0x00FFFFFFu) != 0,
                     "cavity vertices must not be black (0x%08X).",
                     v.diffuse);
    }
    // The ring neighbors are LOWER (height 100) and carry no transparent
    // ranges. Ownership: every plane is drawn once, by the west/south
    // column's +x/+y walk - so our column emits its east/north walls (a
    // small epsilon inside the neighbor cell), while the west/south walls
    // come from the ring neighbors' meshes (epsilon inside OUR cell) and
    // run up to max(own, neighbor) height = 500.
    {
        bool hasEast = false, hasNorth = false;
        for (const VoxCavityVertex& v : tris) {
            if (v.x > kPx + 1.05f && v.x < kPx + 1.25f) {
                hasEast = true;
            }
            if (v.y > kPy + 1.05f && v.y < kPy + 1.25f) {
                hasNorth = true;
            }
        }
        TEST_REQUIRE(hasEast,
                     "the east face against lower terrain must be emitted "
                     "by the tall column itself.");
        TEST_REQUIRE(hasNorth,
                     "the north face against lower terrain must be emitted "
                     "by the tall column itself.");
        std::vector<VoxCavityVertex> westTris, southTris;
        voxCavityAppendColumn(vMap, kPx - 1, kPy, westTris);
        voxCavityAppendColumn(vMap, kPx, kPy - 1, southTris);
        bool hasWest = false, hasSouth = false, westTall = false;
        for (const VoxCavityVertex& v : westTris) {
            if (v.x > kPx + 0.05f && v.x < kPx + 0.25f) {
                hasWest = true;
            }
            if (v.z > 400.f) {
                westTall = true;
            }
        }
        for (const VoxCavityVertex& v : southTris) {
            if (v.y > kPy + 0.05f && v.y < kPy + 0.25f) {
                hasSouth = true;
            }
        }
        TEST_REQUIRE(hasWest,
                     "the west face against lower terrain must be emitted "
                     "by the west ring neighbor.");
        TEST_REQUIRE(hasSouth,
                     "the south face against lower terrain must be emitted "
                     "by the south ring neighbor.");
        TEST_REQUIRE(westTall,
                     "the west wall must run to the taller column's height "
                     "(max(own, neighbor) walk bound).");
    }
    std::printf("[PASS] the cavity mesher emits floor/ceiling with colors.\n");

    // A ring column (height 100, no transparent ranges) emits nothing.
    std::vector<VoxCavityVertex> ringTris;
    voxCavityAppendColumn(vMap, kPx + 1, kPy, ringTris);
    TEST_REQUIRE(ringTris.empty(),
                 "a column without transparent ranges must emit nothing.");
    std::printf("[PASS] non-cavity columns emit no overlay geometry.\n");

    // The normalize pass must not break the mesher: an interior pocket is
    // not stripped (height stays 500) and re-meshing is stable.
    vMap.normalizeTransparentColumn(offAt(kPx, kPy), 500);
    std::vector<VoxCavityVertex> strippedTris;
    voxCavityAppendColumn(vMap, kPx, kPy, strippedTris);
    TEST_REQUIRE((vMap.SGetAlt(offAt(kPx, kPy)) >> VX_FRACTION) == 500,
                 "an interior pocket must survive normalization.");
    TEST_REQUIRE(strippedTris.size() == tris.size(),
                 "re-meshing a normalized column must be stable.");
    std::printf("[PASS] the mesher handles stripped columns.\n");

    session.unloadWorld();
    return true;
}


// ---------------------------------------------------------------------------
// 126 — painting a colored layer under/into a cavity: the cavity floor the
// heightfield renders (at the display height) must show the PAINTED color,
// not the column top color.
// ---------------------------------------------------------------------------

bool runVoxCavityFloorColorTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_voxfloorcolor";
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

    // Plateau 500, geo 10, transparent pocket [251..300] in the centre.
    for (int j = -3; j <= 3; ++j) {
        for (int i = -3; i <= 3; ++i) {
            const int off = offAt(kPx + i, kPy + j);
            vMap.VxGBuf[off] = 10;
            vMap.VxDBuf[off] = 500;
            vMap.AtrBuf[off] = 0;
            vMap.SurBuf[off] = 5;
        }
    }
    {
        const int off = offAt(kPx, kPy);
        vMap.voxLayers.paint(static_cast<std::uint32_t>(off), 251, 300, 0, 0,
                             0, /*transparent=*/true);
    }
    TEST_REQUIRE(vMap.cavityDisplayHeight(offAt(kPx, kPy)) == 251,
                 "the display height must start at 251.");

    // The user's scenario: yellow paint at H=252, depth=100 → layers
    // [153..252] — the paint eats into the cavity bottom (transparent
    // becomes [253..300]) and colors the base under the island.
    {
        MapEditor::BrushTool brush(session);
        MapEditor::BrushParams p;
        p.effect = MapEditor::BrushEffect::SetColor;
        p.mode = MapEditor::BrushMode::Dam;
        p.shape = MapEditor::BrushShape::FilledCircle;
        p.radius = 2;
        p.colorRgb = 0x00E6D200; // yellow
        p.paintHeight = 252;
        p.paintDepth = 100;
        TEST_REQUIRE(brush.applyStamp(kPx, kPy, p),
                     "the under-island paint was refused.");
    }
    // The cavity shrunk to [253..300] — the display height follows.
    TEST_REQUIRE(vMap.cavityDisplayHeight(offAt(kPx, kPy)) == 253,
                 "painting into the cavity bottom must shrink it.");
    // The color the RENDERER shows at the column: the painted layer under
    // the cavity (seen through the floor cut), NOT the column top color.
    {
        const int off = offAt(kPx, kPy);
        const unsigned int light = vMap.RnrBuf[off] & 0x7F;
        TEST_REQUIRE(vMap.getColor32(off) ==
                         vrtMap::trueColor32(0xFF000000u | 0x00E6D200u,
                                             light),
                     "the cavity floor must show the painted yellow layer "
                     "(got 0x%06X).",
                     vMap.getColor32(off) & 0xFFFFFF);
    }
    std::printf("[PASS] painting under the island colors the cavity floor.\n");

    // The island is fully interior (same-height solid ring), but the
    // transparent band [253..300] borders solid rock on every side - the
    // symmetric differ emits the band's INTERIOR walls (visible through
    // the slit). Everything emitted must live inside the band: no caps
    // (merged per row), no floor (heightfield), no exterior walls.
    std::vector<VoxCavityVertex> tris;
    voxCavityAppendColumn(vMap, kPx, kPy, tris);
    TEST_REQUIRE(!tris.empty(),
                 "the transparent band against solid rock must emit the "
                 "interior pocket walls.");
    for (const VoxCavityVertex& v : tris) {
        TEST_REQUIRE(v.z >= 253.f && v.z <= 301.f,
                     "interior pocket geometry must stay inside the "
                     "transparent band (z=%.0f).",
                     v.z);
    }
    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Voxel layer painting", 117, runVoxLayerTests);
MAP_EDITOR_REGISTER_TEST("Voxel layer paint through an image mask", 122,
                         runVoxLayerMaskTests);
MAP_EDITOR_REGISTER_TEST("Transparent voxels in the dam", 124,
                         runVoxTransparentTests);
MAP_EDITOR_REGISTER_TEST("Cavity overlay mesher", 125,
                         runVoxCavityMesherTests);
MAP_EDITOR_REGISTER_TEST("Cavity floor paint color", 126,
                         runVoxCavityFloorColorTests);

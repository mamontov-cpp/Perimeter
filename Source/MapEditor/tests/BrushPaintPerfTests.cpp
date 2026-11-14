// ============================================================================
// Brush paint performance — regression hunting for the color paint slowness
// (voxel-layer painting made stamps crawl). Times three paint workloads on a
// loaded world and prints the per-stamp milliseconds; the suite only checks
// that the stamps succeed — the numbers are read from the log.
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

#include <chrono>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::Shape3DKind;

bool runBrushPaintPerfTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    const fs::path loadRoot = ctx.tempRoot / "load_root_paintperf";
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the load content root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.tempWorldDir, loadRoot / "RESOURCE" / "Worlds" /
                                  fs::u8path(ctx.worldName));
    }

    MapEditor::EditorSession session;
    session.setUndoCoalescingWindowMs(0);
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512, "the world is too small.");

    // A low flat dam pad as the paint playground.
    const int cx = mapW / 2;
    const int cy = mapH / 2;
    for (int j = -40; j <= 40; ++j) {
        for (int i = -40; i <= 40; ++i) {
            const int off = (cy + j) * mapW + (cx + i);
            vMap.VxGBuf[off] = 10;
            vMap.VxDBuf[off] = 60;
            vMap.AtrBuf[off] = 0;
        }
    }

    MapEditor::BrushTool brush(session);
    const auto measure = [&](const char* name, int stamps, BrushParams p,
                             int dxStep, bool drain = true) {
        // Warm up (cache-warming) then measure.
        double totalMs = 0.0;
        for (int k = 0; k < stamps; ++k) {
            p.colorRgb = 0x00204000u + static_cast<std::uint32_t>(k * 7);
            const auto t0 = std::chrono::steady_clock::now();
            TEST_REQUIRE(brush.applyStamp(cx + k * dxStep, cy, p),
                         "the %s stamp %d was refused.", name, k);
            const auto t1 = std::chrono::steady_clock::now();
            totalMs += std::chrono::duration<double, std::milli>(t1 - t0)
                           .count();
        }
        std::printf("[PERF] %s: %.2f ms/stamp (%d stamps, %d layer "
                    "columns)\n",
                    name, totalMs / stamps, stamps,
                    vMap.voxLayers.columnCount());
        if (drain) {
            while (session.undo()) {
            }
        }
    };

    // 1: 2D top paint (H = -1) — the plain color brush.
    {
        BrushParams p;
        p.effect = BrushEffect::SetColor;
        p.mode = BrushMode::Dam;
        p.shape = MapEditor::BrushShape::FilledCircle;
        p.radius = 24;
        p.paintHeight = -1;
        measure("2D top paint r=24", 10, p, 0);
    }

    // 2: 2D inner-layer paint (H below the surface, depth 8). The stamps
    //    are NOT undone afterwards: the next measure stamps the TOP paint
    //    over the created layer columns — the regression scenario (the undo
    //    snapshot captures the layer columns of the rect on every stamp).
    {
        BrushParams p;
        p.effect = BrushEffect::SetColor;
        p.mode = BrushMode::Dam;
        p.shape = MapEditor::BrushShape::FilledCircle;
        p.radius = 24;
        p.paintHeight = 40;
        p.paintDepth = 8;
        measure("2D layer paint r=24 d=8", 10, p, 0, /*drain=*/false);

        // 2b: top paint STAMPING OVER the layer columns the stamps above
        // created.
        BrushParams top;
        top.effect = BrushEffect::SetColor;
        top.mode = BrushMode::Dam;
        top.shape = MapEditor::BrushShape::FilledCircle;
        top.radius = 24;
        top.paintHeight = -1;
        measure("2D top paint over layers r=24", 10, top, 0);
    }

    // 3: 3D cone volume paint (radius 24, interior on).
    {
        BrushParams p;
        p.effect = BrushEffect::SetColor;
        p.mode = BrushMode::Dam;
        p.shape3d = Shape3DKind::Cone;
        p.radius = 24;
        p.shapeHeight = 40;
        p.paintHeight = 10;
        p.paintInterior3d = true;
        measure("3D cone volume r=24", 5, p, 60);
    }

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush paint performance", 123,
                         runBrushPaintPerfTests);

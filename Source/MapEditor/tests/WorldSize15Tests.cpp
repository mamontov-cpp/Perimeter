// ============================================================================
// Map size 2^15 — the wide-map coordinate path at H_SIZE_POWER 15.
//
// Terra's limit is MAX_H/V_SIZE_POWER = 15 (32768). A SQUARE 2^15 world
// needs ~11 GB of voxel buffers, so this test exercises the full
// X-coordinate path on a 32768 x 512 world instead (cells = 2^24):
//   * world.ini Map Power X = 15 loads (the old MAX was 14 and
//     vrtMap::getWorld_H_SIZE xasserted on it — the assert the 3D view
//     tripped on);
//   * WorldRender() — RenderStr's full-row pass at power 15 (its
//     edge-darkening math and the rect extents are 32-bit-safe now);
//   * the chunky 3D view (Terra viewV2D) renders a sane frame with the
//     camera near the far map edge — its 16.16 ground coordinates are
//     int64 at 2^15;
//   * the dirty-rect pipeline carries int32 sRectS (int16 used to wrap a
//     32768 rect edge);
//   * a brush stamp + RGB paint + undo at the x = 32767 torus edge;
//   * save3Buf/fullLoad round-trips the far edge byte-exact.
//
// Runs last (order 114); builds its own content root.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "View3DRenderer.h"
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

const int kWidePowerX = 15;
const int kWidePowerY = 9; // 32768 x 512 — 2^24 cells, buffers stay small.
const int kFlatHeight = 40;

/// A flat S2T0 output.vmp: geo at `height`, no dams/attributes, surface 0.
bool writeFlatVmp(const fs::path& path, int hSize, int vSize, int geoHeight)
{
    const std::vector<unsigned char> geoRow(
        static_cast<std::size_t>(hSize),
        static_cast<unsigned char>(std::min(geoHeight, 255)));
    const std::vector<unsigned char> zeroRow(
        static_cast<std::size_t>(hSize), 0);

    vrtMap::sVmpHeader header{};
    header.setID("S2T0");
    header.XS = hSize;
    header.YS = vSize;

    XStream out;
    out.ErrHUsed = false;
    if (!out.open(path.u8string(), XS_OUT)) {
        return false;
    }
    out.write(&header, sizeof(header));
    for (int y = 0; y < vSize; ++y) out.write(geoRow.data(), geoRow.size());
    for (int y = 0; y < vSize; ++y) out.write(zeroRow.data(), zeroRow.size());
    for (int y = 0; y < vSize; ++y) out.write(zeroRow.data(), zeroRow.size());
    for (int y = 0; y < vSize; ++y) out.write(zeroRow.data(), zeroRow.size());
    out.close();
    return true;
}

bool runWorldSize15Tests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Content root: the template world's auxiliary files (palettes, geoTx,
    // leveled texture, ini) + a freshly written flat 32768x512 vmp.
    const fs::path loadRoot = ctx.tempRoot / "load_root_wide15";
    const fs::path worldDir =
        loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName);
    {
        std::error_code ec;
        fs::remove_all(loadRoot, ec);
        fs::create_directories(worldDir, ec);
        TEST_REQUIRE(!ec, "could not create the wide-world folder.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini.");
        // Palette/texture/template files from the round-tripped world.
        for (const char* name : {
                 "inGeo.act", "inDam.act", "geoPal.xml", "geoTx.xml",
                 "geoLattice.bin", "leveledSurfaceTexture.tga", "world.ini"}) {
            const fs::path src = ctx.tempWorldDir / name;
            if (fs::exists(src)) {
                fs::copy_file(src, worldDir / name,
                              fs::copy_options::overwrite_existing, ec);
                ec.clear();
            }
        }
        TEST_REQUIRE(writeFlatVmp(worldDir / "output.vmp",
                                  1 << kWidePowerX, 1 << kWidePowerY,
                                  kFlatHeight),
                     "could not write the flat wide output.vmp.");
        const MapEditor::WorldFiles files(worldDir);
        TEST_REQUIRE(files.writeMapPower(kWidePowerX, kWidePowerY),
                     "could not set the wide map powers.");
        TEST_REQUIRE(files.writeHZeroPlast(kFlatHeight),
                     "could not set hZeroPlast.");
        // The full loader xasserts on a missing cache.tga — write a full-size
        // black dummy (the loader reads H_SIZE*V_SIZE pixels).
        const int w = 1 << kWidePowerX;
        const int h = 1 << kWidePowerY;
        std::vector<std::uint8_t> tga(
            18 + static_cast<std::size_t>(w) * h * 3, 0);
        tga[2] = 2;
        tga[12] = static_cast<std::uint8_t>(w & 0xFF);
        tga[13] = static_cast<std::uint8_t>((w >> 8) & 0xFF);
        tga[14] = static_cast<std::uint8_t>(h & 0xFF);
        tga[15] = static_cast<std::uint8_t>((h >> 8) & 0xFF);
        tga[16] = 24;
        tga[17] = 0x20;
        MapEditor::Tests::writeFileBytes(worldDir / "cache.tga", tga);
    }

    MapEditor::EditorSession session;
    session.setUndoCoalescingWindowMs(0);
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "the wide world failed to load.");

    TEST_REQUIRE(vMap.H_SIZE == 32768 && vMap.V_SIZE == 512,
                 "expected 32768x512, got %ux%u.", vMap.H_SIZE, vMap.V_SIZE);
    TEST_REQUIRE(vMap.H_SIZE_POWER == 15, "H_SIZE_POWER must be 15.");
    std::printf("[PASS] a Map Power 15 world loads (the old MAX was 14).\n");

    // ------------------------------------------------------------------
    // WorldRender at power 15 — RenderStr's full-row pass (edge math,
    // inclusive extents up to 32767).
    // ------------------------------------------------------------------
    vMap.WorldRender();
    for (int x : {0, 1, 16383, 16384, 32766, 32767}) {
        const unsigned int col = vMap.getColor32(x, 256);
        TEST_REQUIRE(col != 0, "getColor32(%d,256) returned black.", x);
    }
    std::printf("[PASS] WorldRender + getColor32 across the 32768-wide map.\n");

    // ------------------------------------------------------------------
    // The chunky 3D view (Terra viewV2D) near the far map edge — the
    // 16.16 ground coordinates are int64 at 2^15.
    // ------------------------------------------------------------------
    {
        MapEditor::View3DRenderer renderer;
        renderer.resizeWindow(320, 240);
        MapEditor::View3DCamera camera; // defaults: rot 0, lean 140, lift 100, focus 600
        renderer.setCamera(camera);
        renderer.render(32760, 256); // camera near the far X edge
        const std::uint16_t* px = renderer.pixels();
        TEST_REQUIRE(px != nullptr, "the 3D renderer produced no frame.");
        int covered = 0;
        for (int i = 0; i < 320 * 240; ++i) {
            if (px[i] != 0) ++covered;
        }
        TEST_REQUIRE(covered * 10 >= 320 * 240,
                     "the edge frame covers only %.1f%% of the window.",
                     100.0 * covered / (320 * 240.0));
        std::printf("[PASS] viewV2D renders at the far edge of a 2^15 map "
                    "(%.1f%% covered).\n",
                    100.0 * covered / (320 * 240.0));
    }

    // ------------------------------------------------------------------
    // The dirty-rect pipeline with a full-map rect (int32 sRectS — int16
    // wrapped a 32768 edge before).
    // ------------------------------------------------------------------
    vMap.regRender(0, 0, static_cast<int>(vMap.H_SIZE),
                   static_cast<int>(vMap.V_SIZE));
    TEST_REQUIRE(!vMap.changedAreas.empty(),
                 "a full-map regRender must enqueue dirty rects.");
    for (const sRectS& r : vMap.changedAreas) {
        TEST_REQUIRE(r.x >= 0 && r.dx >= 0 && r.x1() < 32768 &&
                         r.y >= 0 && r.dy >= 0 && r.y1() < 512,
                     "dirty rect %d,%d+%d,%d out of range at 2^15.",
                     r.x, r.y, r.dx, r.dy);
    }
    vMap.changedAreas.clear();
    vMap.renderQuant();
    std::printf("[PASS] the dirty-rect pipeline holds int32 rects at 2^15.\n");

    // ------------------------------------------------------------------
    // Brush + RGB paint + undo at the x = 32767 torus edge (offsets near
    // 2^24; the torus wraps into x = 0).
    // ------------------------------------------------------------------
    {
        MapEditor::BrushTool brush(session);
        MapEditor::BrushParams params;
        params.effect = MapEditor::BrushEffect::ChangeHeight;
        params.mode = MapEditor::BrushMode::Geo;
        params.direction = 1;
        params.radius = 4;
        TEST_REQUIRE(brush.applyStamp(32767, 100, params),
                     "the edge raise stamp was refused.");
        const int offEdge = 100 * 32768 + 32767;
        const int offClip = 100 * 32768; // x = 0 — one cell east of the seam
        TEST_REQUIRE(vMap.VxGBuf[offEdge] > kFlatHeight,
                     "the edge cell must rise.");
        // Editor masks CLIP at the border (no torus wrap, like
        // recordUndoArea) — x = 0 must stay untouched by the edge stamp.
        TEST_REQUIRE(vMap.VxGBuf[offClip] == kFlatHeight,
                     "the clipped cell (x=0) must stay untouched.");

        MapEditor::BrushParams paint = params;
        paint.effect = MapEditor::BrushEffect::SetColor;
        paint.colorRgb = 0x00ABCDEF;
        TEST_REQUIRE(brush.applyStamp(32767, 100, paint),
                     "the edge paint stamp was refused.");
        TEST_REQUIRE(vMap.SurColBuf != nullptr &&
                         vMap.SurColBuf[offEdge] == 0xFFABCDEFu,
                     "the edge cell must carry the exact RGB.");
        TEST_REQUIRE(session.undo(), "undo 1 failed.");
        TEST_REQUIRE(session.undo(), "undo 2 failed.");
        TEST_REQUIRE(vMap.VxGBuf[offEdge] == kFlatHeight &&
                         vMap.VxGBuf[offClip] == kFlatHeight &&
                         vMap.SurColBuf[offEdge] == 0,
                     "undo must restore the edge byte-exact.");
        std::printf("[PASS] brush/paint/undo at the 2^15 torus edge.\n");
    }

    // ------------------------------------------------------------------
    // save3Buf/fullLoad round-trips the far edge.
    // ------------------------------------------------------------------
    vMap.VxGBuf[100 * 32768 + 32767] = 200; // tall marker at the edge
    vMap.save3Buf();
    TEST_REQUIRE(fs::exists(worldDir / "output.vmp"), "output.vmp missing.");
    // Reload through the session path (fresh buffers + sidecar handling).
    session.unloadWorld();
    TEST_REQUIRE(session.loadWorld(0), "the reload failed.");
    TEST_REQUIRE(vMap.VxGBuf[100 * 32768 + 32767] == 200,
                 "the far-edge height must survive the round trip.");
    std::printf("[PASS] save/load round-trips the far edge at 2^15.\n");

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Map size 2^15 (wide-world path)", 114,
                         runWorldSize15Tests);

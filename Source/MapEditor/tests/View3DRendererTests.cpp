// ============================================================================
// T2.3 — View3DRenderer: the oblique view over vrtMap::viewV2D.
//
// Loads the pristine game world read-only (like ViewportRendererTests) and
// checks:
//   * window sizing — width clamped to Terra's MAX_XSIZE_VX_WINDOW (viewV2D
//     keeps its lasty/lastc scratch arrays at that size, VMAP.h:125),
//   * a default-camera frame is sane (no fully-blank buffer, several distinct
//     palette colors, most of the window covered by terrain),
//   * rotating the camera by 90° re-renders a substantially different frame,
//   * stableLeanDeg() keeps viewV2D's row-division singular point
//     d0 = |tan(lean)|*focusC outside the scanned frustum range.
//
// set MAP_EDITOR_3D_DUMP=<dir> to also write the frames as PPM files for
// visual inspection.
//
// Everything is discarded by unloadWorld(); the game folder stays read-only.
// ============================================================================

#include "TestHarness.h"

// EditorSession + View3DRenderer (MapEditorCore) — the classes under test.
#include "EditorSession.h"
#include "View3DRenderer.h"

// errh_wrapper.h (MapEditorSupport) — ErrH setup, already done by earlier
// tests but repeated here so this file is self-contained.
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace {

using MapEditor::View3DCamera;
using MapEditor::View3DRenderer;

/// Sanity counters over one RGB565 frame: coverage (non-background pixels)
/// and palette diversity (distinct 565 colors).
struct FrameStats
{
    int covered = 0;
    int total = 0;
    std::set<unsigned short> colors;
};

FrameStats frameStats(const View3DRenderer &renderer)
{
    FrameStats stats;
    const int w = renderer.windowWidth();
    const int h = renderer.windowHeight();
    const unsigned short *px = renderer.pixels();
    stats.total = w * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // 0 is the sky color viewV2D fills above the horizon;
            // 0x0000-or-near-black counts as sky here on purpose.
            const unsigned short c = px[y * w + x];
            if (c != 0) {
                ++stats.covered;
                stats.colors.insert(c);
            }
        }
    }
    return stats;
}

void dumpPpm(const char *name, const MapEditor::View3DRenderer &renderer)
{
    const char *dir = std::getenv("MAP_EDITOR_3D_DUMP");
    if (!dir || !*dir) {
        return;
    }
    std::filesystem::path outDir(dir);
    std::filesystem::create_directories(outDir);
    std::filesystem::path file = outDir / (std::string(name) + ".ppm");

    std::ofstream out(file, std::ios::binary);
    if (!out) {
        std::printf("[WARN] cannot open %s for writing\n", file.u8string().c_str());
        return;
    }
    const int w = renderer.windowWidth();
    const int h = renderer.windowHeight();
    out << "P6\n" << w << " " << h << "\n255\n";
    const unsigned short *px = renderer.pixels();
    for (int i = 0; i < w * h; ++i) {
        const unsigned short c = px[i];
        out.put(char(((c & 0xF800u) >> 8)));
        out.put(char(((c & 0x07E0u) >> 3)));
        out.put(char(((c & 0x001Fu) << 3)));
    }
    std::printf("[INFO] wrote %s\n", file.u8string().c_str());
}

} // namespace

namespace {

bool runView3DRendererTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.gameDir.empty() && !ctx.worldName.empty(),
                 "test context not initialised (the world round-trip must run first).");

    // --- Step 1: load the pristine game world read-only. -------------------
    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(ctx.gameDir.u8string()) && session.scanWorlds(),
                 "setGameDir()/scanWorlds() failed for '%s'.",
                 ctx.gameDir.u8string().c_str());

    std::size_t worldIndex = session.getWorldList().size();
    for (std::size_t i = 0; i < session.getWorldList().size(); ++i) {
        if (session.getWorldList()[i].name == ctx.worldName) {
            worldIndex = i;
            break;
        }
    }
    TEST_REQUIRE(worldIndex < session.getWorldList().size(),
                 "world '%s' not found in RESOURCE/Worlds.", ctx.worldName.c_str());
    TEST_REQUIRE(session.loadWorld(worldIndex) && session.isWorldLoaded(),
                 "loadWorld('%s') failed.", ctx.worldName.c_str());

    // --- Step 2: window clamping to MAX_XSIZE_VX_WINDOW. --------------------
    View3DRenderer renderer;
    TEST_REQUIRE(View3DRenderer::isWorldAvailable(),
                 "world buffers not available after EditorSession::loadWorld().");

    renderer.resizeWindow(5000, 700);
    TEST_REQUIRE(renderer.windowWidth() == View3DRenderer::kMaxWindowSize &&
                     renderer.windowHeight() == View3DRenderer::kMaxWindowSize,
                 "window clamp failed: got %dx%d, expected max %dx%d.",
                 renderer.windowWidth(), renderer.windowHeight(),
                 View3DRenderer::kMaxWindowSize, View3DRenderer::kMaxWindowSize);

    const int frameW = 320;
    const int frameH = 240;
    renderer.resizeWindow(frameW, frameH);
    TEST_REQUIRE(renderer.windowWidth() == frameW &&
                     renderer.windowHeight() == frameH &&
                     renderer.pixels() != nullptr,
                 "window resize to %dx%d failed.", frameW, frameH);

    // --- Step 3: default-camera frame is sane. ------------------------------
    View3DCamera camera; // game defaults: rot 0, lean 140, lift 100, focus 600
    renderer.setCamera(camera);
    const int cx = static_cast<int>(vMap.H_SIZE) / 2;
    const int cy = static_cast<int>(vMap.V_SIZE) / 2;
    renderer.render(cx, cy);

    const FrameStats reference = frameStats(renderer);
    TEST_REQUIRE(reference.covered * 10 >= reference.total * 6,
                 "default frame covers only %.1f%% of the window (expected >= 60%%).",
                 100.0 * reference.covered / reference.total);
    TEST_REQUIRE(reference.colors.size() >= 8,
                 "default frame uses only %zu distinct colors (washed-out render?).",
                 reference.colors.size());
    std::printf("[PASS] default 3D frame sane (%.1f%% covered, %zu colors).\n",
                100.0 * reference.covered / reference.total,
                reference.colors.size());
    dumpPpm("view3d_default", renderer);

    // --- Step 4: rotation re-renders the frame. ------------------------------
    camera.turnAngleDeg = 90.0;
    renderer.setCamera(camera);
    renderer.render(cx, cy);
    const FrameStats rotated = frameStats(renderer);
    TEST_REQUIRE(rotated.covered * 10 >= rotated.total * 6,
                 "rotated frame covers only %.1f%% of the window.",
                 100.0 * rotated.covered / rotated.total);

    std::vector<unsigned short> first;
    {
        camera.turnAngleDeg = 0.0;
        renderer.setCamera(camera);
        renderer.render(cx, cy);
        first.assign(renderer.pixels(), renderer.pixels() + frameW * frameH);
        camera.turnAngleDeg = 90.0;
        renderer.setCamera(camera);
        renderer.render(cx, cy);
    }
    int differing = 0;
    for (int i = 0; i < frameW * frameH; ++i) {
        if (first[i] != renderer.pixels()[i]) {
            ++differing;
        }
    }
    TEST_REQUIRE(differing * 10 >= frameW * frameH * 2,
                 "90° rotation changed only %.1f%% of the pixels.",
                 100.0 * differing / (frameW * frameH));
    std::printf("[PASS] 90° rotation re-renders %.1f%% of the frame.\n",
                100.0 * differing / (frameW * frameH));
    dumpPpm("view3d_rot90", renderer);

    session.unloadWorld();
    TEST_REQUIRE(!View3DRenderer::isWorldAvailable(),
                 "world buffers should be released after unloadWorld().");
    return true;
}

// ---------------------------------------------------------------------------
// stableLeanDeg: singularity-free tilt band (the only non-Terra logic here).
// ---------------------------------------------------------------------------

bool runStableLeanTests()
{
    // The invariant viewV2D needs: |tan(lean)| * focusC stays outside the
    // scanned d range [-yside-200, yside] → for upright angles d0 > yside.
    auto verify = [] (double lean, int focus, int yside) {
        const double rad = View3DRenderer::stableLeanDeg(lean, focus, yside) *
                           3.14159265358979323846 / 180.0;
        const double absTan = std::fabs(std::tan(rad));
        return absTan * focus > yside + 8.0;
    };
    TEST_REQUIRE(verify(140.0, 600, 320) && verify(170.0, 600, 320) &&
                     verify(100.0, 150, 320) && verify(170.0, 8000, 320) &&
                     verify(120.0, 400, 240),
                 "stableLeanDeg produced a tilt inside the singular band.");
    const double leanDefault = View3DRenderer::stableLeanDeg(140.0, 600, 320);
    TEST_REQUIRE(leanDefault == 140.0,
                 "the default game tilt 140° must not be touched at focus 600 "
                 "(got %.9f).",
                 leanDefault);
    std::printf("[PASS] stableLeanDeg keeps the frustum row division clean.\n");
    return true;
}

} // namespace

// Registered after the ViewportRenderer tests (order 55) so the same context
// is ready; unloadWorld() inside restores the pristine state for any later file.
bool view3dRunAll()
{
    // Two small tests, one per aspect (frame rendering; tilt band math).
    if (!runView3DRendererTests()) {
        return false;
    }
    return runStableLeanTests();
}

MAP_EDITOR_REGISTER_TEST("View3D renderer (T2.3)", 57, view3dRunAll);

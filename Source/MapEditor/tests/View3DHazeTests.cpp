// ============================================================================
// 3D horizon haze (fix for the "bottom of the frame is one flat color"
// report: XELA, rot 210 / tilt 185 (clamped to ~114) / lift 0 / focus 150).
//
// At small focus the horizon band enters the frame; the frame-edge rows map
// to ground hundreds/thousands of cells away, the mip pyramid samples its
// coarsest levels and the edge collapses into a flat average-color slab
// (XELA's dirt brown). renderSmooth now fades strips toward the sky color
// once the per-pixel ground footprint exceeds the detail threshold.
//
//   76  regression: the user's camera — the bottom band must be substantially
//       darker than the mid frame (faded), while the mid frame stays rich;
//   77  safety: at the default zoom (lean 140 / focus 600 / lift 100) the
//       footprint stays below the threshold — no fade, bottom band as bright
//       as the mid frame.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "View3DRenderer.h"
#include "ViewportRenderer.h"
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
#include <map>

namespace MapEditor {
void init_content_root();
}

namespace {

struct BandStats
{
    long long meanLum = 0;
    std::size_t distinct = 0;
};

BandStats bandStats(const MapEditor::View3DRenderer& renderer, int y0, int y1)
{
    const int w = renderer.windowWidth();
    const unsigned short* px = renderer.pixels();
    std::map<unsigned short, int> freq;
    long long lum = 0;
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            const unsigned short v = px[y * w + x];
            ++freq[v];
            const int r8 = ((v >> 11) & 31) << 3;
            const int g8 = ((v >> 5) & 63) << 2;
            const int b8 = (v & 31) << 3;
            lum += (r8 * 76 + g8 * 150 + b8 * 29) >> 8;
            ++count;
        }
    }
    BandStats stats;
    stats.meanLum = count ? lum / count : 0;
    stats.distinct = freq.size();
    return stats;
}

/// Loads XELA, renders with the given camera. Returns false on failure
/// (the static renderer keeps the frame for the caller to inspect).
bool renderXela(MapEditor::Tests::TestContext& ctx, double rot, double lean,
                int lift, int focus, MapEditor::View3DRenderer*& out)
{
    static MapEditor::View3DRenderer renderer;
    static MapEditor::ViewportRenderer viewport;
    // Deliberately leaked: it must outlive both tests (its destructor would
    // unload the world the static renderer/viewport keep sampling).
    static MapEditor::EditorSession* session = nullptr;
    static bool ready = false;
    if (!ready) {
        session = new MapEditor::EditorSession();
        TEST_REQUIRE(session->setGameDir(ctx.gameDir.u8string()), "setGameDir.");
        TEST_REQUIRE(session->scanWorlds(), "scanWorlds.");
        bool loaded = false;
        for (size_t i = 0; i < session->worldCount(); ++i) {
            if (session->getWorldList()[i].name == "XELA") {
                loaded = session->loadWorld(i);
                break;
            }
        }
        TEST_REQUIRE(loaded, "XELA not found in the game dir.");
        TEST_REQUIRE(viewport.bindWorld(), "viewport bind.");
        renderer.resizeWindow(640, 640);
        renderer.setColorSource(&viewport);
        renderer.setSmooth(true);
        ready = true;
    }
    MapEditor::View3DCamera cam;
    cam.turnAngleDeg = rot;
    cam.leanDeg = lean;
    cam.cameraDZ = lift;
    cam.focusC = focus;
    renderer.setCamera(cam);
    renderer.render(static_cast<int>(vMap.H_SIZE) / 2,
                    static_cast<int>(vMap.V_SIZE) / 2);
    out = &renderer;
    return true;
}

} // namespace

bool runView3DHazeRegression()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    // The user's camera: the bottom band (far behind the camera, footprint
    // far above the haze threshold) must be faded toward the sky, while the
    // mid frame keeps its surface detail.
    MapEditor::View3DRenderer* rPtr = nullptr;
    TEST_REQUIRE(renderXela(ctx, 210.0, 185.0, 0, 150, rPtr), "render failed.");
    const MapEditor::View3DRenderer& r = *rPtr;
    const BandStats bottom = bandStats(r, 620, 640);
    const BandStats mid = bandStats(r, 300, 320);

    printf("haze regression: bottom meanLum=%lld distinct=%zu | mid "
           "meanLum=%lld distinct=%zu\n",
           bottom.meanLum, bottom.distinct, mid.meanLum, mid.distinct);
    TEST_REQUIRE(mid.distinct > 500,
                 "the mid frame lost its detail (%zu distinct colors) — the "
                 "haze must only affect the horizon band.",
                 mid.distinct);
    TEST_REQUIRE(bottom.meanLum * 2 < mid.meanLum,
                 "the bottom band is not faded (meanLum %lld vs mid %lld).",
                 bottom.meanLum, mid.meanLum);

    return true;
}

bool runView3DHazeDefaultZoomSafety()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    // Default-ish zoom: the frame never approaches the horizon, footprints
    // stay below the threshold — the image must be untouched by the haze
    // (the bottom band keeps its brightness relative to the mid frame).
    MapEditor::View3DRenderer* rPtr = nullptr;
    TEST_REQUIRE(renderXela(ctx, 45.0, 140.0, 100, 600, rPtr), "render failed.");
    const MapEditor::View3DRenderer& r = *rPtr;
    const BandStats bottom = bandStats(r, 620, 640);
    const BandStats mid = bandStats(r, 300, 320);

    printf("haze default-zoom safety: bottom meanLum=%lld distinct=%zu | mid "
           "meanLum=%lld distinct=%zu\n",
           bottom.meanLum, bottom.distinct, mid.meanLum, mid.distinct);
    TEST_REQUIRE(bottom.distinct > 100,
                 "the bottom band collapsed at the default zoom (%zu "
                 "distinct colors) — the haze must not engage here.",
                 bottom.distinct);
    TEST_REQUIRE(bottom.meanLum * 5 > mid.meanLum,
                 "the bottom band is faded at the default zoom (meanLum %lld "
                 "vs mid %lld).",
                 bottom.meanLum, mid.meanLum);

    return true;
}

MAP_EDITOR_REGISTER_TEST("View3D horizon haze", 76, runView3DHazeRegression);
MAP_EDITOR_REGISTER_TEST("View3D haze default-zoom safety", 77,
                         runView3DHazeDefaultZoomSafety);

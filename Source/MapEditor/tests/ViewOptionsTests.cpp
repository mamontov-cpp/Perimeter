// ============================================================================
// ViewOptionsTests — the post-T4 view toggles (user feedback round):
//
//   * Geo dimming (ViewportRenderer::setGeoDimmed): cells without a dam
//     render as darkened grey shades in the composite raster (the in-game
//     look of the un-terraformed surface); dam cells keep their palette
//     colors; the toggle restores the previous pixels.
//   * Grid overlay in the 3D view (View3DRenderer::drawGridOverlay): the
//     post-render pass marks ground samples on grid lines — the frame must
//     change and the marks must be the overlay color.
//   * Freeze-dam 3D scroll (View3DRenderer::setDamFrozen): a camera placed
//     outside the real map (so torus repeats are in view) renders the
//     repeats through the geo-only pyramid — the frame must differ from the
//     all-composite render; a camera fully inside the real map must be
//     unaffected by the toggle.
//
// Loads the pristine game world read-only through EditorSession; everything
// is discarded on unloadWorld() and the game folder stays untouched.
// ============================================================================

#include "TestHarness.h"

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
#include <vector>

namespace {

using MapEditor::View3DCamera;
using MapEditor::View3DRenderer;
using MapEditor::ViewportRenderer;

/// Shared read-only world load (the pattern of ViewportRendererTests).
bool loadPristineWorld(MapEditor::EditorSession &session)
{
    MapEditor::Tests::TestContext &ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();
    TEST_REQUIRE(!ctx.gameDir.empty() && !ctx.worldName.empty(),
                 "test context not initialised.");
    TEST_REQUIRE(session.setGameDir(ctx.gameDir.u8string()) &&
                     session.scanWorlds(),
                 "setGameDir()/scanWorlds() failed.");
    std::size_t worldIndex = session.getWorldList().size();
    for (std::size_t i = 0; i < session.getWorldList().size(); ++i) {
        if (session.getWorldList()[i].name == ctx.worldName) {
            worldIndex = i;
            break;
        }
    }
    TEST_REQUIRE(worldIndex < session.getWorldList().size(),
                 "world '%s' not found.", ctx.worldName.c_str());
    TEST_REQUIRE(session.loadWorld(worldIndex) && session.isWorldLoaded(),
                 "loadWorld('%s') failed.", ctx.worldName.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Test 1: geo dimming.
// ---------------------------------------------------------------------------

bool runGeoDimTests()
{
    MapEditor::EditorSession session;
    TEST_REQUIRE(loadPristineWorld(session), "world load failed.");

    // One dam cell and one dam-free cell anywhere in the map.
    int damX = -1, damY = -1, geoX = -1, geoY = -1;
    for (int y = 0; y < static_cast<int>(vMap.V_SIZE) &&
                   (damX < 0 || geoX < 0); ++y) {
        for (int x = 0; x < static_cast<int>(vMap.H_SIZE); ++x) {
            const int off = vMap.offsetBuf(x, y);
            if (vMap.VxDBuf[off] != 0 && damX < 0) {
                damX = x; damY = y;
            } else if (vMap.VxDBuf[off] == 0 && geoX < 0) {
                geoX = x; geoY = y;
            }
        }
    }
    TEST_REQUIRE(damX >= 0 && geoX >= 0,
                 "the world needs both dam and dam-free cells.");

    ViewportRenderer renderer;
    TEST_REQUIRE(renderer.bindWorld(), "bindWorld() failed.");
    renderer.setGeoDimmed(false);
    const std::uint32_t geoPlain = renderer.colorAt(geoX, geoY);
    const std::uint32_t damPlain = renderer.colorAt(damX, damY);

    TEST_REQUIRE(renderer.setGeoDimmed(true), "the toggle must report change.");
    const std::uint32_t geoDim = renderer.colorAt(geoX, geoY);
    const std::uint32_t damDim = renderer.colorAt(damX, damY);

    // Grey: R == G == B, and never brighter than the plain pixel's luma.
    const unsigned r = (geoDim >> 16) & 0xFF, g = (geoDim >> 8) & 0xFF,
                   b = geoDim & 0xFF;
    TEST_REQUIRE(r == g && g == b,
                 "a dimmed geo cell must be grey (got %02X%02X%02X).", r, g, b);
    TEST_REQUIRE(geoDim != geoPlain,
                 "a dam-free cell must change when geo dimming turns on.");
    TEST_REQUIRE(damDim == damPlain,
                 "a dam cell must keep its palette color under geo dimming.");
    TEST_REQUIRE(r <= ((geoPlain >> 16) & 0xFF) ||
                     r <= ((geoPlain >> 8) & 0xFF) ||
                     r <= (geoPlain & 0xFF),
                 "the grey must not exceed the plain pixel brightness.");

    // Toggle back: byte-exact restore.
    TEST_REQUIRE(renderer.setGeoDimmed(false), "the toggle must report change.");
    TEST_REQUIRE(renderer.colorAt(geoX, geoY) == geoPlain &&
                     renderer.colorAt(damX, damY) == damPlain,
                 "turning the dimming off must restore the plain pixels.");
    TEST_REQUIRE(!renderer.setGeoDimmed(false),
                 "re-setting the same state must be a no-op.");

    // --- Chunky 3D: the dimming greys Terra's 16-bit tables around the
    // viewV2D call and restores them byte-exact afterwards.
    {
        View3DRenderer chunky;
        chunky.resizeWindow(320, 240);
        chunky.setCamera(View3DCamera{});
        chunky.setSmooth(false);
        chunky.setGeoDimmed(false);
        chunky.render(static_cast<int>(vMap.H_SIZE) / 2,
                      static_cast<int>(vMap.V_SIZE) / 2);
        const std::vector<unsigned short> plainFrame(
            chunky.pixels(), chunky.pixels() + 320 * 240);
        // Probe entries of both halves (geo rows 0..127, dam rows 128..255)
        // to prove the restore leaves even the dimmed rows byte-exact.
        std::vector<unsigned short> surProbe;
        for (int i = 0; i < 256; i += 17) {
            surProbe.push_back(vMap.Sur2Col[i][i & 127]);
            surProbe.push_back(vMap.Sur2Col[i][128 + (i & 127)]);
            surProbe.push_back(vMap.Tex2Col[i & 127][i]);
            surProbe.push_back(vMap.Tex2Col[128 + (i & 127)][i]);
        }

        chunky.setGeoDimmed(true);
        chunky.render(static_cast<int>(vMap.H_SIZE) / 2,
                      static_cast<int>(vMap.V_SIZE) / 2);
        const std::vector<unsigned short> dimFrame(
            chunky.pixels(), chunky.pixels() + 320 * 240);
        std::size_t dimDiff = 0;
        for (std::size_t i = 0; i < plainFrame.size(); ++i) {
            if (plainFrame[i] != dimFrame[i]) {
                ++dimDiff;
            }
        }
        TEST_REQUIRE(dimDiff > 100,
                     "the dimmed chunky frame must differ (%zu px).", dimDiff);
        std::size_t probe = 0;
        for (int i = 0; i < 256; i += 17) {
            TEST_REQUIRE(vMap.Sur2Col[i][i & 127] == surProbe[probe++] &&
                             vMap.Sur2Col[i][128 + (i & 127)] ==
                                 surProbe[probe++] &&
                             vMap.Tex2Col[i & 127][i] == surProbe[probe++] &&
                             vMap.Tex2Col[128 + (i & 127)][i] ==
                                 surProbe[probe++],
                         "the color tables must be restored byte-exact.");
        }
    }

    std::printf("[PASS] geo dimming: grey non-dam cells, colored dams, "
                "byte-exact restore, chunky 3D dimmed.\n");
    return true;
}

// ---------------------------------------------------------------------------
// Test 2: 3D grid overlay.
// ---------------------------------------------------------------------------

bool runGridOverlayTests()
{
    MapEditor::EditorSession session;
    TEST_REQUIRE(loadPristineWorld(session), "world load failed.");

    ViewportRenderer source;
    TEST_REQUIRE(source.bindWorld(), "bindWorld() failed.");

    View3DRenderer view3d;
    view3d.resizeWindow(320, 240);
    view3d.setCamera(View3DCamera{});
    view3d.setColorSource(&source);
    view3d.setSmooth(true);
    view3d.render(static_cast<int>(vMap.H_SIZE) / 2,
                  static_cast<int>(vMap.V_SIZE) / 2);
    TEST_REQUIRE(view3d.pixels() != nullptr, "no 3D frame rendered.");

    const std::vector<unsigned short> before(
        view3d.pixels(), view3d.pixels() + 320 * 240);
    constexpr unsigned short kWhite = 0xFFFF;
    view3d.drawGridOverlay(64, 64, kWhite);
    const std::vector<unsigned short> after(
        view3d.pixels(), view3d.pixels() + 320 * 240);

    std::size_t marked = 0;
    bool foreignColor = false;
    std::size_t verticalPairs = 0;
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * 320 + x;
            if (after[i] == before[i]) {
                continue;
            }
            ++marked;
            if (after[i] != kWhite) {
                foreignColor = true;
            }
            if (y + 1 < 240 && after[i + 320] != before[i + 320]) {
                ++verticalPairs;
            }
        }
    }
    TEST_REQUIRE(marked > 100,
                 "the 64-cell grid must mark plane samples (%zu marked).",
                 marked);
    TEST_REQUIRE(!foreignColor,
                 "every mark must be exactly the overlay color.");
    // The runs are painted two pixels thick: most marked pixels have a
    // marked vertical neighbour.
    TEST_REQUIRE(verticalPairs * 4 > marked,
                 "the grid lines must be >= 2 px thick (%zu pairs of %zu).",
                 verticalPairs, marked);
    // A step below one pixel per line is refused (guard against a bogus 0).
    view3d.drawGridOverlay(0, 0, kWhite);
    TEST_REQUIRE(true, "step <= 0 is a harmless no-op.");

    // Regression (user report): at rotation 0 the horizontal map lines run
    // PARALLEL to the camera scan rows — the old crossing enumeration never
    // drew them. The forward-projected plane must produce both families:
    // a long horizontal run of marks on some screen row, a long vertical
    // run on some screen column. Both cameras from the report: the default
    // and the close-up (tilt 130 / lift 100 / focus 150, where stableLeanDeg
    // clamps the effective tilt).
    const View3DCamera cams[2] = {View3DCamera{}, {0.0, 130.0, 100, 150}};
    for (const View3DCamera &cam : cams) {
        view3d.setCamera(cam);
        view3d.render(static_cast<int>(vMap.H_SIZE) / 2,
                      static_cast<int>(vMap.V_SIZE) / 2);
        const std::vector<unsigned short> plain(
            view3d.pixels(), view3d.pixels() + 320 * 240);
        view3d.drawGridOverlay(64, 64, kWhite);
        int rowHits[240] = {};
        int colHits[320] = {};
        for (int y = 0; y < 240; ++y) {
            for (int x = 0; x < 320; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * 320 + x;
                if (view3d.pixels()[i] != plain[i]) {
                    ++rowHits[y];
                    ++colHits[x];
                }
            }
        }
        const int maxRow = *std::max_element(rowHits, rowHits + 240);
        const int maxCol = *std::max_element(colHits, colHits + 320);
        TEST_REQUIRE(maxRow > 100,
                     "rotation 0 must draw horizontal grid lines "
                     "(longest row run %d px).", maxRow);
        TEST_REQUIRE(maxCol > 50,
                     "rotation 0 must draw vertical grid lines "
                     "(longest column run %d px).", maxCol);
    }

    std::printf("[PASS] 3D grid overlay marks grid cells on the frame "
                "(%zu px), both line families at rotation 0.\n", marked);
    return true;
}

// ---------------------------------------------------------------------------
// Test 3: freeze-dam 3D scroll.
// ---------------------------------------------------------------------------

bool runFreezeDamTests()
{
    MapEditor::EditorSession session;
    TEST_REQUIRE(loadPristineWorld(session), "world load failed.");

    ViewportRenderer source;
    TEST_REQUIRE(source.bindWorld(), "bindWorld() failed.");

    View3DRenderer view3d;
    view3d.resizeWindow(320, 240);
    view3d.setCamera(View3DCamera{});
    view3d.setColorSource(&source);
    view3d.setSmooth(true);

    const auto renderFrame = [&view3d](int cx, int cy) {
        view3d.render(cx, cy);
        return std::vector<unsigned short>(
            view3d.pixels(), view3d.pixels() + 320 * 240);
    };

    // Camera outside the real map: the frame is mostly torus repeats, the
    // freeze-dam mode must render them geo-only and the frame must change.
    const int off = -300;
    view3d.setDamFrozen(false);
    const std::vector<unsigned short> repeatPlain = renderFrame(off, off);
    view3d.setDamFrozen(true);
    const std::vector<unsigned short> repeatFrozen = renderFrame(off, off);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < repeatPlain.size(); ++i) {
        if (repeatPlain[i] != repeatFrozen[i]) {
            ++differing;
        }
    }
    TEST_REQUIRE(differing > 100,
                 "frozen repeats must render differently (%zu px differ).",
                 differing);

    // Camera in the map centre: no repeat in view, the toggle changes
    // nothing (the whole frame samples the composite pyramid).
    const int midX = static_cast<int>(vMap.H_SIZE) / 2;
    const int midY = static_cast<int>(vMap.V_SIZE) / 2;
    view3d.setDamFrozen(false);
    const std::vector<unsigned short> centerPlain = renderFrame(midX, midY);
    view3d.setDamFrozen(true);
    const std::vector<unsigned short> centerFrozen = renderFrame(midX, midY);
    std::size_t centerDiff = 0;
    for (std::size_t i = 0; i < centerPlain.size(); ++i) {
        if (centerPlain[i] != centerFrozen[i]) {
            ++centerDiff;
        }
    }
    TEST_REQUIRE(centerDiff == 0,
                 "a fully in-map camera must ignore the freeze (%zu differ).",
                 centerDiff);

    std::printf("[PASS] freeze-dam scroll: repeats geo-only (%zu px), "
                "in-map frame unaffected.\n", differing);
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("View options (dim geo / 3D grid / freeze dam)", 66,
                         runGeoDimTests);
MAP_EDITOR_REGISTER_TEST("View options: 3D grid overlay", 67,
                         runGridOverlayTests);
MAP_EDITOR_REGISTER_TEST("View options: freeze-dam scroll", 68,
                         runFreezeDamTests);

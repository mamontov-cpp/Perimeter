// ============================================================================
// View3DSmoothTests — post-T3.3 smooth (mip-filtered) 3D view integration.
//
// Loads the pristine game world read-only (like View3DRendererTests), then:
//
//   * flattens the unbound vMap copy in memory: uniform heights, surfaces,
//     lighting (Rnr=64, dam bit cleared) and zero Atr flags. With every
//     sampled input constant, the mip pyramid degenerates to a constant and
//     the smooth path must render BIT-EXACT to Terra's point-sampled
//     viewV2D — sampling the same Sur2Col table entries, same projection,
//     same overdraw logic. Any indexing or projection mistake in the smooth
//     re-render shows up as a pixel difference.
//
//   * verifies the pyramid stays in step with the dirty-rect pipeline: a
//     one-cell surface edit (regRender + ViewportRenderer::update()) must
//     refresh the level-0 cell and every upper level containing it.
//
//   * guards the near-field half of the frame: for rows with negative d the
//     loop negates DY, and a regression once fed the next-row footprint the
//     unsigned magnitude — |DYnext − DY| degenerated to ≈ 2·|DY| and the
//     whole lower half of the screen sampled the coarsest pyramid levels
//     (one flat blur instead of terrain). On real (unflattened) terrain the
//     smooth bottom third must stay at least as rich in distinct colors as
//     the chunky reference within a tolerant factor.
//
// The widening gap to Terrain renderers is the actual feature (distant
// structures stop aliasing); arithmetic truth of averaging/blending lives in
// ColorMipmapTests (pure unit tests), so this file keeps the two mechanism
// checks above.
//
// Everything happens on the in-memory world copy; unloadWorld() discards it
// and the game folder stays read-only.
// ============================================================================

#include "TestHarness.h"

// EditorSession + renderers (MapEditorCore) — the classes under test.
#include "EditorSession.h"
#include "View3DRenderer.h"
#include "ViewportRenderer.h"

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
#include <cstdio>
#include <vector>

namespace {

using MapEditor::View3DCamera;
using MapEditor::View3DRenderer;
using MapEditor::ViewportRenderer;

bool runSmoothUniformParity()
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
                 "world '%s' not found in RESOURCE/Worlds.",
                 ctx.worldName.c_str());
    TEST_REQUIRE(session.loadWorld(worldIndex) && session.isWorldLoaded(),
                 "loadWorld('%s') failed.", ctx.worldName.c_str());

    const int sizeW = static_cast<int>(vMap.H_SIZE);
    const int sizeH = static_cast<int>(vMap.V_SIZE);
    const int cx = sizeW / 2;
    const int cy = sizeH / 2;

    // --- Step 2: flatten the in-memory world. -------------------------------
    // One exact color/height everywhere → the pyramid blends nothing, both
    // render styles walk identical projections of identical values.
    for (int y = 0; y < sizeH; ++y) {
        for (int x = 0; x < sizeW; ++x) {
            const int off = vMap.offsetBuf(x, y);
            vMap.VxGBuf[off] = 100;
            vMap.VxDBuf[off] = 0;
            vMap.SurBuf[off] = 3;
            vMap.RnrBuf[off] = 64;  // mid lighting, bit 7 (dam) clear
            vMap.AtrBuf[off] = 0;   // no leveled / zeroplast / shadow variants
        }
    }

    ViewportRenderer raster;
    TEST_REQUIRE(raster.bindWorld(), "bindWorld failed on the flattened world.");

    View3DRenderer renderer;
    renderer.resizeWindow(320, 240);
    const View3DCamera camera;  // game defaults: rot 0, lean 140, lift 100, focus 600
    renderer.setCamera(camera);

    // --- Step 3: chunky reference frame (Terra's point-sampled viewV2D). ---
    renderer.setSmooth(false);
    renderer.render(cx, cy);
    TEST_REQUIRE(renderer.pixels() != nullptr, "chunky render produced no buffer");
    std::vector<unsigned short> chunky(renderer.pixels(),
                                       renderer.pixels() + 320 * 240);

    // --- Step 4: smooth frame through the mip pyramid — must match. --------
    renderer.setColorSource(&raster);
    renderer.setSmooth(true);
    renderer.render(cx, cy);
    TEST_REQUIRE(renderer.pixels() != nullptr, "smooth render produced no buffer");

    int differing = 0;
    int firstDiffering = -1;
    unsigned short firstGot = 0;
    unsigned short firstExpected = 0;
    for (int i = 0; i < 320 * 240; ++i) {
        if (chunky[i] != renderer.pixels()[i]) {
            if (firstDiffering < 0) {
                firstDiffering = i;
                firstGot = renderer.pixels()[i];
                firstExpected = chunky[i];
            }
            ++differing;
        }
    }
    TEST_REQUIRE(differing == 0,
                 "smooth frame differs from chunky in %d pixels (first at "
                 "index %d: got %04X expected %04X).",
                 differing, firstDiffering, firstGot, firstExpected);
    std::printf("[PASS] smooth render is bit-exact with viewV2D on a flat world.\n");

    // --- Step 5: the pyramid follows the dirty-rect pipeline. --------------
    // Edit one cell's surface, mark it through the usual regRender flow and
    // run the ViewportRenderer update; the pyramid must reflect it at every
    // level that contains the cell.
    const int editX = cx;
    const int editY = cy;
    const int editOff = vMap.offsetBuf(editX, editY);
    vMap.SurBuf[editOff] = 7;  // different palette entry → different color
    vMap.regRender(editX, editY, editX, editY);
    const std::vector<MapEditor::DirtyRect> dirty = raster.update();
    TEST_REQUIRE(!dirty.empty(),
                 "regRender(1 cell) must push at least one dirty rect.");

    const MapEditor::ColorMipmap& mips = raster.colorMipmap();
    TEST_REQUIRE(raster.hasMips() && !mips.empty(),
                 "smooth render must have built the mip pyramid.");


    // Level 0: the edited cell now equals the fresh composite table entry.
    const std::uint32_t expected0 =
        ViewportRenderer::kOpaque | vMap.Sur2Col32[vMap.SurBuf[editOff]][vMap.RnrBuf[editOff]];
    TEST_REQUIRE(mips.colorCell(0, editX, editY) == expected0,
                 "level-0 cell not refreshed after edit: %06X vs %06X.",
                 mips.colorCell(0, editX, editY), expected0);

    // Every upper level containing the cell must be the rounded mean of its
    // current children (the pipeline recomputes them, not a stale value).
    for (int level = 1; level < mips.levelCount(); ++level) {
        const int cw = std::max(1, mips.levelWidth(level));
        const int ch = std::max(1, mips.levelHeight(level));
        const int cxk = std::min(editX >> level, cw - 1);
        const int cyk = std::min(editY >> level, ch - 1);
        // The children of the level-k cell at level k-1 (twice the cell's
        // own coordinates there — one level down per axis).
        int r = 0, g = 0, b = 0, count = 0;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                const int px = std::min((cxk << 1) + dx,
                                        mips.levelWidth(level - 1) - 1);
                const int py = std::min((cyk << 1) + dy,
                                        mips.levelHeight(level - 1) - 1);
                const std::uint32_t c = mips.colorCell(level - 1, px, py);
                r += (c >> 16) & 0xFF;
                g += (c >> 8) & 0xFF;
                b += c & 0xFF;
                ++count;
            }
        }
        const std::uint32_t expected =
            (static_cast<std::uint32_t>((r + count / 2) / count) << 16) |
            (static_cast<std::uint32_t>((g + count / 2) / count) << 8) |
            static_cast<std::uint32_t>((b + count / 2) / count);
        TEST_REQUIRE(mips.colorCell(level, cxk, cyk) == expected,
                     "level-%d cell not refreshed after edit: %06X vs %06X.",
                     level, mips.colorCell(level, cxk, cyk), expected);
    }
    std::printf("[PASS] pyramid follows the dirty-rect pipeline.\n");

    session.unloadWorld();
    return true;
}

/// Near-field regression: with the user-reported camera (rot 271, tilt 29 —
/// clamped, cameraDZ 100, focus 150) the lower half of the frame shows ground
/// close to the camera. A footprint bug there picks the coarsest pyramid
/// levels and the smooth bottom third collapses into one flat patch. On real
/// terrain the smooth frame must keep roughly the chunky reference's color
/// variety (a tolerant factor, so terrain character does not matter).
bool runSmoothNearFieldVariety()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.gameDir.empty() && !ctx.worldName.empty(),
                 "test context not initialised (the world round-trip must run first).");

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
                 "world '%s' not found in RESOURCE/Worlds.",
                 ctx.worldName.c_str());
    TEST_REQUIRE(session.loadWorld(worldIndex) && session.isWorldLoaded(),
                 "loadWorld('%s') failed.", ctx.worldName.c_str());

    // NOTE: the world is NOT flattened here — the check depends on real
    // terrain variety near the camera. No edits are made, unloadWorld() is
    // just the symmetric teardown.
    const int cx = static_cast<int>(vMap.H_SIZE) / 2;
    const int cy = static_cast<int>(vMap.V_SIZE) / 2;

    MapEditor::ViewportRenderer raster;
    TEST_REQUIRE(raster.bindWorld(), "bindWorld failed.");

    View3DRenderer renderer;
    renderer.resizeWindow(320, 240);
    View3DCamera camera;
    camera.turnAngleDeg = 271.0;
    camera.leanDeg = 29.0;     // low tilt; setCamera clamps into the stable band
    camera.cameraDZ = 100;
    camera.focusC = 150;
    renderer.setCamera(camera);
    renderer.setColorSource(&raster);

    // Bottom third of the frame, per 16x16 tile: distinct color count.
    auto avgDistinct = [](const unsigned short* px, int w, int h) {
        double sum = 0.0;
        int tiles = 0;
        for (int by = h * 2 / 3; by + 16 <= h; by += 16) {
            for (int bx = 0; bx + 16 <= w; bx += 16) {
                unsigned short seen[256];
                int seenCount = 0;
                for (int y = by; y < by + 16; ++y) {
                    for (int x = bx; x < bx + 16; ++x) {
                        const unsigned short c = px[y * w + x];
                        bool found = false;
                        for (int k = 0; k < seenCount; ++k) {
                            if (seen[k] == c) { found = true; break; }
                        }
                        if (!found && seenCount < 256) {
                            seen[seenCount++] = c;
                        }
                    }
                }
                sum += static_cast<double>(seenCount);
                ++tiles;
            }
        }
        return tiles > 0 ? sum / tiles : 0.0;
    };

    renderer.setSmooth(false);
    renderer.render(cx, cy);
    TEST_REQUIRE(renderer.pixels() != nullptr, "chunky render produced no buffer");
    const std::vector<unsigned short> chunky(renderer.pixels(),
                                             renderer.pixels() + 320 * 240);
    const double chunkyDistinct = avgDistinct(chunky.data(), 320, 240);

    renderer.setSmooth(true);
    renderer.render(cx, cy);
    TEST_REQUIRE(renderer.pixels() != nullptr, "smooth render produced no buffer");
    const double smoothDistinct = avgDistinct(renderer.pixels(), 320, 240);

    TEST_REQUIRE(smoothDistinct >= 0.4 * chunkyDistinct && smoothDistinct > 1.0,
                 "smooth bottom third lost terrain variety: avgDistinct %.2f "
                 "vs chunky %.2f (near-field rows sampled coarse mip levels).",
                 smoothDistinct, chunkyDistinct);
    std::printf("[PASS] smooth near field keeps terrain variety "
                "(%.1f distinct vs chunky %.1f).\n",
                smoothDistinct, chunkyDistinct);

    session.unloadWorld();
    return true;
}

} // namespace

bool view3dSmoothRunAll()
{
    return runSmoothUniformParity();
}

bool view3dSmoothNearFieldRunAll()
{
    return runSmoothNearFieldVariety();
}

MAP_EDITOR_REGISTER_TEST("Smooth 3D view (post-T3.3)", 58, view3dSmoothRunAll);
MAP_EDITOR_REGISTER_TEST("Smooth 3D near field", 62, view3dSmoothNearFieldRunAll);
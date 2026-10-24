// ============================================================================
// T2.1 — ViewportRenderer: cached 1:1 raster, dirty rectangles, layer filters
//        and GABuf overlays.
//
// Loads the pristine game world read-only through EditorSession (the earlier
// tests only write to their temporary copies) and checks, against expectations
// recomputed from the Terra tables:
//   * bind + the full raster equals vrtMap::getColor32() everywhere (sampled),
//   * the dirty pipeline: regRender() -> update() repaints exactly the pushed
//     box, consumes vMap.changedAreas/renderAreas and touches nothing else,
//   * At_LEVELED / At_ZEROPLAST branches (shipped worlds contain no such
//     cells, so the test forges a few — see docs/context.md),
//   * Geo / Dam / Composite filters including a synthesized dam cell whose
//     RnrBuf lighting bit must be recomputed by renderQuant(),
//   * zeroplast / leveled / non-diggable overlays driven by GABuf flags.
//
// Everything is discarded by unloadWorld(); the game folder stays read-only.
// ============================================================================

#include "TestHarness.h"

// EditorSession + ViewportRenderer (MapEditorCore) — the classes under test.
#include "EditorSession.h"
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

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using MapEditor::DirtyRect;
using MapEditor::LayerFilter;
using MapEditor::ViewportRenderer;

constexpr std::uint32_t kOpaque = 0xFF000000u;

inline int offBuf(int x, int y)
{
    return vMap.offsetBuf(x, y);
}

/// First cell inside the scan box (inclusive) that matches the predicate;
/// the predicate receives a buffer offset.
template <typename Pred>
bool scanCell(int x0, int y0, int x1, int y1, Pred pred, int& outX, int& outY)
{
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (pred(offBuf(x, y))) {
                outX = x;
                outY = y;
                return true;
            }
        }
    }
    return false;
}

/// Reference implementation of the overlay contract (docs/architecture.md),
/// valid with all three overlays enabled: zeroplast wins over leveled,
/// the hardness tint is applied on top, alpha 0 leaves the pixel alone.
std::uint32_t expectedOverlayPixel(std::uint32_t base, unsigned short ga)
{
    using MR = ViewportRenderer;
    std::uint32_t col = base;
    bool zeroplastShown = false;
    if (ga & GRIDAT_ZEROPLAST) {
        col = MR::blendTint(col, MR::kZeroplastTint, MR::kZeroplastAlphaFull);
        zeroplastShown = true;
    } else if (ga & GRIDAT_ZEROPLAST_IS_PRESENT) {
        col = MR::blendTint(col, MR::kZeroplastTint, MR::kZeroplastAlphaPartial);
        zeroplastShown = true;
    }
    if (!zeroplastShown && (ga & GRIDAT_LEVELED)) {
        col = MR::blendTint(col, MR::kLeveledTint, MR::kLeveledAlpha);
    }
    col = MR::blendTint(col, MR::kHardTint, MR::kHardAlpha[ga & GRIDAT_MASK_HARDNESS]);
    return col;
}

} // namespace

namespace {

bool runViewportRendererTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.gameDir.empty() && !ctx.worldName.empty(),
                 "test context not initialised (the world round-trip must run first).");
    TEST_REQUIRE(ctx.mapWidth >= 1024 && ctx.mapHeight >= 1024,
                 "this test expects a map of at least 1024x1024 (got %dx%d).",
                 ctx.mapWidth, ctx.mapHeight);

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
    TEST_REQUIRE(static_cast<int>(vMap.H_SIZE) == ctx.mapWidth &&
                     static_cast<int>(vMap.V_SIZE) == ctx.mapHeight,
                 "loaded world is %ux%u, context says %dx%d.",
                 static_cast<unsigned>(vMap.H_SIZE), static_cast<unsigned>(vMap.V_SIZE),
                 ctx.mapWidth, ctx.mapHeight);

    // --- Step 2: bind, then the composite raster equals getColor32. --------
    ViewportRenderer renderer;
    TEST_REQUIRE(renderer.bindWorld(), "bindWorld() failed on a loaded world.");
    TEST_REQUIRE(renderer.isBound(), "renderer not bound after bindWorld().");
    TEST_REQUIRE(renderer.width() == ctx.mapWidth && renderer.height() == ctx.mapHeight,
                 "cache is %dx%d, world is %dx%d.",
                 renderer.width(), renderer.height(), ctx.mapWidth, ctx.mapHeight);
    TEST_REQUIRE(renderer.pixels() != nullptr, "pixels() is null after bind.");

    // bindWorld() paints the full map itself, so the next update() is clean.
    std::vector<DirtyRect> dirty = renderer.update();
    TEST_REQUIRE(dirty.empty(), "update() right after bind reported %zu dirty rects.",
                 dirty.size());

    int sampled = 0;
    for (int y = 0; y < ctx.mapHeight; y += 97) {
        for (int x = 0; x < ctx.mapWidth; x += 89) {
            const std::uint32_t want = kOpaque | vMap.getColor32(offBuf(x, y));
            TEST_REQUIRE(renderer.pixels()[y * ctx.mapWidth + x] == want,
                         "composite pixel mismatch at (%d,%d).", x, y);
            ++sampled;
        }
    }
    TEST_REQUIRE(sampled > 100, "sample loop too small (%d samples).", sampled);
    std::printf("[PASS] bind + composite raster matches getColor32 (%d samples).\n",
                sampled);

    // --- Step 3: dirty rectangles. -----------------------------------------
    const int rx1 = 100, ry1 = 100, rx2 = 150, ry2 = 140;
    const DirtyRect stampRect{rx1, ry1, rx2 - rx1 + 1, ry2 - ry1 + 1};
    renderer.pixels()[120 * ctx.mapWidth + 120] = 0x11223344u; // inside the box
    renderer.pixels()[120 * ctx.mapWidth + 99] = 0x55667788u;  // just outside it

    vMap.regRender(rx1, ry1, rx2, ry2);
    dirty = renderer.update();
    TEST_REQUIRE(dirty.size() == 1,
                 "regRender must yield exactly one dirty rect (got %zu).", dirty.size());
    TEST_REQUIRE(dirty[0] == stampRect,
                 "dirty rect {%d,%d,%d,%d} != expected {%d,%d,%d,%d}.",
                 dirty[0].x, dirty[0].y, dirty[0].w, dirty[0].h,
                 stampRect.x, stampRect.y, stampRect.w, stampRect.h);
    TEST_REQUIRE(vMap.changedAreas.empty() && vMap.renderAreas.empty(),
                 "update() must consume changedAreas and renderAreas.");
    TEST_REQUIRE(renderer.pixels()[120 * ctx.mapWidth + 120] ==
                     (kOpaque | vMap.getColor32(offBuf(120, 120))),
                 "the sentinel inside the dirty rect must be repainted.");
    TEST_REQUIRE(renderer.pixels()[120 * ctx.mapWidth + 99] == 0x55667788u,
                 "repaint escaped the dirty rect.");

    dirty = renderer.update();
    TEST_REQUIRE(dirty.empty(), "a second update() must find nothing dirty.");
    std::printf("[PASS] dirty pipeline: one {%d,%d,%d,%d} rect, areas consumed.\n",
                stampRect.x, stampRect.y, stampRect.w, stampRect.h);

    // --- Step 4: At_LEVELED / At_ZEROPLAST color branches. -----------------
    // Shipped worlds contain no leveled/zeroplast cells (docs/context.md), so
    // forge a few; both cells sit inside a single regRender box.
    const int lx = 500, ly = 500; // leveled, not zeroplast
    const int zx = 505, zy = 505; // zeroplast (At_ZEROPLAST includes At_LEVELED)
    TEST_REQUIRE((vMap.AtrBuf[offBuf(lx, ly)] & 0xC0) == 0 &&
                     (vMap.AtrBuf[offBuf(zx, zy)] & 0xC0) == 0,
                 "cells (%d,%d)/(%d,%d) unexpectedly carry leveled/zeroplast flags.",
                 lx, ly, zx, zy);

    vMap.AtrBuf[offBuf(lx, ly)] |= At_LEVELED;
    vMap.AtrBuf[offBuf(zx, zy)] |= At_ZEROPLAST;
    vMap.regRender(lx - 1, ly - 1, zx + 1, zy + 1);
    dirty = renderer.update();
    const DirtyRect forgeRect{lx - 1, ly - 1, (zx + 1) - (lx - 1) + 1, (zy + 1) - (ly - 1) + 1};
    TEST_REQUIRE(dirty.size() == 1 && dirty[0] == forgeRect,
                 "forged cells must yield exactly the small dirty rect.");

    // Expected values recomputed independently from the Terra color tables.
    {
        const int offL = offBuf(lx, ly);
        const unsigned rnr = vMap.RnrBuf[offL];
        unsigned texOff = offL & vMap.LvdTex_clip_mask_x;
        texOff |= (offL >> vMap.LvdTex_SPEC_SHIFT_X) & vMap.LvdTex_spec_clip_mask_y;
        const std::uint32_t tex = vMap.Tex2Col32[rnr][vMap.LvdTex[texOff]];
        const std::uint32_t sur = vMap.Sur2Col32[vMap.SurBuf[offL]][rnr];
        const std::uint32_t want =
            kOpaque | (((tex & 0x00FEFEFFu) + (sur & 0x00FEFEFFu)) >> 1);
        TEST_REQUIRE(renderer.pixels()[ly * ctx.mapWidth + lx] == want,
                     "leveled cell (%d,%d) must render via the Tex2Col32 blend.", lx, ly);

        const int offZ = offBuf(zx, zy);
        // The editor renders zeroplast cells like leveled ones — own color
        // with the leveledSurfaceTexture layered on top (the blue zero-layer
        // tint in the game comes from the powered core's field, which does
        // not exist in the editor; docs/context.md).
        const unsigned rnrZ = vMap.RnrBuf[offZ];
        unsigned texOffZ = offZ & vMap.LvdTex_clip_mask_x;
        texOffZ |= (offZ >> vMap.LvdTex_SPEC_SHIFT_X) & vMap.LvdTex_spec_clip_mask_y;
        const std::uint32_t texZ = vMap.Tex2Col32[rnrZ][vMap.LvdTex[texOffZ]];
        const std::uint32_t surZ = vMap.Sur2Col32[vMap.SurBuf[offZ]][rnrZ];
        const std::uint32_t wantZ =
            kOpaque | (((texZ & 0x00FEFEFFu) + (surZ & 0x00FEFEFFu)) >> 1);
        TEST_REQUIRE(renderer.pixels()[zy * ctx.mapWidth + zx] == wantZ,
                     "zeroplast cell (%d,%d) must render as leveled (own "
                     "color + leveledSurfaceTexture), not the blue tint.",
                     zx, zy);
    }
    std::printf("[PASS] forged leveled/zeroplast cells render via the leveled blend.\n");

    // --- Step 5: layer filters. --------------------------------------------
    // Region survey of COMPASS (docs/context.md): the geo layer's whole
    // heights are 0..31 and dam/geo areas come in large contiguous blocks —
    // x 768..1023 / y 256..511 is solid geo, (256..384)^2 is solid dam.
    // Geo cell: no dam, not leveled, low enough to raise a dam height on it.
    int gx = 0, gy = 0;
    TEST_REQUIRE(scanCell(800, 300, 902, 402,
                          [](int off) {
                              return vMap.VxDBuf[off] == 0 &&
                                     !(vMap.AtrBuf[off] & At_LEVELED) &&
                                     vMap.VxGBuf[off] <= 239;
                          },
                          gx, gy),
                 "no suitable geo cell in the (800..902, 300..402) scan box.");
    const int offG = offBuf(gx, gy);

    // Untouched non-dam control cell (must differ from the synthesized dam).
    int cx = 0, cy = 0;
    TEST_REQUIRE(scanCell(850, 350, 950, 450,
                          [offG](int off) {
                              return off != offG && vMap.VxDBuf[off] == 0 &&
                                     !(vMap.AtrBuf[off] & At_LEVELED);
                          },
                          cx, cy),
                 "no suitable non-dam control cell in the scan box.");
    const int offC = offBuf(cx, cy);

    // A cell that already ships with a dam (its load-time lighting must have
    // RnrBuf bit 7 set by WorldRender()).
    int damX = 0, damY = 0;
    TEST_REQUIRE(scanCell(256, 256, 383, 383,
                          [](int off) {
                              return vMap.VxDBuf[off] != 0 &&
                                     !(vMap.AtrBuf[off] & At_LEVELED);
                          },
                          damX, damY),
                 "no dam cell found in the (256..383)^2 scan box.");
    const int offD = offBuf(damX, damY);

    // Geo filter equals getColor32 where there is no dam ...
    TEST_REQUIRE(renderer.setLayerFilter(LayerFilter::Geo),
                 "switching to Geo must invalidate.");
    dirty = renderer.update();
    const DirtyRect fullRect{0, 0, ctx.mapWidth, ctx.mapHeight};
    TEST_REQUIRE(dirty.size() == 1 && dirty[0] == fullRect,
                 "a filter change must repaint the whole map.");
    TEST_REQUIRE(!renderer.setLayerFilter(LayerFilter::Geo),
                 "an unchanged filter must not invalidate.");
    TEST_REQUIRE(renderer.colorAt(cx, cy) == (kOpaque | vMap.getColor32(offC)),
                 "Geo filter must equal getColor32 on a cell without a dam.");

    // ... and shows the geo-palette half under a dam: RnrBuf bit 7 selects
    // the DamPal rows 128..255 of the color tables, clearing it yields GeoPal.
    const unsigned rnrD = vMap.RnrBuf[offD];
    TEST_REQUIRE((rnrD & 0x80u) != 0,
                 "load did not compute dam lighting for (%d,%d).", damX, damY);
    TEST_REQUIRE(renderer.colorAt(damX, damY) ==
                     (kOpaque | vMap.Sur2Col32[vMap.SurBuf[offD]][rnrD & 0x7Fu]),
                 "Geo filter under an existing dam must use the geo-palette half.");

    // Synthesize a dam on the geo cell (whole part must stay above the geo
    // layer, as PutAltDam requires) and let renderQuant recompute lighting.
    const unsigned whole = vMap.VxGBuf[offG];
    const unsigned damWhole = (whole + 16 <= 255) ? whole + 16 : 255;
    TEST_REQUIRE(damWhole > whole,
                 "geo cell (%d,%d) is too high to raise a dam on.", gx, gy);
    vMap.SPutAltDam(offG,
                    (damWhole << VX_FRACTION) |
                        (vMap.AtrBuf[offG] & VX_FRACTION_MASK));
    TEST_REQUIRE(vMap.VxDBuf[offG] != 0, "SPutAltDam() did not create a dam.");

    vMap.regRender(gx - 2, gy - 2, gx + 2, gy + 2);
    dirty = renderer.update();
    const DirtyRect damRect{gx - 2, gy - 2, 5, 5};
    TEST_REQUIRE(dirty.size() == 1 && dirty[0] == damRect,
                 "the dam edit must yield exactly its small dirty rect.");
    TEST_REQUIRE(vMap.changedAreas.empty() && vMap.renderAreas.empty(),
                 "update() must consume changedAreas and renderAreas.");
    const unsigned rnrG = vMap.RnrBuf[offG];
    TEST_REQUIRE((rnrG & 0x80u) != 0,
                 "renderQuant did not recompute dam lighting for (%d,%d).", gx, gy);
    TEST_REQUIRE(renderer.colorAt(gx, gy) ==
                     (kOpaque | vMap.Sur2Col32[vMap.SurBuf[offG]][rnrG & 0x7Fu]),
                 "Geo filter under the synthesized dam must use the geo-palette half.");
    TEST_REQUIRE(renderer.colorAt(cx, cy) == (kOpaque | vMap.getColor32(offC)),
                 "Geo filter must still equal getColor32 on the untouched cell.");

    // Dam filter: dam cells as-is, everything else dimmed.
    TEST_REQUIRE(renderer.setLayerFilter(LayerFilter::Dam),
                 "switching to Dam must invalidate.");
    dirty = renderer.update();
    TEST_REQUIRE(dirty.size() == 1 && dirty[0] == fullRect,
                 "a filter change must repaint the whole map.");
    TEST_REQUIRE(renderer.colorAt(gx, gy) == (kOpaque | vMap.getColor32(offG)),
                 "Dam filter must show getColor32 on a dam cell.");
    TEST_REQUIRE(renderer.colorAt(cx, cy) == ViewportRenderer::dimColor(vMap.getColor32(offC)),
                 "Dam filter must dim a cell without a dam.");

    // Back to composite: the synthesized dam is part of it as well.
    TEST_REQUIRE(renderer.setLayerFilter(LayerFilter::Composite),
                 "switching back to Composite must invalidate.");
    dirty = renderer.update();
    TEST_REQUIRE(dirty.size() == 1 && dirty[0] == fullRect,
                 "a filter change must repaint the whole map.");
    TEST_REQUIRE(renderer.colorAt(gx, gy) == (kOpaque | vMap.getColor32(offG)),
                 "composite pixel must equal getColor32 on the synthesized dam.");
    std::printf("[PASS] Geo/Dam/Composite filters (existing + synthesized dam).\n");

    // --- Step 6: GABuf overlays. -------------------------------------------
    struct GridCase
    {
        int gx;
        int gy;
    };
    const GridCase zpFull{10, 10};
    const GridCase zpPartial{11, 11}; // IS_PRESENT + LEVELED: partial wins
    const GridCase leveled{12, 12};
    const GridCase hard0{13, 13};
    const GridCase hard1{14, 14};
    const GridCase hard2{15, 15};
    const GridCase hard3{16, 16};
    const GridCase untouched{20, 20};
    const GridCase cases[] = {zpFull, zpPartial, leveled, hard0,
                              hard1, hard2, hard3, untouched};
    const int caseCount = 8;

    std::uint32_t bases[8];
    unsigned short gas[8];
    for (int i = 0; i < caseCount; ++i) {
        const int og = vMap.offsetGBuf(cases[i].gx, cases[i].gy);
        // Shipped worlds have no leveled/zeroplast grid flags (initGrid
        // derives them from the — empty — At_LEVELED cells); only hardness
        // varies, because loadHardness() rewrites it per cell.
        TEST_REQUIRE((vMap.GABuf[og] & (GRIDAT_LEVELED | GRIDAT_ZEROPLAST |
                                        GRIDAT_ZEROPLAST_IS_PRESENT)) == 0,
                     "grid cell (%d,%d) unexpectedly carries leveled/zeroplast flags.",
                     cases[i].gx, cases[i].gy);
    }
    // Forge the flag combinations. The hardness loop below normalizes every
    // case (so zp/leveled cells isolate their tint) and sets the ramp 0..3;
    // the last cell is left untouched.
    vMap.GABuf[vMap.offsetGBuf(zpFull.gx, zpFull.gy)] |= GRIDAT_ZEROPLAST;
    vMap.GABuf[vMap.offsetGBuf(zpPartial.gx, zpPartial.gy)] |=
        GRIDAT_ZEROPLAST_IS_PRESENT | GRIDAT_LEVELED;
    vMap.GABuf[vMap.offsetGBuf(leveled.gx, leveled.gy)] |= GRIDAT_LEVELED;
    const unsigned hardValues[8] = {3, 3, 3, 0, 1, 2, 3, 0xFFFFu};
    for (int i = 0; i < caseCount; ++i) {
        if (hardValues[i] == 0xFFFFu) {
            continue; // untouched: keep whatever hardness.bin said
        }
        const int og = vMap.offsetGBuf(cases[i].gx, cases[i].gy);
        vMap.GABuf[og] = (vMap.GABuf[og] & ~GRIDAT_MASK_HARDNESS) |
                         static_cast<unsigned short>(hardValues[i]);
    }
    // Capture base pixels before the overlays are enabled.
    for (int i = 0; i < caseCount; ++i) {
        const int px = cases[i].gx * 4 + 2;
        const int py = cases[i].gy * 4 + 2;
        bases[i] = renderer.colorAt(px, py);
        gas[i] = vMap.GABuf[vMap.offsetGBuf(cases[i].gx, cases[i].gy)];
        if (i == 6 /* hard3 */) {
            TEST_REQUIRE(bases[i] == expectedOverlayPixel(bases[i], gas[i]),
                         "hardness 3 must not tint anything.");
        }
    }

    // The alpha levels must encode the documented directions.
    TEST_REQUIRE(ViewportRenderer::kHardAlpha[3] == 0,
                 "diggable hardness must not tint.");
    TEST_REQUIRE(ViewportRenderer::kHardAlpha[0] > ViewportRenderer::kHardAlpha[1] &&
                     ViewportRenderer::kHardAlpha[1] > ViewportRenderer::kHardAlpha[2] &&
                     ViewportRenderer::kHardAlpha[2] > 0,
                 "non-diggable tint must scale with hardness (0 strongest).");
    TEST_REQUIRE(ViewportRenderer::kZeroplastAlphaFull >
                         ViewportRenderer::kZeroplastAlphaPartial &&
                     ViewportRenderer::kZeroplastAlphaPartial > 0,
                 "partial zeroplast must tint weaker than a full block.");

    const std::uint32_t allOverlays = MapEditor::OverlayZeroplast |
                                      MapEditor::OverlayLeveled |
                                      MapEditor::OverlayNonDiggable;
    TEST_REQUIRE(renderer.setOverlays(allOverlays),
                 "enabling overlays must invalidate.");
    dirty = renderer.update();
    TEST_REQUIRE(dirty.size() == 1 && dirty[0] == fullRect,
                 "an overlay change must repaint the whole map.");
    TEST_REQUIRE(!renderer.setOverlays(allOverlays),
                 "unchanged overlays must not invalidate.");

    for (int i = 0; i < caseCount; ++i) {
        const int px = cases[i].gx * 4 + 2;
        const int py = cases[i].gy * 4 + 2;
        const std::uint32_t want = expectedOverlayPixel(bases[i], gas[i]);
        TEST_REQUIRE(renderer.pixels()[py * ctx.mapWidth + px] == want,
                     "overlay mismatch at grid cell (%d,%d) [flags 0x%04X].",
                     cases[i].gx, cases[i].gy, gas[i]);
    }
    // Whole-map sample: every pixel follows its 4x4 grid block's flags.
    for (int y = 0; y < ctx.mapHeight; y += 97) {
        for (int x = 0; x < ctx.mapWidth; x += 89) {
            const int og = vMap.offsetBuf2offsetGBuf(vMap.offsetBuf(x, y));
            const std::uint32_t want = expectedOverlayPixel(
                kOpaque | vMap.getColor32(offBuf(x, y)), vMap.GABuf[og]);
            TEST_REQUIRE(renderer.pixels()[y * ctx.mapWidth + x] == want,
                         "overlay sample mismatch at (%d,%d).", x, y);
        }
    }
    std::printf("[PASS] overlays: zeroplast/leveled/non-diggable over GABuf flags.\n");

    // Turning the overlays off restores the plain raster everywhere.
    TEST_REQUIRE(renderer.setOverlays(MapEditor::OverlayNone),
                 "disabling overlays must invalidate.");
    dirty = renderer.update();
    TEST_REQUIRE(dirty.size() == 1 && dirty[0] == fullRect,
                 "an overlay change must repaint the whole map.");
    for (int i = 0; i < caseCount; ++i) {
        const int px = cases[i].gx * 4 + 2;
        const int py = cases[i].gy * 4 + 2;
        TEST_REQUIRE(renderer.pixels()[py * ctx.mapWidth + px] == bases[i],
                     "overlay removal must restore the base pixel at grid cell (%d,%d).",
                     cases[i].gx, cases[i].gy);
    }
    std::printf("[PASS] overlays are fully reverted when disabled.\n");

    // --- Step 7: cleanup. ---------------------------------------------------
    renderer.unbindWorld();
    TEST_REQUIRE(!renderer.isBound() && renderer.pixels() == nullptr,
                 "unbindWorld() must free the cache.");
    session.unloadWorld();
    TEST_REQUIRE(!session.isWorldLoaded(), "unloadWorld() failed.");
    std::printf("[PASS] ViewportRenderer (T2.1).\n");
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Viewport renderer (T2.1)", 55, runViewportRendererTests);

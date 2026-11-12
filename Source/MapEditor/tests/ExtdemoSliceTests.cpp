// ============================================================================
// VoxLayers on the real EXTDEMO world — the user's repro:
//   * stamp a filled circle r=20 SetColor at (2181, 2367), paintHeight=180,
//     depth=11;
//   * the slice view at 180 must show the stamped circle (getColor32);
//   * stamping again (paint-at-slice) must not crash and must paint the
//     layer at 180.
// Skips (passes) when EXTDEMO is absent.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ViewportRenderer.h"
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
#include <filesystem>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

bool runExtdemoSliceTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    if (!session.setGameDir(ctx.gameDir.u8string())) {
        std::printf("[SKIP] EXTDEMO slice: no game dir.\n");
        return true;
    }
    if (!session.scanWorlds()) {
        std::printf("[SKIP] EXTDEMO slice: world scan failed.\n");
        return true;
    }
    bool found = false;
    for (const MapEditor::WorldInfo& info : session.getWorldList()) {
        if (info.name == "EXTDEMO") {
            found = true;
            break;
        }
    }
    if (!found) {
        std::printf("[SKIP] EXTDEMO slice: world EXTDEMO not found.\n");
        return true;
    }
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    const int cx = 2181;
    const int cy = 2367;
    const int stampRadius = 20;
    const int paintH = 180;
    const int depth = 11;
    const std::uint32_t kStampRgb = 0x00AA3366;

    MapEditor::BrushTool brush(session);
    session.setUndoCoalescingWindowMs(0);

    MapEditor::BrushParams params;
    params.effect = MapEditor::BrushEffect::SetColor;
    params.mode = MapEditor::BrushMode::Dam;
    params.shape = MapEditor::BrushShape::FilledCircle;
    params.radius = stampRadius;
    params.colorRgb = kStampRgb;
    params.paintHeight = paintH;
    params.paintDepth = depth;

    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the EXTDEMO stamp was refused.");
    std::printf("[PASS] the r=20 height-180 stamp applied on EXTDEMO.\n");

    // The layer exists at the centre cell: [170..180] with the stamp color
    // (below the surface), or the default was painted (at/above it).
    const int off = cy * static_cast<int>(vMap.H_SIZE) + cx;
    const int surface = vMap.SGetAlt(off) >> VX_FRACTION;
    std::printf("  centre surface=%d vox\n", surface);
    if (paintH < surface) {
        uint32_t rgb = 0;
        TEST_REQUIRE(vMap.voxLayers.lookup(off, paintH, &rgb) &&
                         rgb == kStampRgb,
                     "layer 180 must carry the stamp color (surface %d).",
                     surface);
        for (int z = paintH - depth + 1; z <= paintH; ++z) {
            TEST_REQUIRE(vMap.voxLayers.lookup(off, z, &rgb) &&
                             rgb == kStampRgb,
                         "layer %d must carry the stamp color.", z);
        }
        std::printf("[PASS] the depth-11 layer range is painted.\n");
    } else {
        std::printf("  (surface <= 180: the stamp is a default top paint)\n");
    }

    // The slice view at 180 shows the stamped circle.
    session.setViewSliceHeight(paintH);
    TEST_REQUIRE(session.viewSliceHeight() == paintH,
                 "the slice must be active.");
    {
        const unsigned int light = vMap.RnrBuf[off] & 0x7F;
        const unsigned int col = vMap.getColor32(off);
        if (paintH < surface) {
            TEST_REQUIRE(col == vrtMap::trueColor32(0xFF000000u | kStampRgb,
                                                    light),
                         "the slice at 180 must show the stamp color "
                         "(got 0x%06X).",
                         col & 0xFFFFFF);
        }
        // A cell outside the stamp must NOT show the stamp color.
        const int offOut =
            (cy + 200) * static_cast<int>(vMap.H_SIZE) + (cx + 200);
        const unsigned int colOut = vMap.getColor32(offOut);
        TEST_REQUIRE((colOut & 0xFFFFFF) != kStampRgb,
                     "a cell outside the stamp must not show the stamp "
                     "color.");
        std::printf("[PASS] the slice at 180 shows the stamped circle.\n");

        // trueColor32 channel order: red must sit in bits 16..23 (0x00RRGGBB
        // like Sur2Col32 / sColor4c::v) — a regression guard for the R/B
        // swap that tinted every true-color cell.
        TEST_REQUIRE(vrtMap::trueColor32(0xFFFF0000u, 126) == 0x00FF0000u &&
                         vrtMap::trueColor32(0xFF0000FFu, 126) == 0x000000FFu,
                     "trueColor32 must keep the 0x00RRGGBB channel order.");

        // The editor's color source (2D cache + GPU tilemap bake) must
        // resolve the slice layer color too — not only getColor32.
        if (paintH < surface) {
            MapEditor::ViewportRenderer renderer;
            TEST_REQUIRE(renderer.bindWorld(), "bindWorld failed.");
            const std::uint32_t cacheCol = renderer.colorAt(cx, cy);
            const unsigned int expected =
                vrtMap::trueColor32(0xFF000000u | kStampRgb, light) & 0xFFFFFFu;
            TEST_REQUIRE((cacheCol & 0x00FFFFFFu) == expected,
                         "the viewport cache must show the slice layer color "
                         "(got 0x%06X, expected 0x%06X).",
                         cacheCol & 0x00FFFFFFu, expected);
            const std::uint32_t outCol = renderer.colorAt(cx + 200, cy + 200);
            TEST_REQUIRE((outCol & 0x00FFFFFFu) != expected,
                         "a cell outside the stamp must not show the layer "
                         "color.");
            renderer.unbindWorld();
            std::printf("[PASS] the viewport cache resolves the slice layer "
                        "color.\n");
        }
    }
    session.setViewSliceHeight(-1);

    // Paint-at-slice: with the slice on, stamp again — must not crash and
    // must paint the layer at the slice height.
    session.setViewSliceHeight(paintH);
    {
        MapEditor::BrushParams again = params;
        // The "2D brushes paint at this height" option mirrors the slice
        // height into paintHeight — same as the panel's wiring.
        again.paintHeight = session.viewSliceHeight();
        TEST_REQUIRE(brush.applyStamp(cx + 100, cy, again),
                     "the paint-at-slice stamp was refused.");
        const int off2 = cy * static_cast<int>(vMap.H_SIZE) + (cx + 100);
        const int surface2 = vMap.SGetAlt(off2) >> VX_FRACTION;
        if (paintH < surface2) {
            uint32_t rgb = 0;
            TEST_REQUIRE(vMap.voxLayers.lookup(off2, paintH, &rgb),
                         "the paint-at-slice stamp must create the layer.");
        }
        TEST_REQUIRE(session.undo(), "the paint-at-slice undo failed.");
    }
    session.setViewSliceHeight(-1);
    std::printf("[PASS] paint-at-slice stamps and undoes cleanly.\n");

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("EXTDEMO slice + paint-at-height repro", 118,
                         runExtdemoSliceTests);

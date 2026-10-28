// ============================================================================
// Live minimap raster — WorldImages::renderMinimapToBgr() and the single-cell
// sampler ViewportRenderer::compositeColorAt() behind it.
//
// The minimap panel used to show map.tga only, and that file is regenerated
// exclusively by the save pipeline — so a brush stamp or an undo/redo never
// reached the panel. It now re-samples its overview from the live terrain
// (ui/MinimapWidget.cpp::refreshFromLiveTerrain(), wired to
// ViewportWidget::terrainRepainted()). These tests pin down what that relies
// on:
//
//   1. renderMinimapToBgr() is pixel-identical to
//      downscaleNearest(renderToBgr()) — the raster generateMapTga() writes
//      into map.tga, so the live view can never drift from the saved radar;
//   2. a brush stamp moves the sampled pixels, undo takes them back and redo
//      puts them there again — the panel mirrors the terrain, it holds no
//      history of its own;
//   3. the shared sampler answers false outside the world, so a re-sample
//      never invents pixels, and refuses an empty raster.
//
// Runs after the world round-trip (its temporary world copy is reused).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ViewportRenderer.h"
#include "WorldImages.h"
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
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushShape;

int mapOffset(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

bool cellIsZeroplast(int x, int y)
{
    return (vMap.AtrBuf[mapOffset(x, y)] & At_ZPMASK) == At_ZEROPLAST;
}

/// Find a stamp centre whose radius bbox satisfies the predicate on every
/// cell (map-cell coordinates, no torus wrap).
bool findStampArea(int mapW, int mapH, int radius,
                   const std::function<bool(int, int)>& predicate,
                   int& outCx, int& outCy)
{
    const int pad = radius + 8;
    for (int cy = pad; cy < mapH - pad; cy += 24) {
        for (int cx = pad; cx < mapW - pad; cx += 24) {
            bool ok = true;
            for (int dy = -radius; dy <= radius && ok; ++dy) {
                for (int dx = -radius; dx <= radius && ok; ++dx) {
                    ok = predicate(cx + dx, cy + dy);
                }
            }
            if (ok) {
                outCx = cx;
                outCy = cy;
                return true;
            }
        }
    }
    return false;
}

/// Load a private copy of the temporary test world into its own content root.
/// The caller aborts via TEST_REQUIRE(!ok) when this returns false.
bool prepareBrushTestSession(const char* subdirName,
                             MapEditor::EditorSession& session)
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    if (ctx.tempRoot.empty() || !fs::exists(ctx.tempRoot) ||
        ctx.worldName.empty()) {
        return false;
    }
    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / subdirName;
    std::error_code ec;
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(
        ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
    fs::copy_file(ctx.gameDir / "Perimeter.ini",
                  loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(
        ctx.tempWorldDir,
        loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName));

    if (!session.setGameDir(loadRoot.u8string())) {
        return false;
    }
    if (!session.scanWorlds() || session.worldCount() < 1) {
        return false;
    }
    return session.loadWorld(0);
}

/// The size generateMapTga() writes into map.tga for the loaded world.
unsigned minimapRasterSize()
{
    return std::min(std::min(static_cast<unsigned>(vMap.H_SIZE),
                             static_cast<unsigned>(vMap.V_SIZE)),
                    MapEditor::WorldImages::kMinimapMaxSize);
}

/// Number of differing bytes of two BGR rasters (their sizes must match).
int countRasterDiff(const std::vector<unsigned char>& a,
                    const std::vector<unsigned char>& b)
{
    if (a.size() != b.size()) {
        return -1;
    }
    int diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            ++diff;
        }
    }
    return diff;
}

/// Re-sample the live overview the way MinimapWidget does: the viewport's
/// update() has already run renderQuant() for the edited cells, so the
/// lighting the sampler reads is final.
bool sampleMinimap(std::vector<unsigned char>& bgr)
{
    const unsigned size = minimapRasterSize();
    vMap.renderQuant();
    return MapEditor::WorldImages::renderMinimapToBgr(size, size, bgr);
}

// ---------------------------------------------------------------------------
// Test 82: the live overview is the saved radar picture, pixel for pixel
// ---------------------------------------------------------------------------

bool runMinimapRasterIdentityTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_minimap_raster",
                                                session);
    TEST_REQUIRE(loaded,
                 "the minimap raster test could not load the temporary world.");

    std::vector<unsigned char> full;
    unsigned mapW = 0;
    unsigned mapH = 0;
    TEST_REQUIRE(MapEditor::WorldImages::renderToBgr(full, mapW, mapH),
                 "renderToBgr() could not raster the loaded world.");
    TEST_REQUIRE(mapW == session.mapWidth() && mapH == session.mapHeight(),
                 "the full raster is %ux%u, the session says %ux%u.",
                 mapW, mapH, session.mapWidth(), session.mapHeight());

    const unsigned size = minimapRasterSize();
    std::vector<unsigned char> saved;
    MapEditor::WorldImages::downscaleNearest(full, mapW, mapH, size, size,
                                            saved);
    TEST_REQUIRE(!saved.empty(), "downscaleNearest() produced an empty raster.");

    std::vector<unsigned char> live;
    TEST_REQUIRE(MapEditor::WorldImages::renderMinimapToBgr(size, size, live),
                 "renderMinimapToBgr() could not raster the loaded world.");

    const int diff = countRasterDiff(saved, live);
    TEST_REQUIRE(diff == 0,
                 "the live %ux%u overview differs from the map.tga raster in "
                 "%d bytes.",
                 size, size, diff);
    std::printf("[PASS] renderMinimapToBgr() == downscaleNearest(renderToBgr()) "
                "at %ux%u.\n",
                size, size);
    return true;
}

MAP_EDITOR_REGISTER_TEST("Live minimap raster equals the saved map.tga raster",
                         82, runMinimapRasterIdentityTests);

// ---------------------------------------------------------------------------
// Test 83: a brush stamp and undo/redo move the same pixels of the overview
// ---------------------------------------------------------------------------

bool runMinimapRasterEditTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_minimap_edit",
                                                session);
    TEST_REQUIRE(loaded,
                 "the minimap edit test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int radius = 8;

    // A clean geo-only patch with raise headroom: the composite color of the
    // cells follows the surface, so the stamp has to change the overview.
    int cx = 0;
    int cy = 0;
    const bool found = findStampArea(
        mapW, mapH, radius,
        [](int x, int y) {
            if (vMap.VxDBuf[mapOffset(x, y)] != 0 || cellIsZeroplast(x, y)) {
                return false;
            }
            return static_cast<int>(vMap.SGetAlt(x, y)) <=
                   MAX_VX_HEIGHT - 2048;
        },
        cx, cy);
    TEST_REQUIRE(found, "no clean stamp area was found.");

    std::vector<unsigned char> before;
    TEST_REQUIRE(sampleMinimap(before), "the baseline overview is unavailable.");

    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = radius;
    params.effect = BrushEffect::ChangeHeight;
    params.mode = BrushMode::Geo;
    params.direction = 1;
    params.strength = 4;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the Geo raise stamp was refused.");
    TEST_REQUIRE(session.hasUnsavedChanges(),
                 "a terrain stamp must mark the world as unsaved — the "
                 "minimap only re-samples while that flag is set.");

    std::vector<unsigned char> stamped;
    TEST_REQUIRE(sampleMinimap(stamped), "the overview is unavailable after "
                                         "the stamp.");
    const int changed = countRasterDiff(before, stamped);
    TEST_REQUIRE(changed > 0,
                 "the stamp at (%d,%d) did not move a single pixel of the "
                 "live overview.",
                 cx, cy);

    // Undo gives every pixel back — the overview is a pure function of the
    // terrain, so it can never keep a trace of an undone edit.
    TEST_REQUIRE(session.undo(), "the stamp undo failed.");
    std::vector<unsigned char> undone;
    TEST_REQUIRE(sampleMinimap(undone), "the overview is unavailable after undo.");
    TEST_REQUIRE(countRasterDiff(before, undone) == 0,
                 "undo left %d bytes of the stamp in the overview.",
                 countRasterDiff(before, undone));

    TEST_REQUIRE(session.redo(), "the stamp redo failed.");
    std::vector<unsigned char> redone;
    TEST_REQUIRE(sampleMinimap(redone), "the overview is unavailable after redo.");
    TEST_REQUIRE(countRasterDiff(stamped, redone) == 0,
                 "redo did not restore the stamped overview.");

    std::printf("[PASS] The live overview follows a brush stamp and undo/redo "
                "(%d raster bytes moved).\n",
                changed);
    return true;
}

MAP_EDITOR_REGISTER_TEST("Live minimap raster follows brush and undo/redo", 83,
                         runMinimapRasterEditTests);

// ---------------------------------------------------------------------------
// Test 84: the sampler refuses what is not in the world
// ---------------------------------------------------------------------------

bool runMinimapSamplerBoundsTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_minimap_bounds",
                                                session);
    TEST_REQUIRE(loaded,
                 "the minimap bounds test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);

    std::uint32_t rgb = 0xFFFFFFFFu;
    TEST_REQUIRE(!MapEditor::ViewportRenderer::compositeColorAt(-1, 0, rgb),
                 "the sampler must refuse x = -1.");
    TEST_REQUIRE(rgb == 0u, "a refused sample must not keep a stale color.");
    TEST_REQUIRE(!MapEditor::ViewportRenderer::compositeColorAt(0, -1, rgb),
                 "the sampler must refuse y = -1.");
    TEST_REQUIRE(!MapEditor::ViewportRenderer::compositeColorAt(mapW, 0, rgb),
                 "the sampler must refuse x = H_SIZE.");
    TEST_REQUIRE(!MapEditor::ViewportRenderer::compositeColorAt(0, mapH, rgb),
                 "the sampler must refuse y = V_SIZE.");

    TEST_REQUIRE(MapEditor::ViewportRenderer::compositeColorAt(0, 0, rgb),
                 "the sampler must answer for cell (0,0).");
    TEST_REQUIRE((rgb & 0xFF000000u) == 0u,
                 "the sampler must hand out a raw 0x00RRGGBB color.");

    std::vector<unsigned char> bgr(16, 0xAB);
    TEST_REQUIRE(!MapEditor::WorldImages::renderMinimapToBgr(0, 0, bgr),
                 "an empty overview must be refused, not written.");
    TEST_REQUIRE(bgr.empty(), "a refused raster must be cleared, not left over.");
    std::printf("[PASS] The minimap sampler stays inside the world.\n");
    return true;
}

MAP_EDITOR_REGISTER_TEST("Minimap sampler rejects out-of-world cells", 84,
                         runMinimapSamplerBoundsTests);

} // namespace

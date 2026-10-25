// ============================================================================
// Undo/redo re-renders the stale shadow trail (T3.2 rendering fix).
//
// Terra's RenderStr renders the shadow continuation WEST of every dirty
// rectangle while prevShadow persists (the `j-- ... while (j>=0 ||
// prevShadow!=0)` loop, LAND.cpp:1469) and marks the extra cells it rendered
// in shadow with At_SHADOW (== At_SHADOWV, 0x20). Those cells lie outside
// the requested dirty rect, so they are not part of changedAreas and — worst
// of all — an undo that re-renders only the snapped rect does not reach them:
// after lowering the terrain back, the cells keep the dark lighting computed
// while the ridge was up. Both 3D views (viewV2D and the smooth mip pyramid)
// read RnrBuf directly, so the stale shading shows as black stripes near the
// restored area.
//
// The fix under test: every editor render ticket goes through
// EditorSession::regRenderWithShadowTrail(), which walks the At_SHADOW trail
// west of the rect and includes it in the re-render. The test
//   1. raises terrain on flat ground (a fresh full-power shadow trail
//      appears west of the stamp and gets flagged At_SHADOW),
//   2. undoes it,
//   3. asserts that the undo's dirty area covers the trail and that a
//      re-render recomputes the trail: the flags are cleared (the restored
//      low terrain shadows nothing) and the lighting comes back to the
//      non-shadowed brightness.
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

#include <algorithm>
#include <cstdio>
#include <filesystem>
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

/// Load a private copy of the temporary test world into its own content root
/// (same pattern as the other brush tests).
bool prepareShadowTrailTestSession(const char* subdirName,
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

/// minimum shadow trail run (cells) the flat-terrain stamps are expected to
/// produce — full-power raises must be far past it so the assertion of exact
/// re-render coverage stays meaningful.
constexpr int kTrailThreshold = 16;

struct TrailCell
{
    int x = 0;
    int y = 0;
    unsigned char rnr = 0;
    unsigned char atr = 0;
};

bool runShadowTrailTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareShadowTrailTestSession("load_root_shadows", session);
    TEST_REQUIRE(loaded, "the shadow trail test could not load the world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the shadow trail test.");

    const int radius = 4;
    // A stamp area where the ground west of the stamp is flat and unflagged:
    // no pre-existing At_SHADOW within the westward window the raise's trail
    // would grow into, so the new trail is unambiguous. The stamp is kept far
    // from the east edge of the map line so the trail ends naturally.
    int cx = 0;
    int cy = 0;
    bool found = false;
    for (int y = 200; y < mapH - 200 && !found; y += 16) {
        for (int x = 200; x < mapW - 200; x += 16) {
            bool ok = true;
            for (int dy = -radius; dy <= radius && ok; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int off = mapOffset(x + dx, y + dy);
                    if (vMap.VxDBuf[off] != 0 ||
                        (vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST ||
                        (vMap.AtrBuf[off] & At_SHADOW) != 0) {
                        ok = false;
                        break;
                    }
                }
                // Westward window: cells the trail must fit into.
                for (int dx = kTrailThreshold + radius; ok && dx >= 1; --dx) {
                    const int off = mapOffset(x - dx, y + dy);
                    if ((vMap.AtrBuf[off] & At_SHADOW) != 0) {
                        ok = false;
                    }
                }
            }
            if (ok) {
                cx = x;
                cy = y;
                found = true;
                break;
            }
        }
    }
    TEST_REQUIRE(found, "no flat unshadowed stamp area was found.");
    std::printf("Shadow trail test stamp area around (%d, %d)\n", cx, cy);

    // --- 1. Raise with a full-power stamp: a shadow trail grows west. ------
    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.shape = BrushShape::FilledCircle;
    params.radius = radius;
    params.strength = 100;               // delta = 100 voxels per stamp
    params.mode = BrushMode::Geo;
    params.direction = 1;
    params.effect = BrushEffect::ChangeHeight;

    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the raise stamp was refused.");

    // Render the dirty ticket the brush created (renderQuant drains
    // renderAreas — the same work ViewportRenderer::update() step 1 does).
    vMap.renderQuant();

    // The trail: At_SHADOW-flagged cells west of the stamp area that were
    // rendered dark while the ridge was up.
    std::vector<TrailCell> trail;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = radius + 1; dx <= kTrailThreshold + 64; ++dx) {
            const int x = cx - dx;
            const int off = mapOffset(x, cy + dy);
            if (vMap.AtrBuf[off] & At_SHADOW) {
                TrailCell cell;
                cell.x = x;
                cell.y = cy + dy;
                cell.rnr = vMap.RnrBuf[off];
                cell.atr = vMap.AtrBuf[off];
                trail.push_back(cell);
            }
        }
    }
    TEST_REQUIRE(trail.size() >= 32,
                 "the raise did not leave a shadow trail west of the stamp "
                 "(found %zu cells) — the test scenario is invalid.",
                 trail.size());
    std::printf("Trail after raise: %zu flagged cells\n", trail.size());

    // --- 2. Undo: the dirty ticket must cover the trail. -------------------
    TEST_REQUIRE(session.undo(), "the undo failed.");

    // The undo push happened before the test can drain changedAreas —
    // inspect+clear them here (ViewportRenderer::update() would do the same).
    TEST_REQUIRE(!vMap.changedAreas.empty(),
                 "the undo must push dirty rectangles for the viewport.");
    int changedMinX = mapW;
    for (const sRectS& r : vMap.changedAreas) {
        changedMinX = std::min(changedMinX, static_cast<int>(r.x));
    }
    const int snapMinX = cx - radius - 1; // what a plain snapped re-render covers
    TEST_REQUIRE(changedMinX < snapMinX,
                 "the undo's dirty area must extend over the shadow trail "
                 "west of the snapshot: min x %d, snapped rect starts at %d.",
                 changedMinX, snapMinX);
    vMap.changedAreas.clear();

    vMap.renderQuant();

    // --- 3. The trail cells must be recomputed after the restore. ----------
    std::size_t clearedFlags = 0;
    for (const TrailCell& cell : trail) {
        const int off = mapOffset(cell.x, cell.y);
        const unsigned char atr = vMap.AtrBuf[off];
        if (!(atr & At_SHADOW)) {
            ++clearedFlags;
        }
        TEST_REQUIRE(vMap.RnrBuf[off] != cell.rnr || !(atr & At_SHADOW),
                     "trail cell (%d,%d) kept its stale state after undo: "
                     "RnrBuf %u vs %u, atr %02X vs %02X.",
                     cell.x, cell.y, vMap.RnrBuf[off], cell.rnr,
                     atr, cell.atr);
    }
    TEST_REQUIRE(clearedFlags == trail.size(),
                 "every restored trail cell must come out of shadow "
                 "(%zu of %zu kept the flag).",
                 trail.size() - clearedFlags, trail.size());
    std::printf("[PASS] undo re-renders the shadow trail (%zu cells, %d west "
                "past the snapshot).\n",
                trail.size(), snapMinX - changedMinX);

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Undo renders the shadow trail", 65, runShadowTrailTests);

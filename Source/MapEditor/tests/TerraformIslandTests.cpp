// ============================================================================
// In-game terraforming of a transparent-split island (regression).
//
// Reproduces the EXTDEMO scenario: a high parallelepiped whose middle was
// painted transparent, splitting it into a lower solid part and an upper
// "island" cap. The game's leveling tool (individualToolzer<
// T2TE_ALIGNMENT_TERRAIN_4ZP>, driven by the Brigadier's trucks via
// digToolzer4K) must remove the WHOLE column down to hZeroPlast:
//   * the transparent-top invariant holds after every dig step (a
//     transparent voxel may never become the top voxel — the surface
//     collapses to the nearest solid voxel instead, see
//     vrtMap::normalizeTransparentColumn);
//   * vMap.terraVersion is bumped, otherwise VoxCavityOverlay::drainPending
//     drops the terraforming rects as "vision-only" and the island cap +
//     walls stay rendered forever;
//   * the end state renders nothing above hZeroPlast.
//
// Runs on its own content root (order 127), same harness as VoxLayerTests.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "WorldFiles.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"
#include "tools.h"

#include "VoxCavity.h"
#include "VoxLayers.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

const int kPx = 1400;
const int kPy = 1400;
const int kHalf = 4; // 9x9 island patch

int offAt(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

bool buildLoadRoot(const fs::path& gameDir, const fs::path& savedWorldDir,
                   const fs::path& worldName, const fs::path& loadRoot)
{
    std::error_code ec;
    fs::remove_all(loadRoot, ec);
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(gameDir / "RESOURCE" / "GeoTx",
                                             loadRoot / "RESOURCE" / "GeoTx");
    fs::copy_file(gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);
    MapEditor::Tests::copyDirectoryRecursive(
        savedWorldDir, loadRoot / "RESOURCE" / "Worlds" / worldName);
    return true;
}

bool columnTopIsTransparent(int off)
{
    const int h = vMap.VxDBuf[off];
    return h > 0 &&
           vMap.voxLayers.isTransparent(static_cast<std::uint32_t>(off), h - 1);
}

bool runTerraformIslandTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_terraform_island";
    TEST_REQUIRE(buildLoadRoot(ctx.gameDir, ctx.tempWorldDir,
                               fs::u8path(ctx.worldName), loadRoot),
                 "could not build the load content root.");

    MapEditor::EditorSession session;
    session.setUndoCoalescingWindowMs(0);
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");
    TEST_REQUIRE(session.isWorldLoaded(), "session reports no loaded world.");

    // Island layout, relative to the zeroplast plane (EXTDEMO block A):
    //   geo base at hZ+35, dam column up to hZ+385, transparent band
    //   [hZ+186..hZ+285] splitting off the upper cap [hZ+286..hZ+385].
    const int hZ = vMap.hZeroPlast;
    const int kBase = hZ + 35;
    const int kTop = hZ + 385;
    const int kBand0 = hZ + 186;
    const int kBand1 = hZ + 285;
    const int kBandSolidTop = kBand0 - 1; // nearest solid below the band

    for (int j = -kHalf; j <= kHalf; ++j) {
        for (int i = -kHalf; i <= kHalf; ++i) {
            const int off = offAt(kPx + i, kPy + j);
            vMap.VxGBuf[off] = static_cast<vrtMap::VxWholeHeight>(kBase);
            vMap.VxDBuf[off] = static_cast<vrtMap::VxWholeHeight>(kTop);
            vMap.AtrBuf[off] = 0;
            vMap.SurBuf[off] = 5;
            vMap.voxLayers.paint(static_cast<std::uint32_t>(off), kBand0,
                                 kBand1, 0, 0, 0, true);
        }
    }
    vMap.voxLayers.buildBlockIndex();

    // The game dig path goes through tVoxSet, which scales the delta by the
    // cell hardness (0 = non-diggable). Make sure the patch and the tool
    // reach around it sit on diggable ground regardless of the world's
    // hardness.bin.
    for (int gy = (kPy - kHalf - 24) >> kmGrid;
         gy <= (kPy + kHalf + 24) >> kmGrid; ++gy) {
        for (int gx = (kPx - kHalf - 24) >> kmGrid;
             gx <= (kPx + kHalf + 24) >> kmGrid; ++gx) {
            const int goff = vMap.offsetGBuf(vMap.XCYCLG(gx), vMap.YCYCLG(gy));
            vMap.GABuf[goff] = static_cast<unsigned short>(
                (vMap.GABuf[goff] & ~GRIDAT_MASK_HARDNESS) |
                GRIDAT_MASK_HARDNESS);
        }
    }

    const std::uint64_t terraVersionBefore = vMap.terraVersion;

    // The truck dig: digToolzer4K::simpleQuant phases 0+1 — a 4ZP dig disk
    // (radius 10, dh=-3 voxels) followed by the dh=0 leveling/marking pass
    // (radius 20), with the approximation target at hZeroPlast.
    individualToolzer<T2TE_ALIGNMENT_TERRAIN_4ZP> digToolzer;
    digToolzer.setHAppoximation(vMap.hZeroPlast);

    const int kTarget = hZ << VX_FRACTION;
    const int centerOff = offAt(kPx, kPy);
    bool reached = false;
    bool collapseChecked = false;
    int iterations = 0;
    for (; iterations < 4000; ++iterations) {
        digToolzer.influenceDZ(kPx, kPy, 10, -3 << VX_FRACTION, 3);
        digToolzer.influenceDZ(kPx, kPy, 20, 0, 5);

        // Transparent-top invariant after every step of the leveling.
        for (int j = -kHalf; j <= kHalf; ++j) {
            for (int i = -kHalf; i <= kHalf; ++i) {
                TEST_REQUIRE(!columnTopIsTransparent(offAt(kPx + i, kPy + j)),
                             "transparent voxel became the column top at "
                             "(%d,%d) during 4ZP leveling.",
                             kPx + i, kPy + j);
            }
        }

        const int h = vMap.VxDBuf[centerOff];
        if (!collapseChecked && h <= kBand1) {
            // The step where the dig reaches the transparent band: the
            // surface must collapse to the nearest solid voxel below and
            // the band itself must stay untouched (no accidental
            // solidify — that would mean normalize got a truncated old
            // height and treated the dig as a raise).
            TEST_REQUIRE(h == kBand0,
                         "digging into the transparent band must collapse "
                         "the surface to %d, got %d.",
                         kBand0, h);
            TEST_REQUIRE(vMap.voxLayers.isTransparent(
                             static_cast<std::uint32_t>(centerOff),
                             (kBand0 + kBand1) / 2),
                         "the transparent band must survive the collapse.");
            collapseChecked = true;
        }

        if (vMap.SGetAlt(centerOff) == kTarget) {
            reached = true;
            break;
        }
    }

    TEST_REQUIRE(reached,
                 "truck-leveling the island did not reach hZeroPlast in %d "
                 "iterations (center height %d, target %d).",
                 iterations, vMap.SGetAlt(centerOff) >> VX_FRACTION, hZ);
    TEST_REQUIRE(collapseChecked,
                 "the dig never reached the transparent band — test setup "
                 "is wrong.");
    TEST_REQUIRE(vMap.terraVersion > terraVersionBefore,
                 "4ZP leveling did not bump vMap.terraVersion — the cavity "
                 "overlay would drop the update and keep rendering the "
                 "island.");

    // The whole patch must be leveled to the zeroplast plane...
    for (int j = -kHalf; j <= kHalf; ++j) {
        for (int i = -kHalf; i <= kHalf; ++i) {
            const int off = offAt(kPx + i, kPy + j);
            TEST_REQUIRE(vMap.SGetAlt(off) == kTarget,
                         "island cell (%d,%d) not leveled: height %d, "
                         "target %d.",
                         kPx + i, kPy + j, vMap.SGetAlt(off) >> VX_FRACTION,
                         hZ);
            TEST_REQUIRE(vMap.cavityDisplayHeight(off) < 0,
                         "cavity display height must be gone after leveling "
                         "(%d,%d).",
                         kPx + i, kPy + j);
        }
    }

    // ...and render nothing above the plane: the mesher must produce no
    // geometry for the leveled columns (the island cap + walls are gone).
    for (int j = -kHalf; j <= kHalf; ++j) {
        for (int i = -kHalf; i <= kHalf; ++i) {
            std::vector<VoxCavityVertex> tris;
            voxCavityAppendColumn(vMap, kPx + i, kPy + j, tris);
            for (const VoxCavityVertex& v : tris) {
                TEST_REQUIRE(v.z <= static_cast<float>(hZ + 1),
                             "leftover island geometry above zeroplast at "
                             "(%d,%d): z=%.0f.",
                             kPx + i, kPy + j, v.z);
            }
        }
    }

    session.unloadWorld();
    return true;
}

// Painted island column leveled in GAME mode: the layers above the final
// surface must be pruned to nothing AND the true-color top paint cleared
// (a leveled surface shows the zeroplast/common color, not the old
// decal). Editor semantics are pinned by other tests; here we drive
// vrtMap::normalizeTransparentColumn exactly like the game wrapper does
// (pruneVoxRangesAboveSurface on).
bool runTerraformPaintClearTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_terraform_paint";
    std::error_code ec;
    fs::remove_all(loadRoot, ec);
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    MapEditor::Tests::copyDirectoryRecursive(ctx.gameDir / "RESOURCE" / "GeoTx",
                                             loadRoot / "RESOURCE" / "GeoTx");
    fs::copy_file(ctx.gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);
    MapEditor::Tests::copyDirectoryRecursive(
        ctx.tempWorldDir, loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName));

    MapEditor::EditorSession session;
    session.setUndoCoalescingWindowMs(0);
    session.setGameDir(loadRoot.u8string());
    session.scanWorlds();
    session.loadWorld(0);

    // Game hygiene on for this test (same as terHyperSpace sets).
    vMap.pruneVoxRangesAboveSurface = true;

    const int hZ = vMap.hZeroPlast;
    const int kBase = hZ + 35;
    const int kTop = hZ + 385;
    const int PX = 1500, PY = 1500;
    const int off = PY * vMap.H_SIZE + PX;
    vMap.VxGBuf[off] = static_cast<vrtMap::VxWholeHeight>(kBase);
    vMap.VxDBuf[off] = static_cast<vrtMap::VxWholeHeight>(kTop);
    vMap.AtrBuf[off] = 0;
    vMap.SurBuf[off] = 5;
    // Solid painted range below the top + a transparent band.
    vMap.voxLayers.paint(static_cast<std::uint32_t>(off), kTop - 40, kTop - 5,
                         0x10, 0x20, 0x30, false);
    vMap.voxLayers.paint(static_cast<std::uint32_t>(off), kTop - 184, kTop - 85,
                         0, 0, 0, true);
    vMap.voxLayers.buildBlockIndex();
    // True-color decal on the top.
    vMap.ensureSurColBuf();
    vMap.SurColBuf[off] = 0xFF000000u | 0x00ABCDEF;

    // Drive the dig down to the zeroplast plane in game-sized steps,
    // calling the game wrapper each height change.
    individualToolzer<T2TE_ALIGNMENT_TERRAIN_4ZP> toolzer;
    toolzer.setHAppoximation(vMap.hZeroPlast);
    for (int iter = 0; iter < 4000 &&
                        vMap.SGetAlt(off) != (hZ << VX_FRACTION);
         ++iter) {
        toolzer.influenceDZ(PX, PY, 10, -3 << VX_FRACTION, 3);
        toolzer.influenceDZ(PX, PY, 20, 0, 5);
    }
    TEST_REQUIRE(vMap.SGetAlt(off) == (hZ << VX_FRACTION),
                 "painted column not leveled to zeroplast.");
    TEST_REQUIRE(vMap.voxLayers.findColumn(static_cast<std::uint32_t>(off)) < 0 ||
                     [] (int idx) {
                         int count = 0;
                         (void)vMap.voxLayers.columnRanges(idx, &count);
                         return count == 0;
                     }(vMap.voxLayers.findColumn(static_cast<std::uint32_t>(off))),
                 "layers must be pruned to nothing after leveling.");
    TEST_REQUIRE(vMap.SurColBuf[off] == 0,
                 "true-color paint must be cleared when prune cuts the "
                 "column (leveled surface shows zeroplast/common color).");

    // Wall merging: a run of identical hollow columns against lower
    // ground merges into ONE quad set (unshaded-color comparison; the
    // old shaded comparison never merged and left ~700k verts).
    const int MX = 1700, MY = 1700;
    std::vector<std::uint32_t> offs;
    for (int i = 0; i < 4; ++i) {
        const int moff = MY * vMap.H_SIZE + (MX + i);
        vMap.VxGBuf[moff] = 10;
        vMap.VxDBuf[moff] = 40;
        vMap.AtrBuf[moff] = 0;
        vMap.SurBuf[moff] = 5;
        vMap.voxLayers.paint(static_cast<std::uint32_t>(moff), 20, 25,
                             0, 0, 0, true);
        // Lower plain neighbor to the +y side: walls exist.
        const int noff = (MY + 1) * vMap.H_SIZE + (MX + i);
        vMap.VxGBuf[noff] = 5;
        vMap.VxDBuf[noff] = 0;
        vMap.AtrBuf[noff] = 0;
        offs.push_back(static_cast<std::uint32_t>(moff));
    }
    vMap.voxLayers.buildBlockIndex();
    std::vector<VoxCavityVertex> tris;
    voxCavityAppendRowWallY(vMap, MY, offs.data(), offs.size(), tris);
    // Two runs per column ([5..19] and [26..39]); 4 identical columns
    // merged -> 2 quads = 12 verts. Unmerged would be 48.
    TEST_REQUIRE(tris.size() == 12,
                 "identical neighbor columns must merge into one wall quad "
                 "set: got %zu verts, want 12.", tris.size());

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Terraforming removes transparent-split island",
                         127, runTerraformIslandTests);
MAP_EDITOR_REGISTER_TEST("Leveling painted columns clears paint and "
                         "layers (game)",
                         129, runTerraformPaintClearTests);

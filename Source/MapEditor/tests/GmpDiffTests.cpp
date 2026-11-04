// ============================================================================
// Difference mode + .gmp — EditorSession diff model in the game's legacy
// format (vrtMap::loadGameMap/saveGameMap, VMAP.cpp:952-1090).
//
// Checks on a loaded world:
//
//   1. mode toggle: ON snapshots the base and shows BASE+DIFF (brush edits
//      land in the difference model via the regRender hook); OFF restores
//      the base terrain — the difference model survives for the next ON;
//   2. .gmp save: header (world index) + coarse-area flags + the four
//      absolute blocks per flagged area, exactly the game's layout;
//   3. .gmp load: a hand-crafted file with absolute values overlays the base
//      (the game semantics), in both toggle orders;
//   4. undo re-syncs the difference model (undo in diff mode → empty .gmp);
//   5. save → (mode off) → load round-trips the same terrain.
//
// Runs after the world round-trip (order 98) and reuses its temp world.
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

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;

/// Byte size of one .gmp area block (64x64).
constexpr std::streamsize kBlock = sizeCellGridCA * sizeCellGridCA;

/// Write a .gmp with a single flagged area whose four blocks are filled with
/// the given constants (absolute values, the game's format).
bool writeSingleAreaGmp(const fs::path& path, int gw, int gh, int area,
                        unsigned char geo, unsigned char dam,
                        unsigned char atr, unsigned char sur)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const int worldIndex = 0;
    out.write(reinterpret_cast<const char*>(&worldIndex), sizeof(worldIndex));
    std::vector<unsigned char> flags(static_cast<std::size_t>(gw) * gh, 0);
    flags[static_cast<std::size_t>(area)] = 1;
    out.write(reinterpret_cast<const char*>(flags.data()),
              static_cast<std::streamsize>(flags.size()));
    const unsigned char fills[4] = {geo, dam, atr, sur};
    for (int b = 0; b < 4; ++b) {
        const std::vector<unsigned char> block(
            static_cast<std::size_t>(kBlock), fills[b]);
        out.write(reinterpret_cast<const char*>(block.data()), kBlock);
    }
    return out.good();
}

bool runGmpDiffTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    const fs::path loadRoot = ctx.tempRoot / "load_root_gmp";
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the load content root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.tempWorldDir, loadRoot / "RESOURCE" / "Worlds" /
                                  fs::u8path(ctx.worldName));
    }

    MapEditor::EditorSession session;
    session.setUndoCoalescingWindowMs(0);
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int gw = mapW >> kmGridChA;
    const int gh = mapH >> kmGridChA;
    // Centre of a coarse area (map/2 lies exactly on an area boundary).
    const int cx = (mapW / 2) + (sizeCellGridCA / 2);
    const int cy = (mapH / 2) + (sizeCellGridCA / 2);
    const auto altAt = [](int x, int y) {
        return static_cast<int>(vMap.SGetAlt(x, y));
    };
    const int baseAlt = altAt(cx, cy);

    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.radius = 4;
    params.strength = 10;
    params.mode = BrushMode::Dam;
    params.effect = BrushEffect::ChangeHeight;
    params.direction = 1;

    // ------------------------------------------------------------------
    // 1: toggle semantics — ON shows BASE+DIFF and captures brush edits,
    //    OFF restores the base, the model survives for the next ON.
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.setDiffMode(true), "setDiffMode(true) refused.");
    TEST_REQUIRE(session.isDiffMode(), "the mode must be on.");
    TEST_REQUIRE(!session.canUndo(),
                 "the mode switch must clear the undo history.");
    TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the diff stamp refused.");
    TEST_REQUIRE(session.hasDiffData(),
                 "the brush edit must land in the difference model.");
    TEST_REQUIRE(altAt(cx, cy) > baseAlt,
                 "the effective terrain must show the stamp.");
    TEST_REQUIRE(session.setDiffMode(false), "setDiffMode(false) refused.");
    TEST_REQUIRE(altAt(cx, cy) == baseAlt,
                 "OFF must show the base terrain.");
    TEST_REQUIRE(session.hasDiffData(),
                 "the difference model must survive the OFF toggle.");
    TEST_REQUIRE(session.setDiffMode(true), "setDiffMode(true) refused.");
    TEST_REQUIRE(altAt(cx, cy) > baseAlt,
                 "ON must re-apply the difference data.");
    std::printf("[PASS] difference mode toggles base vs base+diff and "
                "captures brush edits.\n");

    // ------------------------------------------------------------------
    // 2: .gmp save — the game's layout: header, flags, absolute blocks.
    // ------------------------------------------------------------------
    const fs::path gmpPath = ctx.tempRoot / "gmp" / "test.gmp";
    fs::create_directories(gmpPath.parent_path());
    TEST_REQUIRE(session.saveDiffToGmp(gmpPath.u8string()),
                 "saveDiffToGmp failed.");
    {
        std::ifstream in(gmpPath, std::ios::binary);
        TEST_REQUIRE(static_cast<bool>(in), "cannot re-read the .gmp.");
        int worldIndex = -1;
        in.read(reinterpret_cast<char*>(&worldIndex), sizeof(worldIndex));
        TEST_REQUIRE(static_cast<bool>(in), "missing header.");
        std::vector<unsigned char> flags(static_cast<std::size_t>(gw) * gh, 0);
        in.read(reinterpret_cast<char*>(flags.data()),
                static_cast<std::streamsize>(flags.size()));
        TEST_REQUIRE(static_cast<bool>(in), "missing flags.");
        int flagged = 0;
        int flaggedArea = -1;
        for (int a = 0; a < gw * gh; ++a) {
            if (flags[static_cast<std::size_t>(a)] == 1) {
                ++flagged;
                flaggedArea = a;
            }
        }
        TEST_REQUIRE(flagged == 1,
                     "one coarse area must be flagged, got %d.", flagged);
        TEST_REQUIRE(flaggedArea == (cy >> kmGridChA) * gw + (cx >> kmGridChA),
                     "the flagged area must cover the stamp centre.");
        std::vector<unsigned char> block(static_cast<std::size_t>(kBlock));
        in.read(reinterpret_cast<char*>(block.data()), kBlock);
        TEST_REQUIRE(static_cast<bool>(in), "missing geo block.");
        const int cell = (cy & (sizeCellGridCA - 1)) * sizeCellGridCA +
                         (cx & (sizeCellGridCA - 1));
        TEST_REQUIRE(block[static_cast<std::size_t>(cell)] ==
                         vMap.VxGBuf[cy * mapW + cx],
                     "the geo block must carry the absolute stamped value.");
        std::vector<unsigned char> rest(
            static_cast<std::size_t>(kBlock) * 3);
        in.read(reinterpret_cast<char*>(rest.data()),
                static_cast<std::streamsize>(rest.size()));
        TEST_REQUIRE(static_cast<bool>(in), "missing dam/atr/sur blocks.");
        in.get();
        TEST_REQUIRE(in.eof(), "no trailing bytes — the core game format.");
    }
    std::printf("[PASS] .gmp save matches the game's format (header, flags, "
                "absolute blocks).\n");

    // ------------------------------------------------------------------
    // 3: .gmp load — a hand-crafted file overlays ABSOLUTE values.
    // ------------------------------------------------------------------
    {
        const int areaX = 96 >> kmGridChA;
        const int areaY = 96 >> kmGridChA;
        const int area = areaY * gw + areaX;
        const fs::path crafted = ctx.tempRoot / "gmp" / "crafted.gmp";
        TEST_REQUIRE(writeSingleAreaGmp(crafted, gw, gh, area,
                                        /*geo=*/40, /*dam=*/100,
                                        /*atr=*/0, /*sur=*/5),
                     "could not write the crafted .gmp.");
        // Load while ON: the display updates at once.
        TEST_REQUIRE(session.loadDiffFromGmp(crafted.u8string()),
                     "loadDiffFromGmp refused.");
        TEST_REQUIRE(altAt(96, 96) == (100 << VX_FRACTION),
                     "the absolute dam value (100) must win over the base.");
        TEST_REQUIRE(vMap.SurBuf[96 * mapW + 96] == 5,
                     "the absolute SurBuf value must apply.");
        // A stamp somewhere else must not resurrect the old difference.
        TEST_REQUIRE(session.setDiffMode(false), "toggle OFF refused.");
        TEST_REQUIRE(altAt(96, 96) != (100 << VX_FRACTION),
                     "OFF must hide the loaded difference.");
        TEST_REQUIRE(session.setDiffMode(true), "toggle ON refused.");
        TEST_REQUIRE(altAt(96, 96) == (100 << VX_FRACTION),
                     "ON must show the loaded difference.");
    }
    std::printf("[PASS] .gmp load overlays absolute values (the game "
                "semantics), in both toggle orders.\n");

    // ------------------------------------------------------------------
    // 4: undo re-syncs the difference model.
    // ------------------------------------------------------------------
    {
        // Start from an empty difference model (the crafted area from step 3
        // must not confuse the check).
        const fs::path emptyIn = ctx.tempRoot / "gmp" / "empty_in.gmp";
        {
            std::ofstream out(emptyIn, std::ios::binary | std::ios::trunc);
            const int worldIndex = 0;
            out.write(reinterpret_cast<const char*>(&worldIndex),
                      sizeof(worldIndex));
            const std::vector<unsigned char> flags(
                static_cast<std::size_t>(gw) * gh, 0);
            out.write(reinterpret_cast<const char*>(flags.data()),
                      static_cast<std::streamsize>(flags.size()));
        }
        TEST_REQUIRE(session.loadDiffFromGmp(emptyIn.u8string()),
                     "loading the empty difference failed.");
        TEST_REQUIRE(!session.hasDiffData(),
                     "the empty difference must clear the model.");
        const int before = altAt(cx, cy);
        TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the stamp refused.");
        TEST_REQUIRE(session.hasDiffData(), "the stamp must add difference.");
        TEST_REQUIRE(session.undo(), "undo failed.");
        TEST_REQUIRE(altAt(cx, cy) == before, "undo must restore the cell.");
        TEST_REQUIRE(!session.hasDiffData(),
                     "undo must re-sync the difference model to empty.");
        const fs::path empty = ctx.tempRoot / "gmp" / "empty.gmp";
        TEST_REQUIRE(session.saveDiffToGmp(empty.u8string()),
                     "saving the empty difference failed.");
        TEST_REQUIRE(fs::file_size(empty) ==
                         sizeof(int) + static_cast<std::uintmax_t>(gw) * gh,
                     "an empty difference is header + flags only.");
    }
    std::printf("[PASS] undo re-syncs the difference model.\n");

    // ------------------------------------------------------------------
    // 5: save -> (mode off) -> load round-trips the same terrain.
    // ------------------------------------------------------------------
    {
        TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the stamp refused.");
        const int stampAlt = altAt(cx, cy);
        TEST_REQUIRE(stampAlt > baseAlt,
                     "the stamp must raise the effective terrain.");
        const fs::path round = ctx.tempRoot / "gmp" / "roundtrip.gmp";
        TEST_REQUIRE(session.saveDiffToGmp(round.u8string()),
                     "round-trip save failed.");
        TEST_REQUIRE(session.setDiffMode(false), "toggle OFF refused.");
        TEST_REQUIRE(altAt(cx, cy) == baseAlt,
                     "OFF must hide the stamped difference.");
        TEST_REQUIRE(session.loadDiffFromGmp(round.u8string()),
                     "round-trip load failed.");
        TEST_REQUIRE(session.setDiffMode(true), "toggle ON refused.");
        TEST_REQUIRE(altAt(cx, cy) == stampAlt,
                     "the round-tripped difference must reproduce the stamp.");
    }
    std::printf("[PASS] .gmp save/load round-trips the difference data.\n");

    // ------------------------------------------------------------------
    // 6: the top-down previews come from the BASE terrain — the game's
    //    map-selection screen never applies .gmp difference data.
    // ------------------------------------------------------------------
    {
        const fs::path worldDir =
            loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName);
        auto readBytes = [](const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::vector<unsigned char>(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
        };

        // References with the mode OFF (base terrain), via the session's
        // image regeneration (the same path the autofix and the save use).
        TEST_REQUIRE(session.setDiffMode(false), "toggle OFF refused.");
        TEST_REQUIRE(session.regenerateWorldImages(),
                     "reference image regeneration failed.");
        const std::vector<unsigned char> refMap =
            readBytes(worldDir / "map.tga");
        TEST_REQUIRE(!refMap.empty(), "reference map.tga is empty.");
        const fs::path refTopPath = ctx.tempRoot / "gmp" / "ref_topview.tga";
        TEST_REQUIRE(session.exportTopView(refTopPath.u8string()),
                     "reference top view failed.");
        const std::vector<unsigned char> refTop = readBytes(refTopPath);

        // Difference ON + a stamp: the previews must not change.
        TEST_REQUIRE(session.setDiffMode(true), "toggle ON refused.");
        TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the stamp refused.");
        TEST_REQUIRE(session.regenerateWorldImages(),
                     "diff-mode image regeneration failed.");
        TEST_REQUIRE(readBytes(worldDir / "map.tga") == refMap,
                     "map.tga must be generated from the BASE terrain.");
        const fs::path topPath = ctx.tempRoot / "gmp" / "diff_topview.tga";
        TEST_REQUIRE(session.exportTopView(topPath.u8string()),
                     "diff-mode top view failed.");
        TEST_REQUIRE(readBytes(topPath) == refTop,
                     "Export Top View must ignore the difference data.");
        // And the world save keeps the base preview as well.
        TEST_REQUIRE(session.saveWorld(), "the world save failed.");
        TEST_REQUIRE(readBytes(worldDir / "map.tga") == refMap,
                     "saving the world must keep map.tga base-only.");
    }
    std::printf("[PASS] top-down previews (map.tga / Export Top View) are "
                "generated from the base terrain, difference mode or not.\n");

    // ------------------------------------------------------------------
    // 7: difference-mode edits never dirty the WORLD — the exit prompt
    //    saves the .gmp, not the world terrain.
    // ------------------------------------------------------------------
    {
        // Step 6 ended with saveWorld() — the world is clean, and a stamped
        // (unsaved) difference is still there.
        TEST_REQUIRE(!session.hasUnsavedChanges(),
                     "the world must be clean after saveWorld().");
        const fs::path flagPath = ctx.tempRoot / "gmp" / "flags.gmp";
        TEST_REQUIRE(session.saveDiffToGmp(flagPath.u8string()),
                     "the difference save failed.");
        TEST_REQUIRE(!session.hasUnsavedDiffChanges(),
                     "saving the .gmp clears the difference flag.");

        TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the stamp refused.");
        TEST_REQUIRE(!session.hasUnsavedChanges(),
                     "difference-mode edits must NOT dirty the world.");
        TEST_REQUIRE(session.hasUnsavedDiffChanges(),
                     "the difference data must carry its own unsaved flag.");
        TEST_REQUIRE(session.undo(), "undo failed.");
        TEST_REQUIRE(!session.hasUnsavedChanges(),
                     "undo in difference mode must not dirty the world.");
        TEST_REQUIRE(session.hasUnsavedDiffChanges(),
                     "undo in difference mode still touches the difference "
                     "data.");

        // OFF-mode edits dirty the world as before.
        TEST_REQUIRE(session.setDiffMode(false), "toggle OFF refused.");
        TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the stamp refused.");
        TEST_REQUIRE(session.hasUnsavedChanges(),
                     "off-mode edits must dirty the world as before.");
    }
    std::printf("[PASS] difference-mode edits never dirty the world; the "
                "difference data tracks its own unsaved flag.\n");

    // ------------------------------------------------------------------
    // 8: File/Save NEVER merges difference data into the world; File/Save
    //    Difference works with the mode OFF.
    // ------------------------------------------------------------------
    {
        // OFF mode (step 7 ended with the mode off): the .gmp save still
        // serializes the loaded difference model.
        const fs::path offIn = ctx.tempRoot / "gmp" / "offmode_in.gmp";
        TEST_REQUIRE(writeSingleAreaGmp(offIn, gw, gh, /*area=*/0,
                                        /*geo=*/40, /*dam=*/100,
                                        /*atr=*/0, /*sur=*/5),
                     "could not write the off-mode fixture.");
        TEST_REQUIRE(session.loadDiffFromGmp(offIn.u8string()),
                     "loading with the mode OFF failed.");
        const fs::path offOut = ctx.tempRoot / "gmp" / "offmode_out.gmp";
        TEST_REQUIRE(session.saveDiffToGmp(offOut.u8string()),
                     "saving with the mode OFF failed.");
        std::ifstream a(offIn, std::ios::binary);
        std::ifstream b(offOut, std::ios::binary);
        TEST_REQUIRE(
            std::vector<unsigned char>(
                (std::istreambuf_iterator<char>(a)),
                std::istreambuf_iterator<char>()) ==
                std::vector<unsigned char>(
                    (std::istreambuf_iterator<char>(b)),
                    std::istreambuf_iterator<char>()),
            "the .gmp save must work with the mode OFF.");

        // Difference ON + a stamp + File/Save: the on-disk world must hold
        // the BASE terrain — a reload proves the difference data stayed out.
        TEST_REQUIRE(session.setDiffMode(true), "toggle ON refused.");
        const int baseAlt = altAt(cx, cy);
        TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the stamp refused.");
        TEST_REQUIRE(altAt(cx, cy) > baseAlt,
                     "the stamp must raise the effective terrain.");
        TEST_REQUIRE(session.saveWorld(), "the world save failed.");
        session.unloadWorld();
        TEST_REQUIRE(session.loadWorld(0), "the reload failed.");
        TEST_REQUIRE(altAt(cx, cy) == baseAlt,
                     "File/Save must NOT merge the difference data into the "
                     "world.");
    }
    std::printf("[PASS] File/Save writes the base terrain (never merges); "
                ".gmp save/load works in any mode.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("GMP difference mode", 98, runGmpDiffTests);

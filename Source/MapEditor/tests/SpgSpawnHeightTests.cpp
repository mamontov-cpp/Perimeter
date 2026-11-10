// ============================================================================
// T8 addendum — start-landing normalization, two halves (run before saving):
//
// 1. restPlayersOnZeroLayer — the game installs a frame at battle start AT
//    THE STORED Z (the frame body has gravity = 0 — RigidBodyPrm "Unit
//    Frame"), judging "landed" by a one-sided check
//    (`position().z + boxMin().z - vMap.hZeroPlast < 2`, IronFrame.cpp:173).
//    The grounded pose of the frame model is
//    `terrain height under the frame + kFrameOriginGroundOffset` (114.58 —
//    holds across every shipped attached=true map, e.g. PENTOLL 254.58-140,
//    XELA 234.624-120, COMPASS p3 291.58-177). The COMPASS template stores
//    a HOVER pose instead (ground + 114.58 + ~37, the RigidBodyPrm
//    flying_height = 40) — correct only while waiting to be installed;
//    installed from that z the frame hangs in the air (the EXTDEMO/MapDemo
//    bug). So the frame z (and the packed slots riding with it) is set to
//    the grounded pose over the terrain, and the squad z is pulled down to
//    the zero layer only when above it (a squad on low terrain stays put).
//
// 2. setFramesInstalledForStart — every shipped battle map except the
//    COMPASS family stores the frames attached = true / powered = true /
//    zeroLayerCounter = a unique positive id: universalLoad starts them
//    ATTACHED (resting on the zero layer immediately, with the zeroplast
//    disc pre-painted by loadZeroLayer) instead of hovering in ISOLATED
//    mode until a script or the player installs them.
//
// Checked here on copies of the shipped COMPASS.spg (zl=177; template p0
// frame z=328.353 is the hover pose, squad z=177; player 1 rests below the
// zero layer) with a flat-world terrain provider (everywhere 177):
//   * a lifted +30 frame returns to the grounded pose (177+114.58) and its
//     squad to the zero layer; a squad dropped -50 below the zero layer
//     stays put while its frame is grounded;
//   * every frame ends up attached/powered with a unique positive
//     zeroLayerCounter; a second pass is a byte-exact no-op.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "errh_wrapper.h"

#include "spg/SpgFile.h"
#include "spg/XPrmDocument.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

double vecZ(const MapEditor::XPrmObject* vec)
{
    const std::string raw = vec ? vec->scalar("z") : std::string();
    return raw.empty() ? 0.0 : std::strtod(raw.c_str(), nullptr);
}

MapEditor::XPrmObject* playerElement(MapEditor::XPrmDocument& doc, int index)
{
    MapEditor::XPrmObject* players =
        doc.block("SavePrm")->object->object("players");
    if (!players || static_cast<std::size_t>(index) + 1 >= players->fields.size()) {
        return nullptr;
    }
    return players->fields[static_cast<std::size_t>(index) + 1].object.get();
}

MapEditor::XPrmObject* frameOf(MapEditor::XPrmDocument& doc, int player)
{
    return playerElement(doc, player)->object("frame");
}

double squadZOf(MapEditor::XPrmDocument& doc, int player)
{
    MapEditor::XPrmObject* frame =
        playerElement(doc, player)->object("frame");
    return vecZ(frame->object("squad")->object("position"));
}

double frameZOf(MapEditor::XPrmDocument& doc, int player)
{
    return vecZ(playerElement(doc, player)->object("frame")
                    ->object("position"));
}

bool runSpgSpawnHeightTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path source = ctx.gameDir / "RESOURCE" / "Battle" / "COMPASS.spg";
    TEST_REQUIRE(fs::exists(source), "COMPASS.spg missing in the game dir.");
    const fs::path dir = ctx.tempRoot / "spg" / "spawnheight";
    fs::create_directories(dir);
    constexpr int kZl = 177;

    // Baseline from the untouched source.
    double baseFrame0 = 0.0;
    double baseSquad1 = 0.0;
    {
        MapEditor::XPrmDocument doc;
        TEST_REQUIRE(doc.load(source.u8string()), "source parse failed.");
        TEST_REQUIRE(std::abs(squadZOf(doc, 0) - kZl) < 1e-6,
                     "COMPASS player 0 squad z must equal the zero layer.");
        baseFrame0 = frameZOf(doc, 0);
        TEST_REQUIRE(std::abs(baseFrame0 - 328.353) < 1e-3,
                     "COMPASS player 0 frame z changed (%f).", baseFrame0);
        // Player 1 rests BELOW the zero layer on shipped COMPASS (low
        // terrain) — the one-sided game check counts it as landed.
        baseSquad1 = squadZOf(doc, 1);
        TEST_REQUIRE(baseSquad1 < kZl,
                     "the fixture expects COMPASS player 1 below the zero "
                     "layer, got %f.",
                     baseSquad1);
    }

    // Flat-world terrain provider: everywhere at the zero layer, like the
    // leveled spawn plots of the fixture world.
    const auto flatTerrain = [kZl](int, int) { return kZl; };
    const double kGroundedFrameZ =
        kZl + MapEditor::SpgFile::kFrameOriginGroundOffset;

    // 1: simulate the editor bug (player 0 lifted +30) and a squad resting
    //    below the zero layer (player 1 lowered -50), then normalize.
    const fs::path floatingPath = dir / "floating.spg";
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(source.u8string()), "load failed.");
        TEST_REQUIRE(spg.movePlayerGroup(0, 0.0, 0.0, 30.0),
                     "lift of player 0 failed.");
        TEST_REQUIRE(spg.movePlayerGroup(1, 0.0, 0.0, -50.0),
                     "drop of player 1 failed.");
        TEST_REQUIRE(spg.save(floatingPath.u8string()), "save failed.");
    }
    const fs::path normalizedPath = dir / "normalized.spg";
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(floatingPath.u8string()), "re-load failed.");
        TEST_REQUIRE(spg.restPlayersOnZeroLayer(kZl, flatTerrain),
                     "normalization refused.");
        TEST_REQUIRE(spg.setFramesInstalledForStart(),
                     "installed-state normalization refused.");
        TEST_REQUIRE(spg.save(normalizedPath.u8string()), "save failed.");
    }
    {
        MapEditor::XPrmDocument doc;
        TEST_REQUIRE(doc.load(normalizedPath.u8string()),
                     "normalized parse failed.");
        // Player 0: squad pulled down to the zero layer, the frame set to
        // the grounded pose (terrain + the model's origin offset) — NOT
        // the hover pose the COMPASS template stores (328.353).
        TEST_REQUIRE(std::abs(squadZOf(doc, 0) - kZl) < 1e-6,
                     "player 0 squad z must rest on the zero layer, got %f.",
                     squadZOf(doc, 0));
        TEST_REQUIRE(std::abs(frameZOf(doc, 0) - kGroundedFrameZ) < 1e-3,
                     "player 0 frame z must be the grounded pose %f, got %f.",
                     kGroundedFrameZ, frameZOf(doc, 0));
        TEST_REQUIRE(std::abs(frameZOf(doc, 0) - baseFrame0) > 30.0,
                     "the hover pose (%f) must have been replaced.",
                     baseFrame0);
        // Player 1: squad below the zero layer stays put; the frame still
        // gets its grounded pose over the (flat) terrain.
        TEST_REQUIRE(std::abs(squadZOf(doc, 1) - (baseSquad1 - 50.0)) < 1e-6,
                     "player 1 squad below the zero layer must stay "
                     "untouched, got %f.",
                     squadZOf(doc, 1));
        TEST_REQUIRE(std::abs(frameZOf(doc, 1) - kGroundedFrameZ) < 1e-3,
                     "player 1 frame z must be the grounded pose %f, got %f.",
                     kGroundedFrameZ, frameZOf(doc, 1));

        // Every frame stored "installed at start": attached + powered with
        // a unique positive zeroLayerCounter (loadZeroLayer pre-paints the
        // zeroplast disc for each; duplicate ids would drop a disc).
        std::set<int> counters;
        for (int i = 0; i < 4; ++i) {
            MapEditor::XPrmObject* frame = frameOf(doc, i);
            TEST_REQUIRE(frame->scalar("attached") == "true" &&
                             frame->scalar("powered") == "true",
                         "player %d frame must be stored attached+powered.",
                         i);
            int counter = 0;
            TEST_REQUIRE(frame->scalar("zeroLayerCounter") != "0;",
                         "player %d frame must carry a zeroplast counter.",
                         i);
            const std::string raw = frame->scalar("zeroLayerCounter");
            counter = std::atoi(raw.c_str());
            TEST_REQUIRE(counter > 0,
                         "player %d frame counter must be positive, got %d.",
                         i, counter);
            TEST_REQUIRE(counters.insert(counter).second,
                         "frame counters must be unique, %d repeats.", counter);
        }
    }
    std::printf("[PASS] frames rest on the grounded pose; squads above the "
                "zero layer are pulled down, below stays put; frames stored "
                "installed.\n");

    // 2: idempotency (both halves together, as the save path runs them).
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(normalizedPath.u8string()), "re-load failed.");
        TEST_REQUIRE(spg.restPlayersOnZeroLayer(kZl, flatTerrain),
                     "2nd normalize refused.");
        TEST_REQUIRE(spg.setFramesInstalledForStart(),
                     "2nd installed-state pass refused.");
        // Same file name in a subfolder: save() rewrites originalSaveName
        // to the map's own path, so equal output requires an equal name.
        const fs::path againPath = dir / "again" / "normalized.spg";
        fs::create_directories(againPath.parent_path());
        TEST_REQUIRE(spg.save(againPath.u8string()), "save failed.");
        TEST_REQUIRE(MapEditor::Tests::readFileBytes(againPath) ==
                         MapEditor::Tests::readFileBytes(normalizedPath),
                     "the normalization must be idempotent.");
    }
    std::printf("[PASS] the normalization is idempotent.\n");

    // 3: an already-normalized map survives a reload+resave unchanged; the
    //    installed-state half intentionally rewrites the COMPASS-family
    //    hover pose and attached=false template state (that is the fix).
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(normalizedPath.u8string()), "load failed.");
        TEST_REQUIRE(spg.restPlayersOnZeroLayer(kZl, flatTerrain),
                     "normalize refused.");
        // Same file name in a subfolder (save() rewrites originalSaveName to
        // the map's own path, so byte equality requires an equal name).
        const fs::path stablePath = dir / "stable" / "normalized.spg";
        fs::create_directories(stablePath.parent_path());
        TEST_REQUIRE(spg.save(stablePath.u8string()), "save failed.");
        TEST_REQUIRE(MapEditor::Tests::readFileBytes(stablePath) ==
                         MapEditor::Tests::readFileBytes(normalizedPath),
                     "a normalized map must save byte-exact.");

        MapEditor::SpgFile install;
        TEST_REQUIRE(install.load(source.u8string()), "load failed.");
        TEST_REQUIRE(install.restPlayersOnZeroLayer(kZl, flatTerrain),
                     "normalize refused.");
        TEST_REQUIRE(install.setFramesInstalledForStart(),
                     "installed-state pass refused.");
        const fs::path installedPath = dir / "installed.spg";
        TEST_REQUIRE(install.save(installedPath.u8string()), "save failed.");
        MapEditor::XPrmDocument doc;
        TEST_REQUIRE(doc.load(installedPath.u8string()), "parse failed.");
        TEST_REQUIRE(frameOf(doc, 0)->scalar("attached") == "true",
                     "the installed-state pass must flip attached.");
        TEST_REQUIRE(std::abs(frameZOf(doc, 0) - kGroundedFrameZ) < 1e-3,
                     "the template hover pose must be replaced by the "
                     "grounded pose.");
    }
    std::printf("[PASS] normalized maps are stable; the halves rewrite the "
                "template state by design.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("SPG spawn rest-height normalization", 73,
                         runSpgSpawnHeightTests);

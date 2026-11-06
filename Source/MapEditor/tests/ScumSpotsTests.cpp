// ============================================================================
// Scum nests — SpgFile::scumSpots()/addScumSpot()/setScumSpot*()/
// removeScumSpot()/syncScumTriggerChain()/buildScumTriggerScript().
//
// Checked against the shipped maps:
//   * Missions/03ISOLAND.spg — three SCUM_SPOT mounds, each paired with a
//     labeled FILTH_SPOT spawner (the isoland wiring this feature mirrors);
//   * Battle/COMPASS.spg — a pristine battle map with per-player
//     TriggerChainNames (mp-N.scr).
//
// Covers: pairing + filth parameter parsing, add (mound + spawner + label +
// unique unitIDs), pose/HP/filth edits, remove, the trigger chain
// registration add/remove/no-duplicate semantics, and the generated
// .scr text (game grammar: bare array tails, trigger count, labels).
//
// Runs after the unit model catalog tests (order 102). Writes only into the
// temp root.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "ScumSpotCatalog.h"
#include "spg/SpgFile.h"
#include "errh_wrapper.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

std::string readText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string collapseWs(std::string s)
{
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = !out.empty();
        } else {
            if (space) out += ' ';
            out += c;
            space = false;
        }
    }
    return out;
}

bool runScumSpotsTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path isoland = ctx.gameDir / "RESOURCE" / "Missions" /
                             "03ISOLAND.spg";
    TEST_REQUIRE(fs::exists(isoland), "03ISOLAND.spg missing in game dir.");
    const fs::path compass = ctx.gameDir / "RESOURCE" / "Battle" /
                             "COMPASS.spg";
    TEST_REQUIRE(fs::exists(compass), "COMPASS.spg missing in game dir.");

    // ------------------------------------------------------------------
    // 1: parse the shipped isoland nests (mounds + paired spawners).
    // ------------------------------------------------------------------
    MapEditor::SpgFile spg;
    TEST_REQUIRE(spg.load(isoland.u8string()), "load failed.");
    std::vector<MapEditor::SpgFile::ScumSpot> nests = spg.scumSpots();
    TEST_REQUIRE(nests.size() == 3,
                 "03ISOLAND must carry 3 scum mounds, got %zu.",
                 nests.size());
    for (const MapEditor::SpgFile::ScumSpot& s : nests) {
        TEST_REQUIRE(s.paired, "each shipped mound must pair with a spawner.");
        TEST_REQUIRE(!s.label.empty(), "the paired spawner needs a label.");
    }
    // Spot-check the mound set against the shipped layout.
    TEST_REQUIRE(nests[0].attributeId == 43 && nests[1].attributeId == 44 &&
                     nests[2].attributeId == 45,
                 "the shipped mounds are SCUM_SPOT/2/3 (got %d/%d/%d).",
                 nests[0].attributeId, nests[1].attributeId,
                 nests[2].attributeId);
    TEST_REQUIRE(near(nests[0].x, 3370.72) && near(nests[0].y, 3430.06),
                 "the first mound position must parse.");
    // Pairing on a foreign map is best-effort (isoland anchors several
    // spawners around one mound) — the editor's own nests are strictly 1:1
    // and that is exercised below.

    // ------------------------------------------------------------------
    // 2: add -> edit -> remove, save->re-parse round-trip, game grammar.
    // ------------------------------------------------------------------
    const fs::path edited = ctx.tempRoot / "spg" / "scum_edited.spg";
    const int baseMax = spg.maxUnitId();
    {
        MapEditor::SpgFile::ScumSpot added;
        added.attributeId = 44; // SCUM_SPOT2
        added.x = 1234.5;
        added.y = 678.25;
        added.z = 90.0 + 4.4335;
        added.angleDeg = 30.0;
        added.hp = 100;
        added.filth.filthType = 2; // WASP
        added.filth.creatureNum = 7;
        added.filth.sleep = true;
        added.filth.sleepPeriod = 55.f;
        added.filth.attackPeriond = 66.f;
        added.filth.attackPlayer = 2; // AI
        TEST_REQUIRE(spg.addScumSpot(added), "addScumSpot refused.");
        TEST_REQUIRE(added.unitId == baseMax + 1,
                     "mound unitId must be maxUnitId()+1, got %d.",
                     added.unitId);
        TEST_REQUIRE(added.paired && added.filthIndex >= 0,
                     "the newcomer must be paired.");
        TEST_REQUIRE(added.label == "scum_spot_" + std::to_string(baseMax + 1),
                     "label must be scum_spot_<moundId>, got '%s'.",
                     added.label.c_str());
    }
    std::vector<MapEditor::SpgFile::ScumSpot> withNew = spg.scumSpots();
    TEST_REQUIRE(withNew.size() == 4, "the nest list must grow to 4.");
    const MapEditor::SpgFile::ScumSpot& mine = withNew.back();
    TEST_REQUIRE(mine.attributeId == 44 && near(mine.x, 1234.5) &&
                     near(mine.y, 678.25) && mine.filth.creatureNum == 7 &&
                     mine.filth.filthType == 2 && mine.filth.sleep,
                 "the new nest must round-trip in memory.");

    // Pose + HP + filth edits.
    TEST_REQUIRE(spg.setScumSpotPose(3, 2000.0, 2100.0, -45.0),
                 "setScumSpotPose refused.");
    TEST_REQUIRE(spg.setScumSpotHp(3, 50), "setScumSpotHp refused.");
    MapEditor::SpgFile::FilthParams tweaked = mine.filth;
    tweaked.filthType = 11; // VOLCANO
    tweaked.creatureNum = 3;
    tweaked.attackPlayer = 0;
    TEST_REQUIRE(spg.setScumSpotFilth(3, tweaked),
                 "setScumSpotFilth refused.");

    TEST_REQUIRE(spg.save(edited.u8string()), "the edited save failed.");
    const std::string text = readText(edited);
    const std::string flat = collapseWs(text);
    // Game grammar: no trailing separator before a closing brace anywhere in
    // the generated regions (mounds + spawners). Note a bare ';' before '}'
    // IS valid for struct fields and the array count field — only the ','
    // tail is fatal.
    TEST_REQUIRE(flat.find(", };") == std::string::npos,
                 "no trailing comma before a close brace after add.");
    TEST_REQUIRE(flat.find("\"struct SaveUnitFilthData\"") !=
                     std::string::npos,
                 "the spawner entry must be serialized.");

    MapEditor::SpgFile reparsed;
    TEST_REQUIRE(reparsed.load(edited.u8string()), "the re-load failed.");
    std::vector<MapEditor::SpgFile::ScumSpot> after = reparsed.scumSpots();
    TEST_REQUIRE(after.size() == 4, "the re-loaded list must hold 4.");
    const MapEditor::SpgFile::ScumSpot& editedSpot = after.back();
    TEST_REQUIRE(editedSpot.attributeId == 44 && editedSpot.paired &&
                     near(editedSpot.x, 2000.0) &&
                     near(editedSpot.y, 2100.0) &&
                     std::abs(editedSpot.angleDeg - (-45.0)) < 1e-3,
                 "the pose edit must survive the round-trip.");
    TEST_REQUIRE(editedSpot.hp == 50, "the HP edit must survive (got %d).",
                 editedSpot.hp);
    TEST_REQUIRE(editedSpot.filth.filthType == 11 &&
                     editedSpot.filth.creatureNum == 3 &&
                     editedSpot.filth.attackPlayer == 0,
                 "the filth edits must survive the round-trip.");
    // The mound moved together with its spawner.
    TEST_REQUIRE(near(editedSpot.x, 2000.0), "the spawner must follow the "
                                             "mound.");

    // Removal drops both entries.
    TEST_REQUIRE(reparsed.removeScumSpot(3), "removeScumSpot refused.");
    TEST_REQUIRE(reparsed.scumSpots().size() == 3,
                 "the removal must shrink the list to 3.");
    const fs::path removedPath =
        ctx.tempRoot / "spg" / "scum_removed.spg";
    TEST_REQUIRE(reparsed.save(removedPath.u8string()),
                 "the removal save failed.");
    MapEditor::SpgFile reparsed2;
    TEST_REQUIRE(reparsed2.load(removedPath.u8string()),
                 "the removal re-load failed.");
    TEST_REQUIRE(reparsed2.scumSpots().size() == 3,
                 "the removed nest must stay removed.");
    std::printf("[PASS] scum nest add/edit/remove round-trip with the "
                "isoland pairing.\n");

    // ------------------------------------------------------------------
    // 3: trigger chain registration on a pristine battle map.
    // ------------------------------------------------------------------
    MapEditor::SpgFile battle;
    TEST_REQUIRE(battle.load(compass.u8string()), "COMPASS load failed.");
    TEST_REQUIRE(battle.syncScumTriggerChain("TESTWORLD.scr", true),
                 "syncScumTriggerChain(enable) refused.");
    TEST_REQUIRE(battle.syncScumTriggerChain("TESTWORLD.scr", true),
                 "second enable must be a no-op, not an error.");
    const fs::path chainPath = ctx.tempRoot / "spg" / "chain.spg";
    TEST_REQUIRE(battle.save(chainPath.u8string()), "chain save failed.");
    MapEditor::SpgFile chained;
    TEST_REQUIRE(chained.load(chainPath.u8string()), "chain re-load failed.");
    // Every player must list mp-N.scr AND TESTWORLD.scr exactly once.
    // (Reuse the spg text — cheaper than a full API.)
    const std::string chainText = collapseWs(readText(chainPath));
    const int playerCount = chained.playerCount();
    TEST_REQUIRE(playerCount >= 2, "COMPASS must have players.");
    // Every player must list mp-N.scr AND TESTWORLD.scr exactly once.
    int occurrences = 0;
    std::size_t pos = 0;
    while ((pos = chainText.find("name = \"TESTWORLD.scr\";", pos)) !=
           std::string::npos) {
        ++occurrences;
        ++pos;
    }
    TEST_REQUIRE(occurrences == playerCount,
                 "%s must be registered once per player (%d players, %d "
                 "registrations).",
                 "TESTWORLD.scr", playerCount, occurrences);
    TEST_REQUIRE(chainText.find("name = \"TESTWORLD.scr\"; }, };") ==
                     std::string::npos &&
                     chainText.find("name = \"TESTWORLD.scr\";}, };") ==
                         std::string::npos,
                 "no trailing comma in TriggerChainNames.");

    // Disable removes every registration.
    TEST_REQUIRE(chained.syncScumTriggerChain("TESTWORLD.scr", false),
                 "disable refused.");
    const fs::path unchainPath = ctx.tempRoot / "spg" / "unchain.spg";
    TEST_REQUIRE(chained.save(unchainPath.u8string()),
                 "unchain save failed.");
    const std::string unchainText = collapseWs(readText(unchainPath));
    TEST_REQUIRE(unchainText.find("TESTWORLD.scr") == std::string::npos,
                 "disable must purge the chain name from every player.");
    std::printf("[PASS] trigger chain registration add/no-dup/remove.\n");

    // ------------------------------------------------------------------
    // 3b: removePlayer — arbitrary index, id renumber, round-trip.
    // ------------------------------------------------------------------
    {
        MapEditor::SpgFile players;
        TEST_REQUIRE(players.load(compass.u8string()), "COMPASS load failed.");
        const int before = players.playerCount();
        TEST_REQUIRE(before >= 2, "COMPASS must have at least 2 players.");
        // Remove the FIRST player: the remaining ids must renumber 0..n-1.
        TEST_REQUIRE(players.removePlayer(0), "removePlayer(0) refused.");
        TEST_REQUIRE(players.playerCount() == before - 1,
                     "the player count must drop by one.");
        for (int i = 0; i < players.playerCount(); ++i) {
            const MapEditor::SpgFile::PlayerData data = players.playerData(i);
            TEST_REQUIRE(data.playerID == i,
                         "player %d must be renumbered to id %d, got %d.",
                         i, i, data.playerID);
        }
        // The spawn anchor of the former second player must survive.
        double ax = 0;
        double ay = 0;
        TEST_REQUIRE(players.playerAnchor(0, ax, ay),
                     "the first anchor must survive the removal.");
        TEST_REQUIRE(!players.removePlayer(players.playerCount()),
                     "an out-of-range removal must fail.");
        const fs::path playerPath = ctx.tempRoot / "spg" / "players_removed.spg";
        TEST_REQUIRE(players.save(playerPath.u8string()), "save failed.");
        MapEditor::SpgFile reloaded;
        TEST_REQUIRE(reloaded.load(playerPath.u8string()),
                     "the re-load failed.");
        TEST_REQUIRE(reloaded.playerCount() == before - 1,
                     "the removal must survive the round-trip.");
        for (int i = 0; i < reloaded.playerCount(); ++i) {
            TEST_REQUIRE(reloaded.playerData(i).playerID == i,
                         "ids must stay renumbered after the round-trip.");
        }
           // The grow path (what MCP add_player uses): one more player cloned
            // from the last, ids renumbered, round-trip stable.
            const std::string lastName =
                players.playerData(players.playerCount() - 1).name;
            TEST_REQUIRE(players.setPlayerCount(players.playerCount() + 1),
                         "the grow refused.");
            TEST_REQUIRE(players.playerCount() == before,
                         "the count must return to the original.");
            TEST_REQUIRE(players.playerData(before - 1).name == lastName,
                         "the newcomer must be a clone of the last player.");
    }
    std::printf("[PASS] removePlayer index/renumber/round-trip.\n");

    // ------------------------------------------------------------------
    // 4: the generated .scr trigger script (fresh load of `edited` - 4
    //    nests: the editor nest is mound-labeled, the three isoland ones
    //    are not, so exactly one pair gets wiring).
    // ------------------------------------------------------------------
    MapEditor::SpgFile scriptSrc;
    TEST_REQUIRE(scriptSrc.load(edited.u8string()),
                 "script source re-load failed.");
    const std::string script =
        scriptSrc.buildScumTriggerScript("ISOLAND.scr");
    const std::string flatScript = collapseWs(script);
    TEST_REQUIRE(flatScript.find("TriggerChain = {") == 0,
                 "the chain must open a TriggerChain.");
    TEST_REQUIRE(flatScript.find("name = \"START\";") != std::string::npos,
                 "the chain needs the mandatory START trigger.");
    // Regression: START must be stored CHECKING — TriggerChain::load seeds
    // activeTriggers_ from the stored state; a SLEEPING START leaves the
    // whole chain unquanted (the live bug on EXTDEMO).
    {
        const std::size_t startAt = flatScript.find("name = \"START\";");
        // Bound by the first trigger DEFINITION after START (`{ name = "…"`;
        // link entries spell `triggerName =` and must not match).
        const std::size_t onAt = flatScript.find("{ name = \"", startAt + 10);
        const std::string startBlock =
            flatScript.substr(startAt, onAt - startAt);
        TEST_REQUIRE(startBlock.find("state_ = CHECKING;") !=
                         std::string::npos,
                     "START must be stored CHECKING.");
    }
    int labeledPairs = 0;
    std::string pairLabel;
    for (const MapEditor::SpgFile::ScumSpot& s : scriptSrc.scumSpots()) {
        if (s.paired && s.moundLabeled) {
            ++labeledPairs;
            pairLabel = s.label;
        }
    }
    TEST_REQUIRE(labeledPairs == 1,
                 "exactly the editor nest is mound-labeled, got %d.",
                 labeledPairs);
    TEST_REQUIRE(flatScript.find("triggers = { " +
                                 std::to_string(1 + labeledPairs * 2) + ";") !=
                     std::string::npos,
                 "the trigger count must be 1 + 2 per LABELED pair.");
    // The distance-based wiring is impossible for co-located spawner/mound
    // (findUnit's distanceMin=0 filter excludes a unit at distance 0): the
    // conditions watch the MOUND label, the actions target "<label>_gen".
    TEST_REQUIRE(flatScript.find("ConditionObjectByLabelExists") !=
                     std::string::npos,
                 "the on-trigger must watch the mound by label.");
    TEST_REQUIRE(flatScript.find("ConditionKillObjectByLabel") !=
                     std::string::npos,
                 "the off-trigger must catch the mound destroy event.");
    TEST_REQUIRE(flatScript.find("ConditionObjectNearObjectByLabel") ==
                     std::string::npos,
                 "no distance-based condition — co-located units break it.");
    TEST_REQUIRE(flatScript.find("name = \"" + pairLabel + "_on\";") !=
                     std::string::npos &&
                     flatScript.find("name = \"" + pairLabel + "_off\";") !=
                         std::string::npos,
                 "the pair triggers must be named after the nest label.");
    TEST_REQUIRE(flatScript.find("label = \"" + pairLabel + "_gen\";") !=
                     std::string::npos,
                 "the actions must target the spawner's _gen label.");
    TEST_REQUIRE(flatScript.find("ActionActivateObjectByLabel") !=
                     std::string::npos &&
                     flatScript.find("ActionDeactivateObjectByLabel") !=
                         std::string::npos,
                 "activate/deactivate actions must be present.");
    // No trailing separator between triggers / before the tail braces.
    TEST_REQUIRE(flatScript.find("}, };") == std::string::npos,
                 "no trailing comma between triggers.");
    std::printf("[PASS] generated trigger chain script.\n");

    // ------------------------------------------------------------------
    // 5: the pre-fix label scheme migrates (spawner label -> mound + _gen).
    //    Craft the legacy layout by text surgery on a freshly saved nest.
    // ------------------------------------------------------------------
    {
        MapEditor::SpgFile legacy;
        TEST_REQUIRE(legacy.load(compass.u8string()),
                     "COMPASS re-load failed.");
        MapEditor::SpgFile::ScumSpot added;
        added.attributeId = 43;
        added.x = 500.;
        added.y = 600.;
        added.z = 40.0 + 4.4335;
        added.angleDeg = 0.0;
        added.hp = 100;
        TEST_REQUIRE(legacy.addScumSpot(added), "addScumSpot refused.");
        const fs::path migPath = ctx.tempRoot / "spg" / "scum_migrated.spg";
        TEST_REQUIRE(legacy.save(migPath.u8string()), "migrate save failed.");

        std::string raw = readText(migPath);
        const std::string nestLabel = added.label;         // scum_spot_N
        const std::string genLabel = nestLabel + "_gen";
        // Spawner label loses the _gen suffix (legacy form)…
        TEST_REQUIRE(raw.find("label = \"" + genLabel + "\";") !=
                         std::string::npos,
                     "the saved spawner must carry the _gen label.");
        raw = raw.replace(raw.find("label = \"" + genLabel + "\";"),
                          std::string("label = \"" + genLabel + "\";").size(),
                          "label = \"" + nestLabel + "\";");
        // …and the mound label is blanked (nobodysBuildings follows filth
        // in the file, so the remaining occurrence is the mound's).
        const std::size_t moundLabelAt = raw.rfind("label = \"" + nestLabel + "\";");
        TEST_REQUIRE(moundLabelAt != std::string::npos,
                     "the saved mound label must be locatable.");
        raw = raw.replace(moundLabelAt,
                          std::string("label = \"" + nestLabel + "\";").size(),
                          "label = \"\";");
        {
            std::ofstream out(migPath, std::ios::binary | std::ios::trunc);
            out << raw;
        }

        MapEditor::SpgFile oldStyle;
        TEST_REQUIRE(oldStyle.load(migPath.u8string()),
                     "legacy re-load failed.");
        std::vector<MapEditor::SpgFile::ScumSpot> oldList = oldStyle.scumSpots();
        TEST_REQUIRE(oldList.size() == 1 && oldList[0].paired &&
                         !oldList[0].moundLabeled,
                     "the crafted nest must be paired but not mound-labeled.");
        TEST_REQUIRE(oldStyle.normalizeScumNestLabels(),
                     "normalize must migrate the legacy labels.");
        TEST_REQUIRE(oldStyle.save(migPath.u8string()),
                     "normalize save failed.");
        MapEditor::SpgFile migrated;
        TEST_REQUIRE(migrated.load(migPath.u8string()),
                     "migrate re-load failed.");
        std::vector<MapEditor::SpgFile::ScumSpot> finalList =
            migrated.scumSpots();
        TEST_REQUIRE(finalList.size() == 1 && finalList[0].paired &&
                         finalList[0].moundLabeled,
                     "the migrated nest must stay paired and mound-labeled.");
        const std::string migText = collapseWs(readText(migPath));
        TEST_REQUIRE(migText.find("label = \"" + genLabel + "\";") !=
                         std::string::npos,
                     "the spawner must carry the _gen label after migration.");
        TEST_REQUIRE(migText.find("_gen\"; } };") == std::string::npos &&
                         migText.find("_gen\";};") == std::string::npos,
                     "grammar after migration.");
    }
    std::printf("[PASS] pre-fix label migration.\n");

    return true;
}

MAP_EDITOR_REGISTER_TEST("Scum nests", 102, runScumSpotsTests);

} // namespace

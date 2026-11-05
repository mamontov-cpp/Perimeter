// ============================================================================
// Static nature objects — SpgFile::natureObjects()/addNatureObject()/
// setNatureObjectPose()/setNatureObjectShape()/removeNatureObject() over
// SavePrm.environment ("struct SaveUnitNatureData").
//
// Checked against the shipped maps:
//   * Missions/02PYRAMIDA.spg — 48 nature objects (trees with
//     NATURE_FLAG_DESTROY | NATURE_FLAG_REAL, effects with LIGHT);
//   * Battle/COMPASS.spg — a pristine battle map for the edit cycle.
//
// Covers: parsing (model/radius/z/angle/flags), add (typed struct, unique
// unitID), pose/shape edits, remove, and the game grammar of the saved
// text (bare array tails, natureFlag identifiers).
//
// Runs after the scum nests tests (order 103). Writes only into the
// temp root.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

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

bool runNatureObjectsTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path pyramida = ctx.gameDir / "RESOURCE" / "Missions" /
                              "02PYRAMIDA.spg";
    TEST_REQUIRE(fs::exists(pyramida), "02PYRAMIDA.spg missing in game dir.");
    const fs::path compass = ctx.gameDir / "RESOURCE" / "Battle" /
                             "COMPASS.spg";
    TEST_REQUIRE(fs::exists(compass), "COMPASS.spg missing in game dir.");

    // ------------------------------------------------------------------
    // 1: parse the shipped nature objects.
    // ------------------------------------------------------------------
    MapEditor::SpgFile spg;
    TEST_REQUIRE(spg.load(pyramida.u8string()), "load failed.");
    std::vector<MapEditor::SpgFile::NatureObject> objects =
        spg.natureObjects();
    TEST_REQUIRE(objects.size() == 48,
                 "02PYRAMIDA must carry 48 nature objects, got %zu.",
                 objects.size());
    const MapEditor::SpgFile::NatureObject& tree = objects.front();
    TEST_REQUIRE(tree.modelName ==
                     "Resource\\Models\\Environment\\Tree_Sosna1.M3D",
                 "unexpected first model '%s'.", tree.modelName.c_str());    TEST_REQUIRE(near(tree.radius, 41.58) && near(tree.z, 23.25),
                 "tree radius/z must parse (got %f / %f).", tree.radius,
                 tree.z);
    TEST_REQUIRE(tree.destroyable && tree.collisions && !tree.lighting,
                 "a pine carries DESTROY|REAL.");
    // The five LIGHT entries in this map are the tree_topol1 canopies and
    // the point_y marker.
    int lightCount = 0;
    for (const MapEditor::SpgFile::NatureObject& n : objects) {
        if (n.modelName.find("tree_topol1") != std::string::npos ||
            n.modelName.find("point_y") != std::string::npos) {
            TEST_REQUIRE(n.lighting && !n.destroyable && !n.collisions,
                         "%s must carry LIGHT only (d=%d c=%d l=%d).",
                         n.modelName.c_str(), n.destroyable ? 1 : 0,
                         n.collisions ? 1 : 0, n.lighting ? 1 : 0);
            ++lightCount;
        }
    }
    TEST_REQUIRE(lightCount == 5, "expected 5 LIGHT objects, got %d.",
                 lightCount);

    // ------------------------------------------------------------------
    // 2: add -> edit -> remove on a battle map, save->re-parse round-trip.
    // ------------------------------------------------------------------
    MapEditor::SpgFile battle;
    TEST_REQUIRE(battle.load(compass.u8string()), "COMPASS load failed.");
    const int baseMax = battle.maxUnitId();
    {
        MapEditor::SpgFile::NatureObject added;
        added.modelName = "RESOURCE\\Models\\Environment\\010_tree.M3D";
        added.x = 1200.5;
        added.y = 1300.25;
        added.z = 42.0;
        added.angleDeg = 33.0;
        added.radius = 25.0;
        added.destroyable = true;
        added.collisions = false;
        added.lighting = true;
        TEST_REQUIRE(battle.addNatureObject(added), "addNatureObject refused.");
        TEST_REQUIRE(added.unitId == baseMax + 1,
                     "unitId must be maxUnitId()+1, got %d.", added.unitId);
    }
    std::vector<MapEditor::SpgFile::NatureObject> withNew =
        battle.natureObjects();
    const MapEditor::SpgFile::NatureObject& mine = withNew.back();
    const std::size_t myIndex = mine.index;
    TEST_REQUIRE(mine.modelName.find("010_tree.M3D") != std::string::npos &&
                     near(mine.x, 1200.5) && near(mine.z, 42.0) &&
                     mine.lighting && !mine.collisions,
                 "the new object must round-trip in memory.");

    TEST_REQUIRE(battle.setNatureObjectPose(myIndex, 2000.0, 2100.0, 55.5,
                                            -45.0),
                 "setNatureObjectPose refused.");
    TEST_REQUIRE(battle.setNatureObjectShape(myIndex, 60.0, false, true,
                                             false),
                 "setNatureObjectShape refused.");

    const fs::path edited = ctx.tempRoot / "spg" / "nature_edited.spg";
    TEST_REQUIRE(battle.save(edited.u8string()), "the edited save failed.");
    const std::string flat = collapseWs(readText(edited));
    TEST_REQUIRE(flat.find("\"struct SaveUnitNatureData\"") !=
                     std::string::npos,
                 "the entry must serialize with its type tag.");
    TEST_REQUIRE(flat.find(", };") == std::string::npos,
                 "no trailing comma before a close brace after add.");
    TEST_REQUIRE(flat.find("natureFlag = NATURE_FLAG_REAL | "
                           "NATURE_FLAG_NONE;") != std::string::npos,
                 "the flags must serialize as identifiers.");

    MapEditor::SpgFile reparsed;
    TEST_REQUIRE(reparsed.load(edited.u8string()), "the re-load failed.");
    std::vector<MapEditor::SpgFile::NatureObject> after =
        reparsed.natureObjects();
    TEST_REQUIRE(!after.empty(), "the re-loaded list must not be empty.");
    const MapEditor::SpgFile::NatureObject& editedObj = after.back();
    TEST_REQUIRE(near(editedObj.x, 2000.0) && near(editedObj.y, 2100.0) &&
                     near(editedObj.z, 55.5) &&
                     std::abs(editedObj.angleDeg - (-45.0)) < 1e-3,
                 "the pose edit must survive the round-trip.");
    TEST_REQUIRE(near(editedObj.radius, 60.0) && !editedObj.destroyable &&
                     editedObj.collisions && !editedObj.lighting,
                 "the shape edit must survive the round-trip.");

    TEST_REQUIRE(reparsed.removeNatureObject(editedObj.index),
                 "removeNatureObject refused.");
    TEST_REQUIRE(reparsed.natureObjects().empty() ==
                     (after.size() == 1),
                 "the removal must shrink the list.");
    const fs::path removedPath = ctx.tempRoot / "spg" / "nature_removed.spg";
    TEST_REQUIRE(reparsed.save(removedPath.u8string()),
                 "the removal save failed.");
    MapEditor::SpgFile reparsed2;
    TEST_REQUIRE(reparsed2.load(removedPath.u8string()),
                 "the removal re-load failed.");
    if (after.size() == 1) {
        TEST_REQUIRE(reparsed2.natureObjects().empty(),
                     "the removed object must stay removed.");
    }
    std::printf("[PASS] nature object add/edit/remove round-trip.\n");

    return true;
}

MAP_EDITOR_REGISTER_TEST("Nature objects", 103, runNatureObjectsTests);

} // namespace

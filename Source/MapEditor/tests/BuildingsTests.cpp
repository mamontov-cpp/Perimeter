// ============================================================================
// Pre-placed battle-map buildings — SpgFile::buildings()/addBuilding()/
// setBuildingPose()/setBuildingHp()/removeBuilding() over SavePrm.
// nobodysBuildings.
//
// Checked on the shipped Missions/02PYRAMIDA.spg (11 neutral buildings:
// Cores as SaveUnitProtectorData, the rest SaveUnitBuildingData):
//   * parsing: attribute/position/rotation/HP read back correctly;
//   * untouched bytes stay byte-exact (the XPrmDocument contract);
//   * add → the array grows with a proper typed struct, the previous last
//     element gains its separator, the count updates, the new unitID is
//     unique (maxUnitId() + 1);
//   * pose/HP edits rewrite exactly those fields;
//   * remove shrinks the array;
//   * the whole cycle survives a save → re-parse round-trip.
//
// Runs after the SPG tests (order 100). Reads the game dir only.
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

bool runBuildingsTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path source = ctx.gameDir / "RESOURCE" / "Missions" /
                            "02PYRAMIDA.spg";
    TEST_REQUIRE(fs::exists(source),
                 "02PYRAMIDA.spg missing in the game dir.");

    // ------------------------------------------------------------------
    // 1: parse the shipped buildings.
    // ------------------------------------------------------------------
    MapEditor::SpgFile spg;
    TEST_REQUIRE(spg.load(source.u8string()), "load failed.");
    std::vector<MapEditor::SpgFile::PlacedBuilding> before = spg.buildings();
    TEST_REQUIRE(before.size() == 11,
                 "02PYRAMIDA must carry 11 neutral buildings, got %zu.",
                 before.size());
    TEST_REQUIRE(before[0].attributeId == 0 /* CORE */,
                 "building 0 must be the Core.");
    TEST_REQUIRE(near(before[0].x, 2709.0) && near(before[0].y, 741.0),
                 "the Core position differs from the file.");
    TEST_REQUIRE(before[1].attributeId == 2 /* COLLECTOR */,
                 "building 1 must be the Collector.");
    TEST_REQUIRE(near(before[1].x, 2875.87) && near(before[1].y, 691.406) &&
                     near(before[1].z, 40.6221),
                 "the Collector position differs from the file.");
    for (const MapEditor::SpgFile::PlacedBuilding& b : before) {
        TEST_REQUIRE(b.hp == 100 && near(b.angleDeg, 0.0),
                     "shipped buildings are full-HP, unrotated.");
    }
    std::printf("[PASS] the shipped buildings parse (attribute, position, "
                "rotation, HP).\n");

    // ------------------------------------------------------------------
    // 2: untouched bytes stay byte-exact.
    // ------------------------------------------------------------------
    {
        std::ifstream in(source, std::ios::binary);
        const std::string original((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        const fs::path out = ctx.tempRoot / "spg" / "buildings_untouched.spg";
        fs::create_directories(out.parent_path());
        TEST_REQUIRE(spg.save(out.u8string()), "save failed.");
        std::ifstream back(out, std::ios::binary);
        const std::string rewritten((std::istreambuf_iterator<char>(back)),
                                    std::istreambuf_iterator<char>());
        TEST_REQUIRE(original == rewritten,
                     "an unedited load must stay byte-exact.");
    }
    std::printf("[PASS] an unedited load->save stays byte-exact.\n");

    // ------------------------------------------------------------------
    // 3: add → pose/HP edit → remove, then a save→re-parse round-trip.
    // ------------------------------------------------------------------
    const fs::path edited = ctx.tempRoot / "spg" / "buildings_edited.spg";
    {
        MapEditor::SpgFile::PlacedBuilding added;
        added.unitId = spg.maxUnitId() + 1;
        added.attributeId = 8; // ROCKET_STATION1
        added.x = 1234.5;
        added.y = 678.25;
        added.z = 39.0 + 16.4945; // zero layer + catalog offset
        added.angleDeg = 45.0;
        added.hp = 60;
        TEST_REQUIRE(spg.addBuilding(added), "addBuilding refused.");
    }
    std::vector<MapEditor::SpgFile::PlacedBuilding> added = spg.buildings();
    TEST_REQUIRE(added.size() == 12, "the array must grow to 12.");
    const MapEditor::SpgFile::PlacedBuilding& mine = added.back();
    TEST_REQUIRE(mine.attributeId == 8 && mine.unitId == spg.maxUnitId(),
                 "the new building must keep its attribute/unitID.");
    TEST_REQUIRE(near(mine.x, 1234.5) && near(mine.y, 678.25),
                 "the new building position must round-trip.");
    TEST_REQUIRE(std::abs(mine.angleDeg - 45.0) < 1e-3,
                 "the 45-degree rotation must round-trip, got %f.",
                 mine.angleDeg);
    TEST_REQUIRE(mine.hp == 60, "the 60%% HP must round-trip, got %d.",
                 mine.hp);

    // Pose + HP edits on the new building.
    TEST_REQUIRE(spg.setBuildingPose(11, 2000.0, 2000.0, -90.0),
                 "setBuildingPose refused.");
    TEST_REQUIRE(spg.setBuildingHp(11, 25), "setBuildingHp refused.");
    std::vector<MapEditor::SpgFile::PlacedBuilding> editedList =
        spg.buildings();
    TEST_REQUIRE(near(editedList[11].x, 2000.0) &&
                     near(editedList[11].y, 2000.0),
                 "the pose edit must apply.");
    TEST_REQUIRE(std::abs(editedList[11].angleDeg - (-90.0)) < 1e-3,
                 "the -90-degree rotation must apply.");
    TEST_REQUIRE(editedList[11].hp == 25, "the 25%% HP must apply.");
    // z must be preserved by the pose edit.
    TEST_REQUIRE(near(editedList[11].z, 39.0 + 16.4945),
                 "the pose edit must keep the z.");

    TEST_REQUIRE(spg.save(edited.u8string()), "the edited save failed.");

    // Game-grammar check on the raw text: the game's XPrm reader aborts on
    // a separator before '}' — array elements are ', '-separated with the
    // LAST element bare (a trailing ',' or ';' reads as another element).
    auto readText = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    };
    auto blockOf = [](const std::string& text, const std::string& from,
                      const std::string& to) {
        const std::size_t a = text.find(from);
        const std::size_t b = a == std::string::npos ? a : text.find(to, a);
        return a == std::string::npos || b == std::string::npos
                   ? std::string()
                   : text.substr(a, b - a);
    };
    auto collapse = [](std::string s) {
        std::string out;
        bool space = false;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                space = out.empty() ? false : true;
            } else {
                if (space) out += ' ';
                out += c;
                space = false;
            }
        }
        return out;
    };
    const std::string editedText = readText(edited);
    const std::string editedBlock = collapse(
        blockOf(editedText, "nobodysBuildings", "worldObjects"));
    TEST_REQUIRE(!editedBlock.empty(), "the saved block must be locatable.");
    TEST_REQUIRE(editedBlock.find("objects = { 12;") != std::string::npos,
                 "the objects count must update to 12.");
    TEST_REQUIRE(editedBlock.find("}, };") == std::string::npos &&
                     editedBlock.find("},};") == std::string::npos,
                 "no trailing comma before the objects close brace — the "
                 "game reader treats it as another element.");
    TEST_REQUIRE(editedBlock.find("{ 5; 0, 0, 0, 0, 0; }") ==
                     std::string::npos,
                 "elementsDead's last element must be bare (no ';') — the "
                 "game reader expects '}' right after it.");
    TEST_REQUIRE(editedBlock.find("{ 5; 0, 0, 0, 0, 0 }") !=
                     std::string::npos,
                 "elementsDead must keep its ','-separated bare-tail form.");

    // Re-parse: everything survives, and the untouched buildings are intact.
    MapEditor::SpgFile reparsed;
    TEST_REQUIRE(reparsed.load(edited.u8string()), "the re-load failed.");
    std::vector<MapEditor::SpgFile::PlacedBuilding> after =
        reparsed.buildings();
    TEST_REQUIRE(after.size() == 12, "the re-loaded array must hold 12.");
    TEST_REQUIRE(after[0].attributeId == 0 &&
                     near(after[0].x, 2709.0) && near(after[0].y, 741.0),
                 "the untouched Core must survive the round-trip.");
    TEST_REQUIRE(after[11].attributeId == 8 && after[11].hp == 25 &&
                     std::abs(after[11].angleDeg - (-90.0)) < 1e-3,
                 "the edited building must survive the round-trip.");

    // Remove it again.
    TEST_REQUIRE(reparsed.removeBuilding(11), "removeBuilding refused.");
    std::vector<MapEditor::SpgFile::PlacedBuilding> removed =
        reparsed.buildings();
    TEST_REQUIRE(removed.size() == 11,
                 "the removal must shrink the array to 11.");
    const fs::path removedPath = ctx.tempRoot / "spg" / "buildings_removed.spg";
    TEST_REQUIRE(reparsed.save(removedPath.u8string()),
                 "the removal save failed.");
    MapEditor::SpgFile reparsed2;
    TEST_REQUIRE(reparsed2.load(removedPath.u8string()),
                 "the removal re-load failed.");
    TEST_REQUIRE(reparsed2.buildings().size() == 11,
                 "the removed building must stay removed.");
    // The new tail element must have lost its separator in the raw text.
    const std::string removedBlock = collapse(
        blockOf(readText(removedPath), "nobodysBuildings", "worldObjects"));
    TEST_REQUIRE(removedBlock.find("objects = { 11;") != std::string::npos,
                 "the objects count must update to 11.");
    TEST_REQUIRE(removedBlock.find("}, };") == std::string::npos &&
                     removedBlock.find("},};") == std::string::npos,
                 "no trailing comma after removing the tail building.");
    std::printf("[PASS] add/pose/HP/remove round-trip through a saved "
                "file.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Battle-map buildings", 100, runBuildingsTests);

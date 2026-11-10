// ============================================================================
// Template out-of-bounds filter — SpgFile::removeObjectsOutsideBounds().
//
// When a new battle map is created from a template whose world is larger than
// the current world, the template's buildings, scum nests (mound + FILTH_SPOT
// pair) and nature objects that fall outside the new map's bounds must be
// dropped automatically.
//
// Checked on the shipped Battle/COMPASS.spg. Covers:
//   * inside objects survive (including x = mapW - 0.5 boundary cells);
//   * outside objects (x/y >= mapW/H, negative) are removed;
//   * the removed count is exact with interleaved in/out placement order;
//   * removing a scum nest outside also removes its paired FILTH_SPOT spawner;
//   * the result survives a save -> re-parse round-trip.
//
// Expectations are computed against the document's own base objects, so the
// test holds whatever COMPASS.spg ships with.
//
// Writes only into the temp root.
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

bool runSpgObjectBoundsTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path compass = ctx.gameDir / "RESOURCE" / "Battle" /
                             "COMPASS.spg";
    TEST_REQUIRE(fs::exists(compass), "COMPASS.spg missing in game dir.");

    // Map bounds as passed by BattleMapController::onNewBattleMapRequested.
    const int mapW = 2048;
    const int mapH = 2048;
    const auto outside = [mapW, mapH](double x, double y) {
        return x < 0.0 || y < 0.0 || x >= mapW || y >= mapH;
    };

    MapEditor::SpgFile spg;
    TEST_REQUIRE(spg.load(compass.u8string()), "COMPASS load failed.");
    const std::size_t baseBuildings = spg.buildings().size();
    const std::size_t baseSpots = spg.scumSpots().size();
    const std::size_t baseNature = spg.natureObjects().size();

    // Base objects that already sit outside the test bounds — the filter
    // removes them too, so expectations account for them.
    std::size_t baseOutB = 0;
    for (const MapEditor::SpgFile::PlacedBuilding& b : spg.buildings()) {
        baseOutB += outside(b.x, b.y) ? 1 : 0;
    }
    std::size_t baseOutS = 0;
    for (const MapEditor::SpgFile::ScumSpot& s : spg.scumSpots()) {
        baseOutS += outside(s.x, s.y) ? 1 : 0;
    }
    std::size_t baseOutN = 0;
    for (const MapEditor::SpgFile::NatureObject& n : spg.natureObjects()) {
        baseOutN += outside(n.x, n.y) ? 1 : 0;
    }

    // One in/out pair per category, interleaved so the descending-index purge
    // order is exercised.
    {
        MapEditor::SpgFile::PlacedBuilding b;
        b.unitId = spg.maxUnitId() + 1;
        b.attributeId = 8; // ROCKET_STATION1
        b.x = 100.0;
        b.y = 100.0;
        b.z = 39.0;
        b.hp = 80;
        TEST_REQUIRE(spg.addBuilding(b), "addBuilding (inside) refused.");
    }
    {
        MapEditor::SpgFile::PlacedBuilding b;
        b.unitId = spg.maxUnitId() + 1;
        b.attributeId = 8;
        b.x = static_cast<double>(mapW) + 100.0; // outside
        b.y = 100.0;
        b.z = 39.0;
        TEST_REQUIRE(spg.addBuilding(b), "addBuilding (outside) refused.");
    }
    {
        MapEditor::SpgFile::ScumSpot s;
        s.attributeId = 44; // SCUM_SPOT2
        s.x = static_cast<double>(mapW) - 0.5; // boundary cell: inside
        s.y = 200.0;
        s.z = 90.0;
        TEST_REQUIRE(spg.addScumSpot(s), "addScumSpot (inside) refused.");
    }
    {
        MapEditor::SpgFile::ScumSpot s;
        s.attributeId = 43;
        s.x = 300.0;
        s.y = -1.0; // outside
        s.z = 90.0;
        TEST_REQUIRE(spg.addScumSpot(s), "addScumSpot (outside) refused.");
    }
    {
        MapEditor::SpgFile::NatureObject n;
        n.modelName = "RESOURCE\\Models\\Environment\\010_tree.M3D";
        n.x = 500.0;
        n.y = 500.0;
        TEST_REQUIRE(spg.addNatureObject(n), "addNatureObject (inside) refused.");
    }
    {
        MapEditor::SpgFile::NatureObject n;
        n.modelName = "RESOURCE\\Models\\Environment\\010_tree.M3D";
        n.x = 500.0;
        n.y = static_cast<double>(mapH); // y == mapH: outside
        TEST_REQUIRE(spg.addNatureObject(n),
                     "addNatureObject (outside) refused.");
    }

    TEST_REQUIRE(spg.buildings().size() == baseBuildings + 2,
                 "expected %zu + 2 buildings.", baseBuildings);
    TEST_REQUIRE(spg.scumSpots().size() == baseSpots + 2,
                 "expected %zu + 2 scum nests.", baseSpots);
    TEST_REQUIRE(spg.natureObjects().size() == baseNature + 2,
                 "expected %zu + 2 nature objects.", baseNature);

    // The real pass: exactly the three outside newcomers (plus any base
    // object already outside the bounds — the shipped maps place some
    // objects marginally off-map) go away.
    const std::size_t removed = spg.removeObjectsOutsideBounds(mapW, mapH);
    TEST_REQUIRE(removed == 3 + baseOutB + baseOutS + baseOutN,
                 "expected %zu removals, got %zu.",
                 3 + baseOutB + baseOutS + baseOutN, removed);

    const std::vector<MapEditor::SpgFile::PlacedBuilding> bl = spg.buildings();
    TEST_REQUIRE(bl.size() == baseBuildings - baseOutB + 1,
                 "one inside building must survive, got %zu.", bl.size());
    TEST_REQUIRE(near(bl.back().x, 100.0) && near(bl.back().y, 100.0),
                 "the last building must be the inside newcomer.");

    const std::vector<MapEditor::SpgFile::ScumSpot> ss = spg.scumSpots();
    TEST_REQUIRE(ss.size() == baseSpots - baseOutS + 1,
                 "one inside nest must survive, got %zu.", ss.size());
    TEST_REQUIRE(near(ss.back().x, static_cast<double>(mapW) - 0.5) &&
                     near(ss.back().y, 200.0),
                 "the last nest must be the boundary newcomer.");
    TEST_REQUIRE(ss.back().paired, "the surviving nest must stay paired.");

    const std::vector<MapEditor::SpgFile::NatureObject> no =
        spg.natureObjects();
    TEST_REQUIRE(no.size() == baseNature - baseOutN + 1,
                 "one inside nature object must survive, got %zu.", no.size());
    TEST_REQUIRE(near(no.back().x, 500.0) && near(no.back().y, 500.0),
                 "the last nature object must be the inside newcomer.");

    // Degenerate bounds remove nothing; a small-bounds pass removes the
    // newcomers and keeps every surviving object inside.
    MapEditor::SpgFile all;
    TEST_REQUIRE(all.load(compass.u8string()), "COMPASS reload failed.");
    const int smallMapW = 50;
    const int smallMapH = 50;
    std::string outsideLabel;
    {
        MapEditor::SpgFile::PlacedBuilding b;
        b.unitId = all.maxUnitId() + 1;
        b.attributeId = 8;
        b.x = 100.0;
        b.y = 100.0;
        TEST_REQUIRE(all.addBuilding(b), "addBuilding refused.");
        MapEditor::SpgFile::ScumSpot s;
        s.x = 100.0;
        s.y = 100.0;
        s.z = 90.0;
        TEST_REQUIRE(all.addScumSpot(s), "addScumSpot refused.");
        outsideLabel = s.label;
        MapEditor::SpgFile::NatureObject n;
        n.modelName = "RESOURCE\\Models\\Environment\\010_tree.M3D";
        n.x = 100.0;
        n.y = 100.0;
        TEST_REQUIRE(all.addNatureObject(n), "addNatureObject refused.");
    }
    TEST_REQUIRE(all.removeObjectsOutsideBounds(0, 0) == 0,
                 "degenerate bounds must remove nothing.");
    const std::size_t smallRemoved = all.removeObjectsOutsideBounds(50, 50);
    TEST_REQUIRE(smallRemoved >= 3,
                 "bounds 50x50 must remove at least the three newcomers, "
                 "got %zu.",
                 smallRemoved);
    for (const MapEditor::SpgFile::PlacedBuilding& b : all.buildings()) {
        TEST_REQUIRE(b.x >= 0.0 && b.y >= 0.0 && b.x < 50.0 && b.y < 50.0,
                     "a surviving building must sit inside the 50x50 bounds.");
    }
    for (const MapEditor::SpgFile::ScumSpot& s : all.scumSpots()) {
        TEST_REQUIRE(s.x >= 0.0 && s.y >= 0.0 && s.x < 50.0 && s.y < 50.0,
                     "a surviving nest must sit inside the 50x50 bounds.");
    }
    for (const MapEditor::SpgFile::NatureObject& n : all.natureObjects()) {
        TEST_REQUIRE(n.x >= 0.0 && n.y >= 0.0 && n.x < 50.0 && n.y < 50.0,
                     "a surviving nature object must sit inside the 50x50 "
                     "bounds.");
    }

    // The outside scum nest's FILTH_SPOT spawner must be gone from the
    // serialized file (the paired removal), and the map must re-parse with
    // the filtered lists.
    const fs::path out = ctx.tempRoot / "spg" / "bounds_filtered.spg";
    TEST_REQUIRE(all.save(out.u8string()), "save failed.");
    MapEditor::SpgFile back;
    TEST_REQUIRE(back.load(out.u8string()), "re-parse failed.");
    TEST_REQUIRE(back.buildings().size() == all.buildings().size() &&
                     back.scumSpots().size() == all.scumSpots().size() &&
                     back.natureObjects().size() == all.natureObjects().size(),
                 "the filtered lists must round-trip.");
    TEST_REQUIRE(!outsideLabel.empty() &&
                     readText(out).find(outsideLabel) == std::string::npos,
                 "the removed nest's label (and its spawner) must be absent "
                 "from the saved file.");

    std::printf("[PASS] removeObjectsOutsideBounds drops out-of-bounds "
                "template objects.\n");
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("SPG template out-of-bounds filter", 104,
                         runSpgObjectBoundsTests);

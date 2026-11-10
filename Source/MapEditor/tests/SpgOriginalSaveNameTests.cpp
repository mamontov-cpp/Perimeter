// ============================================================================
// originalSaveName rewrite on save (the skirmish "Again" restart fix).
//
// The mission descriptor's originalSaveName drives the in-game restart:
// MissionDescription::restart() calls setSaveName(originalSaveName) and the
// game reloads the mission (SavePrm + sibling .gmp) from THAT path. Maps
// created by the editor from a template inherited the TEMPLATE's value
// (every map built from COMPASS carried "resource\battle\compass.spg"), so
// pressing "Again" loaded the template's spg/gmp over the played world —
// the template's frame/units and its .gmp terrain stamped on top.
//
// SpgFile::save now rewrites the field to the saved map's own
// resource-relative path. Checked here on the raw saved text:
//   * saving a COMPASS-derived copy under a non-resource path falls back to
//     resource\battle\<file name> (lowercased);
//   * saving under .../RESOURCE/Battle/<name>.spg extracts the key from the
//     resource marker;
//   * the value is a valid XPrm string literal (quotes + doubled
//     backslashes), byte-readable by the game's XPrmIArchive.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "errh_wrapper.h"

#include "spg/SpgFile.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string readText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool runOriginalSaveNameTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();
    const fs::path source = ctx.gameDir / "RESOURCE" / "Battle" / "COMPASS.spg";
    TEST_REQUIRE(fs::exists(source), "COMPASS.spg missing in the game dir.");

    const fs::path tempDir = ctx.tempRoot / "spg_origsave";
    std::error_code ec;
    fs::create_directories(tempDir, ec);

    // Case 1: a save outside the resource tree -> battle-folder fallback.
    const fs::path outside = tempDir / "MyMap.spg";
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(source.u8string()), "template load failed");
        TEST_REQUIRE(spg.save(outside.u8string()), "save failed");
        // The file text carries the XPrm string literal (quotes + doubled
        // backslashes), exactly like the vanilla maps.
        const std::string text = readText(outside);
        TEST_REQUIRE(text.find("originalSaveName = "
                               "\"resource\\\\battle\\\\mymap.spg\";") !=
                         std::string::npos,
                     "originalSaveName not rewritten to the own path:\n%s",
                     text.substr(0, 800).c_str());
    }

    // Case 2: a save inside RESOURCE/Battle -> key taken from the marker.
    const fs::path battleDir = tempDir / "RESOURCE" / "Battle";
    fs::create_directories(battleDir, ec);
    const fs::path inside = battleDir / "SECONDMAP.spg";
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(source.u8string()), "template load failed");
        TEST_REQUIRE(spg.save(inside.u8string()), "save failed");
        const std::string text = readText(inside);
        TEST_REQUIRE(text.find("originalSaveName = "
                               "\"resource\\\\battle\\\\secondmap.spg\";") !=
                         std::string::npos,
                     "originalSaveName not rewritten from the resource "
                     "path:\n%s",
                     text.substr(0, 800).c_str());
    }

    std::printf("[PASS] originalSaveName rewritten to the map's own path.\n");
    return true;
}

MAP_EDITOR_REGISTER_TEST("SPG originalSaveName restart-path rewrite", 99,
                         runOriginalSaveNameTests);

} // namespace

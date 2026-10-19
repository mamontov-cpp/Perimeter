// ============================================================================
// T1.2 — WorldFiles: world.ini read/edit/write round-trip.
//
// Verifies that an untouched document is written back byte-for-byte, that the
// typed getters agree with the file, that an in-place edit touches exactly one
// key, and that new keys/sections can be appended and are readable by the same
// SimpleIni reader the game uses.
//
// Runs after WorldRoundTripTests, which provides the saved temporary world.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

// WorldFiles (MapEditorCore) — file-level world folder access.
#include "WorldFiles.h"
// shims.h (MapEditorSupport) — ReadIniString, the game's SimpleIni reader.
#include "shims.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool runWorldFilesIniTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    const std::filesystem::path& savedIni = ctx.savedIni;

    TEST_REQUIRE(!savedIni.empty() && std::filesystem::exists(savedIni),
                 "WorldRoundTripTests did not provide world.ini.");
    TEST_REQUIRE(ctx.mapWidth > 0 && ctx.mapHeight > 0,
                 "WorldRoundTripTests did not provide the map size.");

    MapEditor::WorldFiles worldFiles(ctx.tempWorldDir);

    // Baseline is the file as it is on disk right now: the round-trip test let
    // SimpleIni rewrite it, and that state must survive WorldFiles unchanged.
    const std::vector<std::uint8_t> iniBaseline =
        MapEditor::Tests::readFileBytes(savedIni);
    TEST_REQUIRE(!iniBaseline.empty(),
                 "world.ini is missing before the WorldFiles checks.");
    ctx.iniBaseline = iniBaseline;

    // ------------------------------------------------------------------
    // 7.1: an untouched document must be written back byte-for-byte.
    // ------------------------------------------------------------------
    {
        const auto document = worldFiles.readIni();
        TEST_REQUIRE(document.has_value(), "WorldFiles::readIni() failed.");
        TEST_REQUIRE(worldFiles.writeIni(*document),
                     "WorldFiles::writeIni() failed.");
        TEST_REQUIRE(MapEditor::Tests::readFileBytes(savedIni) == iniBaseline,
                     "world.ini changed across a read/write cycle.");
        std::printf("[PASS] untouched world.ini is byte-stable (%zu bytes).\n",
                    iniBaseline.size());
    }

    // ------------------------------------------------------------------
    // 7.2: typed getters agree with the file / the loaded world.
    // ------------------------------------------------------------------
    int hZeroPlastValue = 0;
    {
        const auto mapPower = worldFiles.readMapPower();
        TEST_REQUIRE(mapPower && mapPower->first >= 1 && mapPower->second >= 1,
                     "could not read [Global Parameters] Map Power X/Y.");

        const int mapWidth = 1 << mapPower->first;
        const int mapHeight = 1 << mapPower->second;
        TEST_REQUIRE(mapWidth == ctx.mapWidth && mapHeight == ctx.mapHeight,
                     "world.ini map power (%d,%d -> %dx%d) does not match "
                     "the loaded map (%dx%d).",
                     mapPower->first, mapPower->second, mapWidth, mapHeight,
                     ctx.mapWidth, ctx.mapHeight);

        const auto hZeroPlast = worldFiles.readHZeroPlast();
        TEST_REQUIRE(hZeroPlast && *hZeroPlast >= 0 && *hZeroPlast <= 255,
                     "[Global Parameters] hZeroPlast is missing or invalid.");
        hZeroPlastValue = *hZeroPlast;

        const std::string hzpKey =
            std::string(MapEditor::WorldIni::GlobalParameters) + '\x1f' +
            MapEditor::WorldIni::HZeroPlast;
        const auto expectedHzp = ctx.savedIniValues.find(hzpKey);
        TEST_REQUIRE(expectedHzp != ctx.savedIniValues.end() &&
                         std::to_string(hZeroPlastValue) == expectedHzp->second,
                     "hZeroPlast=%d disagrees with the semantic INI parse.",
                     hZeroPlastValue);

        const auto version = worldFiles.readStorageVersion();
        const std::string versionKey =
            std::string(MapEditor::WorldIni::Storage) + '\x1f' +
            MapEditor::WorldIni::Version;
        const auto expectedVersion = ctx.savedIniValues.find(versionKey);
        TEST_REQUIRE(version && expectedVersion != ctx.savedIniValues.end() &&
                         *version == expectedVersion->second,
                     "[Storage] Version is missing or wrong.");

        TEST_REQUIRE(!worldFiles.readIniInt(MapEditor::WorldIni::GlobalParameters,
                                            "No Such Key"),
                     "a missing key was reported as present.");

        std::printf("[PASS] typed getters agree with the file (map %dx%d, "
                    "hZeroPlast=%d, storage version '%s').\n",
                    mapWidth, mapHeight, hZeroPlastValue, version->c_str());
    }

    // ------------------------------------------------------------------
    // 7.3: an in-place edit touches exactly one key and is readable by the
    //      same SimpleIni reader the game uses.
    // ------------------------------------------------------------------
    {
        const int newValue = (hZeroPlastValue == 177) ? 178 : 177;
        TEST_REQUIRE(worldFiles.writeHZeroPlast(newValue),
                     "WorldFiles::writeHZeroPlast() failed.");

        const auto editedValues = MapEditor::Tests::parseIni(savedIni);
        std::size_t changedKeys = 0;
        std::string changedKey;
        for (const auto& entry : ctx.savedIniValues) {
            const auto it = editedValues.find(entry.first);
            if (it == editedValues.end() || it->second != entry.second) {
                ++changedKeys;
                changedKey = entry.first;
            }
        }
        const std::string hzpKey =
            std::string(MapEditor::WorldIni::GlobalParameters) + '\x1f' +
            MapEditor::WorldIni::HZeroPlast;
        const auto editedHzp = editedValues.find(hzpKey);
        TEST_REQUIRE(changedKeys == 1 && changedKey == hzpKey &&
                         editedHzp != editedValues.end() &&
                         editedHzp->second == std::to_string(newValue),
                     "editing hZeroPlast changed %zu key(s) (last: '%s').",
                     changedKeys, changedKey.c_str());

        char iniBuffer[64] = {};
        ReadIniString(MapEditor::WorldIni::GlobalParameters,
                      MapEditor::WorldIni::HZeroPlast, "", iniBuffer,
                      static_cast<std::uint32_t>(sizeof(iniBuffer)),
                      savedIni.u8string());
        TEST_REQUIRE(std::string(iniBuffer) == std::to_string(newValue),
                     "SimpleIni reads hZeroPlast='%s', expected '%d'.",
                     iniBuffer, newValue);
        std::printf("[PASS] in-place edit changes only hZeroPlast and stays "
                    "readable by SimpleIni.\n");

        TEST_REQUIRE(worldFiles.writeHZeroPlast(hZeroPlastValue),
                     "restoring hZeroPlast failed.");
        TEST_REQUIRE(MapEditor::Tests::readFileBytes(savedIni) == iniBaseline,
                     "restoring the old value did not reproduce world.ini.");
        std::printf("[PASS] restoring the old value reproduces world.ini "
                    "byte-for-byte.\n");
    }

    // ------------------------------------------------------------------
    // 7.4: adding a key to an existing section and a whole new section.
    // ------------------------------------------------------------------
    {
        TEST_REQUIRE(worldFiles.writeIniString(
                         MapEditor::WorldIni::RenderingParameters,
                         "Editor Probe", "42") &&
                         worldFiles.writeIniString(
                             "Editor Test Section", "Probe", "hello world"),
                     "appending new keys/sections failed.");

        const auto document = worldFiles.readIni();
        TEST_REQUIRE(document.has_value(),
                     "world.ini unreadable after appending keys.");

        // Lookups are case-insensitive, like SimpleIni.
        const auto existing =
            document->getValue("rendering parameters", "EDITOR PROBE");
        const auto appended =
            document->getValue("Editor Test Section", "Probe");
        TEST_REQUIRE(existing && *existing == "42" && appended &&
                         *appended == "hello world",
                     "appended keys cannot be read back.");

        char iniBuffer[64] = {};
        ReadIniString("Editor Test Section", "Probe", "", iniBuffer,
                      static_cast<std::uint32_t>(sizeof(iniBuffer)),
                      savedIni.u8string());
        TEST_REQUIRE(std::string(iniBuffer) == "hello world",
                     "SimpleIni cannot read the appended section.");
        std::printf("[PASS] new keys/sections are appended and readable by "
                    "SimpleIni.\n");

        // Drop the probe lines again so later checks see the baseline file.
        TEST_REQUIRE(MapEditor::Tests::writeFileBytes(savedIni, iniBaseline),
                     "could not restore the world.ini baseline.");
    }

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("WorldFiles: world.ini", 30, runWorldFilesIniTests);

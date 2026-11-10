// ============================================================================
// T8 addendum — player-name encoding in .spg files.
//
// The shipped COMPASS.spg itself carries a Windows-1251 player name (player
// 2, "Наместник") — the rhombi in the Battle Map window. The editor API
// works in UTF-8. Checked here, built from the shipped COMPASS.spg with its
// player names swapped:
//
//   1. CP1251 name -> playerData() returns UTF-8 (displayable, no U+FFFD),
//      including the shipped "Наместник" case;
//   2. an untouched CP1251 file saves BYTE-EXACT (detection changes no
//      bytes);
//   3. an edited name saves re-encoded in the file's original encoding
//      (CP1251 bytes, not UTF-8) and re-loads as the same UTF-8;
//   4. a uniformly UTF-8 file is detected as UTF-8 and keeps UTF-8 bytes on
//      save (a mixed file is legitimately detected as CP1251).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "errh_wrapper.h"

#include "spg/SpgFile.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// "Виктор" in the encodings under test.
const std::string kViktorCp1251 = "\xC2\xE8\xEA\xF2\xEE\xF0";              // 6 bytes
const std::string kViktorUtf8 =
    "\xD0\x92\xD0\xB8\xD0\xBA\xD1\x82\xD0\xBE\xD1\x80";                     // 12 bytes
// "Игрок" in the encodings under test.
const std::string kIgrokCp1251 = "\xC8\xE3\xF0\xEE\xEA";                   // 5 bytes
const std::string kIgrokUtf8 =
    "\xD0\x98\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA";                             // 10 bytes
// "Жора" (a 4-letter UTF-8 name, longer in bytes than "George").
const std::string kZhoraUtf8 = "\xD0\x96\xD0\xBE\xD1\x80\xD0\xB0";         // 8 bytes
// "Наместник" — the shipped COMPASS.spg player 2 name, CP1251 on disk.
const std::string kNamesnikCp1251 = "\xCD\xE0\xEC\xE5\xF1\xF2\xED\xE8\xEA"; // 9 bytes
const std::string kNamesnikUtf8 =
    "\xD0\x9D\xD0\xB0\xD0\xBC\xD0\xB5\xD1\x81\xD1\x82\xD0\xBD\xD0\xB8\xD0\xBA";

/// COMPASS.spg with the first player's name ("George", 6 bytes) replaced
/// and, optionally, the second player's shipped CP1251 name ("Наместник")
/// replaced too — the second swap makes the file uniformly UTF-8.
bool patchedSource(MapEditor::Tests::TestContext& ctx,
                   const std::string& replacement, std::string& out,
                   const std::string& secondReplacement = std::string())
{
    const fs::path source = ctx.gameDir / "RESOURCE" / "Battle" / "COMPASS.spg";
    const std::vector<std::uint8_t> bytes =
        MapEditor::Tests::readFileBytes(source);
    out.assign(bytes.begin(), bytes.end());
    const std::string from = "\"George\"";
    const std::size_t at = out.find(from);
    if (at == std::string::npos) {
        std::printf("  the COMPASS.spg layout changed (no \"George\" name).\n");
        return false;
    }
    out.replace(at, from.size(), "\"" + replacement + "\"");
    if (!secondReplacement.empty()) {
        const std::string from2 = "\"" + kNamesnikCp1251 + "\"";
        const std::size_t at2 = out.find(from2);
        if (at2 == std::string::npos) {
            std::printf("  the COMPASS.spg layout changed (no CP1251 name).\n");
            return false;
        }
        out.replace(at2, from2.size(), "\"" + secondReplacement + "\"");
    }
    return true;
}

bool runSpgEncodingTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();
    const fs::path dir = ctx.tempRoot / "spg" / "encoding";
    fs::create_directories(dir);

    // ------------------------------------------------------------------
    // 1-3: a Windows-1251 player name.
    // ------------------------------------------------------------------
    const fs::path cp1251Path = dir / "cp1251.spg";
    std::string cp1251Text;
    TEST_REQUIRE(patchedSource(ctx, kViktorCp1251, cp1251Text),
                 "could not build the CP1251 test file.");
    TEST_REQUIRE(MapEditor::Tests::writeFileBytes(
                     cp1251Path,
                     std::vector<std::uint8_t>(cp1251Text.begin(),
                                               cp1251Text.end())),
                 "could not write the CP1251 test file.");

    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(cp1251Path.u8string()), "CP1251 load failed.");
        const MapEditor::SpgFile::PlayerData p0 = spg.playerData(0);
        TEST_REQUIRE(p0.name == kViktorUtf8,
                     "the CP1251 name must surface as UTF-8, got %zu bytes.",
                     p0.name.size());
        // The shipped player 2 name ("Наместник", CP1251 on disk) is the
        // real-world case this fix is for — it must read as proper UTF-8.
        TEST_REQUIRE(spg.playerData(1).name == kNamesnikUtf8,
                     "the shipped CP1251 name must surface as UTF-8.");
        // Untouched ASCII names pass through unchanged.
        TEST_REQUIRE(spg.playerData(3).name == "Vice",
                     "untouched player names must not change.");

        // 2: an untouched CP1251 file saves byte-exact. Saved under the
        // embedded originalSaveName's own file name (COMPASS): save()
        // rewrites originalSaveName to the map's own path, which already
        // matches, so the document must stay untouched.
        const fs::path untouchedPath = dir / "copy" / "COMPASS.spg";
        fs::create_directories(untouchedPath.parent_path());
        TEST_REQUIRE(spg.save(untouchedPath.u8string()), "untouched save failed.");
        TEST_REQUIRE(MapEditor::Tests::readFileBytes(untouchedPath) ==
                         MapEditor::Tests::readFileBytes(cp1251Path),
                     "an untouched CP1251 file must save byte-exact.");

        // 3: an edit re-encodes into the file's original encoding.
        MapEditor::SpgFile::PlayerData edit = p0;
        edit.name = kIgrokUtf8;
        TEST_REQUIRE(spg.setPlayerData(0, edit), "setPlayerData failed.");
        const fs::path editedPath = dir / "cp1251_edited.spg";
        TEST_REQUIRE(spg.save(editedPath.u8string()), "edited save failed.");

        const std::vector<std::uint8_t> editedBytes =
            MapEditor::Tests::readFileBytes(editedPath);
        const std::string editedText(editedBytes.begin(), editedBytes.end());
        TEST_REQUIRE(editedText.find(kIgrokCp1251) != std::string::npos,
                     "the edited name must be written in CP1251.");
        TEST_REQUIRE(editedText.find(kIgrokUtf8) == std::string::npos,
                     "the edited name must NOT be written as UTF-8.");
        TEST_REQUIRE(editedText.find(kViktorCp1251) == std::string::npos,
                     "the old CP1251 name must be gone.");

        MapEditor::SpgFile reloaded;
        TEST_REQUIRE(reloaded.load(editedPath.u8string()), "re-load failed.");
        TEST_REQUIRE(reloaded.playerData(0).name == kIgrokUtf8,
                     "the edited name must re-load as the same UTF-8.");
        TEST_REQUIRE(reloaded.playerData(3).name == "Vice",
                     "untouched names must survive the edit save.");
    }
    std::printf("[PASS] CP1251 player names display as UTF-8 and save "
                "byte-preserved.\n");

    // ------------------------------------------------------------------
    // 4: a uniformly UTF-8 file stays UTF-8 (the shipped CP1251 name is
    //    replaced here, otherwise the file is genuinely mixed and the
    //    CP1251 detection is the correct outcome).
    // ------------------------------------------------------------------
    {
        std::string text;
        TEST_REQUIRE(patchedSource(ctx, kZhoraUtf8, text, kNamesnikUtf8),
                     "could not build the UTF-8 test file.");
        const fs::path utf8Path = dir / "utf8.spg";
        TEST_REQUIRE(MapEditor::Tests::writeFileBytes(
                         utf8Path,
                         std::vector<std::uint8_t>(text.begin(), text.end())),
                     "could not write the UTF-8 test file.");

        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(utf8Path.u8string()), "UTF-8 load failed.");
        TEST_REQUIRE(spg.playerData(0).name == kZhoraUtf8,
                     "the UTF-8 name must pass through unchanged.");

        MapEditor::SpgFile::PlayerData edit = spg.playerData(0);
        edit.name = kViktorUtf8;
        TEST_REQUIRE(spg.setPlayerData(0, edit), "setPlayerData failed.");
        const fs::path editedPath = dir / "utf8_edited.spg";
        TEST_REQUIRE(spg.save(editedPath.u8string()), "edited save failed.");

        const std::vector<std::uint8_t> editedBytes =
            MapEditor::Tests::readFileBytes(editedPath);
        const std::string editedText(editedBytes.begin(), editedBytes.end());
        TEST_REQUIRE(editedText.find(kViktorUtf8) != std::string::npos,
                     "the edited name must stay UTF-8 in a UTF-8 file.");
        TEST_REQUIRE(editedText.find(kViktorCp1251) == std::string::npos,
                     "a UTF-8 file must not be re-encoded to CP1251.");

        MapEditor::SpgFile reloaded;
        TEST_REQUIRE(reloaded.load(editedPath.u8string()), "re-load failed.");
        TEST_REQUIRE(reloaded.playerData(0).name == kViktorUtf8,
                     "the edited name must re-load as the same UTF-8.");
    }
    std::printf("[PASS] UTF-8 player names stay UTF-8 through edit and save.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("SPG player-name encoding", 72,
                         runSpgEncodingTests);

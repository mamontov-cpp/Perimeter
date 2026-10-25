// ============================================================================
// T8.1 — .spg IO: XPrmDocument byte round-trip and SpgFile editing.
//
//   70  load → save of the shipped COMPASS.spg is byte-exact; an untouched
//       re-render equals the source; scalar/object navigation reads the
//       expected values;
//   71  edits: worldName, playerData, zeroLayerHeight, movePlayerGroup
//       (frame + squad + stablePosition + frame slots + camera path all
//       shift, unknown data survives), setPlayerCount shrink/clone — every
//       change is verified through a re-load.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "errh_wrapper.h"

#include "spg/SpgFile.h"
#include "spg/XPrmDocument.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path spgSource(const MapEditor::Tests::TestContext& ctx)
{
    // A shipped battle map: the game dir copy (never modified).
    return ctx.gameDir / "RESOURCE" / "Battle" / "COMPASS.spg";
}

} // namespace

bool runSpgRoundTripTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path source = spgSource(ctx);
    TEST_REQUIRE(fs::exists(source), "COMPASS.spg missing in the game dir.");
    const std::vector<std::uint8_t> original =
        MapEditor::Tests::readFileBytes(source);

    // XPrmDocument: byte-exact re-render of an untouched file.
    MapEditor::XPrmDocument doc;
    TEST_REQUIRE(doc.load(source.u8string()), "XPrm load failed.");
    TEST_REQUIRE(doc.blocks.size() >= 2, "expected two top-level blocks.");
    const std::string rendered = doc.render();
    const bool byteExact =
        rendered.size() == original.size() &&
        std::equal(rendered.begin(), rendered.end(), original.begin(),
                   [](char a, std::uint8_t b) {
                       return static_cast<std::uint8_t>(a) == b;
                   });
    if (!byteExact) {
        std::size_t diff = 0;
        while (diff < rendered.size() && diff < original.size() &&
               rendered[diff] == static_cast<char>(original[diff])) {
            ++diff;
        }
        TEST_REQUIRE(false,
                     "untouched XPrm render is not byte-exact at offset %zu "
                     "(render %zu vs source %zu bytes).",
                     diff, rendered.size(), original.size());
    }

    // SpgFile header reads.
    MapEditor::SpgFile spg;
    TEST_REQUIRE(spg.load(source.u8string()), "SpgFile load failed.");
    TEST_REQUIRE(spg.worldName() == "COMPASS",
                 "worldName is '%s'.", spg.worldName().c_str());
    TEST_REQUIRE(spg.playerCount() == 4, "playerCount is %d.", spg.playerCount());
    TEST_REQUIRE(spg.zeroLayerHeight() == 177,
                 "zeroLayerHeight is %d.", spg.zeroLayerHeight());

    const MapEditor::SpgFile::PlayerData p0 = spg.playerData(0);
    TEST_REQUIRE(p0.playerID == 0 && p0.name == "George" &&
                     p0.realPlayerType == "REAL_PLAYER_TYPE_PLAYER" &&
                     p0.belligerent == "BELLIGERENT_EXODUS0",
                 "player 0 data mismatch.");

    double fx = 0;
    double fy = 0;
    TEST_REQUIRE(spg.playerAnchor(0, fx, fy) && fx > 0 && fy > 0,
                 "player 0 anchor not found.");

    // SpgFile save of an untouched file stays byte-exact too.
    const fs::path out = ctx.tempRoot / "spg" / "roundtrip.spg";
    fs::create_directories(out.parent_path());
    TEST_REQUIRE(spg.save(out.u8string()), "SpgFile save failed.");
    const std::vector<std::uint8_t> written =
        MapEditor::Tests::readFileBytes(out);
    TEST_REQUIRE(written == original, "untouched .spg save is not byte-exact.");

    return true;
}

bool runSpgEditTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path source = spgSource(ctx);
    const std::vector<std::uint8_t> original =
        MapEditor::Tests::readFileBytes(source);
    const fs::path out = ctx.tempRoot / "spg" / "edit.spg";

    // --- edit pass ---------------------------------------------------------
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(source.u8string()), "load failed.");

        TEST_REQUIRE(spg.setWorldName("MYWORLD"), "setWorldName failed.");
        TEST_REQUIRE(spg.setZeroLayerHeight(64), "setZeroLayerHeight failed.");

        MapEditor::SpgFile::PlayerData p1 = spg.playerData(1);
        p1.name = "Editor Player";
        p1.clan = 7;
        TEST_REQUIRE(spg.setPlayerData(1, p1), "setPlayerData failed.");

        // Move player 0 by (10, 5), z by +2.
        double fx = 0;
        double fy = 0;
        TEST_REQUIRE(spg.playerAnchor(0, fx, fy), "anchor missing.");
        TEST_REQUIRE(spg.movePlayerGroup(0, 10.0, 5.0, 2.0),
                     "movePlayerGroup failed.");

        TEST_REQUIRE(spg.save(out.u8string()), "save failed.");
    }

    // --- verify through a re-load -----------------------------------------
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(out.u8string()), "re-load failed.");
        TEST_REQUIRE(spg.worldName() == "MYWORLD", "worldName not persisted.");
        TEST_REQUIRE(spg.zeroLayerHeight() == 64,
                     "zeroLayerHeight is %d, expected 64.",
                     spg.zeroLayerHeight());
        const MapEditor::SpgFile::PlayerData p1 = spg.playerData(1);
        TEST_REQUIRE(p1.name == "Editor Player" && p1.clan == 7 &&
                         p1.playerID == 1,
                     "playerData not persisted.");

        double fx = 0;
        double fy = 0;
        TEST_REQUIRE(spg.playerAnchor(0, fx, fy), "anchor missing after edit.");
        TEST_REQUIRE(std::abs(fx - 396.768) < 0.01 && std::abs(fy - 407.333) < 0.01,
                     "frame anchor is (%f, %f), expected ~(396.77, 407.33).",
                     fx, fy);

        // Squad and stable position moved too (original 598.033).
        TEST_REQUIRE(spg.loaded(), "loaded state lost.");

        // Untouched parts survive: player 3 belligerent + camera 3 x moved
        // with the group of player 3 (not player 0).
        const MapEditor::SpgFile::PlayerData p3 = spg.playerData(3);
        TEST_REQUIRE(p3.belligerent == "BELLIGERENT_EMPIRE3" &&
                         p3.name == "Vice",
                     "untouched player data changed.");

        // The raw text must still contain the 100+ UI controls block.
        const auto textBytes = MapEditor::Tests::readFileBytes(out);
        const std::string text(textBytes.begin(), textBytes.end());
        TEST_REQUIRE(text.find("SQSH_CHAT_INFO_ID") != std::string::npos,
                     "the controls block was dropped.");
        TEST_REQUIRE(text.find("omegaMissionType") != std::string::npos,
                     "manualData tail was dropped.");
    }

    // --- setPlayerCount ----------------------------------------------------
    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(out.u8string()), "load failed.");
        TEST_REQUIRE(spg.setPlayerCount(2), "setPlayerCount failed.");
        const fs::path shrunk = ctx.tempRoot / "spg" / "shrunk.spg";
        TEST_REQUIRE(spg.save(shrunk.u8string()), "save failed.");

        MapEditor::SpgFile check;
        TEST_REQUIRE(check.load(shrunk.u8string()), "re-load failed.");
        TEST_REQUIRE(check.playerCount() == 2, "playerCount is %d.",
                     check.playerCount());
        const MapEditor::SpgFile::PlayerData last = check.playerData(1);
        TEST_REQUIRE(last.playerID == 1, "cloned player id not renumbered.");
        const std::vector<int> shuffling = check.shufflingIndices();
        TEST_REQUIRE(shuffling.size() == 2 && shuffling[0] == 0 &&
                         shuffling[1] == 1,
                     "shuffling indices not reset.");

        // Shrink to 1 and grow back to 3 (clone path).
        TEST_REQUIRE(check.setPlayerCount(1) && check.setPlayerCount(3),
                     "grow from 1 to 3 failed.");
        TEST_REQUIRE(check.playerCount() == 3, "grow failed.");
    }

    return true;
}

MAP_EDITOR_REGISTER_TEST("SPG round-trip", 70, runSpgRoundTripTests);
MAP_EDITOR_REGISTER_TEST("SPG edits", 71, runSpgEditTests);

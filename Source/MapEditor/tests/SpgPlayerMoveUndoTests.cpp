// ============================================================================
// Place Spawn mode — the undoable spawn relocation of SpgFile.
//
// setPlayerAnchorUndoable() moves the whole spawn group like setPlayerAnchor
// but records the delta, so undoPlayerMove()/redoPlayerMove() walk the
// history back and forth. Checked here on the shipped COMPASS.spg:
//
//   1. a move applies (anchor + frame z shift by dz), undo restores the
//      anchor AND the frame z byte-level (saved file re-parsed), redo
//      re-applies;
//   2. moves of two players stack and unwind in LIFO order;
//   3. a fresh move discards the redo tail;
//   4. load() clears the history.
//
// Runs after the other SPG tests (order 74) and only reads the game dir.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "errh_wrapper.h"

#include "spg/SpgFile.h"
#include "spg/XPrmDocument.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

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

double frameZOf(MapEditor::XPrmDocument& doc, int player)
{
    return vecZ(playerElement(doc, player)->object("frame")
                    ->object("position"));
}

bool near(double a, double b)
{
    // The values round-trip through the XPrm text serialization.
    return std::abs(a - b) < 1e-6;
}

bool runSpgPlayerMoveUndoTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path source = ctx.gameDir / "RESOURCE" / "Battle" / "COMPASS.spg";
    TEST_REQUIRE(fs::exists(source), "COMPASS.spg missing in the game dir.");
    const fs::path dir = ctx.tempRoot / "spg" / "playermove";
    fs::create_directories(dir);

    // Baseline frame z of player 0 straight from the source file.
    double baseZ0 = 0.0;
    {
        MapEditor::XPrmDocument doc;
        TEST_REQUIRE(doc.load(source.u8string()), "source parse failed.");
        baseZ0 = frameZOf(doc, 0);
    }

    MapEditor::SpgFile spg;
    TEST_REQUIRE(spg.load(source.u8string()), "load failed.");
    double ax = 0;
    double ay = 0;
    TEST_REQUIRE(spg.playerAnchor(0, ax, ay),
                 "COMPASS player 0 anchor unreadable.");
    TEST_REQUIRE(!spg.canUndoPlayerMove() && !spg.canRedoPlayerMove(),
                 "a fresh document must have an empty move history.");

    // ------------------------------------------------------------------
    // 1: one undoable move; undo restores anchor AND frame z; redo reapplies.
    // ------------------------------------------------------------------
    TEST_REQUIRE(spg.setPlayerAnchorUndoable(0, ax + 40.0, ay - 25.0, 6.5),
                 "the undoable move was refused.");
    {
        double x = 0;
        double y = 0;
        TEST_REQUIRE(spg.playerAnchor(0, x, y), "anchor unreadable.");
        TEST_REQUIRE(near(x, ax + 40.0) && near(y, ay - 25.0),
                     "the anchor did not move to the target.");
        TEST_REQUIRE(spg.canUndoPlayerMove() && !spg.canRedoPlayerMove(),
                     "the move must be undoable with an empty redo side.");
    }
    {
        // The frame z rides with dz: +6.5 on the saved document.
        const fs::path moved = dir / "moved.spg";
        TEST_REQUIRE(spg.save(moved.u8string()), "save of the move failed.");
        MapEditor::XPrmDocument doc;
        TEST_REQUIRE(doc.load(moved.u8string()), "moved parse failed.");
        TEST_REQUIRE(near(frameZOf(doc, 0), baseZ0 + 6.5),
                     "the frame z must shift by dz (+6.5).");
    }
    TEST_REQUIRE(spg.undoPlayerMove(), "undo failed.");
    {
        double x = 0;
        double y = 0;
        TEST_REQUIRE(spg.playerAnchor(0, x, y), "anchor unreadable.");
        TEST_REQUIRE(near(x, ax) && near(y, ay),
                     "undo must restore the previous anchor.");
    }
    {
        // And the frame z comes back exactly (re-parse the saved document).
        const fs::path undone = dir / "undone.spg";
        TEST_REQUIRE(spg.save(undone.u8string()), "save after undo failed.");
        MapEditor::XPrmDocument doc;
        TEST_REQUIRE(doc.load(undone.u8string()), "undone parse failed.");
        TEST_REQUIRE(near(frameZOf(doc, 0), baseZ0),
                     "undo must restore the frame z byte-level.");
    }
    TEST_REQUIRE(spg.canRedoPlayerMove(),
                 "the undone move must sit on the redo side.");
    TEST_REQUIRE(spg.redoPlayerMove(), "redo failed.");
    {
        double x = 0;
        double y = 0;
        TEST_REQUIRE(spg.playerAnchor(0, x, y), "anchor unreadable.");
        TEST_REQUIRE(near(x, ax + 40.0) && near(y, ay - 25.0),
                     "redo must re-apply the move.");
        TEST_REQUIRE(!spg.canRedoPlayerMove(),
                     "redo consumes its side of the history.");
    }
    std::printf("[PASS] undoable spawn move: undo restores the anchor and the "
                "frame z, redo re-applies.\n");

    // ------------------------------------------------------------------
    // 2: moves of two players stack and unwind in LIFO order.
    // ------------------------------------------------------------------
    double bx = 0;
    double by = 0;
    TEST_REQUIRE(spg.playerAnchor(1, bx, by),
                 "COMPASS player 1 anchor unreadable.");
    TEST_REQUIRE(spg.setPlayerAnchorUndoable(1, bx + 10.0, by + 10.0, -3.0),
                 "the second player's move was refused.");
    TEST_REQUIRE(spg.undoPlayerMove(), "undo of player 1 failed.");
    {
        double x = 0;
        double y = 0;
        TEST_REQUIRE(spg.playerAnchor(1, x, y), "anchor unreadable.");
        TEST_REQUIRE(near(x, bx) && near(y, by),
                     "player 1 must be back at its anchor.");
        TEST_REQUIRE(spg.playerAnchor(0, x, y), "anchor unreadable.");
        TEST_REQUIRE(near(x, ax + 40.0) && near(y, ay - 25.0),
                     "player 0's move must survive the undo of player 1.");
    }
    TEST_REQUIRE(spg.undoPlayerMove(), "undo of player 0 failed.");
    TEST_REQUIRE(!spg.canUndoPlayerMove(),
                 "both moves undone — the history must be empty.");
    {
        double x = 0;
        double y = 0;
        TEST_REQUIRE(spg.playerAnchor(0, x, y), "anchor unreadable.");
        TEST_REQUIRE(near(x, ax) && near(y, ay),
                     "player 0 must be back at its anchor.");
    }
    TEST_REQUIRE(spg.redoPlayerMove(), "redo of player 0 failed.");
    TEST_REQUIRE(spg.redoPlayerMove(), "redo of player 1 failed.");
    TEST_REQUIRE(!spg.canRedoPlayerMove(),
                 "both moves redone — the redo side must be empty.");
    {
        double x = 0;
        double y = 0;
        TEST_REQUIRE(spg.playerAnchor(0, x, y), "anchor unreadable.");
        TEST_REQUIRE(near(x, ax + 40.0) && near(y, ay - 25.0),
                     "redo must re-apply player 0's move.");
        TEST_REQUIRE(spg.playerAnchor(1, x, y), "anchor unreadable.");
        TEST_REQUIRE(near(x, bx + 10.0) && near(y, by + 10.0),
                     "redo must re-apply player 1's move.");
    }
    std::printf("[PASS] moves of two players stack and unwind in LIFO "
                "order.\n");

    // ------------------------------------------------------------------
    // 3: a fresh move discards the redo tail.
    // ------------------------------------------------------------------
    TEST_REQUIRE(spg.undoPlayerMove(), "undo before the tail check failed.");
    TEST_REQUIRE(spg.canRedoPlayerMove(),
                 "the redo tail must be non-empty here.");
    TEST_REQUIRE(spg.setPlayerAnchorUndoable(1, bx + 20.0, by, 0.0),
                 "the fresh move was refused.");
    TEST_REQUIRE(!spg.canRedoPlayerMove(),
                 "a fresh move must discard the redo tail.");
    std::printf("[PASS] a fresh move discards the redo tail.\n");

    // ------------------------------------------------------------------
    // 4: load() clears the history.
    // ------------------------------------------------------------------
    MapEditor::SpgFile fresh;
    TEST_REQUIRE(fresh.load(source.u8string()), "re-load failed.");
    TEST_REQUIRE(!fresh.canUndoPlayerMove() && !fresh.canRedoPlayerMove(),
                 "load() must clear the move history.");
    std::printf("[PASS] load() clears the move history.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("SPG player move undo", 74, runSpgPlayerMoveUndoTests);

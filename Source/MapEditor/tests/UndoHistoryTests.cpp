// ============================================================================
// T1.4 — EditorSession editor-side undo/redo history.
//
// Terra's UndoDispatcher_PutPreChangedArea() records nothing outside _SURMAP_,
// so the session keeps its own snapshot stack (recordUndoArea() → undo/redo
// swap). These tests exercise the public session API end to end on a loaded
// world:
//
//   1. record → edit → undo restores the four voxel buffers byte-exact.
//   2. redo re-applies the edit.
//   3. a new record discards the undo tail and keeps the stack consistent.
//   4. clearUndoHistory / unloadWorld reset the state and the Modified flag.
//   5. switching to another world (and back) resets the stack and the
//      modified flag; histories of the two worlds cannot see each other.
//
// Runs after SaveWorldTests (order 53) and reuses its temporary world.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

// EditorSession (MapEditorCore) — the class under test.
#include "EditorSession.h"
// errh_wrapper.h (MapEditorSupport) — ErrH setup, already done by earlier
// tests but repeated here so this file is self-contained.
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

/// Byte copies of a rectangle across the four voxel buffers.
struct RegionBytes
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> geo;
    std::vector<std::uint8_t> dam;
    std::vector<std::uint8_t> atr;
    std::vector<std::uint8_t> sur;
};

RegionBytes readRegion(int x, int y, int w, int h)
{
    RegionBytes region;
    region.x = x;
    region.y = y;
    region.w = w;
    region.h = h;
    const std::size_t cells = static_cast<std::size_t>(w) * h;
    region.geo.resize(cells);
    region.dam.resize(cells);
    region.atr.resize(cells);
    region.sur.resize(cells);
    std::size_t cnt = 0;
    for (int j = 0; j < h; ++j) {
        const int row = (y + j) * vMap.H_SIZE + x;
        for (int i = 0; i < w; ++i, ++cnt) {
            const int off = row + i;
            region.geo[cnt] = vMap.VxGBuf[off];
            region.dam[cnt] = vMap.VxDBuf[off];
            region.atr[cnt] = vMap.AtrBuf[off];
            region.sur[cnt] = vMap.SurBuf[off];
        }
    }
    return region;
}

bool regionEquals(const RegionBytes& a, const RegionBytes& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h &&
           a.geo == b.geo && a.dam == b.dam && a.atr == b.atr &&
           a.sur == b.sur;
}

/// Square region used by the whole test, comfortably inside the map.
struct TestRegion
{
    static constexpr int kX = 10;
    static constexpr int kY = 10;
    static constexpr int kSize = 32;
};

/// Fill the region with pseudo-random values derived from `salt` so a
/// subsequent undo/redo round trip has clearly distinct states.
void writeRegion(int salt)
{
    for (int j = 0; j < TestRegion::kSize; ++j) {
        const int row = (TestRegion::kY + j) * vMap.H_SIZE + TestRegion::kX;
        for (int i = 0; i < TestRegion::kSize; ++i) {
            const int off = row + i;
            const std::uint8_t value =
                static_cast<std::uint8_t>(salt * 37 + i * 5 + j * 11);
            vMap.VxGBuf[off] = value;
            vMap.VxDBuf[off] = static_cast<std::uint8_t>(value / 2);
            vMap.AtrBuf[off] = static_cast<std::uint8_t>(value / 3);
            vMap.SurBuf[off] = static_cast<std::uint8_t>(value / 7);
        }
    }
    // Register the change so the dirty-rect pipeline a repaint would use sees
    // it (a GUI undo does exactly this through vrtMap::regRender).
    vMap.regRender(TestRegion::kX - 1, TestRegion::kY - 1,
                   TestRegion::kX + TestRegion::kSize,
                   TestRegion::kY + TestRegion::kSize);
}

bool runUndoHistoryTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Self-contained content root: the temp world copy plus the shared
    // resources the Terra loader needs. The real game folder stays read-only.
    const fs::path loadRoot = ctx.tempRoot / "load_root";
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec,
                     "could not create the load content root at '%s'.",
                     loadRoot.u8string().c_str());
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.tempWorldDir, loadRoot / "RESOURCE" / "Worlds" /
                                 fs::u8path(ctx.worldName));
    }

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    TEST_REQUIRE(vMap.H_SIZE > TestRegion::kX + TestRegion::kSize &&
                     vMap.V_SIZE > TestRegion::kY + TestRegion::kSize,
                 "the world is too small for the test region.");

    // ------------------------------------------------------------------
    // 10.1: a fresh session has nothing to undo or redo.
    // ------------------------------------------------------------------
    TEST_REQUIRE(!session.canUndo(), "fresh session must not be undoable.");
    TEST_REQUIRE(!session.canRedo(), "fresh session must not be redoable.");

    // ------------------------------------------------------------------
    // 10.2: record → edit → undo restores all four buffers byte-exact.
    // ------------------------------------------------------------------
    const RegionBytes before = readRegion(TestRegion::kX, TestRegion::kY,
                                          TestRegion::kSize, TestRegion::kSize);

    session.recordUndoArea(TestRegion::kX, TestRegion::kY,
                           TestRegion::kSize, TestRegion::kSize);
    TEST_REQUIRE(session.canUndo(),
                 "recordUndoArea() did not make the edit undoable.");
    TEST_REQUIRE(!session.canRedo(),
                 "a fresh record must not enable redo.");

    writeRegion(1);
    TEST_REQUIRE(session.isModified(),
                 "an edit right after a record must mark the world modified.");

    TEST_REQUIRE(session.undo(), "EditorSession::undo() failed.");
    const RegionBytes restored = readRegion(TestRegion::kX, TestRegion::kY,
                                            TestRegion::kSize, TestRegion::kSize);
    TEST_REQUIRE(regionEquals(before, restored),
                 "undo() did not restore the edited region byte-exact.");
    TEST_REQUIRE(!session.canUndo(),
                 "undoing the only record must empty the undo side of the stack.");
    TEST_REQUIRE(session.canRedo() && session.redo(),
                 "the undone record must be redoable.");
    std::printf("[PASS] record → edit → undo → redo round-trips all four "
                "voxel buffers byte-exact.\n");

    // ------------------------------------------------------------------
    // 10.3: the undo position tracks the modified state against the save
    // checkpoint (hasSavedUndoPos_ starts false, so any redo state counts as
    // modified — the buffers differ from the file on disk).
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.isModified(),
                 "the world must stay modified after redo (never saved).");

    // ------------------------------------------------------------------
    // 10.4: a new record after undo discards the redo tail.
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.undo(), "second undo (redo-tail state) failed.");
    TEST_REQUIRE(session.canRedo(),
                 "undo must keep the redo tail until a new record.");
    session.recordUndoArea(TestRegion::kX - 16, TestRegion::kY - 16,
                           64, 64);
    TEST_REQUIRE(!session.canRedo(),
                 "recordUndoArea() must discard the redo tail.");
    TEST_REQUIRE(session.undo(), "undo of the re-recorded region failed.");
    const RegionBytes reRestore = readRegion(TestRegion::kX, TestRegion::kY,
                                             TestRegion::kSize, TestRegion::kSize);
    TEST_REQUIRE(regionEquals(before, reRestore),
                 "undo did not restore the pre-record state again.");
    std::printf("[PASS] a new record discards the redo tail, undo stays "
                "consistent.\n");

    // ------------------------------------------------------------------
    // 10.5: recordUndoArea() clips out-of-bounds rectangles instead of
    // producing a corrupted snapshot.
    // ------------------------------------------------------------------
    writeRegion(1); // put the salted state back after 10.4's undo
    const RegionBytes salted = readRegion(TestRegion::kX, TestRegion::kY,
                                          TestRegion::kSize, TestRegion::kSize);
    session.recordUndoArea(vMap.H_SIZE - 8, vMap.V_SIZE - 8, 64, 64);
    TEST_REQUIRE(!session.canRedo(),
                 "clipping at the map border must behave like a new record.");
    TEST_REQUIRE(session.undo(), "undo of a border-clipped record failed.");
    const RegionBytes afterClip = readRegion(TestRegion::kX, TestRegion::kY,
                                             TestRegion::kSize,
                                             TestRegion::kSize);
    TEST_REQUIRE(regionEquals(salted, afterClip),
                 "the salted state must survive a border-clipped record.");
    std::printf("[PASS] recordUndoArea() clips to the map bounds safely.\n");

    // ------------------------------------------------------------------
    // 10.6: clearUndoHistory() and unloadWorld() reset everything.
    // ------------------------------------------------------------------
    session.clearUndoHistory();
    TEST_REQUIRE(!session.canUndo() && !session.canRedo(),
                 "clearUndoHistory() must empty both sides of the stack.");
    session.unloadWorld();
    TEST_REQUIRE(!session.canUndo() && !session.canRedo(),
                 "unloadWorld() must reset the undo state.");
    std::printf("[PASS] clearUndoHistory() and unloadWorld() reset the state.\n");

    // ------------------------------------------------------------------
    // 10.7: switching worlds resets the history. The undo stack must never
    // carry snapshots of world A into world B (same session): loading another
    // world goes through unloadWorld()/loadWorld(), both of which clear the
    // stack. Also verifies the histories stay isolated — after the switch the
    // new snapshot/undo pair refers to world B's buffers only.
    // ------------------------------------------------------------------
    {
        // A second world folder, so a switch is possible inside one content
        // root. The copy is made on disk only — world A's edits live in the
        // buffers and are dropped by the switch, never saved over the copy.
        const fs::path worldsRoot = loadRoot / "RESOURCE" / "Worlds";
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.tempWorldDir,
            worldsRoot / (fs::u8path(ctx.worldName).u8string() + "_switch"));
        TEST_REQUIRE(fs::exists(worldsRoot /
                                fs::u8path(ctx.worldName + "_switch") /
                                "world.ini"),
                     "the second world copy for the switch test is missing.");

        // scanWorlds() again so the new folder appears in the session's list.
        TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 2,
                     "world rescan after the copy failed.");

        TEST_REQUIRE(session.loadWorldByName(ctx.worldName),
                     "re-loading world A failed.");
        session.recordUndoArea(TestRegion::kX, TestRegion::kY,
                               TestRegion::kSize, TestRegion::kSize);
        writeRegion(1);
        TEST_REQUIRE(session.canUndo(),
                     "world A must be undoable before the switch.");

        // --- the switch under test ---
        const std::string worldB = ctx.worldName + "_switch";
        TEST_REQUIRE(session.loadWorldByName(worldB),
                     "loading world B failed.");
        TEST_REQUIRE(!session.canUndo() && !session.canRedo(),
                     "the undo/redo history of world A leaked into world B.");
        TEST_REQUIRE(!session.isModified(),
                     "world A's modified flag leaked into world B.");
        std::printf("[PASS] opening another world resets the undo history "
                    "and the modified flag.\n");

        // Histories must stay isolated: a record/undo cycle in world B works
        // purely on B's buffers (identical copies, so byte-exact checks hold).
        const RegionBytes beforeB = readRegion(TestRegion::kX, TestRegion::kY,
                                               TestRegion::kSize,
                                               TestRegion::kSize);
        session.recordUndoArea(TestRegion::kX, TestRegion::kY,
                               TestRegion::kSize, TestRegion::kSize);
        writeRegion(2);
        TEST_REQUIRE(session.undo(), "undo in world B failed.");
        const RegionBytes restoredB = readRegion(
            TestRegion::kX, TestRegion::kY, TestRegion::kSize, TestRegion::kSize);
        TEST_REQUIRE(regionEquals(beforeB, restoredB),
                     "undo in world B did not restore B's own state.");
        std::printf("[PASS] after the switch, undo operates on world B only.\n");

        // The same must hold the other way round: history built in world B
        // must not follow back into world A.
        TEST_REQUIRE(session.canRedo(),
                     "the B record must stay redoable inside world B.");
        TEST_REQUIRE(session.loadWorldByName(ctx.worldName),
                     "switching back to world A failed.");
        TEST_REQUIRE(!session.canUndo() && !session.canRedo(),
                     "the history of world B leaked into world A.");
        std::printf("[PASS] switching worlds in both directions keeps the "
                    "stacks isolated.\n");
    }

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Undo/Redo history", 53, runUndoHistoryTests);

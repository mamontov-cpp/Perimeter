// ============================================================================
// Undo coalescing — undoable actions that follow one another within the
// coalescing window (500 ms by default) form ONE undo transaction.
//
// Exercises the EditorSession time-window grouping end to end on a loaded
// world:
//
//   1. two records made back-to-back merge: a single undo() restores the
//      pre-edit state of BOTH edits byte-exact, redo() re-applies both;
//   2. a pause longer than the window splits the history into two entries;
//   3. breakUndoCoalescing() forces a boundary between two close-in-time
//      edits;
//   4. an explicit begin/endUndoGroup() participates in the window: a group
//      followed immediately by a plain record is still ONE transaction.
//
// Runs after UndoHistoryTests (order 53) and reuses its temporary world.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

/// Square region used by the whole test, comfortably inside the map.
struct TestRegion
{
    static constexpr int kX = 10;
    static constexpr int kY = 10;
    static constexpr int kSize = 32;
};

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

/// Overwrite one buffer byte of every cell of the given rectangle with a
/// salt-derived value (the other three buffers stay as they are, so each edit
/// is clearly distinguishable in undo checks).
void editRect(int x, int y, int w, int h, int salt)
{
    for (int j = 0; j < h; ++j) {
        const int row = (y + j) * vMap.H_SIZE + x;
        for (int i = 0; i < w; ++i) {
            vMap.VxGBuf[row + i] =
                static_cast<std::uint8_t>(salt * 37 + i * 5 + j * 11);
        }
    }
    vMap.regRender(x - 1, y - 1, x + w, y + h);
}

bool runUndoCoalescingTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Self-contained content root, the same way UndoHistoryTests builds it.
    const fs::path loadRoot = ctx.tempRoot / "load_root_coalescing";
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
    TEST_REQUIRE(session.undoCoalescingWindowMs() == 500,
                 "the default coalescing window must be 500 ms.");

    TEST_REQUIRE(vMap.H_SIZE > TestRegion::kX + TestRegion::kSize &&
                     vMap.V_SIZE > TestRegion::kY + TestRegion::kSize,
                 "the world is too small for the test region.");

    // ------------------------------------------------------------------
    // 1. Two records back-to-back (< 500 ms apart) are ONE transaction:
    //    a single undo() reverts both edits, a single redo() re-applies both.
    // ------------------------------------------------------------------
    const RegionBytes initial = readRegion(TestRegion::kX, TestRegion::kY,
                                           TestRegion::kSize,
                                           TestRegion::kSize);
    // Edit A: the whole region; then edit B: a sub-rectangle.
    session.recordUndoArea(TestRegion::kX, TestRegion::kY,
                           TestRegion::kSize, TestRegion::kSize);
    editRect(TestRegion::kX, TestRegion::kY, TestRegion::kSize,
             TestRegion::kSize, 1);
    session.recordUndoArea(TestRegion::kX + 8, TestRegion::kY + 8, 16, 16);
    editRect(TestRegion::kX + 8, TestRegion::kY + 8, 16, 16, 2);

    TEST_REQUIRE(session.canUndo(),
                 "the merged transaction must be undoable.");
    TEST_REQUIRE(session.undo(), "undo of the merged transaction failed.");
    const RegionBytes restored = readRegion(TestRegion::kX, TestRegion::kY,
                                            TestRegion::kSize,
                                            TestRegion::kSize);
    TEST_REQUIRE(regionEquals(initial, restored),
                 "one undo must revert BOTH edits of the transaction.");
    TEST_REQUIRE(!session.canUndo(),
                 "the merged transaction was a single entry — nothing else "
                 "to undo.");
    TEST_REQUIRE(session.canRedo() && session.redo(),
                 "the merged transaction must be redoable.");
    const RegionBytes redone = readRegion(TestRegion::kX, TestRegion::kY,
                                          TestRegion::kSize,
                                          TestRegion::kSize);
    TEST_REQUIRE(!regionEquals(initial, redone),
                 "redo must re-apply the edits.");
    {
        // The sub-rectangle carries edit B's salt, the margin around it edit
        // A's — both edits must come back with one redo.
        const RegionBytes bRegion = readRegion(TestRegion::kX + 8,
                                               TestRegion::kY + 8, 16, 16);
        RegionBytes expectB;
        expectB.x = TestRegion::kX + 8;
        expectB.y = TestRegion::kY + 8;
        expectB.w = 16;
        expectB.h = 16;
        expectB.geo.resize(16 * 16);
        for (int j = 0; j < 16; ++j) {
            for (int i = 0; i < 16; ++i) {
                expectB.geo[static_cast<std::size_t>(j) * 16 + i] =
                    static_cast<std::uint8_t>(2 * 37 + i * 5 + j * 11);
            }
        }
        TEST_REQUIRE(bRegion.geo == expectB.geo,
                     "redo must re-apply the second (sub-rectangle) edit.");
    }
    std::printf("[PASS] back-to-back records within the window merge into one "
                "undo transaction (undo reverts both, redo re-applies both).\n");

    // ------------------------------------------------------------------
    // 2. A pause longer than the window splits the history: with a 30 ms
    //    window and a 60 ms pause the two edits need TWO undos.
    // ------------------------------------------------------------------
    session.setUndoCoalescingWindowMs(30);
    session.breakUndoCoalescing(); // settle the transaction above
    TEST_REQUIRE(session.undo(), "undo back to the initial state failed.");
    TEST_REQUIRE(!session.canUndo(),
                 "the pre-coalescing history must be exhausted.");

    const RegionBytes beforeSplit = readRegion(TestRegion::kX, TestRegion::kY,
                                               TestRegion::kSize,
                                               TestRegion::kSize);
    session.recordUndoArea(TestRegion::kX, TestRegion::kY,
                           TestRegion::kSize, TestRegion::kSize);
    editRect(TestRegion::kX, TestRegion::kY, TestRegion::kSize,
             TestRegion::kSize, 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    session.recordUndoArea(TestRegion::kX + 4, TestRegion::kY + 4, 8, 8);
    editRect(TestRegion::kX + 4, TestRegion::kY + 4, 8, 8, 4);

    TEST_REQUIRE(session.undo(), "undo of the second (post-pause) edit failed.");
    const RegionBytes afterFirstUndo = readRegion(TestRegion::kX,
                                                  TestRegion::kY,
                                                  TestRegion::kSize,
                                                  TestRegion::kSize);
    TEST_REQUIRE(afterFirstUndo.geo != beforeSplit.geo,
                 "the first edit must survive the undo of the second one "
                 "(window expired → two entries).");
    TEST_REQUIRE(session.undo(), "undo of the first edit failed.");
    const RegionBytes afterSecondUndo = readRegion(TestRegion::kX,
                                                   TestRegion::kY,
                                                   TestRegion::kSize,
                                                   TestRegion::kSize);
    TEST_REQUIRE(regionEquals(beforeSplit, afterSecondUndo),
                 "undoing both entries must restore the pre-edit state.");
    std::printf("[PASS] a pause longer than the window splits the history "
                "into separate undo entries.\n");

    // ------------------------------------------------------------------
    // 3. breakUndoCoalescing() forces a boundary even inside the window.
    // ------------------------------------------------------------------
    session.setUndoCoalescingWindowMs(500);
    session.recordUndoArea(TestRegion::kX, TestRegion::kY,
                           TestRegion::kSize, TestRegion::kSize);
    editRect(TestRegion::kX, TestRegion::kY, TestRegion::kSize,
             TestRegion::kSize, 5);
    session.breakUndoCoalescing();
    session.recordUndoArea(TestRegion::kX + 4, TestRegion::kY + 4, 8, 8);
    editRect(TestRegion::kX + 4, TestRegion::kY + 4, 8, 8, 6);

    TEST_REQUIRE(session.undo(), "undo of the post-break edit failed.");
    {
        const RegionBytes margin = readRegion(TestRegion::kX, TestRegion::kY,
                                              4, 4);
        TEST_REQUIRE(margin.geo.front() ==
                         static_cast<std::uint8_t>(5 * 37),
                     "the break must keep the first edit in its own entry.");
    }
    TEST_REQUIRE(session.undo(), "undo of the pre-break edit failed.");
    TEST_REQUIRE(!session.canUndo(),
                 "both sides of the break must now be undone.");
    std::printf("[PASS] breakUndoCoalescing() splits close-in-time edits into "
                "separate transactions.\n");

    // ------------------------------------------------------------------
    // 4. An explicit undo group followed immediately by a plain record is
    //    still ONE transaction (the group keeps the window pending).
    // ------------------------------------------------------------------
    session.beginUndoGroup();
    session.recordUndoArea(TestRegion::kX, TestRegion::kY,
                           TestRegion::kSize, TestRegion::kSize);
    editRect(TestRegion::kX, TestRegion::kY, TestRegion::kSize,
             TestRegion::kSize, 7);
    session.recordUndoArea(TestRegion::kX + 2, TestRegion::kY + 2, 4, 4);
    editRect(TestRegion::kX + 2, TestRegion::kY + 2, 4, 4, 8);
    TEST_REQUIRE(session.endUndoGroup(),
                 "the explicit group recorded snapshots.");
    // Immediately after the group (well within the window): a plain record.
    session.recordUndoArea(TestRegion::kX + 16, TestRegion::kY + 16, 8, 8);
    editRect(TestRegion::kX + 16, TestRegion::kY + 16, 8, 8, 9);

    TEST_REQUIRE(session.undo(),
                 "undo of the group+record transaction failed.");
    const RegionBytes afterGroupUndo = readRegion(TestRegion::kX,
                                                  TestRegion::kY,
                                                  TestRegion::kSize,
                                                  TestRegion::kSize);
    TEST_REQUIRE(regionEquals(afterSecondUndo, afterGroupUndo),
                 "one undo must revert the explicit group AND the record "
                 "that followed it inside the window.");
    TEST_REQUIRE(!session.canUndo(),
                 "the group and the following record were one entry.");
    std::printf("[PASS] an explicit undo group joins the coalescing window "
                "(group + immediate record = one transaction).\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Undo coalescing window", 54, runUndoCoalescingTests);

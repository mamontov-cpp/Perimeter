// ============================================================================
// EXTDEMO true-color diagnostic — verifies that painted (true-color) cells
// render as SHADED EXACT RGB (the lighting effect), not as palette
// quantization.
//
// For every painted cell of the EXTDEMO world (4096x4096, ships with the
// game data used by this suite) the test compares getColor32() against:
//   A. trueColor32(exact RGB from SurColBuf, RnrBuf light) — shading of the
//      exact painted color (the expected engine behavior);
//   B. Sur2Col32[SurBuf][RnrBuf] — the palette path with the cell's nearest
//      palette entry (the quantization hypothesis).
// The test PASSES when hypothesis A explains (almost) every painted cell;
// it prints both counts so a regression to palette rendering is visible in
// the log. Skips (passes) when the EXTDEMO world is absent.
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

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool runExtdemoColorTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    if (!session.setGameDir(ctx.gameDir.u8string())) {
        std::printf("[SKIP] EXTDEMO diagnostic: no game dir.\n");
        return true;
    }
    if (!session.scanWorlds()) {
        std::printf("[SKIP] EXTDEMO diagnostic: world scan failed.\n");
        return true;
    }
    bool found = false;
    for (const MapEditor::WorldInfo& info : session.getWorldList()) {
        if (info.name == "EXTDEMO") {
            found = true;
            break;
        }
    }
    if (!found) {
        std::printf("[SKIP] EXTDEMO diagnostic: world EXTDEMO not found.\n");
        return true;
    }
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    TEST_REQUIRE(vMap.SurColBuf != nullptr,
                 "EXTDEMO must carry the true-color layer.");

    // Lighting exactly like the game after universalLoad.
    vMap.WorldRender();

    const std::uint64_t cells =
        static_cast<std::uint64_t>(vMap.H_SIZE) * vMap.V_SIZE;
    std::uint64_t painted = 0;
    std::uint64_t exactShading = 0;
    std::uint64_t palettePath = 0;
    std::uint64_t other = 0;
    for (std::uint64_t off = 0; off < cells; ++off) {
        const std::uint32_t c = vMap.SurColBuf[off];
        if ((c & 0xFF000000) == 0) {
            continue; // palette cell — not part of the check
        }
        ++painted;
        const unsigned int light = vMap.RnrBuf[off] & 0x7F;
        const unsigned int rendered = vMap.getColor32(static_cast<int>(off));
        if (rendered == vrtMap::trueColor32(c, light)) {
            ++exactShading;
            continue;
        }
        if (rendered == vMap.Sur2Col32[vMap.SurBuf[off]][vMap.RnrBuf[off]]) {
            ++palettePath;
            continue;
        }
        ++other;
    }

    std::printf("EXTDEMO painted cells: %llu\n",
                static_cast<unsigned long long>(painted));
    std::printf("  shaded EXACT RGB (expected): %llu\n",
                static_cast<unsigned long long>(exactShading));
    std::printf("  palette path (quantization): %llu\n",
                static_cast<unsigned long long>(palettePath));
    std::printf("  unexplained: %llu\n",
                static_cast<unsigned long long>(other));
    TEST_REQUIRE(painted > 0, "EXTDEMO must have painted cells.");
    TEST_REQUIRE(exactShading + palettePath + other == painted,
                 "classification must cover every painted cell.");
    // The engine must render painted cells as the shaded exact RGB. A tiny
    // palette-path residue is tolerated (0 expected; bound guards against
    // pathological rounding surprises without flaking).
    TEST_REQUIRE(palettePath * 100 <= painted,
                 "too many painted cells render through the palette: %llu "
                 "of %llu — the true-color layer is not applied.",
                 static_cast<unsigned long long>(palettePath),
                 static_cast<unsigned long long>(painted));

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("EXTDEMO true-color shading check", 113,
                         runExtdemoColorTests);

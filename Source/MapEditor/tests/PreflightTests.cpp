// ============================================================================
// T0/T1 — game directory preflight check (MapEditorCore/preflight).
// ============================================================================

#include "TestHarness.h"

#include "preflight.h"

namespace {

bool runPreflightTests()
{
    const std::string gameDir = MapEditor::Tests::context().gameDir.u8string();

    const MapEditor::PreflightResult result =
        MapEditor::preflightCheck(gameDir);
    MapEditor::printPreflightResult(result, gameDir);

    TEST_REQUIRE(result.ok, "Preflight failed - cannot continue.");

    std::printf("[PASS] Preflight check passed.\n");
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Preflight check", 10, runPreflightTests);

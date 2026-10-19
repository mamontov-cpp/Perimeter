// ============================================================================
// Map editor headless test runner.
//
// Usage:
//   map_editor_tests [game_dir]
//
// game_dir is the folder that contains RESOURCE/ (defaults to the current
// directory, i.e. the build output folder where this executable is deployed).
//
// The individual tests live next to this file and register themselves with the
// harness (see TestHarness.h). They run in a fixed order because the later
// WorldFiles tests reuse the world that the round-trip test loaded and saved:
//
//   10  PreflightTests          game directory check
//   20  WorldRoundTripTests     Terra load/save, output.vmp and world.ini
//   30  WorldFilesIniTests      world.ini read/edit/write
//   40  WorldFilesDataTests     palettes, hardness, XML, leveled texture
//
// Exit codes:
//  0 — all checks passed
//  1 — failure
// ============================================================================

#include "TestHarness.h"

#include <cstdio>
#include <string>

int main(int argc, char* argv[])
{
    std::printf("Map Editor Round-Trip Test\n");
    std::printf("==========================\n");

    std::string gameDir = ".";
    if (argc > 1) {
        gameDir = argv[1];
    }

    const int result = MapEditor::Tests::runAll(gameDir);

    std::printf("\nTest %s\n", result == 0 ? "PASSED" : "FAILED");
    return result;
}

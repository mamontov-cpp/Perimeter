// ============================================================================
// T5.1 — WorldParams (world parameters read/write) and the output.vmp
// compression converter.
//
//   55  read WorldParams for the saved temp world and verify it against the
//       world.ini contents; write modified parameters into a scratch copy and
//       verify the round-trip;
//   56  output.vmp S2T0 -> S2T1 -> S2T0 conversion round-trip (byte-exact);
//   57  EditorSession::applyWorldSettings / currentWorldParams / saveWorld
//       integration: hZeroPlast live-apply + persistence, generation powers
//       into world.ini, S2T1 re-applied on save.
//
// Runs after the SaveWorld tests (order 50) and reuses their temporary world;
// every write happens in a scratch copy, never in the real game folder.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "WorldFiles.h"
#include "WorldParams.h"
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
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

fs::path scratchDir(const MapEditor::Tests::TestContext& ctx)
{
    return ctx.tempRoot / "worldparams";
}

/// Content root with the temp world plus the shared resources the Terra
/// loader needs (same recipe as SaveWorldTests, rebuilt independently).
bool buildLoadRoot(const fs::path& gameDir, const fs::path& savedWorldDir,
                   const std::string& worldName, const fs::path& loadRoot)
{
    std::error_code ec;
    fs::remove_all(loadRoot, ec);
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(gameDir / "RESOURCE" / "GeoTx",
                                             loadRoot / "RESOURCE" / "GeoTx");
    if (fs::exists(gameDir / "Perimeter.ini")) {
        fs::copy_file(gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return false;
        }
    }
    MapEditor::Tests::copyDirectoryRecursive(
        savedWorldDir, loadRoot / "RESOURCE" / "Worlds" / worldName);
    return true;
}

std::string iniValue(const fs::path& iniPath, const std::string& section,
                     const std::string& key)
{
    const auto values = MapEditor::Tests::parseIni(iniPath);
    const auto it = values.find(section + '\x1f' + key);
    return it == values.end() ? std::string() : it->second;
}

} // namespace

// ---------------------------------------------------------------------------
// 55 — WorldParams read/write round-trip
// ---------------------------------------------------------------------------

bool runWorldParamsTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempWorldDir.empty() && fs::exists(ctx.tempWorldDir),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(ctx.mapWidth > 0 && ctx.mapHeight > 0,
                 "WorldRoundTripTests did not record the map size.");

    const fs::path scratch = scratchDir(ctx);
    std::error_code ec;
    fs::create_directories(scratch, ec);
    TEST_REQUIRE(!ec, "cannot create scratch dir '%s'.", scratch.u8string().c_str());

    // --- read --------------------------------------------------------------
    const MapEditor::WorldFiles worldFiles(ctx.tempWorldDir);
    const std::optional<MapEditor::WorldParams> params =
        MapEditor::WorldParams::readFromWorld(worldFiles);
    TEST_REQUIRE(params.has_value(),
                 "WorldParams::readFromWorld() failed for '%s'.",
                 ctx.tempWorldDir.u8string().c_str());

    TEST_REQUIRE(params->mapWidth() == ctx.mapWidth &&
                     params->mapHeight() == ctx.mapHeight,
                 "map powers do not match the map size (%d x %d).",
                 params->mapWidth(), params->mapHeight());
    TEST_REQUIRE(params->hZeroPlast ==
                     std::atoi(iniValue(ctx.tempWorldDir / "world.ini",
                                        "Global Parameters", "hZeroPlast")
                                   .c_str()),
                 "hZeroPlast does not match world.ini.");
    TEST_REQUIRE(params->geoNetPower ==
                     std::atoi(iniValue(ctx.tempWorldDir / "world.ini",
                                        "Global Parameters", "GeoNet Power")
                                   .c_str()),
                 "GeoNet Power does not match world.ini.");
    TEST_REQUIRE(params->f3dFunction ==
                     std::atoi(iniValue(ctx.tempWorldDir / "world.ini",
                                        "3DF Parameters", "Function")
                                   .c_str()),
                 "3DF Function does not match world.ini.");
    TEST_REQUIRE(params->compressedFormat == 0,
                 "expected S2T0 for the freshly saved temp world, got %d.",
                 params->compressedFormat);

    // --- write into a scratch copy ----------------------------------------
    const fs::path copyDir = scratch / "world_copy";
    MapEditor::Tests::copyDirectoryRecursive(ctx.tempWorldDir, copyDir);
    const MapEditor::WorldFiles copyFiles(copyDir);

    MapEditor::WorldParams modified = *params;
    modified.hZeroPlast = (modified.hZeroPlast + 1) % 256;
    modified.geoNetPower = 5;
    modified.sectionSizePower = 6;
    modified.minimalSquarePower = 3;
    modified.f3dKmx = 300;
    modified.f3dKmy = 301;
    modified.f3dKmz = 302;
    modified.f3dFunction = 1;
    modified.compressedFormat = 1; // documentation key only in this test

    TEST_REQUIRE(modified.writeToIni(copyFiles),
                 "WorldParams::writeToIni() failed.");

    const std::optional<MapEditor::WorldParams> reread =
        MapEditor::WorldParams::readFromWorld(copyFiles);
    TEST_REQUIRE(reread.has_value(), "re-reading the modified world failed.");
    TEST_REQUIRE(reread->hZeroPlast == modified.hZeroPlast &&
                     reread->geoNetPower == modified.geoNetPower &&
                     reread->sectionSizePower == modified.sectionSizePower &&
                     reread->minimalSquarePower == modified.minimalSquarePower,
                 "generation parameters did not round-trip.");
    TEST_REQUIRE(reread->f3dKmx == 300 && reread->f3dKmy == 301 &&
                     reread->f3dKmz == 302 &&
                     reread->f3dFunction == modified.f3dFunction,
                 "3DF parameters did not round-trip.");
    TEST_REQUIRE(iniValue(copyDir / "world.ini", "Storage",
                          "Compressed Format Using") == "1",
                 "storage documentation key was not updated.");

    return true;
}

// ---------------------------------------------------------------------------
// 56 — output.vmp compression round-trip
// ---------------------------------------------------------------------------

bool runVmpCompressionTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempWorldDir.empty() && fs::exists(ctx.tempWorldDir),
                 "WorldRoundTripTests did not provide the temporary world.");

    const fs::path scratch = scratchDir(ctx);
    std::error_code ec;
    fs::create_directories(scratch, ec);

    const fs::path original = scratch / "output_original.vmp";
    fs::copy_file(ctx.tempWorldDir / "output.vmp", original,
                  fs::copy_options::overwrite_existing, ec);
    TEST_REQUIRE(!ec, "cannot copy output.vmp into the scratch dir.");

    const std::vector<std::uint8_t> originalBytes =
        MapEditor::Tests::readFileBytes(original);
    TEST_REQUIRE(originalBytes.size() > sizeof(vrtMap::sVmpHeader),
                 "output.vmp copy is too small.");
    TEST_REQUIRE(MapEditor::vmpCompressionOfFile(original) == 0,
                 "expected S2T0 for the freshly saved world.");

    // Compress.
    const fs::path compressed = scratch / "output_work.vmp";
    fs::copy_file(original, compressed, fs::copy_options::overwrite_existing, ec);
    TEST_REQUIRE(MapEditor::convertVmpCompression(compressed, 1),
                 "convertVmpCompression(S2T1) failed.");
    TEST_REQUIRE(MapEditor::vmpCompressionOfFile(compressed) == 1,
                 "header id is not S2T1 after compression.");
    const std::vector<std::uint8_t> packedBytes =
        MapEditor::Tests::readFileBytes(compressed);
    TEST_REQUIRE(packedBytes.size() < originalBytes.size(),
                 "S2T1 payload is not smaller than S2T0 (%zu vs %zu).",
                 packedBytes.size(), originalBytes.size());

    // Idempotent when already in the requested mode.
    TEST_REQUIRE(MapEditor::convertVmpCompression(compressed, 1),
                 "converting an already-compressed file must be a no-op.");
    TEST_REQUIRE(MapEditor::Tests::readFileBytes(compressed) == packedBytes,
                 "no-op conversion changed the file.");

    // And back — byte-exact.
    TEST_REQUIRE(MapEditor::convertVmpCompression(compressed, 0),
                 "convertVmpCompression(S2T0) failed.");
    TEST_REQUIRE(MapEditor::vmpCompressionOfFile(compressed) == 0,
                 "header id is not S2T0 after decompression.");
    TEST_REQUIRE(MapEditor::Tests::readFileBytes(compressed) == originalBytes,
                 "S2T1 -> S2T0 round-trip is not byte-exact.");

    // A missing file reports "unknown" and conversion fails cleanly.
    TEST_REQUIRE(MapEditor::vmpCompressionOfFile(scratch / "missing.vmp") == -1,
                 "missing file must report compression -1.");
    TEST_REQUIRE(!MapEditor::convertVmpCompression(scratch / "missing.vmp", 1),
                 "conversion of a missing file must fail.");

    return true;
}

// ---------------------------------------------------------------------------
// 57 — EditorSession::applyWorldSettings integration
// ---------------------------------------------------------------------------

bool runWorldSettingsApplyTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempWorldDir.empty() && !ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the temporary world.");

    const fs::path loadRoot =
        ctx.tempRoot / "worldparams" / "load_root";
    TEST_REQUIRE(buildLoadRoot(ctx.gameDir, ctx.tempWorldDir, ctx.worldName,
                               loadRoot),
                 "could not build the load root at '%s'.",
                 loadRoot.u8string().c_str());

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "setGameDir() failed for the load root.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "no world in the load root.");
    TEST_REQUIRE(session.loadWorld(0), "loading the temp world failed.");

    const std::optional<MapEditor::WorldParams> before =
        session.currentWorldParams();
    TEST_REQUIRE(before.has_value(), "currentWorldParams() failed.");

    // Change everything the dialog can change.
    MapEditor::WorldParams changed = *before;
    changed.hZeroPlast = (before->hZeroPlast + 5) % 256;
    changed.geoNetPower = before->geoNetPower == 5 ? 6 : 5;
    changed.sectionSizePower = before->sectionSizePower == 6 ? 7 : 6;
    changed.minimalSquarePower = before->minimalSquarePower == 3 ? 4 : 3;
    changed.f3dKmx = before->f3dKmx + 11;
    changed.f3dKmy = before->f3dKmy + 12;
    changed.f3dKmz = before->f3dKmz + 13;
    changed.f3dFunction = before->f3dFunction == 0 ? 1 : 0;
    changed.compressedFormat = before->compressedFormat == 0 ? 1 : 0;

    TEST_REQUIRE(session.applyWorldSettings(changed),
                 "applyWorldSettings() failed.");

    // hZeroPlast is live in vMap and marks the world modified.
    TEST_REQUIRE(vMap.hZeroPlast == changed.hZeroPlast,
                 "vMap.hZeroPlast is %d, expected %d.",
                 static_cast<int>(vMap.hZeroPlast), changed.hZeroPlast);
    TEST_REQUIRE(session.isModified(),
                 "hZeroPlast change must mark the world modified.");
    TEST_REQUIRE(session.vmpCompressionTarget() == changed.compressedFormat,
                 "compression target was not stored in the session.");

    // f3d is live.
    TEST_REQUIRE(f3d.kmx == changed.f3dKmx && f3d.kmy == changed.f3dKmy &&
                     f3d.kmz == changed.f3dKmz,
                 "f3d scaling was not applied live.");
    TEST_REQUIRE(f3d.currentFunction == changed.f3dFunction,
                 "f3d function was not applied live.");

    // Save — hZeroPlast, the storage key and the S2T1 format must persist.
    TEST_REQUIRE(session.saveWorld(), "saveWorld() failed after apply.");

    const std::string hzp = MapEditor::Tests::trim(
        iniValue(loadRoot / "RESOURCE" / "Worlds" / ctx.worldName / "world.ini",
                 "Global Parameters", "hZeroPlast"));
    TEST_REQUIRE(hzp == std::to_string(changed.hZeroPlast),
                 "world.ini hZeroPlast is '%s', expected %d.", hzp.c_str(),
                 changed.hZeroPlast);

    MapEditor::WorldFiles savedFiles(loadRoot / "RESOURCE" / "Worlds" /
                                     ctx.worldName);
    TEST_REQUIRE(MapEditor::vmpCompressionOfFile(
                     savedFiles.filePath(MapEditor::WorldFiles::OutputVmpFile)) ==
                     changed.compressedFormat,
                 "saved output.vmp is not in the requested compression mode.");
    TEST_REQUIRE(iniValue(loadRoot / "RESOURCE" / "Worlds" / ctx.worldName /
                              "world.ini",
                          "Storage", "Compressed Format Using") ==
                     std::to_string(changed.compressedFormat),
                 "storage key does not match the saved format.");

    // After save the world is no longer modified.
    TEST_REQUIRE(!session.hasUnsavedChanges(),
                 "world still reports unsaved changes after save.");

    return true;
}

MAP_EDITOR_REGISTER_TEST("World params read/write", 55, runWorldParamsTests);
MAP_EDITOR_REGISTER_TEST("VMP compression round-trip", 56, runVmpCompressionTests);
MAP_EDITOR_REGISTER_TEST("World settings apply", 57, runWorldSettingsApplyTests);

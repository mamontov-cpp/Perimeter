// ============================================================================
// T5.2 — createNewWorld (new world from a template).
//
//   58  flat mode: 64x64 world created from the temp world as template —
//       files exist, world.ini carries the wizard values, the session loads
//       the new world with the requested size/hZeroPlast and a fully flat
//       geo layer at the requested height, and the initial save produced
//       cache.tga/map.tga and an S2T0 output.vmp;
//   59  generated mode: same creation with buildWorld() terrain — the world
//       loads, has the requested size and its output.vmp was saved.
//
// Everything happens inside a dedicated content root built from the temp
// world; the game folder is never touched.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "WorldCreation.h"
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
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

/// Fresh content root: the temp world (template donor) + shared resources.
bool buildCreationRoot(const fs::path& gameDir, const fs::path& templateDir,
                       const std::string& worldName, const fs::path& root)
{
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(gameDir / "RESOURCE" / "GeoTx",
                                             root / "RESOURCE" / "GeoTx");
    if (fs::exists(gameDir / "Perimeter.ini")) {
        fs::copy_file(gameDir / "Perimeter.ini", root / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return false;
        }
    }
    MapEditor::Tests::copyDirectoryRecursive(
        templateDir, root / "RESOURCE" / "Worlds" / worldName);
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

bool runWorldCreationFlatTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempWorldDir.empty() && !ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the temporary world.");

    const fs::path root = ctx.tempRoot / "worldcreation_flat";
    TEST_REQUIRE(buildCreationRoot(ctx.gameDir, ctx.tempWorldDir, ctx.worldName,
                                   root),
                 "could not build the creation root.");

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(root.u8string()) && session.scanWorlds(),
                 "session setup failed in the creation root.");

    MapEditor::NewWorldParams params;
    params.name = "TESTFLAT";
    params.mapPowerX = 6; // 64 x 64 — small and fast
    params.mapPowerY = 6;
    params.hZeroPlast = 40;
    params.geoLayerHeight = 25;
    params.geoNetPower = 6;
    params.sectionSizePower = 7;
    params.minimalSquarePower = 4;
    params.flatSurface = true;

    const MapEditor::WorldCreateResult result =
        MapEditor::createNewWorld(session, params, ctx.worldName);
    TEST_REQUIRE(result.ok, "createNewWorld (flat) failed: %s",
                 result.error.c_str());

    // The session holds the new world with the requested geometry.
    TEST_REQUIRE(session.isWorldLoaded() &&
                     session.currentWorldName() == "TESTFLAT",
                 "session did not switch to the new world.");
    TEST_REQUIRE(vMap.H_SIZE == 64 && vMap.V_SIZE == 64,
                 "map size is %u x %u, expected 64 x 64.",
                 vMap.H_SIZE, vMap.V_SIZE);
    TEST_REQUIRE(static_cast<int>(vMap.hZeroPlast) == params.hZeroPlast,
                 "hZeroPlast is %d, expected %d.",
                 static_cast<int>(vMap.hZeroPlast), params.hZeroPlast);

    // Flat geo layer at the requested height, no dams.
    bool flat = true;
    for (int off = 0; off < 64 * 64; ++off) {
        if (vMap.VxGBuf[off] != params.geoLayerHeight || vMap.VxDBuf[off] != 0) {
            flat = false;
            break;
        }
    }
    TEST_REQUIRE(flat, "the geo layer is not flat at the requested height.");

    // Files of the new world.
    const fs::path newDir = root / "RESOURCE" / "Worlds" / "TESTFLAT";
    const MapEditor::WorldFiles files(newDir);
    for (const char* fileName : {MapEditor::WorldFiles::IniFileName,
                                 MapEditor::WorldFiles::OutputVmpFile,
                                 MapEditor::WorldFiles::GeoPalFile,
                                 MapEditor::WorldFiles::DamPalFile,
                                 MapEditor::WorldFiles::GeoPalXmlFile,
                                 MapEditor::WorldFiles::GeoTxXmlFile,
                                 MapEditor::WorldFiles::LeveledTextureFile,
                                 MapEditor::WorldFiles::CacheTgaFile,
                                 MapEditor::WorldFiles::MapTgaFile}) {
        TEST_REQUIRE(files.exists(fileName),
                     "the new world is missing %s.", fileName);
    }
    TEST_REQUIRE(iniValue(newDir / "world.ini", "Global Parameters",
                          "hZeroPlast") == "40",
                 "world.ini hZeroPlast was not written.");
    TEST_REQUIRE(iniValue(newDir / "world.ini", "Global Parameters",
                          "Map Power X") == "6",
                 "world.ini Map Power X was not written.");
    TEST_REQUIRE(MapEditor::vmpCompressionOfFile(
                     files.filePath(MapEditor::WorldFiles::OutputVmpFile)) == 0,
                 "the flat output.vmp is not S2T0.");

    // The world list contains both worlds now.
    TEST_REQUIRE(session.worldCount() == 2,
                 "expected 2 worlds after creation, got %zu.",
                 session.worldCount());

    // A second creation with the same name must fail cleanly.
    const MapEditor::WorldCreateResult again =
        MapEditor::createNewWorld(session, params, ctx.worldName);
    TEST_REQUIRE(!again.ok, "duplicate creation must fail.");
    TEST_REQUIRE(!MapEditor::isValidWorldName("bad/name") &&
                     !MapEditor::isValidWorldName("") &&
                     MapEditor::isValidWorldName("Ok_World-2"),
                 "isValidWorldName() rejects/accepts the wrong names.");

    return true;
}

bool runWorldCreationGeneratedTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path root = ctx.tempRoot / "worldcreation_gen";
    TEST_REQUIRE(buildCreationRoot(ctx.gameDir, ctx.tempWorldDir, ctx.worldName,
                                   root),
                 "could not build the creation root.");

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(root.u8string()) && session.scanWorlds(),
                 "session setup failed in the creation root.");

    MapEditor::NewWorldParams params;
    params.name = "TESTGEN";
    params.mapPowerX = 6;
    params.mapPowerY = 6;
    params.hZeroPlast = 40;
    params.geoLayerHeight = 25; // unused in generated mode
    params.flatSurface = false; // Terra buildWorld() terrain

    const MapEditor::WorldCreateResult result =
        MapEditor::createNewWorld(session, params, ctx.worldName);
    TEST_REQUIRE(result.ok, "createNewWorld (generated) failed: %s",
                 result.error.c_str());

    TEST_REQUIRE(session.isWorldLoaded() &&
                     session.currentWorldName() == "TESTGEN",
                 "session did not switch to the generated world.");
    TEST_REQUIRE(vMap.H_SIZE == 64 && vMap.V_SIZE == 64,
                 "generated map size is %u x %u, expected 64 x 64.",
                 vMap.H_SIZE, vMap.V_SIZE);

    const fs::path newDir = root / "RESOURCE" / "Worlds" / "TESTGEN";
    const MapEditor::WorldFiles files(newDir);
    TEST_REQUIRE(files.exists(MapEditor::WorldFiles::OutputVmpFile) &&
                     files.exists(MapEditor::WorldFiles::CacheTgaFile),
                 "the generated world is missing output.vmp/cache.tga.");

    return true;
}

MAP_EDITOR_REGISTER_TEST("World creation flat", 58, runWorldCreationFlatTests);
MAP_EDITOR_REGISTER_TEST("World creation generated", 59,
                         runWorldCreationGeneratedTests);

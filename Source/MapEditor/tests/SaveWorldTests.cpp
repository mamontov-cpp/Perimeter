// ============================================================================
// T1.3 — EditorSession::saveWorld().
//
// Exercises the full save pipeline on a temporary copy of a game world:
//   * Terra's save3Buf() (output.vmp, inGeo.act / inDam.act, [3DF Parameters]),
//   * saveHardness() (hardness.bin),
//   * world.ini refresh ([Global Parameters] Map Power/hZeroPlast, Version),
//   * image generation (cache.tga = 24-bit H_SIZE x V_SIZE, map.tga = minimap).
//
// The session is pointed at the temporary content root, so the original game
// world is never touched. The test then verifies that the saved world is
// complete and reloadable: all mandatory files exist, the generated images have
// the right format and size, and world.ini round-trips semantically.
//
// Runs after the WorldFiles tests (order 50) and reuses their temporary world.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

// EditorSession (MapEditorCore) — the class under test.
#include "EditorSession.h"
#include "WorldFiles.h"
// errh_wrapper.h (MapEditorSupport) — ErrH setup, already done by the
// round-trip test but repeated here so this file is self-contained.
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

/// Path of the temp content root the save test loads from. Building a full
/// content root (shared resources + the saved world) lets EditorSession reload
/// the world exactly as the game would, without touching the real game folder.
fs::path loadRootPath(const fs::path& tempRoot)
{
    return tempRoot / "load_root";
}

/// Create a content root that the Terra loader can open: the game's shared
/// resources (the world's geoTx.xml references Resource\GeoTx\*.tga) plus the
/// saved world copy. Everything is copied so the game folder stays read-only.
bool buildLoadRoot(const fs::path& gameDir, const fs::path& savedWorldDir,
                   const fs::path& worldName, const fs::path& loadRoot)
{
    std::error_code ec;
    fs::remove_all(loadRoot, ec);
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }

    // Shared, world-independent resources referenced by the world's XML.
    MapEditor::Tests::copyDirectoryRecursive(
        gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");

    // Runtime configuration read by vrtMap::analyzeINI() (Color ZeroLayer).
    fs::copy_file(gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);

    // The world itself, exactly as saveWorld() wrote it. The folder name is the
    // world name (the Terra key is the full relative path, not a folder name).
    MapEditor::Tests::copyDirectoryRecursive(
        savedWorldDir, loadRoot / "RESOURCE" / "Worlds" / worldName);
    return true;
}

/// 18-byte TGA header fields that the image checks care about.
struct TgaHeader
{
    std::uint8_t imageType = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t pixelDepth = 0;
    std::size_t fileSize = 0;

    std::size_t expectedPixelBytes() const
    {
        return static_cast<std::size_t>(width) * height * (pixelDepth / 8);
    }
};

bool readTgaHeader(const fs::path& path, TgaHeader& header)
{
    const std::vector<std::uint8_t> bytes = MapEditor::Tests::readFileBytes(path);
    if (bytes.size() < 18) {
        return false;
    }
    header.imageType = bytes[2];
    header.width = static_cast<std::uint16_t>(bytes[12] | (bytes[13] << 8));
    header.height = static_cast<std::uint16_t>(bytes[14] | (bytes[15] << 8));
    header.pixelDepth = bytes[16];
    header.fileSize = bytes.size();
    return true;
}

bool runSaveWorldTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldKey.empty(),
                 "WorldRoundTripTests did not provide the world key.");

    MapEditor::setupErrHWrapper();

    // Build a self-contained content root: the saved world plus the shared
    // resources the Terra loader needs. The session is pointed at it so the
    // real game folder is never touched.
    const fs::path loadRoot = loadRootPath(ctx.tempRoot);
    TEST_REQUIRE(buildLoadRoot(ctx.gameDir, ctx.tempWorldDir, fs::u8path(ctx.worldName),
                               loadRoot),
                 "could not build the load content root at '%s'.",
                 loadRoot.u8string().c_str());

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed for '%s'.",
                 loadRoot.u8string().c_str());
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world in the temp root.");

    TEST_REQUIRE(session.loadWorld(0),
                 "EditorSession::loadWorld(0) failed.");
    TEST_REQUIRE(session.isWorldLoaded(), "session reports no loaded world.");

    const fs::path worldDir = session.currentWorldFiles()->worldDir();
    std::printf("  Saving world '%s' in %s\n",
                session.currentWorldName().c_str(),
                worldDir.u8string().c_str());

    // ------------------------------------------------------------------
    // 9.1: saveWorld() runs the whole pipeline and clears the dirty flag.
    // ------------------------------------------------------------------
    session.setModified(true);
    TEST_REQUIRE(session.isModified(),
                 "isModified() must be true after setModified(true).");
    TEST_REQUIRE(session.saveWorld(), "EditorSession::saveWorld() failed.");
    TEST_REQUIRE(!session.isModified(),
                 "the world is still marked modified after a successful save.");
    std::printf("[PASS] saveWorld() completed and cleared the modified flag.\n");

    // ------------------------------------------------------------------
    // 9.2: every file the game needs is present after the save.
    // ------------------------------------------------------------------
    {
        const MapEditor::WorldFiles files(worldDir);
        const char* mandatory[] = {
            "output.vmp", "world.ini", "inGeo.act", "inDam.act",
            "cache.tga", "map.tga",
        };
        for (const char* name : mandatory) {
            TEST_REQUIRE(files.exists(name),
                         "mandatory file '%s' is missing after saveWorld().",
                         name);
        }
        TEST_REQUIRE(files.exists("hardness.bin"),
                     "hardness.bin is missing after saveWorld().");
        std::printf("[PASS] output.vmp, world.ini, palettes, hardness and "
                    "images are all present.\n");
    }

    // ------------------------------------------------------------------
    // 9.3: generated images have the format and size the game expects.
    // ------------------------------------------------------------------
    {
        TgaHeader cache;
        TEST_REQUIRE(readTgaHeader(worldDir / "cache.tga", cache),
                     "cache.tga is missing or truncated.");
        TEST_REQUIRE(cache.imageType == 2 && cache.pixelDepth == 24,
                     "cache.tga must be an uncompressed 24-bit TGA "
                     "(type=%u, depth=%u).",
                     cache.imageType, cache.pixelDepth);
        TEST_REQUIRE(cache.width == ctx.mapWidth && cache.height == ctx.mapHeight,
                     "cache.tga is %ux%u, expected %dx%d.",
                     cache.width, cache.height, ctx.mapWidth, ctx.mapHeight);
        TEST_REQUIRE(cache.fileSize >= 18 + cache.expectedPixelBytes(),
                     "cache.tga payload is truncated (%zu bytes).",
                     cache.fileSize);

        TgaHeader minimap;
        TEST_REQUIRE(readTgaHeader(worldDir / "map.tga", minimap),
                     "map.tga is missing or truncated.");
        TEST_REQUIRE(minimap.imageType == 2 && minimap.pixelDepth == 24,
                     "map.tga must be an uncompressed 24-bit TGA "
                     "(type=%u, depth=%u).",
                     minimap.imageType, minimap.pixelDepth);
        TEST_REQUIRE(minimap.width > 0 && minimap.height > 0,
                     "map.tga has a zero size.");
        TEST_REQUIRE(minimap.width <= static_cast<std::uint16_t>(ctx.mapWidth) &&
                         minimap.height <= static_cast<std::uint16_t>(ctx.mapHeight),
                     "map.tga (%ux%u) is larger than the map (%dx%d).",
                     minimap.width, minimap.height, ctx.mapWidth, ctx.mapHeight);
        std::printf("[PASS] cache.tga is %ux%u, map.tga is %ux%u, both 24-bit "
                    "TGA.\n", cache.width, cache.height, minimap.width,
                    minimap.height);
    }

    // ------------------------------------------------------------------
    // 9.4: world.ini is refreshed and stays semantically valid.
    // ------------------------------------------------------------------
    {
        MapEditor::WorldFiles files(worldDir);
        const auto power = files.readMapPower();
        TEST_REQUIRE(power.has_value(),
                     "world.ini lost [Global Parameters] Map Power after save.");
        TEST_REQUIRE((1 << power->first) == ctx.mapWidth &&
                         (1 << power->second) == ctx.mapHeight,
                     "world.ini map power (%d,%d) no longer matches the map.",
                     power->first, power->second);

        const auto hZeroPlast = files.readHZeroPlast();
        TEST_REQUIRE(hZeroPlast.has_value() && *hZeroPlast >= 0 && *hZeroPlast <= 255,
                     "[Global Parameters] hZeroPlast is missing or out of range "
                     "after save.");

        const auto version = files.readStorageVersion();
        TEST_REQUIRE(version.has_value() && !version->empty(),
                     "[Storage] Version is missing after save.");
        TEST_REQUIRE(*version == "1.4",
                     "[Storage] Version is '%s', expected '1.4'.",
                     version->c_str());
        std::printf("[PASS] world.ini refreshed (power %d/%d, hZeroPlast=%d, "
                    "version=%s).\n", power->first, power->second, *hZeroPlast,
                    version->c_str());
    }

    // ------------------------------------------------------------------
    // 9.5: the saved world reloads through the Terra API.
    // ------------------------------------------------------------------
    {
        session.unloadWorld();
        TEST_REQUIRE(!session.isWorldLoaded(), "unloadWorld() failed.");

        MapEditor::EditorSession reloaded;
        TEST_REQUIRE(reloaded.setGameDir(loadRoot.u8string()),
                     "could not point a fresh session at the saved world.");
        TEST_REQUIRE(reloaded.scanWorlds() && reloaded.worldCount() >= 1,
                     "the saved world is no longer scannable.");
        TEST_REQUIRE(reloaded.loadWorld(0),
                     "the saved world failed to reload through Terra.");
        std::printf("[PASS] the saved world reloads through EditorSession.\n");
    }

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Save world", 50, runSaveWorldTests);

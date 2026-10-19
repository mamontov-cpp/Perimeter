// ============================================================================
// T1.2 — WorldFiles: palettes, hardness, XML and leveled texture.
//
// Verifies byte-identical round-trips of inGeo.act / inDam.act, hardness.bin,
// geoPal.xml / geoTx.xml and leveledSurfaceTexture.tga, plus the validation
// that rejects malformed hardness and TGA payloads.
//
// Runs after WorldRoundTripTests, which provides the source and temporary
// world folders and the map size.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

// WorldFiles (MapEditorCore) — file-level world folder access.
#include "WorldFiles.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool runWorldFilesDataTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    const fs::path& originalWorldDir = ctx.originalWorldDir;
    const int mapWidth = ctx.mapWidth;
    const int mapHeight = ctx.mapHeight;

    TEST_REQUIRE(!originalWorldDir.empty() &&
                     fs::exists(originalWorldDir / "inGeo.act"),
                 "WorldRoundTripTests did not provide the source world.");
    TEST_REQUIRE(mapWidth > 0 && mapHeight > 0,
                 "WorldRoundTripTests did not provide the map size.");

    const fs::path probeDir = ctx.tempRoot / "worldfiles_probe";
    fs::remove_all(probeDir);
    fs::create_directories(probeDir);
    MapEditor::WorldFiles probe(probeDir);
    MapEditor::WorldFiles worldFiles(ctx.tempWorldDir);

    // ------------------------------------------------------------------
    // 8.1: inGeo.act / inDam.act (768-byte BGR palettes).
    // ------------------------------------------------------------------
    {
        MapEditor::GeoDamPalettes palettes;
        TEST_REQUIRE(worldFiles.readPalettes(palettes),
                     "WorldFiles::readPalettes() failed.");

        const std::vector<std::uint8_t> geoOriginal =
            MapEditor::Tests::readFileBytes(originalWorldDir / "inGeo.act");
        const std::vector<std::uint8_t> damOriginal =
            MapEditor::Tests::readFileBytes(originalWorldDir / "inDam.act");
        TEST_REQUIRE(geoOriginal.size() == MapEditor::GeoDamPalettes::SizeBytes &&
                         damOriginal.size() == MapEditor::GeoDamPalettes::SizeBytes,
                     "source palettes are not 768 bytes long.");
        TEST_REQUIRE(
            std::vector<std::uint8_t>(palettes.geo.begin(), palettes.geo.end()) ==
                geoOriginal &&
                std::vector<std::uint8_t>(palettes.dam.begin(), palettes.dam.end()) ==
                    damOriginal,
            "read palettes differ from inGeo.act / inDam.act.");

        TEST_REQUIRE(probe.writePalettes(palettes),
                     "WorldFiles::writePalettes() failed.");
        TEST_REQUIRE(
            MapEditor::Tests::readFileBytes(probeDir / "inGeo.act") == geoOriginal &&
                MapEditor::Tests::readFileBytes(probeDir / "inDam.act") ==
                    damOriginal,
            "written palettes are not byte-identical.");
        std::printf("[PASS] inGeo.act / inDam.act round-trip byte-identically "
                    "(768 bytes each).\n");
    }

    // ------------------------------------------------------------------
    // 8.2: hardness.bin (uint16 width/height + packed 2-bit values).
    // ------------------------------------------------------------------
    {
        MapEditor::HardnessData hardness;
        TEST_REQUIRE(worldFiles.readHardness(hardness),
                     "WorldFiles::readHardness() failed.");

        const auto expectedDims =
            MapEditor::HardnessData::dimensionsForMap(mapWidth, mapHeight);
        TEST_REQUIRE(hardness.width == expectedDims.first &&
                         hardness.height == expectedDims.second,
                     "hardness grid is %ux%u, expected %ux%u for a %dx%d map.",
                     static_cast<unsigned>(hardness.width),
                     static_cast<unsigned>(hardness.height),
                     static_cast<unsigned>(expectedDims.first),
                     static_cast<unsigned>(expectedDims.second),
                     mapWidth, mapHeight);

        const std::vector<std::uint8_t> hardnessOriginal =
            MapEditor::Tests::readFileBytes(originalWorldDir / "hardness.bin");
        TEST_REQUIRE(!hardnessOriginal.empty(),
                     "source hardness.bin is missing.");

        // A mismatched payload must be rejected before anything is written.
        MapEditor::HardnessData broken = hardness;
        broken.packed.pop_back();
        TEST_REQUIRE(!probe.writeHardness(broken),
                     "writeHardness() accepted a truncated payload.");

        TEST_REQUIRE(probe.writeHardness(hardness),
                     "WorldFiles::writeHardness() failed.");
        TEST_REQUIRE(
            MapEditor::Tests::readFileBytes(probeDir / "hardness.bin") ==
                hardnessOriginal,
            "written hardness.bin is not byte-identical.");
        std::printf("[PASS] hardness.bin round-trips (%ux%u grid, %zu bytes).\n",
                    static_cast<unsigned>(hardness.width),
                    static_cast<unsigned>(hardness.height),
                    hardness.packed.size());
    }

    // ------------------------------------------------------------------
    // 8.3: geoPal.xml / geoTx.xml copy and rewrite.
    // ------------------------------------------------------------------
    {
        const fs::path xmlDir = ctx.tempRoot / "worldfiles_xml";
        fs::remove_all(xmlDir);
        fs::create_directories(xmlDir);
        MapEditor::WorldFiles xmlWorld(xmlDir);

        TEST_REQUIRE(!xmlWorld.copyLayerXmlFrom(ctx.tempRoot / "does_not_exist"),
                     "copyLayerXmlFrom() accepted a missing source.");
        TEST_REQUIRE(xmlWorld.copyLayerXmlFrom(originalWorldDir),
                     "WorldFiles::copyLayerXmlFrom() failed.");

        const std::vector<std::uint8_t> geoXmlOriginal =
            MapEditor::Tests::readFileBytes(originalWorldDir / "geoPal.xml");
        const std::vector<std::uint8_t> txXmlOriginal =
            MapEditor::Tests::readFileBytes(originalWorldDir / "geoTx.xml");
        TEST_REQUIRE(!geoXmlOriginal.empty() && !txXmlOriginal.empty(),
                     "source XML files are missing.");
        TEST_REQUIRE(
            MapEditor::Tests::readFileBytes(xmlDir / "geoPal.xml") ==
                    geoXmlOriginal &&
                MapEditor::Tests::readFileBytes(xmlDir / "geoTx.xml") ==
                    txXmlOriginal,
            "copied XML files are not byte-identical.");

        const auto geoXml = xmlWorld.readGeoPalXml();
        TEST_REQUIRE(geoXml && xmlWorld.writeGeoPalXml(*geoXml),
                     "geoPal.xml text read/write failed.");
        TEST_REQUIRE(
            MapEditor::Tests::readFileBytes(xmlDir / "geoPal.xml") ==
                geoXmlOriginal,
            "geoPal.xml changed across a text round-trip.");
        std::printf("[PASS] geoPal.xml / geoTx.xml copy and text round-trip "
                    "byte-identically.\n");
    }

    // ------------------------------------------------------------------
    // 8.4: leveledSurfaceTexture.tga validation and round-trip.
    // ------------------------------------------------------------------
    {
        std::vector<std::uint8_t> texture;
        TEST_REQUIRE(worldFiles.readLeveledTexture(texture),
                     "WorldFiles::readLeveledTexture() failed.");

        const auto header = MapEditor::LeveledTextureHeader::fromBytes(texture);
        TEST_REQUIRE(header.has_value(),
                     "leveledSurfaceTexture.tga has no TGA header.");
        TEST_REQUIRE(header->width <= mapWidth && header->height <= mapHeight &&
                         header->isPowerOfTwoSized(),
                     "unexpected leveled texture header.");

        const auto errors = MapEditor::WorldFiles::validateLeveledTexture(
            texture, mapWidth, mapHeight);
        if (!errors.empty()) {
            std::fprintf(stderr,
                         "[FAIL] a valid leveled texture was rejected:\n");
            for (const auto& error : errors) {
                std::fprintf(stderr, "  - %s\n", error.c_str());
            }
            TEST_REQUIRE(false, "a valid leveled texture was rejected.");
        }

        // A smaller map must still accept the texture.
        TEST_REQUIRE(MapEditor::WorldFiles::validateLeveledTexture(
                         texture, mapWidth / 4, mapHeight / 4).empty(),
                     "a texture was rejected for a smaller map.");

        // A true-color (unpalettized) TGA must be rejected.
        std::vector<std::uint8_t> trueColor(18, 0);
        trueColor[1] = 0;    // no color map
        trueColor[2] = 2;    // uncompressed true-color
        trueColor[12] = 0x00;
        trueColor[13] = 0x01; // width 256
        trueColor[14] = 0x00;
        trueColor[15] = 0x01; // height 256
        trueColor[16] = 24;   // 24 bits per pixel
        TEST_REQUIRE(!MapEditor::WorldFiles::validateLeveledTexture(
                          trueColor, mapWidth, mapHeight).empty(),
                     "a true-color TGA was accepted.");
        TEST_REQUIRE(!probe.writeLeveledTexture(trueColor, mapWidth, mapHeight),
                     "writeLeveledTexture() wrote an invalid texture.");

        const std::vector<std::uint8_t> textureOriginal =
            MapEditor::Tests::readFileBytes(originalWorldDir /
                                            "leveledSurfaceTexture.tga");
        TEST_REQUIRE(!textureOriginal.empty(),
                     "source leveledSurfaceTexture.tga is missing.");
        TEST_REQUIRE(probe.writeLeveledTexture(texture, mapWidth, mapHeight),
                     "WorldFiles::writeLeveledTexture() failed.");
        TEST_REQUIRE(
            MapEditor::Tests::readFileBytes(probeDir /
                                            "leveledSurfaceTexture.tga") ==
                textureOriginal,
            "written leveledSurfaceTexture.tga is not byte-identical.");
        std::printf("[PASS] leveledSurfaceTexture.tga validates (%ux%u) and "
                    "round-trips.\n",
                    static_cast<unsigned>(header->width),
                    static_cast<unsigned>(header->height));
    }

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("WorldFiles: palettes / hardness / xml / texture",
                         40, runWorldFilesDataTests);

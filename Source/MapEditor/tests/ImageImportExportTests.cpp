// ============================================================================
// T6 — image import/export.
//
//   60  export formats: heightmap TGA (8-bit gray, SGetAlt >> 5 per cell),
//       top-view TGA (24-bit, map-sized), up.tga (512x512 24-bit), the
//       indexed-TGA reader round-trip and its rejection of 24-bit files;
//   61  surface import: a solid-color RGB image paints geo cells with the
//       nearest GeoPal index and dam cells with the nearest DamPal index;
//       layer gating (geo only / dam only) skips the other layer; a whole-map
//       undo restores the buffers byte-exact; an indexed TGA writes its
//       indices as-is.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "WorldImages.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

struct TgaHead
{
    std::uint8_t imageType = 0;
    unsigned width = 0;
    unsigned height = 0;
    std::uint8_t depth = 0;
    std::uint8_t descriptor = 0;
    std::size_t payload = 0;
};

bool readTgaHead(const fs::path& path, TgaHead& head)
{
    const auto bytes = MapEditor::Tests::readFileBytes(path);
    if (bytes.size() < 18) {
        return false;
    }
    head.imageType = bytes[2];
    head.width = static_cast<unsigned>(bytes[12] | (bytes[13] << 8));
    head.height = static_cast<unsigned>(bytes[14] | (bytes[15] << 8));
    head.depth = bytes[16];
    head.descriptor = bytes[17];
    head.payload = bytes.size() - 18;
    return true;
}

/// Write a minimal uncompressed indexed TGA (type 1, top-left).
bool writeIndexedTga(const fs::path& path, unsigned width, unsigned height,
                     const std::vector<unsigned char>& indices)
{
    std::vector<unsigned char> bytes(18 + 768 + indices.size(), 0);
    bytes[1] = 1;  // color map present
    bytes[2] = 1;  // uncompressed color-mapped
    bytes[5] = 256 & 0xFF; // map length low
    bytes[6] = 256 >> 8;   // map length high
    bytes[7] = 24;         // map entry depth
    bytes[12] = width & 0xFF;
    bytes[13] = (width >> 8) & 0xFF;
    bytes[14] = height & 0xFF;
    bytes[15] = (height >> 8) & 0xFF;
    bytes[16] = 8;
    bytes[17] = 0x20;
    for (int i = 0; i < 256; ++i) {
        bytes[18 + i * 3 + 0] = static_cast<unsigned char>(i);
        bytes[18 + i * 3 + 1] = static_cast<unsigned char>(255 - i);
        bytes[18 + i * 3 + 2] = static_cast<unsigned char>(i / 2);
    }
    std::copy(indices.begin(), indices.end(), bytes.begin() + 18 + 768);
    return MapEditor::Tests::writeFileBytes(path, bytes);
}

} // namespace

bool runImageExportTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempRoot.empty(), "temp root missing.");
    const fs::path out = ctx.tempRoot / "imageexport";
    std::error_code ec;
    fs::create_directories(out, ec);

    // A world must be loaded for the exports. The session (and thus the
    // loaded world) stays alive for the whole test — SGetAlt is read after
    // the exports.
    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(ctx.gameDir.u8string()) &&
                     session.scanWorlds() && session.worldCount() >= 1,
                 "no world available for the export tests.");
    TEST_REQUIRE(session.loadWorld(0), "world load failed.");
    TEST_REQUIRE(
        MapEditor::WorldImages::exportHeightmapTga(out / "heightmap.tga"),
        "heightmap export failed.");
    TEST_REQUIRE(MapEditor::WorldImages::exportTopViewTga(out / "top.tga"),
                 "top view export failed.");

    const unsigned mapW = vMap.H_SIZE;
    const unsigned mapH = vMap.V_SIZE;

    TgaHead head;
    TEST_REQUIRE(readTgaHead(out / "heightmap.tga", head) &&
                     head.imageType == 3 && head.depth == 8 &&
                     head.width == mapW && head.height == mapH &&
                     head.payload == static_cast<std::size_t>(mapW) * mapH,
                 "heightmap TGA header/payload is wrong.");

    // Spot-check the gray values against SGetAlt >> 5.
    {
        const auto gray = MapEditor::Tests::readFileBytes(out / "heightmap.tga");
        const unsigned char* pixels = gray.data() + 18;
        for (unsigned y = 0; y < mapH; y += mapH / 17 + 1) {
            for (unsigned x = 0; x < mapW; x += mapW / 13 + 1) {
                const unsigned char expected =
                    static_cast<unsigned char>(vMap.SGetAlt(x, y) >> VX_FRACTION);
                TEST_REQUIRE(pixels[static_cast<std::size_t>(y) * mapW + x] ==
                                 expected,
                             "heightmap mismatch at (%u, %u).", x, y);
            }
        }
    }

    TEST_REQUIRE(readTgaHead(out / "top.tga", head) && head.imageType == 2 &&
                     head.depth == 24 && head.width == mapW &&
                     head.height == mapH,
                 "top view TGA header is wrong.");

    // up.tga — fixed 512x512 24-bit.
    {
        std::vector<unsigned char> bgr(512u * 512u * 3, 0);
        for (std::size_t i = 0; i < bgr.size(); i += 3) {
            bgr[i] = 30;
            bgr[i + 1] = 60;
            bgr[i + 2] = 90;
        }
        TEST_REQUIRE(MapEditor::WorldImages::writeUpTga(out, bgr),
                     "writeUpTga failed.");
        TEST_REQUIRE(readTgaHead(out / "up.tga", head) &&
                         head.imageType == 2 && head.depth == 24 &&
                         head.width == 512 && head.height == 512,
                     "up.tga header is wrong.");
        TEST_REQUIRE(!MapEditor::WorldImages::writeUpTga(out, {1, 2, 3}),
                     "writeUpTga must reject a short buffer.");
    }

    // Indexed TGA reader round-trip.
    {
        std::vector<unsigned char> indices(64 * 32);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = static_cast<unsigned char>(i * 7);
        }
        TEST_REQUIRE(writeIndexedTga(out / "pal.tga", 64, 32, indices),
                     "test TGA write failed.");
        unsigned w = 0;
        unsigned h = 0;
        std::vector<unsigned char> read;
        std::vector<unsigned char> palette;
        TEST_REQUIRE(MapEditor::WorldImages::readIndexedTga(
                         out / "pal.tga", w, h, read, palette) &&
                         w == 64 && h == 32 && read == indices,
                     "indexed TGA round-trip failed.");
        TEST_REQUIRE(palette.size() == 768 && palette[0] == 0 &&
                         palette[1] == 255 && palette[2] == 0,
                     "indexed palette re-basing is wrong.");

        // A 24-bit file must be rejected by the indexed reader.
        std::vector<unsigned char> bgr(8 * 8 * 3, 1);
        TEST_REQUIRE(MapEditor::WorldImages::writeTga24(out / "rgb.tga", 8, 8,
                                                        bgr),
                     "writeTga24 failed.");
        TEST_REQUIRE(!MapEditor::WorldImages::readIndexedTga(
                         out / "rgb.tga", w, h, read, palette),
                     "the indexed reader accepted a true-color file.");
    }

    return true;
}

bool runImageImportTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.gameDir.empty(), "game dir missing.");

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(ctx.gameDir.u8string()) &&
                     session.scanWorlds() && session.worldCount() >= 1,
                 "no world available for the import tests.");
    TEST_REQUIRE(session.loadWorld(0), "world load failed.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);

    // Find one geo and one dam cell.
    int geoCell = -1;
    int damCell = -1;
    for (int off = (mapH / 2) * mapW + mapW / 2;
         off < mapW * mapH && (geoCell < 0 || damCell < 0); ++off) {
        if (vMap.VxDBuf[off] != 0) {
            damCell = off;
        } else {
            geoCell = off;
        }
    }
    TEST_REQUIRE(geoCell >= 0, "no geo cell found.");

    // A snapshot of the whole SurBuf for the undo comparison.
    std::vector<unsigned char> before(vMap.SurBuf,
                                      vMap.SurBuf + static_cast<std::size_t>(mapW) * mapH);

    // Solid image: geo pixels = GeoPal[42], dam pixels = DamPal[7].
    auto palColor = [](const unsigned char* pal, int index) {
        std::vector<unsigned char> bgr(3);
        bgr[0] = pal[index * 3 + 2];
        bgr[1] = pal[index * 3 + 1];
        bgr[2] = pal[index * 3 + 0];
        return bgr;
    };

    MapEditor::EditorSession::SurfaceImportOptions options;
    options.geoCells = true;
    options.damCells = true;
    {
        const auto geo = palColor(vMap.GeoPal, 42);
        const auto dam = palColor(vMap.DamPal, 7);
        std::vector<unsigned char> image(static_cast<std::size_t>(mapW) * mapH * 3);
        for (int off = 0; off < mapW * mapH; ++off) {
            const auto& color = vMap.VxDBuf[off] != 0 ? dam : geo;
            image[static_cast<std::size_t>(off) * 3 + 0] = color[0];
            image[static_cast<std::size_t>(off) * 3 + 1] = color[1];
            image[static_cast<std::size_t>(off) * 3 + 2] = color[2];
        }
        TEST_REQUIRE(session.importSurfaceImageRgb(
                         static_cast<unsigned>(mapW), static_cast<unsigned>(mapH),
                         image, options),
                     "importSurfaceImageRgb failed.");
        TEST_REQUIRE(vMap.SurBuf[geoCell] == 42,
                     "geo cell has SurBuf %d, expected 42.",
                     static_cast<int>(vMap.SurBuf[geoCell]));
        if (damCell >= 0) {
            TEST_REQUIRE(vMap.SurBuf[damCell] == 7,
                         "dam cell has SurBuf %d, expected 7.",
                         static_cast<int>(vMap.SurBuf[damCell]));
        }

        // Whole-map undo restores SurBuf byte-exact.
        TEST_REQUIRE(session.undo(), "undo after import failed.");
        TEST_REQUIRE(std::equal(before.begin(), before.end(), vMap.SurBuf),
                     "undo did not restore SurBuf byte-exact.");
        TEST_REQUIRE(session.redo(), "redo after import failed.");
        TEST_REQUIRE(vMap.SurBuf[geoCell] == 42, "redo did not re-apply.");
        session.clearUndoHistory();
    }

    // Layer gating: geo-only leaves dam cells untouched.
    {
        const auto geo = palColor(vMap.GeoPal, 99);
        std::vector<unsigned char> image(static_cast<std::size_t>(mapW) * mapH * 3);
        for (std::size_t i = 0; i < image.size(); i += 3) {
            image[i + 0] = geo[0];
            image[i + 1] = geo[1];
            image[i + 2] = geo[2];
        }
        const unsigned char damBefore =
            damCell >= 0 ? vMap.SurBuf[damCell] : 0;
        options.geoCells = true;
        options.damCells = false;
        TEST_REQUIRE(session.importSurfaceImageRgb(
                         static_cast<unsigned>(mapW), static_cast<unsigned>(mapH),
                         image, options),
                     "geo-only import failed.");
        if (damCell >= 0) {
            TEST_REQUIRE(vMap.SurBuf[damCell] == damBefore,
                         "geo-only import must not touch dam cells.");
        }
        session.clearUndoHistory();
    }

    // Indexed import: indices go into SurBuf as-is.
    {
        std::vector<unsigned char> indices(static_cast<std::size_t>(mapW) * mapH);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = static_cast<unsigned char>((i * 13) & 0xFF);
        }
        options.geoCells = options.damCells = true;
        TEST_REQUIRE(session.importSurfaceImageIndexed(
                         static_cast<unsigned>(mapW), static_cast<unsigned>(mapH),
                         indices, options),
                     "importSurfaceImageIndexed failed.");
        const int checkOff = (mapH / 3) * mapW + mapW / 3;
        TEST_REQUIRE(vMap.SurBuf[checkOff] ==
                         indices[static_cast<std::size_t>(checkOff)],
                     "indexed import did not write the index as-is.");
        session.clearUndoHistory();
    }

    return true;
}

MAP_EDITOR_REGISTER_TEST("Image export formats", 60, runImageExportTests);
MAP_EDITOR_REGISTER_TEST("Image surface import", 61, runImageImportTests);

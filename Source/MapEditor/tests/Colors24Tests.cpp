// ============================================================================
// True-color (24-bit) surface — SurColBuf, sidecar v2 colors section, XCL0.
//
// Covers the lift of the geo/dam color limit from 256 palette entries to
// per-cell 24-bit RGB (docs/map_logic.md). On an isolated temp copy of the
// round-tripped world:
//   A. An RGB paint (via the ImageDecal brush path) allocates SurColBuf,
//      paints exact colors + keeps the palette index side band; a palette
//      (SetColor) paint clears the override again.
//   B. saveWorld() writes the sidecar with the colors section; a reload
//      restores the colors; getColor32 shades them exactly like the palette
//      tables (convertPal2TableSurCol math).
//   C. vrtMap::saveGameMap/loadGameMap carry the colors in the "XCL0" chunk.
//   D. Undo/redo of the RGB paint restores the color layer.
//
// Runs after Heights16Tests (order 112) and reuses the same temp world.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ImageDecal.h"
#include "WorldFiles.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"
#include "xbuffer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

const int kPatchX = 900;
const int kPatchY = 900;
const int kPatchSize = 8;
const std::uint32_t kTestRgb = 0x123456; // exact 24-bit test color

int patchOffset(int i, int j, int width)
{
    return (kPatchY + j) * width + (kPatchX + i);
}

bool buildLoadRoot(const fs::path& gameDir, const fs::path& savedWorldDir,
                   const fs::path& worldName, const fs::path& loadRoot)
{
    std::error_code ec;
    fs::remove_all(loadRoot, ec);
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(gameDir / "RESOURCE" / "GeoTx",
                                             loadRoot / "RESOURCE" / "GeoTx");
    fs::copy_file(gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);
    MapEditor::Tests::copyDirectoryRecursive(
        savedWorldDir, loadRoot / "RESOURCE" / "Worlds" / worldName);
    return true;
}

/// A decal image of one exact RGB everywhere.
MapEditor::ImageDecal::DecalImage makeSolidDecal(int size, std::uint32_t rgb)
{
    MapEditor::ImageDecal::DecalImage image;
    image.width = size;
    image.height = size;
    image.pixels.assign(static_cast<std::size_t>(size) * size,
                        0xFF000000u | rgb);
    image.indices.assign(static_cast<std::size_t>(size) * size, 7);
    return image;
}

const std::uint32_t kSetColorRgb = 0x00AA1122; // the SetColor sub-test paint

/// Checks the patch cells except (0,0) — that one carries the SetColor
/// sub-test's own RGB; assert it separately where it matters.
bool checkPatchColor(std::uint32_t expectedRgba)
{
    const int width = static_cast<int>(vMap.H_SIZE);
    for (int j = 0; j < kPatchSize; ++j) {
        for (int i = 0; i < kPatchSize; ++i) {
            if (i == 0 && j == 0) {
                continue;
            }
            const int off = patchOffset(i, j, width);
            if (vMap.SurColBuf == nullptr ||
                vMap.SurColBuf[off] != expectedRgba) {
                return false;
            }
        }
    }
    return true;
}

bool runColors24Tests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_colors24";
    TEST_REQUIRE(buildLoadRoot(ctx.gameDir, ctx.tempWorldDir,
                               fs::u8path(ctx.worldName), loadRoot),
                 "could not build the load content root.");

    MapEditor::EditorSession session;
    // One undo entry per action.
    session.setUndoCoalescingWindowMs(0);
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");
    TEST_REQUIRE(session.isWorldLoaded(), "session reports no loaded world.");

    const fs::path worldDir = session.currentWorldFiles()->worldDir();
    const fs::path sidecarPath = worldDir / MapEditor::WorldFiles::Heights16File;
    const int width = static_cast<int>(vMap.H_SIZE);
    const int mapW = width;

    // A dam patch under the paint area (the decal owns Dam only).
    for (int j = -1; j < kPatchSize + 1; ++j) {
        for (int i = -1; i < kPatchSize + 1; ++i) {
            const int off = (kPatchY + j) * mapW + (kPatchX + i);
            vMap.VxGBuf[off] = 10;
            vMap.VxDBuf[off] = 40;
            vMap.AtrBuf[off] = 0;
            vMap.SurBuf[off] = 5;
        }
    }

    TEST_REQUIRE(vMap.SurColBuf == nullptr,
                 "the fresh world must not have the true-color buffer.");
    TEST_REQUIRE(!fs::exists(sidecarPath),
                 "a palette-only world must not carry the sidecar.");

    // ------------------------------------------------------------------
    // A: RGB paint through the decal brush path (exact color, index band).
    // ------------------------------------------------------------------
    MapEditor::BrushTool brush(session);
    MapEditor::BrushParams params;
    params.effect = MapEditor::BrushEffect::ImageDecal;
    params.mode = MapEditor::BrushMode::Dam;
    brush.setDecalImage(makeSolidDecal(kPatchSize, kTestRgb));
    TEST_REQUIRE(brush.applyStamp(kPatchX + kPatchSize / 2,
                                  kPatchY + kPatchSize / 2, params),
                 "the RGB decal stamp was refused.");
    TEST_REQUIRE(vMap.SurColBuf != nullptr,
                 "the RGB paint must allocate the true-color buffer.");
    TEST_REQUIRE(checkPatchColor(0xFF000000u | kTestRgb),
                 "the patch must carry the exact 24-bit RGB.");
    {
        const int off = patchOffset(0, 0, width);
        TEST_REQUIRE(vMap.SurColBuf[off] == (0xFF000000u | kTestRgb),
                     "cell (0,0) must carry the decal RGB.");
        TEST_REQUIRE(vMap.SurBuf[off] == 7,
                     "the nearest palette index side band must be written.");
    }
    std::printf("[PASS] RGB paint stores the exact color + index band.\n");

    // A SetColor stamp paints the picker's exact RGB over the decal color
    // (the true-color layer takes the new value; SurBuf gets the nearest
    // palette entry of the cell's layer as the side band). A radius-0 stamp
    // paints exactly one cell.
    {
        MapEditor::BrushParams colorParams;
        colorParams.effect = MapEditor::BrushEffect::SetColor;
        colorParams.mode = MapEditor::BrushMode::Dam;
        colorParams.radius = 0;
        colorParams.colorRgb = kSetColorRgb;
        const int off = patchOffset(0, 0, width);
        TEST_REQUIRE(brush.applyStamp(kPatchX, kPatchY, colorParams),
                     "the radius-0 SetColor stamp was refused.");
        TEST_REQUIRE(vMap.SurColBuf[off] == (0xFF000000u | kSetColorRgb),
                     "the SetColor stamp must write its exact RGB (got "
                     "0x%08X).",
                     vMap.SurColBuf[off]);
    }
    std::printf("[PASS] A SetColor stamp paints the picker's exact RGB.\n");

    // ------------------------------------------------------------------
    // D: undo/redo restores the color layer.
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.undo(), "undo of the SetColor stamp failed.");
    TEST_REQUIRE(session.undo(), "undo of the RGB stamp failed.");
    TEST_REQUIRE(vMap.SurColBuf == nullptr ||
                     (checkPatchColor(0) &&
                      vMap.SurColBuf[patchOffset(0, 0, width)] == 0),
                 "undo must clear the painted colors.");
    TEST_REQUIRE(session.redo(), "redo of the RGB stamp failed.");
    TEST_REQUIRE(session.redo(), "redo of the SetColor stamp failed.");
    {
        const int off = patchOffset(0, 0, width);
        TEST_REQUIRE(vMap.SurColBuf != nullptr &&
                         vMap.SurColBuf[off] == (0xFF000000u | kSetColorRgb),
                     "redo of the SetColor stamp must restore its exact RGB.");
    }
    std::printf("[PASS] undo/redo restores the color layer.\n");

    // ------------------------------------------------------------------
    // B: save -> sidecar v2 with colors -> reload -> exact colors.
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.saveWorld(), "saveWorld() with colors failed.");
    TEST_REQUIRE(fs::exists(sidecarPath),
                 "the colors world must write the sidecar.");

    const std::vector<std::uint8_t> sidecar =
        MapEditor::Tests::readFileBytes(sidecarPath);
    TEST_REQUIRE(sidecar.size() >= 20, "the sidecar has no v2 header.");
    TEST_REQUIRE(std::memcmp(sidecar.data(), "H16", 3) == 0,
                 "the sidecar has a wrong id.");
    const std::uint32_t version = static_cast<std::uint32_t>(
        sidecar[4] | (sidecar[5] << 8) | (sidecar[6] << 16) |
        (sidecar[7] << 24));
    const std::uint32_t flags = static_cast<std::uint32_t>(
        sidecar[16] | (sidecar[17] << 8) | (sidecar[18] << 16) |
        (sidecar[19] << 24));
    TEST_REQUIRE(version == 2 && (flags & 0x1) != 0,
                 "the sidecar must be v2 with the colors flag.");
    const std::uint64_t cells =
        static_cast<std::uint64_t>(vMap.H_SIZE) * vMap.V_SIZE;
    TEST_REQUIRE(sidecar.size() >=
                     20 + cells * 4 + cells * 4,
                 "the sidecar colors section is truncated.");
    const std::uint64_t firstOff =
        static_cast<std::uint64_t>(patchOffset(1, 1, width));
    const std::uint64_t colByte = 20 + cells * 4 + firstOff * 4;
    const std::uint32_t stored =
        static_cast<std::uint32_t>(sidecar[colByte] |
                                   (sidecar[colByte + 1] << 8) |
                                   (sidecar[colByte + 2] << 16) |
                                   (sidecar[colByte + 3] << 24));
    TEST_REQUIRE(stored == (0xFF000000u | kTestRgb),
                 "the sidecar must carry the exact cell color.");
    std::printf("[PASS] the sidecar stores the colors section (v2 + flag).\n");

    session.unloadWorld();
    TEST_REQUIRE(session.loadWorld(0), "reload with colors failed.");
    TEST_REQUIRE(vMap.SurColBuf != nullptr,
                 "the reload must allocate the true-color buffer.");
    TEST_REQUIRE(checkPatchColor(0xFF000000u | kTestRgb) &&
                     vMap.SurColBuf[patchOffset(0, 0, width)] ==
                         (0xFF000000u | kSetColorRgb),
                 "the reload must restore the exact colors.");
    {
        // getColor32 must shade the RGB exactly like the palette tables do:
        // same math as convertPal2TableSurCol for the cell's light level.
        const int off = patchOffset(1, 1, width);
        const unsigned int light = vMap.RnrBuf[off] & 0x7F;
        const unsigned int expected =
            vrtMap::trueColor32(0xFF000000u | kTestRgb, light);
        TEST_REQUIRE(vMap.getColor32(off) == expected,
                     "getColor32 0x%06X, expected 0x%06X.",
                     vMap.getColor32(off), expected);
    }
    std::printf("[PASS] reload restores colors; getColor32 shades them exactly.\n");

    // ------------------------------------------------------------------
    // C: saveGameMap/loadGameMap carry the colors ("XCL0").
    // ------------------------------------------------------------------
    vMap.regRender(kPatchX - 2, kPatchY - 2, kPatchX + kPatchSize + 2,
                   kPatchY + kPatchSize + 2);
    XBuffer saveBuf(1 << 20, true);
    TEST_REQUIRE(vMap.saveGameMap(saveBuf), "saveGameMap() failed.");
    bool foundXH16 = false;
    bool foundXCL0 = false;
    for (size_t p = 0; p + 4 <= saveBuf.tell(); ++p) {
        if (!foundXH16 && std::memcmp(saveBuf.address() + p, "XH16", 4) == 0) {
            foundXH16 = true;
        }
        if (!foundXCL0 && std::memcmp(saveBuf.address() + p, "XCL0", 4) == 0) {
            foundXCL0 = true;
        }
    }
    TEST_REQUIRE(!foundXH16,
                 "this world has no tall heights — no XH16 expected.");
    TEST_REQUIRE(foundXCL0, "saveGameMap() must append the XCL0 colors chunk.");

    // Trample the colors, then read the saved map back.
    for (int j = 0; j < kPatchSize; ++j) {
        for (int i = 0; i < kPatchSize; ++i) {
            vMap.SurColBuf[patchOffset(i, j, width)] = 0xFFABCDEFu;
        }
    }
    saveBuf.set(0, XB_BEG);
    TEST_REQUIRE(vMap.loadGameMap(saveBuf, true),
                 "loadGameMap() with the XCL0 chunk failed.");
    TEST_REQUIRE(checkPatchColor(0xFF000000u | kTestRgb) &&
                     vMap.SurColBuf[patchOffset(0, 0, width)] ==
                         (0xFF000000u | kSetColorRgb),
                 "loadGameMap must restore the exact colors from XCL0.");
    std::printf("[PASS] saveGameMap/loadGameMap round-trip colors (XCL0).\n");

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("True-color 24-bit surface", 112, runColors24Tests);

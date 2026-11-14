// ============================================================================
// ImageDecal — true-color image stamp.
//
// Test 81:
//   A. prepare() validation: the 2048 px size cap and the all-transparent
//      image refusal;
//   B. prepare() conversion: colors pass through EXACTLY (no k-means, no
//      palette capacity checks), transparent / almost-transparent pixels
//      stay unpainted, and the nearest Dam palette entry is kept as the
//      SurBuf side band;
//   C. scaleNearest(): the brush scale control keeps colors and transparency,
//      only the pixel grid changes;
//   D. the brush itself: ImageDecal paints the exact 24-bit RGB into the
//      true-color layer plus the nearest palette index into SurBuf, only on
//      dam cells, never for transparent pixels, one undo entry per stamp,
//      refused in the Geo mode and without a decal image; undo restores the
//      color layer too.
//
// Runs after the world round-trip (its temporary world copy is reused).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ImageDecal.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::ImageDecal::DecalImage;
using MapEditor::ImageDecal::PrepareResult;

int mapOffset(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

/// RGBA helper: one pixel of an image buffer.
void setPixel(std::vector<unsigned char>& rgba, int width, int x, int y,
              int r, int g, int b, int a)
{
    const std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
    rgba[base + 0] = static_cast<unsigned char>(r);
    rgba[base + 1] = static_cast<unsigned char>(g);
    rgba[base + 2] = static_cast<unsigned char>(b);
    rgba[base + 3] = static_cast<unsigned char>(a);
}

/// A palette where every entry is black except `red` and `green`.
std::vector<unsigned char> makeTestPalette(int redIndex, int greenIndex)
{
    std::vector<unsigned char> palette(256 * 3, 0);
    palette[static_cast<std::size_t>(redIndex) * 3 + 0] = 255;
    palette[static_cast<std::size_t>(greenIndex) * 3 + 1] = 255;
    return palette;
}

/// The 8x4 test image: red / green / blue blocks plus transparent pixels.
std::vector<unsigned char> makeTestImage()
{
    const int width = 8;
    const int height = 4;
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4, 0);
    for (int x = 0; x < 4; ++x) {
        setPixel(rgba, width, x, 0, 255, 0, 0, 255);       // red
        setPixel(rgba, width, x, 1, 0, 255, 0, 255);       // green
    }
    for (int x = 4; x < 8; ++x) {
        setPixel(rgba, width, x, 0, 0, 0, 255, 255);       // blue
        setPixel(rgba, width, x, 1, 0, 0, 255, 255);       // blue
    }
    // Almost opaque — must be treated as transparent.
    setPixel(rgba, width, 0, 2, 255, 0, 0, 254);
    return rgba;
}

/// Craft one cell (direct buffer write, like the other brush tests).
void setCell(int x, int y, int geoWhole, int damWhole, int fraction,
             unsigned char surface)
{
    const int off = mapOffset(x, y);
    vMap.VxGBuf[off] = static_cast<unsigned char>(geoWhole);
    vMap.VxDBuf[off] = static_cast<unsigned char>(damWhole);
    vMap.AtrBuf[off] = static_cast<unsigned char>(fraction);
    vMap.SurBuf[off] = surface;
}

void fillPatch(int cx, int cy, int half, int geoWhole, int damWhole,
               int fraction, unsigned char surface)
{
    for (int y = cy - half; y <= cy + half; ++y) {
        for (int x = cx - half; x <= cx + half; ++x) {
            setCell(x, y, geoWhole, damWhole, fraction, surface);
        }
    }
}

bool prepareBrushTestSession(const char* subdirName,
                             MapEditor::EditorSession& session)
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    if (ctx.tempRoot.empty() || !fs::exists(ctx.tempRoot) ||
        ctx.worldName.empty()) {
        return false;
    }
    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / subdirName;
    std::error_code ec;
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(
        ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
    fs::copy_file(ctx.gameDir / "Perimeter.ini",
                  loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(
        ctx.tempWorldDir,
        loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName));

    if (!session.setGameDir(loadRoot.u8string())) {
        return false;
    }
    if (!session.scanWorlds() || session.worldCount() < 1) {
        return false;
    }
    return session.loadWorld(0);
}

} // namespace

// ---------------------------------------------------------------------------
// Test 81
// ---------------------------------------------------------------------------

bool runImageDecalTests()
{
    const int redIndex = 10;
    const int greenIndex = 20;
    const std::vector<unsigned char> palette =
        makeTestPalette(redIndex, greenIndex);

    // =====================================================================
    // A. Validation.
    // =====================================================================
    {
        // Size cap: 3000 px wide is refused with the 2048 px message.
        const int width = 3000;
        const int height = 10;
        std::vector<unsigned char> rgba(
            static_cast<std::size_t>(width) * height * 4, 255);
        const PrepareResult result = MapEditor::ImageDecal::prepare(
            rgba.data(), width, height, palette.data());
        TEST_REQUIRE(!result.ok, "a 3000 px wide image must be refused.");
        TEST_REQUIRE(result.error.find("2048") != std::string::npos,
                     "the size error must mention the 2048 px limit: %s",
                     result.error.c_str());
        // 600x10 is fine now (the limit was raised from 512 to 2048).
        const PrepareResult accepted = MapEditor::ImageDecal::prepare(
            rgba.data(), 600, 10, palette.data());
        TEST_REQUIRE(accepted.ok, "a 600 px image must be accepted: %s",
                     accepted.error.c_str());
        std::printf("[PASS] The 2048 px size cap is enforced.\n");
    }
    {
        // No fully opaque pixel.
        const int width = 4;
        const int height = 4;
        std::vector<unsigned char> rgba(
            static_cast<std::size_t>(width) * height * 4, 0);
        for (int i = 0; i < width * height; ++i) {
            rgba[static_cast<std::size_t>(i) * 4 + 3] = 254;
        }
        const PrepareResult result = MapEditor::ImageDecal::prepare(
            rgba.data(), width, height, palette.data());
        TEST_REQUIRE(!result.ok,
                     "an image with only transparent pixels must be refused.");
        std::printf("[PASS] An image without fully opaque pixels is refused.\n");
    }
    {
        // A FULL palette no longer matters: true-color pass-through does not
        // need free entries.
        std::vector<unsigned char> rgba(8 * 4 * 4, 0);
        setPixel(rgba, 8, 0, 0, 1, 2, 3, 255);
        setPixel(rgba, 8, 1, 0, 250, 130, 40, 255);
        const PrepareResult result = MapEditor::ImageDecal::prepare(
            rgba.data(), 8, 4, palette.data());
        TEST_REQUIRE(result.ok,
                     "a full palette must not limit a true-color decal: %s",
                     result.error.c_str());
        std::printf("[PASS] The palette capacity no longer limits the decal.\n");
    }

    // =====================================================================
    // B. Conversion (true-color pass-through).
    // =====================================================================
    const std::vector<unsigned char> image = makeTestImage();
    const PrepareResult prepared = MapEditor::ImageDecal::prepare(
        image.data(), 8, 4, palette.data());
    TEST_REQUIRE(prepared.ok, "the test image was refused: %s",
                 prepared.error.c_str());

    const DecalImage& decal = prepared.image;
    TEST_REQUIRE(decal.width == 8 && decal.height == 4,
                 "decal is %dx%d, expected 8x4.", decal.width, decal.height);
    for (int x = 0; x < 4; ++x) {
        TEST_REQUIRE(decal.at(x, 0) == 0xFFFF0000u,
                     "red pixel (%d,0) is 0x%08X, expected 0xFFFF0000.", x,
                     decal.at(x, 0));
        TEST_REQUIRE(decal.indexAt(x, 0) == redIndex,
                     "red pixel (%d,0) SurBuf index %d, expected %d.", x,
                     decal.indexAt(x, 0), redIndex);
        TEST_REQUIRE(decal.at(x, 1) == 0xFF00FF00u,
                     "green pixel (%d,1) is 0x%08X, expected 0xFF00FF00.", x,
                     decal.at(x, 1));
        TEST_REQUIRE(decal.indexAt(x, 1) == greenIndex,
                     "green pixel (%d,1) SurBuf index %d, expected %d.", x,
                     decal.indexAt(x, 1), greenIndex);
    }
    for (int x = 4; x < 8; ++x) {
        TEST_REQUIRE(decal.at(x, 0) == 0xFF0000FFu,
                     "blue pixel (%d,0) is 0x%08X, expected 0xFF0000FF.", x,
                     decal.at(x, 0));
        TEST_REQUIRE(decal.at(x, 1) == 0xFF0000FFu,
                     "blue pixel (%d,1) is 0x%08X, expected 0xFF0000FF.", x,
                     decal.at(x, 1));
    }
    TEST_REQUIRE(decal.at(0, 2) == 0,
                 "an almost opaque (alpha 254) pixel must stay transparent.");
    TEST_REQUIRE(decal.at(1, 2) == 0, "an empty pixel must stay transparent.");
    TEST_REQUIRE(decal.at(7, 3) == 0, "an empty pixel must stay transparent.");
    std::printf("[PASS] Colors pass through exactly, transparency is kept.\n");

    // =====================================================================
    // C. Scaling keeps colors and transparency.
    // =====================================================================
    {
        const DecalImage scaled = MapEditor::ImageDecal::scaleNearest(decal, 2.0);
        TEST_REQUIRE(scaled.width == 16 && scaled.height == 8,
                     "scaled decal is %dx%d, expected 16x8.", scaled.width,
                     scaled.height);
        TEST_REQUIRE(scaled.at(0, 0) == 0xFFFF0000u, "scaled red pixel changed.");
        TEST_REQUIRE(scaled.at(14, 0) == 0xFF0000FFu,
                     "scaled blue pixel changed.");
        TEST_REQUIRE(scaled.at(0, 5) == 0,
                     "scaled transparent pixel must stay transparent.");
        std::printf("[PASS] The decal scale keeps colors and transparency.\n");
    }

    // =====================================================================
    // D. The brush itself.
    // =====================================================================
    MapEditor::EditorSession session;
    // Undo coalescing off: these tests expect one undo entry per action.
    session.setUndoCoalescingWindowMs(0);
    const bool loaded = prepareBrushTestSession("load_root_decal", session);
    TEST_REQUIRE(loaded, "the decal test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the decal test.");

    const int cx = mapW / 2;
    const int cy = mapH / 2;
    const unsigned char baseSurface = 5;

    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.effect = BrushEffect::ImageDecal;
    params.mode = BrushMode::Dam;

    // Without a decal image the brush must refuse to stamp.
    TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                 "the decal brush must refuse to stamp without an image.");

    brush.setDecalImage(decal);
    TEST_REQUIRE(brush.hasDecalImage(), "the decal image was not stored.");

    // Geo mode is not a decal layer: the stamp is refused and nothing changes.
    fillPatch(cx, cy, 12, 10, 40, 0, baseSurface);
    params.mode = BrushMode::Geo;
    TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                 "the decal brush must be Dam-only.");
    params.mode = BrushMode::Dam;

    // Snapshot the patch for the undo check (the color layer too — the stamp
    // below allocates the true-color buffer, undo must clear it back).
    struct Cell {
        uint16_t geo, dam;
        unsigned char atr, sur;
        std::uint32_t col;
    };
    std::vector<Cell> before;
    const int half = 12;
    TEST_REQUIRE(vMap.SurColBuf == nullptr,
                 "the fresh world must not have the true-color buffer yet.");
    for (int y = cy - half; y <= cy + half; ++y) {
        for (int x = cx - half; x <= cx + half; ++x) {
            const int off = mapOffset(x, y);
            before.push_back({vMap.VxGBuf[off], vMap.VxDBuf[off],
                              vMap.AtrBuf[off], vMap.SurBuf[off], 0});
        }
    }

    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the decal stamp was refused.");

    // The decal is centred on (cx,cy): origin = (cx - 4, cy - 2).
    const int originX = cx - decal.width / 2;
    const int originY = cy - decal.height / 2;
    TEST_REQUIRE(vMap.SurColBuf != nullptr,
                 "the decal stamp must allocate the true-color buffer.");
    for (int dy = 0; dy < decal.height; ++dy) {
        for (int dx = 0; dx < decal.width; ++dx) {
            const int x = originX + dx;
            const int y = originY + dy;
            const int off = mapOffset(x, y);
            const std::uint32_t pixel = decal.at(dx, dy);
            const std::uint32_t expectedCol =
                (pixel >> 24) == 0xFF ? pixel : 0;
            TEST_REQUIRE(vMap.SurColBuf[off] == expectedCol,
                         "decal pixel (%d,%d): color 0x%08X, expected 0x%08X.",
                         dx, dy, vMap.SurColBuf[off], expectedCol);
            const unsigned char expectedSur =
                (pixel >> 24) == 0xFF
                    ? decal.indexAt(dx, dy)
                    : baseSurface;
            TEST_REQUIRE(vMap.SurBuf[off] == expectedSur,
                         "decal pixel (%d,%d): SurBuf %d, expected %d.", dx, dy,
                         vMap.SurBuf[off], expectedSur);
            // Height buffers are never touched by a colour brush.
            TEST_REQUIRE(vMap.VxDBuf[off] == 40 && vMap.VxGBuf[off] == 10 &&
                             vMap.AtrBuf[off] == 0,
                         "the decal must not change the height at (%d,%d).",
                         dx, dy);
        }
    }
    // Outside the decal bbox nothing changes.
    TEST_REQUIRE(vMap.SurBuf[mapOffset(cx + half, cy)] == baseSurface,
                 "a cell outside the decal must stay untouched.");
    std::printf("[PASS] The decal paints exact RGB + palette index on dam cells.\n");

    TEST_REQUIRE(session.undo(), "the dam decal stamp undo failed.");
    std::size_t i = 0;
    for (int y = cy - half; y <= cy + half; ++y) {
        for (int x = cx - half; x <= cx + half; ++x, ++i) {
            const int off = mapOffset(x, y);
            TEST_REQUIRE(vMap.VxGBuf[off] == before[i].geo &&
                             vMap.VxDBuf[off] == before[i].dam &&
                             vMap.AtrBuf[off] == before[i].atr &&
                             vMap.SurBuf[off] == before[i].sur &&
                             vMap.SurColBuf[off] == before[i].col,
                         "undo must restore cell (%d,%d) byte-exact.", x, y);
        }
    }
    std::printf("[PASS] Undo restores the decal stamp byte-exact (colors too).\n");

    // Geo cells under the same stamp are skipped (the decal owns Dam only).
    fillPatch(cx, cy, 12, 10, 0, 0, baseSurface);
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the decal stamp over geo cells was refused.");
    for (int dy = 0; dy < decal.height; ++dy) {
        for (int dx = 0; dx < decal.width; ++dx) {
            const int off = mapOffset(originX + dx, originY + dy);
            TEST_REQUIRE(vMap.SurBuf[off] == baseSurface,
                         "a geo cell (%d,%d) must keep its surface.", dx, dy);
            TEST_REQUIRE(vMap.SurColBuf[off] == 0,
                         "a geo cell (%d,%d) must keep no color override.", dx,
                         dy);
        }
    }
    std::printf("[PASS] Geo cells are never painted by the decal.\n");

    TEST_REQUIRE(session.undo(), "the geo-cell decal stamp undo failed.");

    // ------------------------------------------------------------------
    // E: an explicit paint height below the surface stamps the pixel
    //    colors into the internal voxel layers [h-depth+1 .. h] instead
    //    of the column tops.
    // ------------------------------------------------------------------
    fillPatch(cx, cy, 12, 10, 40, 0, baseSurface);
    params.paintHeight = 30;
    params.paintDepth = 4;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the layer decal stamp was refused.");
    for (int dy = 0; dy < decal.height; ++dy) {
        for (int dx = 0; dx < decal.width; ++dx) {
            const int off = mapOffset(originX + dx, originY + dy);
            const std::uint32_t pixel = decal.at(dx, dy);
            if ((pixel >> 24) == 0xFF) {
                std::uint32_t rgb = 0;
                for (int z = 28; z <= 30; ++z) {
                    TEST_REQUIRE(
                        vMap.voxLayers.lookup(
                            static_cast<std::uint32_t>(off), z, &rgb) &&
                            rgb == (pixel & 0x00FFFFFFu),
                        "decal pixel (%d,%d) must paint the layer %d with "
                        "its exact RGB.",
                        dx, dy, z);
                }
                TEST_REQUIRE(!vMap.voxLayers.lookup(
                                 static_cast<std::uint32_t>(off), 31, &rgb),
                             "above the paint depth nothing is painted.");
                // The top keeps NO color override — the paint went inside.
                TEST_REQUIRE(vMap.SurColBuf[off] == 0,
                             "the decal layer paint must not touch the top "
                             "color.");
            } else {
                std::uint32_t rgb = 0;
                TEST_REQUIRE(!vMap.voxLayers.lookup(
                                 static_cast<std::uint32_t>(off), 30, &rgb),
                             "a transparent decal pixel must not stamp.");
            }
            // Heights are never touched by a colour brush.
            TEST_REQUIRE(vMap.VxDBuf[off] == 40 && vMap.VxGBuf[off] == 10,
                         "the decal layer paint must not change heights.");
        }
    }
    std::printf("[PASS] the decal paints inner layers at height+depth.\n");

    TEST_REQUIRE(session.undo(), "the layer decal stamp undo failed.");
    {
        // The red pixel (0,0) was painted at layers [28 .. 30].
        std::uint32_t rgb = 0;
        TEST_REQUIRE(!vMap.voxLayers.lookup(
                         static_cast<std::uint32_t>(mapOffset(originX, originY)),
                         30, &rgb) ||
                         rgb != 0x00FF0000u,
                     "undo must remove the painted decal layers.");
    }
    std::printf("[PASS] the decal layer paint is one undo entry.\n");
    params.paintHeight = -1;
    params.paintDepth = 1;

    return true;
}

MAP_EDITOR_REGISTER_TEST("Image decal brush (true-color stamp)", 81,
                         runImageDecalTests);

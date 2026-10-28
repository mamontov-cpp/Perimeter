// ============================================================================
// ImageDecal — palette-limited image stamp.
//
// Test 81:
//   A. prepare() validation: the 512 px size cap, an all-transparent image and
//      the "not enough free Dam palette colours" error (fewer than two free
//      entries vs. more than two image colours that are not in the palette);
//   B. prepare() conversion: exact palette colours are reused, a colour that
//      is not in the palette becomes a new palette write on a free entry, and
//      transparent / almost-transparent (alpha 254) pixels stay -1;
//   C. scaleNearest(): the brush scale control keeps the palette indices and
//      the transparency, only the pixel grid changes;
//   D. the brush itself: ImageDecal paints SurBuf exactly like SetColor per
//      pixel, only on dam cells, never for -1 pixels, one undo entry per
//      stamp, refused in the Geo mode and without a decal image.
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
    std::vector<unsigned char> usage(256, 0);
    usage[redIndex] = 1;

    // =====================================================================
    // A. Validation.
    // =====================================================================
    {
        // Size cap: 600 px wide is refused with the 512 px message.
        const int width = 600;
        const int height = 10;
        std::vector<unsigned char> rgba(
            static_cast<std::size_t>(width) * height * 4, 255);
        const PrepareResult result = MapEditor::ImageDecal::prepare(
            rgba.data(), width, height, palette.data(), usage.data());
        TEST_REQUIRE(!result.ok, "a 600 px wide image must be refused.");
        TEST_REQUIRE(result.error.find("512") != std::string::npos,
                     "the size error must mention the 512 px limit: %s",
                     result.error.c_str());
        std::printf("[PASS] Images larger than 512 px are refused.\n");
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
            rgba.data(), width, height, palette.data(), usage.data());
        TEST_REQUIRE(!result.ok,
                     "an image with only transparent pixels must be refused.");
        std::printf("[PASS] An image without fully opaque pixels is refused.\n");
    }
    {
        // Not enough free palette entries: 0 free, 3 image colours absent
        // from the palette.
        std::vector<unsigned char> fullUsage(256, 1);
        std::vector<unsigned char> rgba(8 * 4 * 4, 0);
        setPixel(rgba, 8, 0, 0, 1, 2, 3, 255);
        setPixel(rgba, 8, 1, 0, 4, 5, 6, 255);
        setPixel(rgba, 8, 2, 0, 7, 8, 9, 255);
        PrepareResult result = MapEditor::ImageDecal::prepare(
            rgba.data(), 8, 4, palette.data(), fullUsage.data());
        TEST_REQUIRE(!result.ok,
                     "a full palette must refuse a multi-colour image.");
        TEST_REQUIRE(result.error.find("palette") != std::string::npos,
                     "the palette error must mention the palette: %s",
                     result.error.c_str());

        // Exactly one free entry is still "fewer than two".
        fullUsage[7] = 0;
        result = MapEditor::ImageDecal::prepare(rgba.data(), 8, 4,
                                                palette.data(),
                                                fullUsage.data());
        TEST_REQUIRE(!result.ok, "one free entry is not enough either.");

        // Two free entries are accepted.
        fullUsage[8] = 0;
        result = MapEditor::ImageDecal::prepare(rgba.data(), 8, 4,
                                                palette.data(),
                                                fullUsage.data());
        TEST_REQUIRE(result.ok, "two free entries must be accepted: %s",
                     result.error.c_str());
        TEST_REQUIRE(result.unusedDamColors == 2,
                     "unusedDamColors %d, expected 2.",
                     result.unusedDamColors);
        TEST_REQUIRE(result.imageForeignColors == 3,
                     "imageForeignColors %d, expected 3.",
                     result.imageForeignColors);
        std::printf("[PASS] The palette capacity check follows the 2-free / "
                    "2-foreign rule.\n");
    }

    // =====================================================================
    // B. Conversion (k-means over the palette candidates).
    // =====================================================================
    const std::vector<unsigned char> image = makeTestImage();
    PrepareResult prepared = MapEditor::ImageDecal::prepare(
        image.data(), 8, 4, palette.data(), usage.data());
    TEST_REQUIRE(prepared.ok, "the test image was refused: %s",
                 prepared.error.c_str());
    TEST_REQUIRE(prepared.candidateColors >=
                     MapEditor::ImageDecal::kMinPaletteColors,
                 "the candidate list is %d, expected at least %d.",
                 prepared.candidateColors,
                 MapEditor::ImageDecal::kMinPaletteColors);
    TEST_REQUIRE(prepared.resultColors == 3,
                 "the image must reduce to 3 colours, got %d.",
                 prepared.resultColors);
    TEST_REQUIRE(prepared.paletteWrites.size() == 1,
                 "exactly the blue colour is new, got %d writes.",
                 static_cast<int>(prepared.paletteWrites.size()));
    TEST_REQUIRE(prepared.paletteWrites[0].rgb == 0x0000FFu,
                 "the new colour is 0x%06X, expected 0x0000FF.",
                 prepared.paletteWrites[0].rgb);
    TEST_REQUIRE(prepared.paletteWrites[0].index != redIndex &&
                     prepared.paletteWrites[0].index != greenIndex,
                 "a new colour must not overwrite a palette entry in use.");

    const DecalImage& decal = prepared.image;
    TEST_REQUIRE(decal.width == 8 && decal.height == 4,
                 "decal is %dx%d, expected 8x4.", decal.width, decal.height);
    for (int x = 0; x < 4; ++x) {
        TEST_REQUIRE(decal.at(x, 0) == redIndex,
                     "red pixel (%d,0) is index %d, expected %d.",
                     x, decal.at(x, 0), redIndex);
        TEST_REQUIRE(decal.at(x, 1) == greenIndex,
                     "green pixel (%d,1) is index %d, expected %d.",
                     x, decal.at(x, 1), greenIndex);
    }
    for (int x = 4; x < 8; ++x) {
        TEST_REQUIRE(decal.at(x, 0) == prepared.paletteWrites[0].index,
                     "blue pixel (%d,0) is index %d, expected %d.",
                     x, decal.at(x, 0), prepared.paletteWrites[0].index);
        TEST_REQUIRE(decal.at(x, 1) == prepared.paletteWrites[0].index,
                     "blue pixel (%d,1) is index %d, expected %d.",
                     x, decal.at(x, 1), prepared.paletteWrites[0].index);
    }
    TEST_REQUIRE(decal.at(0, 2) == -1,
                 "an almost opaque (alpha 254) pixel must stay transparent.");
    TEST_REQUIRE(decal.at(1, 2) == -1, "an empty pixel must stay transparent.");
    TEST_REQUIRE(decal.at(7, 3) == -1, "an empty pixel must stay transparent.");
    std::printf("[PASS] k-means reuses palette colours and adds the new one.\n");

    // =====================================================================
    // C. Scaling keeps indices and transparency.
    // =====================================================================
    {
        const DecalImage scaled = MapEditor::ImageDecal::scaleNearest(decal, 2.0);
        TEST_REQUIRE(scaled.width == 16 && scaled.height == 8,
                     "scaled decal is %dx%d, expected 16x8.", scaled.width,
                     scaled.height);
        TEST_REQUIRE(scaled.at(0, 0) == redIndex, "scaled red pixel changed.");
        TEST_REQUIRE(scaled.at(14, 0) == prepared.paletteWrites[0].index,
                     "scaled blue pixel changed.");
        TEST_REQUIRE(scaled.at(0, 5) == -1,
                     "scaled transparent pixel must stay transparent.");
        std::printf("[PASS] The decal scale keeps indices and transparency.\n");
    }

    // =====================================================================
    // D. The brush itself.
    // =====================================================================
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_decal", session);
    TEST_REQUIRE(loaded, "the decal test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the decal test.");

    const int cx = mapW / 2;
    const int cy = mapH / 2;
    const unsigned char baseSurface = 5;

    // The decal indices must exist in the Dam palette of the loaded world:
    // write the two colours and the new one through the session, exactly like
    // the UI does after prepare().
    TEST_REQUIRE(session.setPaletteRgb(
                     MapEditor::EditorSession::PaletteLayer::Dam, redIndex,
                     0x00FF0000u),
                 "the red palette write failed.");
    TEST_REQUIRE(session.setPaletteRgb(
                     MapEditor::EditorSession::PaletteLayer::Dam, greenIndex,
                     0x0000FF00u),
                 "the green palette write failed.");
    TEST_REQUIRE(session.applyPalettes(), "applyPalettes() failed.");

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

    // Snapshot the patch for the undo check.
    struct Cell { unsigned char geo, dam, atr, sur; };
    std::vector<Cell> before;
    const int half = 12;
    for (int y = cy - half; y <= cy + half; ++y) {
        for (int x = cx - half; x <= cx + half; ++x) {
            const int off = mapOffset(x, y);
            before.push_back({vMap.VxGBuf[off], vMap.VxDBuf[off],
                              vMap.AtrBuf[off], vMap.SurBuf[off]});
        }
    }

    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the decal stamp was refused.");

    // The decal is centred on (cx,cy): origin = (cx - 4, cy - 2).
    const int originX = cx - decal.width / 2;
    const int originY = cy - decal.height / 2;
    for (int dy = 0; dy < decal.height; ++dy) {
        for (int dx = 0; dx < decal.width; ++dx) {
            const int x = originX + dx;
            const int y = originY + dy;
            const int off = mapOffset(x, y);
            const int index = decal.at(dx, dy);
            const unsigned char expected =
                index < 0 ? baseSurface : static_cast<unsigned char>(index);
            TEST_REQUIRE(vMap.SurBuf[off] == expected,
                         "decal pixel (%d,%d): SurBuf %d, expected %d.", dx, dy,
                         vMap.SurBuf[off], expected);
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
    std::printf("[PASS] The decal paints its palette indices on dam cells.\n");

    TEST_REQUIRE(session.undo(), "the dam decal stamp undo failed.");
    std::size_t i = 0;
    for (int y = cy - half; y <= cy + half; ++y) {
        for (int x = cx - half; x <= cx + half; ++x, ++i) {
            const int off = mapOffset(x, y);
            TEST_REQUIRE(vMap.VxGBuf[off] == before[i].geo &&
                             vMap.VxDBuf[off] == before[i].dam &&
                             vMap.AtrBuf[off] == before[i].atr &&
                             vMap.SurBuf[off] == before[i].sur,
                         "undo must restore cell (%d,%d) byte-exact.", x, y);
        }
    }
    std::printf("[PASS] Undo restores the decal stamp byte-exact.\n");

    // Geo cells under the same stamp are skipped (the decal owns Dam only).
    fillPatch(cx, cy, 12, 10, 0, 0, baseSurface);
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the decal stamp over geo cells was refused.");
    for (int dy = 0; dy < decal.height; ++dy) {
        for (int dx = 0; dx < decal.width; ++dx) {
            const int off = mapOffset(originX + dx, originY + dy);
            TEST_REQUIRE(vMap.SurBuf[off] == baseSurface,
                         "a geo cell (%d,%d) must keep its surface.", dx, dy);
        }
    }
    std::printf("[PASS] Geo cells are never painted by the decal.\n");

    TEST_REQUIRE(session.undo(), "the geo-cell decal stamp undo failed.");

    return true;
}

MAP_EDITOR_REGISTER_TEST("Image decal brush (palette-limited stamp)", 81,
                         runImageDecalTests);

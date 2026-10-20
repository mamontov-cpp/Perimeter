// ============================================================================
// T2.4 — WorldImages: 24-bit TGA write / load round-trip and origin handling.
//
// WorldImages is the shared image core of the save pipeline (cache.tga /
// map.tga) and the minimap panel (MinimapWidget). The tests are pure file
// operations — no world loading needed:
//   * write/top-left origin: writeTga24 → loadTga24 round-trip preserves
//     every byte,
//   * bottom-left origin: a descriptor-0 TGA is flipped into top-left rows
//     on load (game and foreign map.tga files may use it),
//   * rejection: anything other than uncompressed 24-bit true-color (the
//     MinimapWidget then falls back to a live render), and truncated files,
//   * downscaleNearest picks the same pixel the map.tga generator documents.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "WorldImages.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::WorldImages::loadTga24;
using MapEditor::WorldImages::writeTga24;

fs::path probeDir();

/// Deterministic 24-bit BGR test raster.
std::vector<unsigned char> makePattern(unsigned width, unsigned height)
{
    std::vector<unsigned char> bgr(static_cast<std::size_t>(width) *
                                   height * 3);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
            bgr[i + 0] = static_cast<unsigned char>((x * 3 + y) & 0xFF);       // B
            bgr[i + 1] = static_cast<unsigned char>((x * 5 + y * 2) & 0xFF);   // G
            bgr[i + 2] = static_cast<unsigned char>((x * 7 + y * 11) & 0xFF);  // R
        }
    }
    return bgr;
}

bool rasterEquals(const std::vector<unsigned char>& a,
                  const std::vector<unsigned char>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

/// Probe directory for the TGA files (removed on each run start).
fs::path probeDir()
{
    const MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    if (!ctx.tempRoot.empty()) {
        const fs::path probe = ctx.tempRoot / "worldimages_probe";
        std::error_code ec;
        fs::create_directories(probe, ec);
        return probe;
    }
    // Standalone runs without a world: still give the probe a fixed home.
    const fs::path probe = fs::temp_directory_path() /
                           "perimeter_map_editor_worldimages_probe";
    std::error_code ec;
    fs::remove_all(probe, ec);
    fs::create_directories(probe, ec);
    return probe;
}

} // namespace

bool runWorldImagesTests()
{
    const fs::path dir = probeDir();
    TEST_REQUIRE(!dir.empty() && fs::exists(dir), "probe dir must exist");

    // ------------------------------------------------------------------
    // 1. Round-trip with the top-left origin writeTga24 produces.
    // ------------------------------------------------------------------
    constexpr unsigned kW = 9;
    constexpr unsigned kH = 5;
    const std::vector<unsigned char> source = makePattern(kW, kH);

    const fs::path tgaPath = dir / "roundtrip.tga";
    TEST_REQUIRE(writeTga24(tgaPath, kW, kH, source),
                 "writeTga24 must succeed on a valid raster");

    unsigned width = 0;
    unsigned height = 0;
    std::vector<unsigned char> loaded;
    TEST_REQUIRE(loadTga24(tgaPath, width, height, loaded),
                 "loadTga24 must read back its own file");
    TEST_REQUIRE(width == kW && height == kH,
                 "round-trip dimensions must match");
    TEST_REQUIRE(rasterEquals(source, loaded),
                 "round-trip pixels must match byte for byte");

    // ------------------------------------------------------------------
    // 2. Bottom-left origin: rows are flipped into top-left order.
    // ------------------------------------------------------------------
    const unsigned bw = 4;
    const unsigned bh = 3;
    std::vector<unsigned char> fileBytes;
    fileBytes.push_back(0);  // id length
    fileBytes.push_back(0);  // color map type
    fileBytes.push_back(2);  // uncompressed true-color
    fileBytes.push_back(0); fileBytes.push_back(0);  // color map start
    fileBytes.push_back(0); fileBytes.push_back(0);  // color map length
    fileBytes.push_back(0);                          // color map depth
    fileBytes.push_back(0); fileBytes.push_back(0);  // x origin
    fileBytes.push_back(0); fileBytes.push_back(0);  // y origin
    fileBytes.push_back(static_cast<unsigned char>(bw & 0xFF));
    fileBytes.push_back(static_cast<unsigned char>(bw >> 8));
    fileBytes.push_back(static_cast<unsigned char>(bh & 0xFF));
    fileBytes.push_back(static_cast<unsigned char>(bh >> 8));
    fileBytes.push_back(24);                         // pixel depth
    fileBytes.push_back(0x00);                       // bottom-left origin
    // Rows stored bottom-up: file row 0 is map row bh-1.
    const std::vector<unsigned char> topUpPattern = makePattern(bw, bh);
    for (int y = static_cast<int>(bh) - 1; y >= 0; --y) {
        const auto row = static_cast<std::ptrdiff_t>(y) * bw * 3;
        fileBytes.insert(fileBytes.end(),
                         topUpPattern.begin() + row,
                         topUpPattern.begin() + row + bw * 3);
    }
    const fs::path bottomUpPath = dir / "bottom_up.tga";
    TEST_REQUIRE(MapEditor::Tests::writeFileBytes(bottomUpPath, fileBytes),
                 "bottom-up TGA fixture must be writable");

    width = 0;
    height = 0;
    loaded.clear();
    TEST_REQUIRE(loadTga24(bottomUpPath, width, height, loaded),
                 "loadTga24 must accept a bottom-left origin TGA");
    TEST_REQUIRE(width == bw && height == bh,
                 "bottom-left origin dimensions must match");
    TEST_REQUIRE(rasterEquals(topUpPattern, loaded),
                 "bottom-left rows must be flipped to top-left on load");

    // ------------------------------------------------------------------
    // 3. Rejection of files the minimap never accepts.
    // ------------------------------------------------------------------
    // Grayscale (image type 3) instead of true-color.
    std::vector<unsigned char> grayFile = fileBytes;
    grayFile[2] = 3;
    grayFile.resize(18 + bw * bh); // one byte per pixel
    const fs::path grayPath = dir / "gray.tga";
    TEST_REQUIRE(MapEditor::Tests::writeFileBytes(grayPath, grayFile),
                 "grayscale TGA fixture must be writable");
    width = 1;
    height = 1;
    loaded.assign(1, 0);
    TEST_REQUIRE(!loadTga24(grayPath, width, height, loaded),
                 "grayscale TGA must be rejected");
    TEST_REQUIRE(width == 0 && height == 0 && loaded.empty(),
                 "outputs must be cleared on rejection");

    // Truncated payload.
    std::vector<unsigned char> truncated(fileBytes);
    truncated.resize(18 + static_cast<std::size_t>(bw * 3)); // one row only
    const fs::path truncatedPath = dir / "truncated.tga";
    TEST_REQUIRE(MapEditor::Tests::writeFileBytes(truncatedPath, truncated),
                 "truncated TGA fixture must be writable");
    width = 1;
    height = 1;
    loaded.assign(1, 0);
    TEST_REQUIRE(!loadTga24(truncatedPath, width, height, loaded),
                 "truncated TGA must be rejected");

    // Missing file.
    TEST_REQUIRE(!loadTga24(dir / "does_not_exist.tga", width, height, loaded),
                 "a missing map.tga must be rejected, not crashed on");

    // ------------------------------------------------------------------
    // 4. downscaleNearest picks the documented corner pixels.
    // ------------------------------------------------------------------
    const std::vector<unsigned char> big = makePattern(8, 8);
    std::vector<unsigned char> small;
    MapEditor::WorldImages::downscaleNearest(big, 8, 8, 4, 4, small);
    TEST_REQUIRE(small.size() == static_cast<std::size_t>(4) * 4 * 3,
                 "downscaleNearest must produce the target raster size");
    // out[y][x] == in[y * 8 / 4][x * 8 / 4] == in[y * 2][x * 2].
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            const std::size_t src =
                (static_cast<std::size_t>(y * 2) * 8 + x * 2) * 3;
            const std::size_t dst = (static_cast<std::size_t>(y) * 4 + x) * 3;
            TEST_REQUIRE(small[dst + 0] == big[src + 0] &&
                         small[dst + 1] == big[src + 1] &&
                         small[dst + 2] == big[src + 2],
                         "downscaleNearest pixel mismatch at (%d, %d)",
                         static_cast<int>(x), static_cast<int>(y));
        }
    }

    std::printf("  WorldImages: TGA round-trip, origin flip and rejection OK\n");
    return true;
}

MAP_EDITOR_REGISTER_TEST("WorldImages (T2.4)", 45, runWorldImagesTests);

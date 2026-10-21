// ============================================================================
// ColorMipmapTests — post-T3.3 smooth 3D view pyramid, pure unit tests.
//
// Two tests, no world needed (the pyramid is a plain data structure; the
// world-binding side is covered by View3DSmoothTests):
//   1. build + full updateRegion produce exact rounded 2x2 means per level,
//      and a single-cell edit + incremental updateRegion refresh exactly the
//      cells that intersect the touched base cell.
//   2. bilinear wrap sampling: level 0 degenerates per coordinate fraction
//      (including wrap-around of negative coordinates), level 1 returns the
//      averaged kernel value.
//
// Channel order follows vrtMap::getColor32(): 0x00RRGGBB (no alpha).
// ============================================================================

#include "TestHarness.h"

#include "ColorMipmap.h"

#include <cstdint>

namespace {

using MapEditor::ColorMipmap;

constexpr std::uint32_t kBlockA = 0x00FF0000;   // red   (R=255)
constexpr std::uint32_t kBlockB = 0x000000FF;   // blue  (B=255)
constexpr unsigned char kHeightA = 160;
constexpr unsigned char kHeightB = 40;

/// Rounded 2x2 mean per channel, matching ColorMipmap's (sum + n/2)/n.
std::uint32_t expectedMean(std::uint32_t a, std::uint32_t b,
                           std::uint32_t c, std::uint32_t d)
{
    const auto mean = [](unsigned a1, unsigned b1, unsigned c1, unsigned d1) {
        return (a1 + b1 + c1 + d1 + 2) >> 2;
    };
    const unsigned r = mean(a >> 16, b >> 16, c >> 16, d >> 16);
    const unsigned g = mean((a >> 8) & 0xFF, (b >> 8) & 0xFF,
                            (c >> 8) & 0xFF, (d >> 8) & 0xFF);
    const unsigned bl = mean(a & 0xFF, b & 0xFF, c & 0xFF, d & 0xFF);
    return (r << 16) | (g << 8) | bl;
}

} // namespace

namespace {

bool runPyramidMeanTests()
{
    ColorMipmap mip;
    mip.build(8, 8);
    TEST_REQUIRE(mip.levelCount() == 4 &&
                     mip.levelWidth(0) == 8 && mip.levelHeight(0) == 8 &&
                     mip.levelWidth(1) == 4 && mip.levelHeight(1) == 4 &&
                     mip.levelWidth(2) == 2 && mip.levelHeight(2) == 2 &&
                     mip.levelWidth(3) == 1 && mip.levelHeight(3) == 1,
                 "8x8 pyramid levels wrong: %d levels.", mip.levelCount());

    // Red/blue 2x2 block checkerboard, heights likewise per block.
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool blockA = ((x / 2) + (y / 2)) % 2 == 0;
            mip.setBaseCell(x, y, blockA ? kBlockA : kBlockB,
                            blockA ? kHeightA : kHeightB);
        }
    }
    mip.updateRegion(0, 0, 8, 8);

    // Levels >= 1: uniform blocks stay uniform level-by-level.
    TEST_REQUIRE(mip.colorCell(1, 0, 0) == kBlockA &&
                     mip.colorCell(1, 1, 0) == kBlockB &&
                     mip.colorCell(1, 1, 1) == kBlockA,
                 "uniform level-1 blocks must keep their colors (%06X %06X %06X).",
                 mip.colorCell(1, 0, 0), mip.colorCell(1, 1, 0),
                 mip.colorCell(1, 1, 1));
    TEST_REQUIRE(mip.heightCell(1, 0, 0) == kHeightA &&
                     mip.heightCell(1, 1, 0) == kHeightB,
                 "uniform level-1 heights wrong: %u %u.",
                 mip.heightCell(1, 0, 0), mip.heightCell(1, 1, 0));

    // Level 2 (dims 2x2): every cell blends 2 A- + 2 B-blocks.
    const std::uint32_t expectedLevel2 =
        expectedMean(kBlockA, kBlockB, kBlockB, kBlockA);
    TEST_REQUIRE(mip.colorCell(2, 0, 0) == expectedLevel2 &&
                     mip.colorCell(2, 1, 1) == expectedLevel2,
                 "level-2 mean wrong: %06X vs %06X.",
                 mip.colorCell(2, 0, 0), expectedLevel2);
    const std::uint32_t expectedLevel3 =
        expectedMean(expectedLevel2, expectedLevel2,
                     expectedLevel2, expectedLevel2);
    TEST_REQUIRE(mip.colorCell(3, 0, 0) == expectedLevel3,
                 "level-3 root mean wrong: %06X vs %06X.",
                 mip.colorCell(3, 0, 0), expectedLevel3);
    const unsigned char expectedHeightLevel2 = static_cast<unsigned char>(
        (kHeightA + kHeightB + kHeightB + kHeightA + 2) >> 2);
    TEST_REQUIRE(mip.heightCell(2, 0, 0) == expectedHeightLevel2,
                 "level-2 height mean wrong: %u vs %u.",
                 mip.heightCell(2, 0, 0), expectedHeightLevel2);

    // --- Incremental edit: one base cell, then updateRegion over it only.
    constexpr int kEditX = 3, kEditY = 3;
    constexpr std::uint32_t kGreen = 0x0000FF00;   // G=255
    constexpr unsigned char kHeightEdit = 0;
    mip.setBaseCell(kEditX, kEditY, kGreen, kHeightEdit);
    mip.updateRegion(kEditX, kEditY, kEditX + 1, kEditY + 1);

    // Level 1 cell containing the edit: 3 red children + green.
    const std::uint32_t level1Mean = expectedMean(
        mip.colorCell(0, 2, 2), mip.colorCell(0, 3, 2),
        mip.colorCell(0, 2, 3), mip.colorCell(0, 3, 3));
    TEST_REQUIRE(mip.colorCell(1, 1, 1) == level1Mean,
                 "incremental level-1 update wrong: %06X vs %06X.",
                 mip.colorCell(1, 1, 1), level1Mean);
    const unsigned char level1Height = static_cast<unsigned char>(
        (kHeightA + kHeightA + kHeightA + kHeightEdit + 2) >> 2);
    TEST_REQUIRE(mip.heightCell(1, 1, 1) == level1Height,
                 "incremental level-1 height wrong: %u vs %u.",
                 mip.heightCell(1, 1, 1), level1Height);

    // Level 2 cell containing it: 3 untouched blocks + the updated one.
    const std::uint32_t level2Mean = expectedMean(
        mip.colorCell(1, 0, 0), mip.colorCell(1, 1, 0),
        mip.colorCell(1, 0, 1), mip.colorCell(1, 1, 1));
    TEST_REQUIRE(mip.colorCell(2, 0, 0) == level2Mean,
                 "incremental level-2 update wrong: %06X vs %06X.",
                 mip.colorCell(2, 0, 0), level2Mean);

    // Untouched cells must not have changed.
    TEST_REQUIRE(mip.colorCell(1, 0, 0) == kBlockA &&
                     mip.colorCell(2, 1, 0) == expectedLevel2,
                 "cells outside the updated rect changed.");
    std::printf("[PASS] pyramid means + incremental update exact.\n");
    return true;
}

} // namespace

namespace {

bool runBilinearSampleTests()
{
    // 4x4 red/blue per-cell checker, asymmetric heights so fractions bite.
    ColorMipmap mip;
    mip.build(4, 4);
    TEST_REQUIRE(mip.levelCount() == 3,
                 "4x4 pyramid must have 3 levels, got %d.", mip.levelCount());
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const bool red = (x + y) % 2 == 0;
            mip.setBaseCell(x, y, red ? kBlockA : kBlockB,
                            static_cast<unsigned char>(x * 40));
        }
    }
    mip.updateRegion(0, 0, 4, 4);

    const int fixed = 1 << 16; // one cell

    // Level 0, exact cell corner: the sampled cell's own value.
    TEST_REQUIRE(mip.sampleColor(0, fixed, 0) == kBlockB &&
                     mip.sampleColor(0, 0, fixed) == kBlockB &&
                     mip.sampleColor(0, 0, 0) == kBlockA,
                 "level-0 exact-corner samples wrong: %06X %06X %06X.",
                 mip.sampleColor(0, fixed, 0), mip.sampleColor(0, 0, fixed),
                 mip.sampleColor(0, 0, 0));

    // Half-cell offset: 50/50 per-channel mix (floor rounding).
    TEST_REQUIRE(mip.sampleColor(0, 2 * fixed + fixed / 2, 0) ==
                     ((127u << 16) | 127u),
                 "level-0 half-cell blend wrong: %06X.",
                 mip.sampleColor(0, 2 * fixed + fixed / 2, 0));

    // Negative coordinates wrap like XCYCL: -1 cell == cell 3.
    TEST_REQUIRE(mip.sampleColor(0, -fixed, 0) ==
                     mip.sampleColor(0, 3 * fixed, 0) &&
                     mip.sampleColor(0, 0, -fixed) ==
                     mip.sampleColor(0, 0, 3 * fixed),
                 "level-0 wrap sampling wrong: %06X vs %06X / %06X vs %06X.",
                 mip.sampleColor(0, -fixed, 0), mip.sampleColor(0, 3 * fixed, 0),
                 mip.sampleColor(0, 0, -fixed), mip.sampleColor(0, 0, 3 * fixed));

    // Heights: 3/4 between cells 1 (40) and 2 (80) → 70, exact floor.
    TEST_REQUIRE(mip.sampleHeight(0, fixed + 3 * fixed / 4, 0) == 70,
                 "level-0 height interpolation wrong: %u.",
                 mip.sampleHeight(0, fixed + 3 * fixed / 4, 0));

    // Level 1: kernel means — cell (0,0) mixes cells (0,0)/(1,0)/(0,1)/(1,1).
    const std::uint32_t level1Mid =
        expectedMean(kBlockA, kBlockB, kBlockB, kBlockA);
    TEST_REQUIRE(mip.colorCell(1, 0, 0) == level1Mid &&
                     mip.sampleColor(1, 0, 0) == level1Mid,
                 "level-1 exact sample wrong: %06X vs %06X.",
                 mip.sampleColor(1, 0, 0), level1Mid);
    // Heights at that level-1 cell: mean of 0,40,0,40 = 20 (with rounding).
    TEST_REQUIRE(mip.heightCell(1, 0, 0) == 20 &&
                     mip.sampleHeight(1, 0, 0) == 20,
                 "level-1 height sample wrong: %u.",
                 mip.sampleHeight(1, 0, 0));
    std::printf("[PASS] bilinear wrap sampling exact.\n");
    return true;
}

} // namespace

bool colorMipmapRunAll()
{
    if (!runPyramidMeanTests()) {
        return false;
    }
    return runBilinearSampleTests();
}

MAP_EDITOR_REGISTER_TEST("ColorMipmap pyramid", 56, colorMipmapRunAll);

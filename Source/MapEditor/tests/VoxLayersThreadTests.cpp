// ============================================================================
// VoxColLayers thread-safety (regression for the in-game
// "subscription out of range" assert).
//
// In the game the LOGIC thread mutates the layer columns while trucks
// terraform (normalizeTransparentColumn -> solidify -> setColumn reallocates
// the vectors), and the RENDER thread concurrently walks the same vectors
// (getColor32 -> lookupSolidBelow, TerraInterface::GetZ, the cavity overlay
// mesher/hash). Without VoxColLayers::accessMtx that interleaving corrupts
// the walks (MSVC Debug: "subscription out of range" in lookupSolidBelow).
//
// This stress drives both roles against one standalone instance: a writer
// thread paints/solidifies random columns while the main thread hammers the
// read API, then verifies the structural invariants survived. Runs
// standalone (order 128, no world needed).
// ============================================================================

#include "TestHarness.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif

#include "VoxLayers.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

bool checkInvariants(const VoxColLayers& l)
{
    // col_ strictly ascending; begin_/end_ parallel; per-column spans
    // within the pool; ranges sorted by z0. (Пул с дырками: begin_ не
    // обязан быть неубывающим, begin_[0] не обязан быть 0.)
    uint64_t liveSum = 0;
    for (size_t i = 1; i < l.col_.size(); ++i) {
        if (l.col_[i] <= l.col_[i - 1]) {
            return false;
        }
    }
    if (l.begin_.size() != l.col_.size() || l.end_.size() != l.col_.size()) {
        return false;
    }
    for (size_t i = 0; i < l.col_.size(); ++i) {
        const uint32_t b = l.begin_[i];
        const uint32_t e = l.end_[i];
        if (b > e || e > l.ranges_.size()) {
            return false;
        }
        liveSum += e - b;
    }
    if (liveSum != l.liveRanges_) {
        return false;
    }
    // Ranges of each column sorted by z0.
    for (size_t i = 0; i < l.col_.size(); ++i) {
        int count = 0;
        const VoxLayerRange* r = l.columnRanges(static_cast<int>(i), &count);
        for (int k = 1; k < count; ++k) {
            if (r[k].z0 <= r[k - 1].z1) {
                return false;
            }
        }
    }
    return true;
}

bool runVoxLayersThreadTests()
{
    VoxColLayers layers;
    const int kColumns = 256;
    const int kStride = 4096;
    // Seed: one solid + one transparent range per column.
    for (int c = 0; c < kColumns; ++c) {
        const std::uint32_t off =
            static_cast<std::uint32_t>(c * kStride + 17);
        layers.paint(off, 10 + (c % 50), 40 + (c % 50), 10, 20, 30, false);
        layers.paint(off, 100, 140, 0, 0, 0, true);
    }
    layers.buildBlockIndex();
    const std::uint64_t versionBefore = layers.version;

    std::atomic<bool> stop{false};
    std::atomic<int> writerOps{0};
    // Writer: the game-logic role (terraforming trucks).
    std::thread writer([&]() {
        std::uint32_t seed = 12345;
        auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 8;
        };
        while (!stop.load(std::memory_order_relaxed)) {
            const int c = static_cast<int>(rnd() % kColumns);
            const std::uint32_t off =
                static_cast<std::uint32_t>(c * kStride + 17);
            const int z0 = 60 + static_cast<int>(rnd() % 80);
            if (rnd() & 1) {
                layers.paint(off, z0, z0 + 20, 1, 2, 3,
                             (rnd() & 1) != 0);
            } else {
                // Same entry point the terraforming normalize uses.
                layers.solidify(off, 0, 250, 40, 40, 40);
            }
            writerOps.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Reader: the render role (tile texturing / mesher walks).
    std::uint32_t seed = 999;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    std::uint64_t lookupHits = 0;
    std::uint64_t solidHits = 0;
    std::uint64_t walked = 0;
    for (int iter = 0; iter < 400000; ++iter) {
        const int c = static_cast<int>(rnd() % kColumns);
        const std::uint32_t off = static_cast<std::uint32_t>(c * kStride + 17);
        const int h = static_cast<int>(rnd() % 300);
        std::uint32_t rgb = 0;
        std::uint8_t flags = 0;
        if (layers.lookup(off, h, &rgb, &flags)) {
            ++lookupHits;
        }
        if (layers.lookupSolidBelow(off, h, &rgb)) {
            ++solidHits;
        }
        layers.isTransparent(off, h);
        const int idx = layers.findColumn(off);
        int count = 0;
        const VoxLayerRange* r = layers.columnRanges(idx, &count);
        for (int k = 0; k < count; ++k) {
            // Raw-pointer walk — the pattern the render mesher uses; it
            // must see a consistent snapshot for the whole iteration.
            walked += (r[k].z0 <= h && h <= r[k].z1) ? 1u : 0u;
        }
        layers.columnHasTransparent(off);
        layers.mightHaveColumn(off);
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    TEST_REQUIRE(writerOps.load() > 0, "writer thread did no work.");
    TEST_REQUIRE(lookupHits > 0 && solidHits > 0 && walked > 0,
                 "readers found no layer data — test setup is wrong.");
    TEST_REQUIRE(layers.version > versionBefore,
                 "mutations did not bump the version counter.");
    TEST_REQUIRE(checkInvariants(layers),
                 "layer store invariants broken after concurrent access.");
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("VoxColLayers concurrent read/write", 128,
                         runVoxLayersThreadTests);

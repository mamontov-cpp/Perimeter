// ============================================================================
// Sculpting brushes — Smooth / Flatten (Dam mode only).
//
// Both are two-pass effects: the reference (average height / least-squares
// plane) is computed over the PRE-edit cells under the mask, then every
// covered cell moves a sculptEffect fraction (0..1) of the way towards its
// target through Terra's own variable-H toolzer — the same mechanism the
// Set Height brush uses.
//
// Checked here on a loaded world (the polygon-test setup pattern), with the
// terrain prepared into exact inclined planes BY THE SCRIPTED BRUSH:
//   1. Smooth effect=1.0 collapses the prepared slope to the pre-edit
//      average (measured), exactly (within toolzer rounding); effect=0.5
//      halves every deviation from the average.
//   2. Flatten effect=1.0 over a perfect plane changes nothing (the fit
//      reproduces the plane — the slope SURVIVES, unlike Smooth); a centre
//      bump is pulled back onto the plane; fraction 0.5 halves the bump.
//   3. Geo mode is refused without touching the world.
//   4. Every stamp (incl. the scripted preparation) is exactly one undo
//      entry; the whole sequence undoes back to the pristine region.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "ScriptedBrush.h"
#include "EditorSession.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMask;
using MapEditor::BrushMasks;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushShape;
using MapEditor::BrushTool;

/// Byte-exact four-buffer snapshot (At_SHADOW masked out — runtime-only).
struct RegionBytes
{
    std::vector<std::uint8_t> geo;
    std::vector<std::uint8_t> dam;
    std::vector<std::uint8_t> atr;
    std::vector<std::uint8_t> sur;

    static RegionBytes read(int x, int y, int w, int h)
    {
        RegionBytes region;
        const std::size_t cells = static_cast<std::size_t>(w) * h;
        region.geo.resize(cells);
        region.dam.resize(cells);
        region.atr.resize(cells);
        region.sur.resize(cells);
        std::size_t cnt = 0;
        for (int j = 0; j < h; ++j) {
            const int row = (y + j) * static_cast<int>(vMap.H_SIZE) + x;
            for (int i = 0; i < w; ++i, ++cnt) {
                const int off = row + i;
                region.geo[cnt] = vMap.VxGBuf[off];
                region.dam[cnt] = vMap.VxDBuf[off];
                region.atr[cnt] = vMap.AtrBuf[off];
                region.sur[cnt] = vMap.SurBuf[off];
            }
        }
        return region;
    }

    bool equals(const RegionBytes& other) const
    {
        if (geo != other.geo || dam != other.dam || sur != other.sur) {
            return false;
        }
        for (std::size_t i = 0; i < atr.size(); ++i) {
            if ((atr[i] & ~0x20) != (other.atr[i] & ~0x20)) {
                return false;
            }
        }
        return true;
    }
};

/// Load a private copy of the temporary test world into its own content root.
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

int wholeAt(int x, int y)
{
    return static_cast<int>(vMap.SGetAlt(x, y)) >> VX_FRACTION;
}

bool runSculptBrushTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_sculpt", session);
    TEST_REQUIRE(loaded,
                 "the sculpt test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the sculpt test.");

    constexpr int kRadius = 6;
    constexpr int kMargin = 12;
    // Low, flat-enough spot with headroom for the prepared planes.
    int cx = 0;
    int cy = 0;
    bool found = false;
    for (int gy = kMargin + 8; gy < mapH - kMargin - 8 && !found; gy += 24) {
        for (int gx = kMargin + 8; gx < mapW - kMargin - 8 && !found;
             gx += 24) {
            int localMax = 0;
            int localMin = MAX_VX_HEIGHT;
            for (int dy = -kMargin - 8; dy <= kMargin + 8; ++dy) {
                for (int dx = -kMargin - 8; dx <= kMargin + 8; ++dx) {
                    const int h = wholeAt(gx + dx, gy + dy);
                    localMax = std::max(localMax, h);
                    localMin = std::min(localMin, h);
                }
            }
            if (localMax < 80 && localMax - localMin < 12) {
                cx = gx;
                cy = gy;
                found = true;
            }
        }
    }
    TEST_REQUIRE(found, "no suitable stamp area for the sculpt test.");

    const int regionX = cx - kMargin;
    const int regionY = cy - kMargin;
    const int regionSide = 2 * kMargin + 1;
    const RegionBytes pristine =
        RegionBytes::read(regionX, regionY, regionSide, regionSide);

    BrushTool brush(session);
    BrushTool scriptBrush(session);

    // Prepares an inclined plane over the working area with the SCRIPTED
    // brush (one undo entry). The exact per-cell result is measured by the
    // caller — overlaps of the 3x3 stamps only shift the plane's phase.
    const auto preparePlane = [&](int base, int slope) {
        BrushParams prep;
        prep.effect = BrushEffect::Script;
        prep.mode = BrushMode::Dam;
        prep.radius = 4;
        prep.script = "var p = getCurrentPos();\n"
                      "for (var dy = -7; dy <= 7; dy++)\n"
                      "  for (var dx = -7; dx <= 7; dx++)\n"
                      "    setHeight({x: p.x + dx, y: p.y + dy}, " +
                      std::to_string(base) + " + " +
                      std::to_string(slope) + " * dx, 1, false);\n";
        std::string error;
        TEST_REQUIRE(MapEditor::runScriptedBrush(session, scriptBrush, prep,
                                                 cx, cy, error),
                     "plane preparation failed: %s", error.c_str());
    };

    const auto sculpt = [&](BrushEffect effect, double fraction) {
        BrushParams params;
        params.effect = effect;
        params.mode = BrushMode::Dam;
        params.shape = BrushShape::FilledCircle;
        params.radius = kRadius;
        params.sculptEffect = fraction;
        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the sculpt stamp was refused.");
    };

    // Mask-cell offsets (relative to the stamp centre) for expectations.
    const std::set<std::pair<int, int>> maskCells = [kRadius] {
        const BrushMask mask = BrushMasks::filledCircle(kRadius);
        std::set<std::pair<int, int>> cells;
        for (int dy = 0; dy < mask.sy; ++dy) {
            for (int dx = 0; dx < mask.sx; ++dx) {
                if (mask.influence[static_cast<std::size_t>(dy) * mask.sx +
                                   dx]) {
                    cells.insert({dx + mask.x, dy + mask.y});
                }
            }
        }
        return cells;
    }();

    const auto measuredHeights = [&]() {
        std::map<std::pair<int, int>, int> heights;
        for (const auto& c : maskCells) {
            heights[c] = wholeAt(cx + c.first, cy + c.second);
        }
        return heights;
    };

    int expectedUndos = 0;

    // ------------------------------------------------------------------
    // 1: Smooth — effect 1.0 collapses the slope to the average; 0.5
    //    halves every deviation.
    // ------------------------------------------------------------------
    constexpr int kBase = 80;
    constexpr int kSlope = 5; // pre(X) = kBase + kSlope*(X+1) — never clamps
    preparePlane(kBase, kSlope);
    ++expectedUndos;

    {
        const auto pre = measuredHeights();
        long long sum = 0;
        for (const auto& [c, h] : pre) {
            sum += h;
        }
        const double avg =
            static_cast<double>(sum) / static_cast<double>(pre.size());
        TEST_REQUIRE(std::abs(pre.at({kRadius, 0}) - pre.at({-kRadius, 0})) >=
                         2 * kSlope - 4,
                     "the fixture slope is missing.");

        sculpt(BrushEffect::Smooth, 1.0);
        ++expectedUndos;
        for (const auto& [c, h] : measuredHeights()) {
            TEST_REQUIRE(std::abs(h - avg) <= 1.5,
                         "Smooth(1.0) must reach the average at (%d,%d): "
                         "expected %.2f, got %d.",
                         c.first, c.second, avg, h);
        }
    }

    // Second area: fraction 0.5 on a fresh plane.
    cx += 64;
    preparePlane(kBase, kSlope);
    ++expectedUndos;
    {
        const auto pre = measuredHeights();
        long long sum = 0;
        for (const auto& [c, h] : pre) {
            sum += h;
        }
        const double avg =
            static_cast<double>(sum) / static_cast<double>(pre.size());
        const int preRight = pre.at({kRadius, 0});

        sculpt(BrushEffect::Smooth, 0.5);
        ++expectedUndos;
        const auto post = measuredHeights();
        for (const auto& [c, h] : post) {
            const double expected = pre.at(c) + (avg - pre.at(c)) * 0.5;
            TEST_REQUIRE(std::abs(h - expected) <= 1.5,
                         "Smooth(0.5) must halve the deviation at (%d,%d): "
                         "expected %.2f, got %d.",
                         c.first, c.second, expected, h);
        }
        // The slope visibly remains (half of it), so this is a blend, not
        // a full flatten.
        TEST_REQUIRE(post.at({kRadius, 0}) - post.at({-kRadius, 0}) >=
                         (preRight - pre.at({-kRadius, 0})) / 2 - 3,
                     "Smooth(0.5) must keep half of the slope.");
    }
    std::printf("[PASS] Smooth reaches the average (1.0) and blends by the "
                "fraction (0.5).\n");

    // ------------------------------------------------------------------
    // 2: Flatten — the fitted plane preserves the slope; bumps die.
    // ------------------------------------------------------------------
    cx += 64;
    preparePlane(kBase, kSlope);
    ++expectedUndos;
    {
        const auto pre = measuredHeights();
        const int preSpread =
            pre.at({kRadius, 0}) - pre.at({-kRadius, 0});
        TEST_REQUIRE(preSpread >= 2 * kSlope - 4,
                     "the fixture slope is missing.");

        sculpt(BrushEffect::Flatten, 1.0);
        ++expectedUndos;
        const auto post = measuredHeights();
        // A perfect plane is reproduced by the fit: nothing moves, the
        // slope SURVIVES (the key difference from Smooth).
        TEST_REQUIRE(post.at({kRadius, 0}) - post.at({-kRadius, 0}) >=
                         preSpread - 3,
                     "Flatten must preserve the large-scale slope.");
        for (const auto& [c, h] : post) {
            TEST_REQUIRE(std::abs(h - pre.at(c)) <= 1,
                         "Flatten(1.0) over a plane must not move (%d,%d): "
                         "%d -> %d.",
                         c.first, c.second, pre.at(c), h);
        }

        // A centre bump is pulled onto the plane.
        BrushParams bump;
        bump.effect = BrushEffect::Script;
        bump.mode = BrushMode::Dam;
        bump.radius = 4;
        bump.script =
            "var p = getCurrentPos();\n"
            "setHeight({x: p.x, y: p.y}, " +
            std::to_string(kBase + 25) + ", 1, false);\n";
        std::string error;
        TEST_REQUIRE(MapEditor::runScriptedBrush(session, scriptBrush, bump,
                                                 cx, cy, error),
                     "bump preparation failed: %s", error.c_str());
        ++expectedUndos;
        const int planeCentre = wholeAt(cx, cy); // == the bump now
        TEST_REQUIRE(planeCentre >= kBase + 20,
                     "the bump was not placed.");

        // Fraction 0.5 halves the bump towards the plane...
        sculpt(BrushEffect::Flatten, 0.5);
        ++expectedUndos;
        const double planeAtCentre =
            static_cast<double>(pre.at({0, 0}));
        TEST_REQUIRE(std::abs(wholeAt(cx, cy) -
                              (planeAtCentre +
                               (kBase + 25 - planeAtCentre) * 0.5)) <= 2.0,
                     "Flatten(0.5) must halve the bump: expected %.2f, got %d.",
                     planeAtCentre + (kBase + 25 - planeAtCentre) * 0.5,
                     wholeAt(cx, cy));

        // ...and 1.0 lands it on the plane.
        sculpt(BrushEffect::Flatten, 1.0);
        ++expectedUndos;
        TEST_REQUIRE(std::abs(wholeAt(cx, cy) - planeAtCentre) <= 2,
                     "Flatten(1.0) must remove the bump: expected %.2f, got %d.",
                     planeAtCentre, wholeAt(cx, cy));
        // The slope is still there.
        TEST_REQUIRE(wholeAt(cx + kRadius, cy) - wholeAt(cx - kRadius, cy) >=
                         preSpread - 5,
                     "Flatten must keep the slope after bump removal.");
    }
    std::printf("[PASS] Flatten preserves the fitted plane and removes "
                "bumps by the fraction.\n");

    // ------------------------------------------------------------------
    // 3: Geo mode is refused without touching the world.
    // ------------------------------------------------------------------
    {
        const RegionBytes before =
            RegionBytes::read(regionX, regionY, regionSide, regionSide);
        const bool undoableBefore = session.canUndo();
        BrushParams geo;
        geo.effect = BrushEffect::Smooth;
        geo.mode = BrushMode::Geo;
        geo.radius = kRadius;
        geo.sculptEffect = 1.0;
        TEST_REQUIRE(!brush.applyStamp(cx, cy, geo),
                     "the sculpt brushes must refuse the Geo mode.");
        TEST_REQUIRE(session.canUndo() == undoableBefore,
                     "a refused sculpt stamp must not record undo.");
        const RegionBytes after =
            RegionBytes::read(regionX, regionY, regionSide, regionSide);
        TEST_REQUIRE(after.equals(before),
                     "a refused sculpt stamp must not touch the world.");
    }
    std::printf("[PASS] the sculpt brushes refuse the Geo mode untouched.\n");

    // ------------------------------------------------------------------
    // 4: clamps + the whole sequence undoes back to the pristine region.
    // ------------------------------------------------------------------
    TEST_REQUIRE(BrushTool::clampSculptEffect(1.5) == 1.0 &&
                     BrushTool::clampSculptEffect(-0.5) == 0.0 &&
                     BrushTool::clampSculptEffect(0.25) == 0.25,
                 "clampSculptEffect must bound the fraction into [0,1].");
    int undos = 0;
    while (session.undo()) {
        ++undos;
    }
    TEST_REQUIRE(undos == expectedUndos,
                 "expected %d undo entries (one per stamp incl. the scripted "
                 "preparation), got %d.",
                 expectedUndos, undos);
    const RegionBytes restored =
        RegionBytes::read(regionX, regionY, regionSide, regionSide);
    TEST_REQUIRE(restored.equals(pristine),
                 "the whole sculpt sequence must undo to the pristine state.");
    std::printf("[PASS] %d undo steps restore the pristine region.\n", undos);

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Sculpt brushes (Smooth / Flatten)", 91,
                         runSculptBrushTests);

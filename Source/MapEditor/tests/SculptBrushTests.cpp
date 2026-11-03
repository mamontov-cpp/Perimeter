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
                      "for (var dy = -12; dy <= 12; dy++)\n"
                      "  for (var dx = -12; dx <= 12; dx++)\n"
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
    // 3: Tricubic — cubic interpolation of a coarse lattice: the inclined
    //    plane is reproduced exactly (cubics preserve lines), fine noise
    //    dies.
    // ------------------------------------------------------------------
    cx += 64;
    preparePlane(kBase, kSlope);
    ++expectedUndos;
    {
        const auto pre = measuredHeights();
        const int preSpread =
            pre.at({kRadius, 0}) - pre.at({-kRadius, 0});

        sculpt(BrushEffect::Tricubic, 1.0);
        ++expectedUndos;
        const auto post = measuredHeights();
        // Block-averaged lattice nodes of a linear field sit exactly on the
        // plane and Catmull-Rom reproduces lines — nothing may move.
        TEST_REQUIRE(post.at({kRadius, 0}) - post.at({-kRadius, 0}) >=
                         preSpread - 3,
                     "Tricubic must preserve the large-scale slope.");
        for (const auto& [c, h] : post) {
            TEST_REQUIRE(std::abs(h - pre.at(c)) <= 2,
                         "Tricubic(1.0) over a plane must not move (%d,%d): "
                         "%d -> %d.",
                         c.first, c.second, pre.at(c), h);
        }
    }

    // Fresh area: a flat pad with a 3x3-block +/-2 checkerboard noise.
    cx += 64;
    {
        BrushParams prep;
        prep.effect = BrushEffect::Script;
        prep.mode = BrushMode::Dam;
        prep.radius = 4;
        prep.script =
            "var p = getCurrentPos();\n"
            "setHeight({x: p.x, y: p.y}, " +
            std::to_string(kBase) + ", 6, false);\n"
            "for (var by = -6; by <= 6; by += 3)\n"
            "  for (var bx = -6; bx <= 6; bx += 3) {\n"
            "    var d = (((bx + by) / 3) % 2 === 0) ? 2 : -2;\n"
            "    setHeight({x: p.x + bx, y: p.y + by}, " +
            std::to_string(kBase) + " + d, 1, false);\n"
            "  }\n";
        std::string error;
        TEST_REQUIRE(MapEditor::runScriptedBrush(session, scriptBrush, prep,
                                                 cx, cy, error),
                     "noise preparation failed: %s", error.c_str());
        ++expectedUndos;
        // The fixture really carries the noise.
        {
            int noisy = 0;
            for (const auto& [c, h] : measuredHeights()) {
                noisy += std::abs(h - kBase) >= 1;
            }
            TEST_REQUIRE(noisy > 0, "the noise fixture is missing.");
        }

        sculpt(BrushEffect::Tricubic, 1.0);
        ++expectedUndos;
        // The lattice averages the +/-2 blocks away and the interpolation
        // stays within a couple of voxels of the flat pad.
        for (const auto& [c, h] : measuredHeights()) {
            TEST_REQUIRE(std::abs(h - kBase) <= 4,
                         "Tricubic must remove the block noise at (%d,%d): "
                         "expected %d +/- 4, got %d.",
                         c.first, c.second, kBase, h);
        }
    }
    std::printf("[PASS] Tricubic preserves the plane and removes block "
                "noise.\n");

    // ------------------------------------------------------------------
    // 4: Inflate / Deflate — vertical normals lift a flat pad by the full
    //    step; at a step edge the displaced parts intersect and the max
    //    (inflate) / min (deflate) height wins.
    // ------------------------------------------------------------------
    cx += 64;
    {
        // The pad must reach beyond the stamp bbox (plus the lateral
        // offset reach) so the rim normals are vertical.
        BrushParams prep;
        prep.effect = BrushEffect::Script;
        prep.mode = BrushMode::Dam;
        prep.radius = 4;
        prep.script = "var p = getCurrentPos();\n"
                      "for (var oy = -4; oy <= 4; oy += 4)\n"
                      "  for (var ox = -4; ox <= 4; ox += 4)\n"
                      "    setHeight({x: p.x + ox, y: p.y + oy}, " +
                      std::to_string(kBase) + ", 6, true);\n";
        std::string error;
        TEST_REQUIRE(MapEditor::runScriptedBrush(session, scriptBrush, prep,
                                                 cx, cy, error),
                     "flat pad preparation failed: %s", error.c_str());
        ++expectedUndos;

        sculpt(BrushEffect::Inflate, 1.0);
        ++expectedUndos;
        for (const auto& [c, h] : measuredHeights()) {
            TEST_REQUIRE(std::abs(h - (kBase + 4)) <= 1,
                         "Inflate(1.0) over a flat pad must lift by the full "
                         "step: expected %d at (%d,%d), got %d.",
                         kBase + 4, c.first, c.second, h);
        }
    }

    // A fresh pad for deflate (the inflated pad would leave a cliff right
    // at the mask rim and tilt the rim normals).
    cx += 64;
    {
        BrushParams prep;
        prep.effect = BrushEffect::Script;
        prep.mode = BrushMode::Dam;
        prep.radius = 4;
        prep.script = "var p = getCurrentPos();\n"
                      "for (var oy = -4; oy <= 4; oy += 4)\n"
                      "  for (var ox = -4; ox <= 4; ox += 4)\n"
                      "    setHeight({x: p.x + ox, y: p.y + oy}, " +
                      std::to_string(kBase) + ", 6, true);\n";
        std::string error;
        TEST_REQUIRE(MapEditor::runScriptedBrush(session, scriptBrush, prep,
                                                 cx, cy, error),
                     "flat pad preparation failed: %s", error.c_str());
        ++expectedUndos;

        sculpt(BrushEffect::Deflate, 1.0);
        ++expectedUndos;
        for (const auto& [c, h] : measuredHeights()) {
            TEST_REQUIRE(std::abs(h - (kBase - 4)) <= 1,
                         "Deflate(1.0) over a flat pad must lower by the full "
                         "step: expected %d at (%d,%d), got %d.",
                         kBase - 4, c.first, c.second, h);
        }
    }

    // Fresh area: a step edge — left half at kBase, right half at +20.
    cx += 64;
    {
        BrushParams prep;
        prep.effect = BrushEffect::Script;
        prep.mode = BrushMode::Dam;
        prep.radius = 4;
        prep.script = "var p = getCurrentPos();\n"
                      "for (var dy = -7; dy <= 7; dy++)\n"
                      "  for (var dx = -7; dx <= 7; dx++) {\n"
                      "    var h = (dx >= 0) ? " +
                      std::to_string(kBase + 20) + " : " +
                      std::to_string(kBase) + ";\n"
                      "    setHeight({x: p.x + dx, y: p.y + dy}, h, 1, "
                      "false);\n"
                      "  }\n";
        std::string error;
        TEST_REQUIRE(MapEditor::runScriptedBrush(session, scriptBrush, prep,
                                                 cx, cy, error),
                     "step preparation failed: %s", error.c_str());
        ++expectedUndos;
        TEST_REQUIRE(wholeAt(cx, cy) == kBase + 20 && wholeAt(cx - 2, cy) == kBase,
                     "the step fixture is missing.");

        // Inflate: the tilted normal at the edge throws the HIGH surface
        // several cells left; max-merge lets it win over the low cells.
        // (The scripted prep's last-writer-wins overlap shifts the edge by
        // one column, so scan the whole low side for the carry.)
        sculpt(BrushEffect::Inflate, 1.0);
        ++expectedUndos;
        int carriedUp = 0;
        for (int dx = -6; dx <= -1; ++dx) {
            carriedUp += wholeAt(cx + dx, cy) >= kBase + 12;
        }
        TEST_REQUIRE(carriedUp > 0,
                     "Inflate must carry the high surface over the low side "
                     "(max wins): no cell on the low side reached >= %d.",
                     kBase + 12);
    }

    // Fresh step for deflate — the inflate above already disturbed the
    // first one.
    cx += 64;
    {
        BrushParams prep;
        prep.effect = BrushEffect::Script;
        prep.mode = BrushMode::Dam;
        prep.radius = 4;
        prep.script = "var p = getCurrentPos();\n"
                      "for (var dy = -7; dy <= 7; dy++)\n"
                      "  for (var dx = -7; dx <= 7; dx++) {\n"
                      "    var h = (dx >= 0) ? " +
                      std::to_string(kBase + 20) + " : " +
                      std::to_string(kBase) + ";\n"
                      "    setHeight({x: p.x + dx, y: p.y + dy}, h, 1, "
                      "false);\n"
                      "  }\n";
        std::string error;
        TEST_REQUIRE(MapEditor::runScriptedBrush(session, scriptBrush, prep,
                                                 cx, cy, error),
                     "step preparation failed: %s", error.c_str());
        ++expectedUndos;

        // Deflate: the low side's -normal points down-and-RIGHT, carrying
        // the LOW surface under the high shelf; min-merge lets it win.
        sculpt(BrushEffect::Deflate, 1.0);
        ++expectedUndos;
        int carriedDown = 0;
        for (int dx = 1; dx <= 6; ++dx) {
            carriedDown += wholeAt(cx + dx, cy) <= kBase + 2;
        }
        TEST_REQUIRE(carriedDown > 0,
                     "Deflate must carry the low surface under the high side "
                     "(min wins): no cell on the high side dropped to <= %d.",
                     kBase + 2);
    }
    std::printf("[PASS] Inflate/Deflate lift flat pads by the step and "
                "resolve intersections by max/min.\n");

    // ------------------------------------------------------------------
    // 5: Gaussian blur — planes reproduce exactly (weights sum to 1);
    //    block noise dies; the radius clamps below the stamp radius.
    // ------------------------------------------------------------------
    cx += 64;
    preparePlane(kBase, kSlope);
    ++expectedUndos;
    {
        const auto pre = measuredHeights();
        const int preSpread =
            pre.at({kRadius, 0}) - pre.at({-kRadius, 0});

        BrushParams blur;
        blur.effect = BrushEffect::GaussianBlur;
        blur.mode = BrushMode::Dam;
        blur.shape = BrushShape::FilledCircle;
        blur.radius = kRadius;
        blur.blurRadius = 2;
        blur.sculptEffect = 1.0;
        TEST_REQUIRE(brush.applyStamp(cx, cy, blur),
                     "the Gaussian-blur stamp was refused.");
        ++expectedUndos;

        const auto post = measuredHeights();
        TEST_REQUIRE(post.at({kRadius, 0}) - post.at({-kRadius, 0}) >=
                         preSpread - 2,
                     "Gaussian blur must preserve the large-scale slope.");
        for (const auto& [c, h] : post) {
            TEST_REQUIRE(std::abs(h - pre.at(c)) <= 1,
                         "Gaussian blur over a plane must not move (%d,%d): "
                         "%d -> %d.",
                         c.first, c.second, pre.at(c), h);
        }
    }

    // Fresh pad with the +/-2 checkerboard noise.
    cx += 64;
    {
        BrushParams prep;
        prep.effect = BrushEffect::Script;
        prep.mode = BrushMode::Dam;
        prep.radius = 4;
        prep.script =
            "var p = getCurrentPos();\n"
            "setHeight({x: p.x, y: p.y}, " +
            std::to_string(kBase) + ", 11, false);\n"
            "for (var by = -6; by <= 6; by += 3)\n"
            "  for (var bx = -6; bx <= 6; bx += 3) {\n"
            "    var d = (((bx + by) / 3) % 2 === 0) ? 2 : -2;\n"
            "    setHeight({x: p.x + bx, y: p.y + by}, " +
            std::to_string(kBase) + " + d, 1, false);\n"
            "  }\n";
        std::string error;
        TEST_REQUIRE(MapEditor::runScriptedBrush(session, scriptBrush, prep,
                                                 cx, cy, error),
                     "noise preparation failed: %s", error.c_str());
        ++expectedUndos;

        // The total deviation from the pad must shrink.
        long long preDev = 0;
        {
            const auto pre = measuredHeights();
            for (const auto& [c, h] : pre) {
                preDev += std::abs(h - kBase);
            }
        }

        BrushParams blur;
        blur.effect = BrushEffect::GaussianBlur;
        blur.mode = BrushMode::Dam;
        blur.shape = BrushShape::FilledCircle;
        blur.radius = kRadius;
        blur.blurRadius = 3;
        blur.sculptEffect = 1.0;
        TEST_REQUIRE(brush.applyStamp(cx, cy, blur),
                     "the Gaussian-blur stamp was refused.");
        ++expectedUndos;

        long long postDev = 0;
        int postMaxDev = 0;
        for (const auto& [c, h] : measuredHeights()) {
            postDev += std::abs(h - kBase);
            postMaxDev = std::max(postMaxDev, std::abs(h - kBase));
        }
        TEST_REQUIRE(postDev < preDev,
                     "Gaussian blur must reduce the total deviation "
                     "(%lld -> %lld).",
                     preDev, postDev);
        TEST_REQUIRE(postMaxDev <= 2,
                     "Gaussian blur must bound the deviation, got %d.",
                     postMaxDev);
    }

    // The blur radius clamps strictly below the stamp radius.
    TEST_REQUIRE(BrushTool::clampBlurRadius(99, kRadius) == kRadius - 1 &&
                     BrushTool::clampBlurRadius(0, kRadius) == 1 &&
                     BrushTool::clampBlurRadius(3, kRadius) == 3,
                 "clampBlurRadius must bound the radius into [1, stamp-1].");
    std::printf("[PASS] Gaussian blur preserves planes, kills block noise, "
                "and clamps its radius below the stamp.\n");

    // ------------------------------------------------------------------
    // 6: Geo mode is refused without touching the world.
    // ------------------------------------------------------------------
    {
        const RegionBytes before =
            RegionBytes::read(regionX, regionY, regionSide, regionSide);
        const bool undoableBefore = session.canUndo();
        for (BrushEffect effect :
             {BrushEffect::Smooth, BrushEffect::Flatten, BrushEffect::Tricubic,
              BrushEffect::Inflate, BrushEffect::Deflate,
              BrushEffect::GaussianBlur}) {
            BrushParams geo;
            geo.effect = effect;
            geo.mode = BrushMode::Geo;
            geo.radius = kRadius;
            geo.sculptEffect = 1.0;
            TEST_REQUIRE(!brush.applyStamp(cx, cy, geo),
                         "the sculpt brushes must refuse the Geo mode.");
        }
        TEST_REQUIRE(session.canUndo() == undoableBefore,
                     "a refused sculpt stamp must not record undo.");
        const RegionBytes after =
            RegionBytes::read(regionX, regionY, regionSide, regionSide);
        TEST_REQUIRE(after.equals(before),
                     "a refused sculpt stamp must not touch the world.");
    }
    std::printf("[PASS] the sculpt brushes refuse the Geo mode untouched.\n");

    // ------------------------------------------------------------------
    // 7: clamps + the whole sequence undoes back to the pristine region.
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

MAP_EDITOR_REGISTER_TEST("Sculpt brushes (Smooth / Flatten / Tricubic / Inflate / Deflate / Gaussian Blur)", 91,
                         runSculptBrushTests);

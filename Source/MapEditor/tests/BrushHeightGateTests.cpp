// ============================================================================
// Brush height-gate modifiers.
//
// Test 92:
//   * the two gate conditions (apply only where the voxel's dam height is
//     ABOVE / BELOW the gate height) restrict ANY Dam-mode brush per VOXEL —
//     one stamp edits the covered cells on the passing side of the gate and
//     leaves their neighbours byte-exact;
//   * the comparison is strict: a voxel exactly at the gate height fails both
//     conditions; a stamp where every voxel fails is REFUSED and records no
//     undo entry;
//   * the gate combines with other effects (raise, absolute set height, the
//     Smooth sculpting pass);
//   * Geo mode ignores the gate (it is a Dam-only modifier).
//
// Runs after the world round-trip (its temporary world copy is reused).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
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
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushShape;
using MapEditor::BrushHeightGate;
using MapEditor::BrushTool;

/// Load a private copy of the temporary test world into its own content root.
bool prepareGateTestSession(const char* subdirName,
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

bool runBrushHeightGateTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareGateTestSession("load_root_gate", session);
    TEST_REQUIRE(loaded,
                 "the height-gate test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the height-gate test.");

    constexpr int kRadius = 4;   // stamp/probe half-size
    constexpr int kMargin = 16;  // fixture reach (step stamps at +/-kRadius)
    // Low, flat-enough spot with headroom for the prepared levels.
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
    TEST_REQUIRE(found, "no suitable stamp area for the height-gate test.");

    BrushTool brush(session);

    const int kLow = 60;
    const int kHigh = 75;
    const int kGate = 70;

    /// Probe heights over the (2*kRadius+1)^2 stamp footprint.
    const auto probe = [&] {
        std::map<std::pair<int, int>, int> heights;
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                heights[{dx, dy}] = wholeAt(cx + dx, cy + dy);
            }
        }
        return heights;
    };

    /// The fixture: a two-level dam pad — columns x < cx at kLow, x >= cx at
    /// kHigh (two absolute-set stamps; the centre column is written last and
    /// lands on kHigh).
    const auto makeStep = [&] {
        for (const auto& [dx, level] :
             {std::pair{-kRadius, kLow}, {kRadius, kHigh}}) {
            BrushParams p;
            p.effect = BrushEffect::AlignVariableH;
            p.mode = BrushMode::Dam;
            p.shape = BrushShape::FilledSquare;
            p.radius = kRadius;
            p.heightLevel = level;
            TEST_REQUIRE(brush.applyStamp(cx + dx, cy, p),
                         "the step fixture stamp was refused.");
        }
        // The fixture must be exact — the gate assertions compare wholes.
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                TEST_REQUIRE(wholeAt(cx + dx, cy + dy) ==
                                 (dx < 0 ? kLow : kHigh),
                             "the step fixture is off at (%d,%d): %d.",
                             dx, dy, wholeAt(cx + dx, cy + dy));
            }
        }
    };

    const auto raiseParams = [&](BrushHeightGate gate, int gateHeight) {
        BrushParams p;
        p.effect = BrushEffect::ChangeHeight;
        p.direction = 1;
        p.mode = BrushMode::Dam;
        p.shape = BrushShape::FilledSquare;
        p.radius = kRadius;
        p.strength = 10;
        p.heightGate = gate;
        p.gateHeight = gateHeight;
        return p;
    };

    // ------------------------------------------------------------------
    // 1: ABOVE gate + raise — only the voxels higher than the gate move,
    //    per voxel; the low half stays byte-exact. Undo restores it all.
    // ------------------------------------------------------------------
    makeStep();
    {
        const auto pre = probe();
        const BrushParams p = raiseParams(BrushHeightGate::Above, kGate);
        const int delta = BrushTool::changeHeightDelta(p, 255) >> VX_FRACTION;
        TEST_REQUIRE(delta > 0, "the raise step must be positive.");
        TEST_REQUIRE(brush.applyStamp(cx, cy, p),
                     "the gated raise stamp was refused.");

        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int expect =
                    pre.at({dx, dy}) + (dx < 0 ? 0 : delta);
                TEST_REQUIRE(wholeAt(cx + dx, cy + dy) == expect,
                             "Above-gate raise must move only the high half "
                             "at (%d,%d): expected %d, got %d.",
                             dx, dy, expect, wholeAt(cx + dx, cy + dy));
            }
        }

        // One stamp = one undo entry restoring the pre-stamp heights.
        TEST_REQUIRE(session.canUndo(), "the gated stamp must be undoable.");
        session.undo();
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                TEST_REQUIRE(wholeAt(cx + dx, cy + dy) == pre.at({dx, dy}),
                             "undo must restore (%d,%d): expected %d, got %d.",
                             dx, dy, pre.at({dx, dy}),
                             wholeAt(cx + dx, cy + dy));
            }
        }
        // Redo re-applies the gated stamp exactly.
        TEST_REQUIRE(session.canRedo(), "the gated stamp must be redoable.");
        session.redo();
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int expect =
                    pre.at({dx, dy}) + (dx < 0 ? 0 : delta);
                TEST_REQUIRE(wholeAt(cx + dx, cy + dy) == expect,
                             "redo must re-apply the gate at (%d,%d).",
                             dx, dy);
            }
        }
        session.undo();
    }

    // ------------------------------------------------------------------
    // 2: BELOW gate + raise — mirror image: only the low half moves.
    // ------------------------------------------------------------------
    cx += 64;
    makeStep();
    {
        const auto pre = probe();
        const BrushParams p = raiseParams(BrushHeightGate::Below, kGate);
        const int delta = BrushTool::changeHeightDelta(p, 255) >> VX_FRACTION;
        TEST_REQUIRE(brush.applyStamp(cx, cy, p),
                     "the Below-gated raise stamp was refused.");
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int expect =
                    pre.at({dx, dy}) + (dx < 0 ? delta : 0);
                TEST_REQUIRE(wholeAt(cx + dx, cy + dy) == expect,
                             "Below-gate raise must move only the low half "
                             "at (%d,%d): expected %d, got %d.",
                             dx, dy, expect, wholeAt(cx + dx, cy + dy));
            }
        }
        session.undo();
    }

    // ------------------------------------------------------------------
    // 3: STRICTNESS + empty-stamp refusal: Below at exactly kLow must not
    //    touch the voxels sitting at kLow (nor the kHigh ones) — the stamp
    //    changes nothing, is refused and records NO undo entry.
    // ------------------------------------------------------------------
    cx += 64;
    makeStep();
    {
        const BrushParams p = raiseParams(BrushHeightGate::Below, kLow);
        TEST_REQUIRE(!brush.applyStamp(cx, cy, p),
                     "a stamp whose voxels all fail the gate must be "
                     "refused.");
        TEST_REQUIRE(session.canUndo(),
                     "the step fixture stamps must be undoable.");
        // The FIRST undo must reach past the gated stamp straight to the
        // second fixture stamp: the centre column (written last by the
        // fixture) must change. If the refused stamp had recorded a spurious
        // empty undo entry, this undo would restore an untouched area and
        // the heights would stay exactly at the fixture levels.
        session.undo();
        bool anyChange = false;
        for (int dy = -kRadius; dy <= kRadius && !anyChange; ++dy) {
            for (int dx = -kRadius; dx <= kRadius && !anyChange; ++dx) {
                if (wholeAt(cx + dx, cy + dy) != (dx < 0 ? kLow : kHigh)) {
                    anyChange = true;
                }
            }
        }
        TEST_REQUIRE(anyChange,
                     "a refused gated stamp must not record an undo entry "
                     "(the first undo must skip it).");
        // The fixture itself is still intact where the second stamp did not
        // write: the far low side stays at kLow.
        TEST_REQUIRE(wholeAt(cx - kRadius, cy) == kLow,
                     "the undo must leave the first fixture stamp alone.");
    }

    // ------------------------------------------------------------------
    // 4: combined with the absolute set-height brush: Above the gate the
    //    cells level to the target, below it they stay untouched.
    // ------------------------------------------------------------------
    cx += 64;
    makeStep();
    {
        BrushParams p;
        p.effect = BrushEffect::AlignVariableH;
        p.mode = BrushMode::Dam;
        p.shape = BrushShape::FilledSquare;
        p.radius = kRadius;
        p.heightLevel = 90;
        p.heightGate = BrushHeightGate::Above;
        p.gateHeight = kGate;
        TEST_REQUIRE(brush.applyStamp(cx, cy, p),
                     "the gated set-height stamp was refused.");
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int expect = dx < 0 ? kLow : 90;
                TEST_REQUIRE(wholeAt(cx + dx, cy + dy) == expect,
                             "the gated set-height must level only the high "
                             "half at (%d,%d): expected %d, got %d.",
                             dx, dy, expect, wholeAt(cx + dx, cy + dy));
            }
        }
        session.undo();
    }

    // ------------------------------------------------------------------
    // 5: combined with the Smooth sculpting pass: the reference is computed
    //    over all covered cells, but only the above-gate voxels move.
    // ------------------------------------------------------------------
    cx += 64;
    makeStep();
    {
        // The covered-cell average (36 low cells, 45 high cells).
        const double avg =
            (36.0 * kLow + 45.0 * kHigh) / 81.0;
        BrushParams p;
        p.effect = BrushEffect::Smooth;
        p.mode = BrushMode::Dam;
        p.shape = BrushShape::FilledSquare;
        p.radius = kRadius;
        p.sculptEffect = 1.0;
        p.heightGate = BrushHeightGate::Above;
        p.gateHeight = kGate;
        TEST_REQUIRE(brush.applyStamp(cx, cy, p),
                     "the gated Smooth stamp was refused.");
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int h = wholeAt(cx + dx, cy + dy);
                if (dx < 0) {
                    TEST_REQUIRE(h == kLow,
                                 "the gated Smooth must not move the low "
                                 "half at (%d,%d): got %d.", dx, dy, h);
                } else {
                    TEST_REQUIRE(std::abs(h - static_cast<int>(std::llround(avg))) <= 1,
                                 "the gated Smooth must pull the high half "
                                 "towards %.2f at (%d,%d): got %d.",
                                 avg, dx, dy, h);
                }
            }
        }
        session.undo();
    }

    // ------------------------------------------------------------------
    // 6: Geo mode IGNORES the gate (Dam-only modifier): both halves rise.
    // ------------------------------------------------------------------
    cx += 64;
    makeStep();
    {
        BrushParams p = raiseParams(BrushHeightGate::Above, kGate);
        p.mode = BrushMode::Geo;
        const int delta = BrushTool::changeHeightDelta(p, 255) >> VX_FRACTION;
        TEST_REQUIRE(brush.applyStamp(cx, cy, p),
                     "the Geo raise stamp was refused.");
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int expect = (dx < 0 ? kLow : kHigh) + delta;
                TEST_REQUIRE(wholeAt(cx + dx, cy + dy) == expect,
                             "the Geo mode must ignore the gate at "
                             "(%d,%d): expected %d, got %d.",
                             dx, dy, expect, wholeAt(cx + dx, cy + dy));
            }
        }
        session.undo();
        session.undo();
        session.undo();
    }

    // The clamps keep wild values in range.
    TEST_REQUIRE(BrushTool::clampGateHeight(-5) == 0 &&
                     BrushTool::clampGateHeight(9999) == MAX_VX_HEIGHT_WHOLE,
                 "clampGateHeight must bound the gate height.");

    std::printf("[PASS] Height gates select voxels per cell, refuse the "
                "all-failing stamp, combine with set/smooth and stay Dam-only.\n");
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Brush height-gate modifiers", 92,
                         runBrushHeightGateTests);

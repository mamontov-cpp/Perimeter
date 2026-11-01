// ============================================================================
// Scripted brush (BrushEffect::Script) — the duktape-driven brush.
//
// Checked here on a loaded world (the polygon-test setup pattern):
//   1. a script exercising the whole API (getCurrent*, raise/lower/set
//      height, setToZP, soot, very light dam, paintColor, circle+square)
//      applies through the normal applyStamp() routing and changes the
//      world; ONE undo step restores the whole evaluation byte-exact
//      (overlapping edits included — the undo-group merge);
//   2. Geo mode, an empty script, a syntax error and a runtime error are
//      refused with clear messages (checkScriptSyntax covers the OK-button
//      gate);
//   3. the 20 s-class time limit aborts an endless loop (tested with a
//      short budget).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ScriptedBrush.h"
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
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushTool;

/// Byte-exact four-buffer snapshot of a region (for the undo check).
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
        // At_SHADOW (0x20) is a runtime-only flag recomputed by the renderer
        // (RenderStr sets it beyond the undo rects along the shadow trail),
        // so it is excluded — the same convention as the world round-trip
        // test.
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

bool runScriptedBrushTests()
{
    MapEditor::EditorSession session;
    const bool loaded = prepareBrushTestSession("load_root_scripted", session);
    TEST_REQUIRE(loaded,
                 "the scripted-brush test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the scripted-brush test.");

    // A flat-enough area with headroom for the raise ops.
    const int radius = 4;
    const int margin = 2 * radius + 10;
    int cx = 0;
    int cy = 0;
    bool found = false;
    for (int gy = margin; gy < mapH - margin && !found; gy += 24) {
        for (int gx = margin; gx < mapW - margin && !found; gx += 24) {
            int localMax = 0;
            for (int dy = -margin; dy <= margin; ++dy) {
                for (int dx = -margin; dx <= margin; ++dx) {
                    localMax = std::max(
                        localMax,
                        static_cast<int>(vMap.SGetAlt(gx + dx, gy + dy)));
                }
            }
            if (localMax + 4096 <= MAX_VX_HEIGHT) {
                cx = gx;
                cy = gy;
                found = true;
            }
        }
    }
    TEST_REQUIRE(found, "no suitable stamp area for the scripted-brush test.");

    BrushTool brush(session);
    BrushParams params;
    params.effect = BrushEffect::Script;
    params.mode = BrushMode::Dam;
    params.radius = radius;
    params.heightLevel = 42;
    params.direction = 1;

    // ------------------------------------------------------------------
    // 1: the full API applies through applyStamp(); one undo restores all.
    // ------------------------------------------------------------------
    params.script = R"JS(
        var p = getCurrentPos();
        var r = getCurrentRadius();
        var t = getCurrentThickness();
        var h = getCurrentHeight();
        if (t !== 0) { throw new Error("unexpected thickness " + t); }
        if (h !== 42) { throw new Error("unexpected height " + h); }
        // Overlapping raises (the undo merge must still restore exactly).
        raiseHeight({x: p.x, y: p.y}, 4, r, false);
        raiseHeight({x: p.x, y: p.y}, 4, r, false);
        // A square pad away from the centre.
        raiseHeight({x: p.x + 2 * r + 4, y: p.y}, 2, 3, true);
        // Absolute set from the UI height value.
        setHeight({x: p.x - 2 * r - 4, y: p.y}, h, 3, false);
        setToZP({x: p.x, y: p.y + 2 * r + 4}, 3, false);
        paintSoot({x: p.x, y: p.y - 2 * r - 4}, 2, false);
        paintVeryLightDam({x: p.x + r + 8, y: p.y + r + 8}, 2, true);
        paintColor({x: p.x - r - 8, y: p.y - r - 8}, 255, 0, 0, 2, false);
    )JS";

    const RegionBytes before =
        RegionBytes::read(cx - margin, cy - margin, 2 * margin + 1,
                          2 * margin + 1);
    const int centerAltBefore =
        static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION;

    TEST_REQUIRE(!session.canUndo(),
                 "a fresh session must start without undo entries.");
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the scripted stamp was refused.");
    TEST_REQUIRE(brush.lastScriptError().empty(),
                 "no error expected, got: %s",
                 brush.lastScriptError().c_str());

    // The overlapping raises moved the centre by 2 * 4 voxels.
    const int centerAltAfter =
        static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION;
    TEST_REQUIRE(centerAltAfter == centerAltBefore + 8,
                 "the centre must rise by 8 voxels, got %d -> %d.",
                 centerAltBefore, centerAltAfter);
    // setHeight landed the target pad at the UI height value.
    const int setX = cx - 2 * radius - 4;
    const int setAlt =
        static_cast<int>(vMap.SGetAlt(setX, cy)) >> VX_FRACTION;
    TEST_REQUIRE(setAlt == params.heightLevel,
                 "setHeight must reach the UI height %d, got %d.",
                 params.heightLevel, setAlt);

    // EXACTLY ONE undo entry for the whole evaluation, and it restores the
    // pre-script state over the whole affected region.
    TEST_REQUIRE(session.canUndo(),
                 "the scripted stamp must create one undo entry.");
    TEST_REQUIRE(session.undo(), "the scripted stamp undo failed.");
    TEST_REQUIRE(!session.canUndo(),
                 "one scripted stamp must be exactly one undo entry.");
    const RegionBytes restored =
        RegionBytes::read(cx - margin, cy - margin, 2 * margin + 1,
                          2 * margin + 1);
    if (!restored.equals(before)) {
        int diffs[4] = {0, 0, 0, 0};
        for (std::size_t i = 0; i < before.geo.size(); ++i) {
            diffs[0] += restored.geo[i] != before.geo[i];
            diffs[1] += restored.dam[i] != before.dam[i];
            diffs[2] += (restored.atr[i] & ~0x20) != (before.atr[i] & ~0x20);
            diffs[3] += restored.sur[i] != before.sur[i];
        }
        std::printf("  debug: geo=%d dam=%d atr=%d sur=%d cells differ\n",
                    diffs[0], diffs[1], diffs[2], diffs[3]);
        for (std::size_t i = 0; i < before.geo.size(); ++i) {
            if (restored.geo[i] != before.geo[i] ||
                restored.dam[i] != before.dam[i] ||
                (restored.atr[i] & ~0x20) != (before.atr[i] & ~0x20) ||
                restored.sur[i] != before.sur[i]) {
                const int dx = static_cast<int>(i % (2 * margin + 1)) - margin;
                const int dy = static_cast<int>(i / (2 * margin + 1)) - margin;
                std::printf("  debug: first diff at (%d,%d): geo %d->%d dam "
                            "%d->%d atr %02x->%02x sur %d->%d\n",
                            dx, dy, before.geo[i], restored.geo[i],
                            before.dam[i], restored.dam[i], before.atr[i],
                            restored.atr[i], before.sur[i], restored.sur[i]);
                break;
            }
        }
    }
    TEST_REQUIRE(restored.equals(before),
                 "one undo must restore the whole scripted evaluation.");
    TEST_REQUIRE(
        static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION ==
            centerAltBefore,
        "the centre height must be back after undo.");
    std::printf("[PASS] the scripted brush applies the full API through "
                "applyStamp; one undo restores everything.\n");

    // ------------------------------------------------------------------
    // 2: refusals with clear messages.
    // ------------------------------------------------------------------
    {
        std::string error;
        BrushParams geo = params;
        geo.mode = BrushMode::Geo;
        TEST_REQUIRE(!MapEditor::runScriptedBrush(session, brush, geo, cx, cy,
                                                  error),
                     "the scripted brush must refuse the Geo mode.");
        TEST_REQUIRE(error.find("Dam") != std::string::npos,
                     "the Geo refusal must mention the Dam mode: %s",
                     error.c_str());

        BrushParams empty = params;
        empty.script.clear();
        TEST_REQUIRE(!MapEditor::runScriptedBrush(session, brush, empty, cx, cy,
                                                  error),
                     "an empty script must be refused.");
        TEST_REQUIRE(!error.empty(), "the empty-script refusal needs a "
                                     "message.");

        TEST_REQUIRE(!MapEditor::checkScriptSyntax("var = ;", error),
                     "a syntax error must fail the OK-button check.");
        TEST_REQUIRE(error.find("SyntaxError") != std::string::npos &&
                         error.find("line") != std::string::npos,
                     "the syntax error must carry the line number: %s",
                     error.c_str());

        BrushParams runtime = params;
        runtime.script = "thisFunctionDoesNotExist();";
        TEST_REQUIRE(!MapEditor::runScriptedBrush(session, brush, runtime, cx,
                                                  cy, error),
                     "a runtime error must fail the stamp.");
        TEST_REQUIRE(error.find("thisFunctionDoesNotExist") != std::string::npos,
                     "the runtime error must name the bad call: %s",
                     error.c_str());
        TEST_REQUIRE(!session.canUndo(),
                     "failed evaluations must not leave undo entries.");
    }
    std::printf("[PASS] Geo mode / empty / syntax / runtime errors are "
                "refused with clear messages.\n");

    // ------------------------------------------------------------------
    // 3: the time limit aborts an endless loop (short test budget).
    // ------------------------------------------------------------------
    {
        std::string error;
        BrushParams loop = params;
        loop.script = "while (true) {}";
        TEST_REQUIRE(!MapEditor::runScriptedBrush(session, brush, loop, cx, cy,
                                                  error, /*timeoutMs=*/200),
                     "an endless loop must be aborted.");
        TEST_REQUIRE(error.find("time limit") != std::string::npos,
                     "the abort must mention the time limit: %s", error.c_str());
    }
    std::printf("[PASS] the execution time limit aborts endless scripts.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Scripted brush (duktape)", 90,
                         runScriptedBrushTests);

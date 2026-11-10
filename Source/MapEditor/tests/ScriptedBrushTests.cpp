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
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::BrushTool;

/// Captures the showInfoMessageBox/showErrorMessageBox texts for the
/// message-hook test (the core is Qt-free, so the test installs this hook
/// instead of a QMessageBox).
std::vector<std::pair<bool, std::string>> g_capturedMessages;

void captureMessageHook(bool isError, const std::string& text)
{
    g_capturedMessages.emplace_back(isError, text);
}

/// Counts the palette-edit hook fires (setScriptPaletteHook takes a plain
/// function pointer, so the counter lives here).
int g_paletteHookFires = 0;
void countPaletteHook()
{
    ++g_paletteHookFires;
}

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
    // Undo coalescing off: these tests expect one undo entry per action.
    session.setUndoCoalescingWindowMs(0);
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

    // ------------------------------------------------------------------
    // 4: the getter/message API: getDamHeight, getGeoColor/getDamColor and
    //    the message-box hook. The script checks the deterministic values
    //    itself (a throw fails the stamp with the message) and writes the
    //    read-back RGB channels into probe cells the C++ side compares
    //    against the palette.
    // ------------------------------------------------------------------
    cx += 64;
    {
        // With no hook installed the message boxes must drop silently.
        MapEditor::setScriptedBrushMessageHook(nullptr);
        BrushParams silent = params;
        silent.script = "showInfoMessageBox('dropped'); "
                        "showErrorMessageBox('dropped');";
        TEST_REQUIRE(brush.applyStamp(cx, cy, silent),
                     "message boxes without a hook must not fail the stamp.");
        TEST_REQUIRE(g_capturedMessages.empty(),
                     "no hook must capture nothing.");

        MapEditor::setScriptedBrushMessageHook(&captureMessageHook);
        g_capturedMessages.clear();

        params.script = R"JS(
            var p = getCurrentPos();
            // A dam pad at a known absolute height + a painted dammed cell.
            setHeight({x: p.x + 8, y: p.y}, 55, 3, false);
            setHeight({x: p.x - 8, y: p.y}, 30, 2, false);
            paintColor({x: p.x - 8, y: p.y}, 255, 0, 0, 2, false);

            if (getDamHeight({x: p.x + 8, y: p.y}) !== 55) {
                throw new Error("getDamHeight must read 55, got " +
                                getDamHeight({x: p.x + 8, y: p.y}));
            }
            if (getDamHeight({x: p.x + 40, y: p.y}) !== 0) {
                throw new Error("getDamHeight on a dam-free cell must be 0");
            }
            var threw = false;
            try { getDamHeight({x: -5, y: 0}); } catch (e) { threw = true; }
            if (!threw) {
                throw new Error("getDamHeight outside the map must throw");
            }
            threw = false;
            try { getGeoColor({x: 1e6, y: 0}); } catch (e) { threw = true; }
            if (!threw) {
                throw new Error("getGeoColor outside the map must throw");
            }

            // The color getters return {r, g, b} numbers.
            var gc = getGeoColor({x: p.x - 8, y: p.y});
            var dc = getDamColor({x: p.x - 8, y: p.y});
            if (typeof gc.r !== "number" || typeof gc.g !== "number" ||
                typeof gc.b !== "number" ||
                typeof dc.r !== "number" || typeof dc.g !== "number" ||
                typeof dc.b !== "number") {
                throw new Error("getGeoColor/getDamColor must return {r,g,b}");
            }

            // Write the read-back channels into probe cells for the C++
            // side (one 3x3 stamp per channel, spaced so no probe is
            // rewritten by a neighbour's stamp).
            setHeight({x: p.x,      y: p.y + 20}, gc.r, 1, true);
            setHeight({x: p.x + 4,  y: p.y + 20}, gc.g, 1, true);
            setHeight({x: p.x + 8,  y: p.y + 20}, gc.b, 1, true);
            setHeight({x: p.x + 16, y: p.y + 20}, dc.r, 1, true);
            setHeight({x: p.x + 20, y: p.y + 20}, dc.g, 1, true);
            setHeight({x: p.x + 24, y: p.y + 20}, dc.b, 1, true);

            showInfoMessageBox("info text from script");
            showErrorMessageBox("error text from script");
        )JS";
        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the getter/message stamp failed: %s",
                     brush.lastScriptError().c_str());

        // The probe cells carry the RGB channels the script read.
        const auto wholeAt = [](int x, int y) {
            return static_cast<int>(vMap.SGetAlt(x, y)) >> VX_FRACTION;
        };
        const int paintOff = cy * mapW + (cx - 8);
        const int index = vMap.SurBuf[paintOff];
        const unsigned int geoRgb = session.paletteRgb(
            MapEditor::EditorSession::PaletteLayer::Geo, index);
        const unsigned int damRgb = session.paletteRgb(
            MapEditor::EditorSession::PaletteLayer::Dam, index);
        TEST_REQUIRE(wholeAt(cx, cy + 20) == ((geoRgb >> 16) & 0xFF) &&
                         wholeAt(cx + 4, cy + 20) == ((geoRgb >> 8) & 0xFF) &&
                         wholeAt(cx + 8, cy + 20) == (geoRgb & 0xFF),
                     "getGeoColor must resolve SurBuf[%d] through the Geo "
                     "palette.", index);
        TEST_REQUIRE(wholeAt(cx + 16, cy + 20) == ((damRgb >> 16) & 0xFF) &&
                         wholeAt(cx + 20, cy + 20) == ((damRgb >> 8) & 0xFF) &&
                         wholeAt(cx + 24, cy + 20) == (damRgb & 0xFF),
                     "getDamColor must resolve SurBuf[%d] through the Dam "
                     "palette.", index);

        // Both message boxes reached the hook, in order, with their modes.
        TEST_REQUIRE(g_capturedMessages.size() == 2,
                     "exactly two message boxes expected, got %d.",
                     static_cast<int>(g_capturedMessages.size()));
        TEST_REQUIRE(!g_capturedMessages[0].first &&
                         g_capturedMessages[0].second == "info text from script",
                     "showInfoMessageBox must arrive as info text.");
        TEST_REQUIRE(g_capturedMessages[1].first &&
                         g_capturedMessages[1].second == "error text from script",
                     "showErrorMessageBox must arrive as error text.");
        MapEditor::setScriptedBrushMessageHook(nullptr);

        // The whole evaluation (fixture + probes) is one undo entry.
        TEST_REQUIRE(session.canUndo(),
                     "the getter stamp must create one undo entry.");
        TEST_REQUIRE(session.undo(), "the getter stamp undo failed.");
        TEST_REQUIRE(wholeAt(cx + 8, cy) != 55,
                     "undo must revert the fixture pad.");
    }
    std::printf("[PASS] getDamHeight/getGeoColor/getDamColor read the world "
                "and the message boxes reach the hook.\n");

    // ------------------------------------------------------------------
    // 5: applySmooth / applyBlur — the sculpting brushes through the API.
    //    The bump is built by a SEPARATE stamp so that undo reverts only
    //    the sculpting op (one stamp = one undo entry).
    //    Heights are computed relative to the geo layer so PutAltDam
    //    actually creates dams (dam is capped at geo, see VMAP.h:855).
    // ------------------------------------------------------------------
    cx += 128;
    {
        // Compute absolute plateau and bump heights from the geo surface.
        const int baseGeo =
            static_cast<int>(vMap.VxGBuf[vMap.offsetBuf(cx, cy)]);
        const int plateauH = baseGeo + 16;
        const int bumpH = baseGeo + 48;
        TEST_REQUIRE(bumpH <= MAX_VX_HEIGHT_WHOLE,
                     "terrain too tall for sculpting test (geo=%d).",
                     baseGeo);

        // A 9x9 plateau at plateauH with a 3x3 bump at bumpH on top. The
        // bump is small enough that both the radius-4 Tricubic reference
        // (lattice stride 4) and a radius-2 Gaussian kernel reach the lower
        // plateau, so one stamp of either brush must pull the bump centre
        // down. applySmooth binds the TRICUBIC sculpting brush.
        BrushParams bump = params;
        bump.script = std::string("var p = getCurrentPos();\n") +
            "setHeight({x: p.x, y: p.y}, " + std::to_string(plateauH) + ", 4, false);\n" +
            "setHeight({x: p.x, y: p.y}, " + std::to_string(bumpH) + ", 1, false);\n" +
            "if (getDamHeight({x: p.x, y: p.y}) !== " + std::to_string(bumpH) + ") {\n" +
            "    throw new Error('bump not set: ' +\n" +
            "                    getDamHeight({x: p.x, y: p.y}));\n" +
            "}\n";
        TEST_REQUIRE(brush.applyStamp(cx, cy, bump),
                     "the bump stamp failed: %s",
                     brush.lastScriptError().c_str());
        TEST_REQUIRE(session.canUndo(), "the bump stamp is one undo entry.");

        // --- applySmooth ---------------------------------------------------
        // The UI sculptEffect is deliberately 1.0 here: the script's own
        // `effect` argument must win over it.
        params.sculptEffect = 1.0;
        params.script = R"JS(
            var p = getCurrentPos();
            applySmooth({x: p.x, y: p.y}, 4, 0.25, false);
        )JS";
        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "applySmooth failed: %s", brush.lastScriptError().c_str());
        const int smoothed25 =
            static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION;
        TEST_REQUIRE(smoothed25 < bumpH && smoothed25 > plateauH,
                     "applySmooth(effect 0.25) must pull the bump towards the "
                     "tricubic reference, got %d (expected plateau %d < x < "
                     "bump %d).",
                     smoothed25, plateauH, bumpH);
        TEST_REQUIRE(session.undo(), "the applySmooth undo failed.");
        TEST_REQUIRE(static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION ==
                         bumpH,
                     "undo must restore the bump after applySmooth.");

        // effect 1.0 reaches the reference in one stamp — strictly closer to
        // the plateau than the 0.25 pass (the argument governs, not the UI).
        params.script = R"JS(
            var p = getCurrentPos();
            applySmooth({x: p.x, y: p.y}, 4, 1.0, false);
        )JS";
        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "applySmooth(1.0) failed: %s",
                     brush.lastScriptError().c_str());
        const int smoothed100 =
            static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION;
        TEST_REQUIRE(smoothed100 < smoothed25,
                     "effect 1.0 must smooth deeper than 0.25, got %d vs %d.",
                     smoothed100, smoothed25);
        TEST_REQUIRE(session.undo(), "the applySmooth(1.0) undo failed.");

        // --- applyBlur -----------------------------------------------------
        params.script = R"JS(
            var p = getCurrentPos();
            applyBlur({x: p.x, y: p.y}, 4, 2, 0.5, false);
        )JS";
        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "applyBlur failed: %s", brush.lastScriptError().c_str());
        const int blurred =
            static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION;
        TEST_REQUIRE(blurred < bumpH && blurred > plateauH,
                     "applyBlur must pull the bump towards the blurred "
                     "height, got %d (expected plateau %d < x < bump %d).",
                     blurred, plateauH, bumpH);
        TEST_REQUIRE(session.undo(), "the applyBlur undo failed.");
        TEST_REQUIRE(static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION ==
                         bumpH,
                     "undo must restore the bump after applyBlur.");

        // A blur kernel that is not smaller than the stamp is clamped the
        // same way the sculpting toolbar clamps it, not rejected.
        params.script = R"JS(
            var p = getCurrentPos();
            applyBlur({x: p.x, y: p.y}, 4, 99, 0.5, false);
        )JS";
        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "an oversized blurRadius must clamp, not fail: %s",
                     brush.lastScriptError().c_str());
        TEST_REQUIRE(static_cast<int>(vMap.SGetAlt(cx, cy)) >> VX_FRACTION <
                         bumpH,
                     "the clamped blur must still smooth the bump.");
        TEST_REQUIRE(session.undo(), "the clamped blur undo failed.");

        // --- argument validation ------------------------------------------
        params.script = "applyBlur({x: 0, y: 0}, 4, 0, 0.5, false);";
        TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                     "a blurRadius below 1 must be refused.");
        TEST_REQUIRE(brush.lastScriptError().find("blurRadius") !=
                         std::string::npos,
                     "the error must name blurRadius: %s",
                     brush.lastScriptError().c_str());

        params.script = "applySmooth({x: 0, y: 0}, 0, 0.5, false);";
        TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                     "a radius below 1 must be refused.");
        TEST_REQUIRE(brush.lastScriptError().find("radius") !=
                         std::string::npos,
                     "the error must name radius: %s",
                     brush.lastScriptError().c_str());

        params.script = "applyBlur({x: 0, y: 0}, 0, 1, 0.5, false);";
        TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                     "applyBlur must validate its radius too.");

        params.script = "applySmooth('not a position', 2, 0.5, false);";
        TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                     "a non-object position must be refused.");
        TEST_REQUIRE(brush.lastScriptError().find("position") !=
                         std::string::npos,
                     "the error must mention the position: %s",
                     brush.lastScriptError().c_str());

        // A non-numeric effect (undefined -> NaN) must be refused.
        params.script = "applySmooth({x: 0, y: 0}, 2, undefined, false);";
        TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                     "a non-finite effect must be refused.");
        TEST_REQUIRE(brush.lastScriptError().find("effect") !=
                         std::string::npos,
                     "the error must mention effect: %s",
                     brush.lastScriptError().c_str());
    }
    std::printf("[PASS] applySmooth/applyBlur sculpt through the scripted API.\n");

    return true;
}

// ---------------------------------------------------------------------------
// SetGeoPalette/SetDamPalette + runEditorScript (the Run Script window path).
// ---------------------------------------------------------------------------

bool runScriptPaletteApiTests()
{
    MapEditor::EditorSession session;
    // Undo coalescing off: these tests expect one undo entry per action.
    session.setUndoCoalescingWindowMs(0);
    const bool loaded = prepareBrushTestSession("load_root_script_palette",
                                                session);
    TEST_REQUIRE(loaded,
                 "the palette-API test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int cx = mapW / 2 + 40;
    const int cy = mapH / 2 + 40;
    MapEditor::BrushTool brush(session);

    // --- 1: runEditorScript runs the SAME API; getCurrentPos reports the ---
    // ---    passed centre; SetGeoPalette/SetDamPalette edit the palettes. ---
    {
        const std::string script =
            "var p = getCurrentPos();\n"
            "if (p.x !== " +
            std::to_string(cx) + " || p.y !== " + std::to_string(cy) +
            ") { throw new Error('wrong current position'); }\n"
            "setGeoPalette(200, {r: 11, g: 22, b: 33});\n"
            "setDamPalette(201, {r: 44, g: 55, b: 66});\n"
            "// a terrain op marking the current position (so undo has data)\n"
            "paintSoot({x: p.x, y: p.y}, 1, false);\n";
        std::string error;
        g_paletteHookFires = 0;
        MapEditor::setScriptPaletteHook(&countPaletteHook);
        TEST_REQUIRE(
            MapEditor::runEditorScript(session, brush, brush.params(), cx, cy,
                                       script, error),
            "runEditorScript refused: %s", error.c_str());
        MapEditor::setScriptPaletteHook(nullptr);
        TEST_REQUIRE(g_paletteHookFires == 2,
                     "each setGeoPalette/setDamPalette must fire the palette "
                     "hook, got %d.", g_paletteHookFires);
        TEST_REQUIRE(session.paletteRgb(
                         MapEditor::EditorSession::PaletteLayer::Geo, 200) ==
                         0x0B1621u,
                     "SetGeoPalette(200) must set 0x0B1621.");
        TEST_REQUIRE(session.paletteRgb(
                         MapEditor::EditorSession::PaletteLayer::Dam, 201) ==
                         0x2C3742u,
                     "SetDamPalette(201) must set 0x2C3742.");
        TEST_REQUIRE(vMap.GeoPal[200 * 3 + 0] == 11 &&
                         vMap.GeoPal[200 * 3 + 1] == 22 &&
                         vMap.GeoPal[200 * 3 + 2] == 33,
                     "vMap.GeoPal[200] must carry the scripted RGB.");
        TEST_REQUIRE((vMap.AtrBuf[cy * mapW + cx] & At_SOOT) == At_SOOT,
                     "the script must see the passed centre via getCurrentPos.");
        // One run = one undo entry; undo reverts the terrain op (palette
        // edits live outside the undo history, like the palette panel).
        TEST_REQUIRE(session.canUndo(),
                     "one run must be one undo entry.");
        TEST_REQUIRE(session.undo(), "undo failed.");
        TEST_REQUIRE((vMap.AtrBuf[cy * mapW + cx] & At_SOOT) != At_SOOT,
                     "undo must revert the scripted soot mark.");
    }
    std::printf("[PASS] runEditorScript: same API, current position, palette "
                "writes, one undo step.\n");

    // --- 2: palette index validation. -------------------------------------
    {
        std::string error;
        TEST_REQUIRE(!MapEditor::runEditorScript(
                         session, brush, brush.params(), cx, cy,
                         "setGeoPalette(300, {r: 0, g: 0, b: 0});", error) &&
                         error.find("0..255") != std::string::npos,
                     "an out-of-range index must fail with a clear error.");
        TEST_REQUIRE(!MapEditor::runEditorScript(
                         session, brush, brush.params(), cx, cy,
                         "setDamPalette(7, 42);", error),
                     "a non-object colour must fail.");
    }
    std::printf("[PASS] palette API validates its arguments.\n");

    // --- 2b: errors carry the script location (brush.js:LINE). -------------
    {
        std::string error;
        TEST_REQUIRE(!MapEditor::runEditorScript(
                         session, brush, brush.params(), cx, cy,
                         "// line 1\nthrow new Error('boom');", error) &&
                         error.find("brush.js:2") != std::string::npos,
                     "a runtime error must carry its location, got: %s",
                     error.c_str());
        TEST_REQUIRE(!MapEditor::runEditorScript(
                         session, brush, brush.params(), cx, cy,
                         "var x = ;", error) &&
                         error.find("brush.js") != std::string::npos,
                     "a syntax error must carry its location, got: %s",
                     error.c_str());
    }
    std::printf("[PASS] runtime and syntax errors carry the script location.\n");

    // --- 3: the scripted-brush stamp shares the new API. -------------------
    {
        BrushParams params;
        params.effect = BrushEffect::Script;
        params.mode = BrushMode::Dam;
        params.radius = 4;
        params.heightLevel = 42;
        params.script =
            "setDamPalette(202, {r: 1, g: 2, b: 3});\n"
            "var p = getCurrentPos();\n"
            "raiseHeight({x: p.x, y: p.y}, 4, 2, false);\n";
        TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                     "the scripted stamp with the palette API was refused.");
        TEST_REQUIRE(session.paletteRgb(
                         MapEditor::EditorSession::PaletteLayer::Dam, 202) ==
                         0x010203u,
                     "the scripted-brush stamp must reach setDamPalette.");
    }
    std::printf("[PASS] the scripted brush shares setGeoPalette/"
                "setDamPalette.\n");

    return true;
}

// ---------------------------------------------------------------------------
// applyHeightFn — the two-pass height-function script API (the seamless
// alternative to overlapping raise/lower stamps, which add up on the
// overlap).
// ---------------------------------------------------------------------------

bool runScriptHeightFnTests()
{
    MapEditor::EditorSession session;
    // Undo coalescing off: these tests expect one undo entry per action.
    session.setUndoCoalescingWindowMs(0);
    const bool loaded = prepareBrushTestSession("load_root_hfn", session);
    TEST_REQUIRE(loaded,
                 "the height-fn test could not load the temporary world.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the height-fn test.");

    // A flat-enough area with headroom (same scan as the scripted test).
    int cx = 0;
    int cy = 0;
    bool found = false;
    for (int gy = 40; gy < mapH - 40 && !found; gy += 24) {
        for (int gx = 40; gx < mapW - 40 && !found; gx += 24) {
            int localMax = 0;
            for (int dy = -20; dy <= 20; ++dy) {
                for (int dx = -20; dx <= 20; ++dx) {
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
    TEST_REQUIRE(found, "no suitable stamp area for the height-fn test.");

    MapEditor::BrushTool brush(session);

    /// The dam surface at a cell in Terra's 1/32 fixed point (0 = no dam).
    const auto damAlt = [&](int x, int y) -> int {
        const int off = y * mapW + x;
        return vMap.VxDBuf[off] != 0
            ? static_cast<int>(vMap.SGetAlt(off))
            : 0;
    };

    // --- 1: every covered cell lands EXACTLY on its function value — the  ---
    // ---    adjacent-cells interference scenario (radius-1-class stamps    ---
    // ---    at distance 1 with different heights): a ramp by x over the    ---
    // ---    5x5 square, checked cell by cell including the shared edges.   ---
    {
        std::string error;
        const std::string script =
            "applyHeightFn({x: " + std::to_string(cx) + ", y: " +
            std::to_string(cy) +
            "}, 2, function(x, y, h) { return 20 + (x - " +
            std::to_string(cx) + ") * 5; }, true);";
        TEST_REQUIRE(
            MapEditor::runEditorScript(session, brush, brush.params(), cx, cy,
                                       script, error),
            "applyHeightFn refused: %s", error.c_str());
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int expected = (20 + dx * 5) << VX_FRACTION;
                TEST_REQUIRE(damAlt(cx + dx, cy + dy) == expected,
                             "cell (%d, %d) must land exactly at %d, got %d.",
                             cx + dx, cy + dy, expected,
                             damAlt(cx + dx, cy + dy));
            }
        }
    }
    std::printf("[PASS] applyHeightFn: adjacent cells land exactly on their "
                "function values (no add-up).\n");

    // --- 2: h is the cell's own PRE-EDIT height — over a slope every cell ---
    // ---    gains exactly 7 (no cell sees a neighbour's half-written       ---
    // ---    value); the fn verifies h itself. The slope is laid first in   ---
    // ---    the SAME script so every covered cell starts from a known h.   ---
    {
        std::string error;
        const std::string script =
            "applyHeightFn({x: " + std::to_string(cx) + ", y: " +
            std::to_string(cy) +
            "}, 3, function(x, y, h) { return 30 + (x - " +
            std::to_string(cx) + "); }, true);\n"
            "applyHeightFn({x: " +
            std::to_string(cx) + ", y: " + std::to_string(cy) +
            "}, 3, function(x, y, h) {\n"
            "  var want = 30 + (x - " +
            std::to_string(cx) +
            ");\n"
            "  if (h !== want) { throw new Error('h=' + h + ' want=' + want); }\n"
            "  return h + 7;\n"
            "}, true);";
        TEST_REQUIRE(
            MapEditor::runEditorScript(session, brush, brush.params(), cx, cy,
                                       script, error),
            "the h-carrying applyHeightFn refused: %s", error.c_str());
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                const int expected = (37 + dx) << VX_FRACTION;
                TEST_REQUIRE(damAlt(cx + dx, cy + dy) == expected,
                             "cell (%d, %d) must be the slope + 7 (%d), got "
                             "%d.",
                             cx + dx, cy + dy, expected,
                             damAlt(cx + dx, cy + dy));
            }
        }
    }
    std::printf("[PASS] applyHeightFn: h carries each cell's pre-edit height "
                "(slope + offset stays exact).\n");

    // --- 3: fractional targets convert to the nearest fixed-point step. ---
    {
        std::string error;
        const std::string script =
            "applyHeightFn({x: " + std::to_string(cx) + ", y: " +
            std::to_string(cy) +
            "}, 1, function(x, y, h) { return 15.5; }, true);";
        TEST_REQUIRE(
            MapEditor::runEditorScript(session, brush, brush.params(), cx, cy,
                                       script, error),
            "the fractional applyHeightFn refused: %s", error.c_str());
        const int expected = (15 << VX_FRACTION) + (1 << (VX_FRACTION - 1));
        TEST_REQUIRE(damAlt(cx, cy) == expected,
                     "15.5 voxels must become exactly 15 + 16/32, got %d.",
                     damAlt(cx, cy));
    }
    std::printf("[PASS] applyHeightFn: fractional heights hit the nearest "
                "fixed-point step.\n");

    // --- 4: a throw aborts the op with NOTHING written and no undo entry. ---
    {
        const int mx = 12;
        const RegionBytes before =
            RegionBytes::read(cx - mx, cy - mx, 2 * mx + 1, 2 * mx + 1);
        const bool couldUndo = session.canUndo();
        std::string error;
        TEST_REQUIRE(!MapEditor::runEditorScript(
                         session, brush, brush.params(), cx, cy,
                         "applyHeightFn({x: " + std::to_string(cx) +
                             ", y: " + std::to_string(cy) +
                             "}, 3, function(x, y, h) {"
                             " throw new Error('boom'); }, true);",
                         error) &&
                         error.find("boom") != std::string::npos,
                     "a throwing fn must abort with its message, got: %s",
                     error.c_str());
        const RegionBytes after =
            RegionBytes::read(cx - mx, cy - mx, 2 * mx + 1, 2 * mx + 1);
        TEST_REQUIRE(after.equals(before),
                     "a throwing fn must leave the world byte-exact.");
        TEST_REQUIRE(session.canUndo() == couldUndo,
                     "a throwing fn must not record an undo entry.");
    }
    std::printf("[PASS] applyHeightFn: a throw aborts with nothing written.\n");

    // --- 5: one run = one undo entry, byte-exact restore. ----------------
    {
        const int mx = 10;
        const RegionBytes before =
            RegionBytes::read(cx - mx, cy - mx, 2 * mx + 1, 2 * mx + 1);
        std::string error;
        TEST_REQUIRE(
            MapEditor::runEditorScript(
                session, brush, brush.params(), cx, cy,
                "applyHeightFn({x: " + std::to_string(cx) + ", y: " +
                    std::to_string(cy) +
                    "}, 4, function(x, y, h) {"
                    " return 50 - Math.abs(x - " +
                    std::to_string(cx) + ") - Math.abs(y - " +
                    std::to_string(cy) + "); }, true);",
                error),
            "the pyramid applyHeightFn refused: %s", error.c_str());
        TEST_REQUIRE(session.canUndo(), "one run must be one undo entry.");
        TEST_REQUIRE(session.undo(), "undo failed.");
        const RegionBytes restored =
            RegionBytes::read(cx - mx, cy - mx, 2 * mx + 1, 2 * mx + 1);
        TEST_REQUIRE(restored.equals(before),
                     "undo must restore the applyHeightFn region byte-exact.");
    }
    std::printf("[PASS] applyHeightFn: one undo entry restores the region "
                "byte-exact.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Scripted brush (duktape)", 90,
                         runScriptedBrushTests);
MAP_EDITOR_REGISTER_TEST("Script palette API + Run Script", 99,
                         runScriptPaletteApiTests);
MAP_EDITOR_REGISTER_TEST("applyHeightFn (script height function)", 105,
                         runScriptHeightFnTests);

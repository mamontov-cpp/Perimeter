// ============================================================================
// DIAGNOSTIC (not a gate): replicates the game's terrain color pipeline.
//
// The game colors terrain tiles through vMap.getColor32() after
// vMap.WorldRender() (Universe.cpp) — both linked from the same Terra
// sources. This test runs exactly that pair against the real worlds and
// prints the average game-color of dam vs no-dam cells, to explain why
// EXTDEMO's undammed geo looks dimmed in game while MCPDEMO's does not.
// Always passes; the output is the evidence.
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

#include <cstdio>
#include <string>

// Terra's vMap.prepare() calls check_command_line() which requires the
// argument vector initialized (the game does it at Runtime.cpp; the test
// runner does not — WorldRoundTripTests does it for the full suite, but
// this diagnostic must work under --filter alone).
void setup_argcv(int argc, char* argv[]);

namespace {

bool inspectWorld(const std::string& name)
{
    MapEditor::EditorSession session;
    session.setUndoCoalescingWindowMs(0);
    MapEditor::setupErrHWrapper();
    setup_argcv(0, nullptr);
    if (!session.setGameDir(".")) {
        std::printf("[DIAG] %s: setGameDir failed\n", name.c_str());
        return true;
    }
    if (!session.scanWorlds()) {
        std::printf("[DIAG] %s: scanWorlds failed\n", name.c_str());
        return true;
    }
    if (!session.loadWorldByName(name)) {
        std::printf("[DIAG] %s: load failed\n", name.c_str());
        return true;
    }

    vMap.WorldRender();

    const int w = static_cast<int>(vMap.H_SIZE);
    const int h = static_cast<int>(vMap.V_SIZE);
    unsigned long long damR = 0, damG = 0, damB = 0, damN = 0;
    unsigned long long geoR = 0, geoG = 0, geoB = 0, geoN = 0;
    unsigned long long geoLvR = 0, geoLvG = 0, geoLvB = 0, geoLvN = 0;
    unsigned long long geoRawR = 0, geoRawG = 0, geoRawB = 0, geoRawN = 0;
    unsigned long long geoBelowZp = 0, geoAboveZp = 0;
    unsigned long long rnrSum = 0;
    unsigned long long rnrN = 0;
    // Sparse regular sample — enough for stable averages on big maps.
    const int step = w / 512 + 1;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            const int off = y * w + x;
            const unsigned int c = vMap.getColor32(off);
            const unsigned char r = c & 0xFF;
            const unsigned char g = (c >> 8) & 0xFF;
            const unsigned char b = (c >> 16) & 0xFF;
            if (vMap.VxDBuf[off]) {
                damR += r; damG += g; damB += b; ++damN;
            } else {
                geoR += r; geoG += g; geoB += b; ++geoN;
                if (vMap.AtrBuf[off] & 0x80) { // At_LEVELED
                    geoLvR += r; geoLvG += g; geoLvB += b; ++geoLvN;
                } else {
                    geoRawR += r; geoRawG += g; geoRawB += b; ++geoRawN;
                }
                if ((vMap.SGetAlt(off) >> 5) <= vMap.hZeroPlast) {
                    ++geoBelowZp;
                } else {
                    ++geoAboveZp;
                }
            }
            rnrSum += vMap.RnrBuf[off];
            ++rnrN;
        }
    }
    std::printf("[DIAG] %s: hZeroPlast=%d  sampled no-dam=%llu (leveled=%llu) dam=%llu\n",
                name.c_str(), vMap.hZeroPlast, geoN, geoLvN, damN);
    if (geoN) {
        std::printf("[DIAG] %s: GAME COLOR no-dam avg=(%llu,%llu,%llu)  "
                    "below-ZP=%llu above-ZP=%llu\n",
                    name.c_str(), geoR / geoN, geoG / geoN, geoB / geoN,
                    geoBelowZp, geoAboveZp);
    }
    if (geoLvN) {
        std::printf("[DIAG] %s: GAME COLOR no-dam LEVELED avg=(%llu,%llu,%llu)\n",
                    name.c_str(), geoLvR / geoLvN, geoLvG / geoLvN, geoLvB / geoLvN);
    }
    if (geoRawN) {
        std::printf("[DIAG] %s: GAME COLOR no-dam PLAIN  avg=(%llu,%llu,%llu)  "
                    "SurBuf~0 count check\n",
                    name.c_str(), geoRawR / geoRawN, geoRawG / geoRawN, geoRawB / geoRawN);
    }
    if (damN) {
        std::printf("[DIAG] %s: GAME COLOR dam   avg=(%llu,%llu,%llu)\n",
                    name.c_str(), damR / damN, damG / damN, damB / damN);
    }
    std::printf("[DIAG] %s: RnrBuf avg=%llu (geo half<128, dam half>=128)\n",
                name.c_str(), rnrSum / rnrN);
    // Focused dump: first few no-dam cells + the table entries themselves.
    {
        int shown = 0;
        const int step2 = w / 512 + 1;
        for (int y = 0; y < h && shown < 6; y += step2) {
            for (int x = 0; x < w && shown < 6; x += step2) {
                const int off = y * w + x;
                if (vMap.VxDBuf[off]) {
                    continue;
                }
                std::printf("[DIAG] %s cell(%d,%d): VxD=%d Sur=%d Rnr=%d color=%06X  "
                            "Sur2Col32[Sur][64]=%06X [Sur][192]=%06X  "
                            "GeoPal[Sur]=(%d,%d,%d) DamPal[Sur]=(%d,%d,%d)\n",
                            name.c_str(), x, y, vMap.VxDBuf[off], vMap.SurBuf[off],
                            vMap.RnrBuf[off], vMap.getColor32(off) & 0xFFFFFF,
                            vMap.Sur2Col32[vMap.SurBuf[off]][64],
                            vMap.Sur2Col32[vMap.SurBuf[off]][192],
                            vMap.GeoPal[vMap.SurBuf[off]*3+0],
                            vMap.GeoPal[vMap.SurBuf[off]*3+1],
                            vMap.GeoPal[vMap.SurBuf[off]*3+2],
                            vMap.DamPal[vMap.SurBuf[off]*3+0],
                            vMap.DamPal[vMap.SurBuf[off]*3+1],
                            vMap.DamPal[vMap.SurBuf[off]*3+2]);
                ++shown;
            }
        }
    }
    session.unloadWorld();
    return true;
}

bool findGreyGeo()
{
    // Where are the no-dam cells whose RnrBuf reached the DAM half (>=128)?
    // Those render through the white DamPal as "grey shades".
    MapEditor::EditorSession session;
    session.setUndoCoalescingWindowMs(0);
    MapEditor::setupErrHWrapper();
    setup_argcv(0, nullptr);
    if (!session.setGameDir(".") || !session.scanWorlds() ||
        !session.loadWorldByName("EXTDEMO")) {
        std::printf("[DIAG] grey-geo: load failed\n");
        return true;
    }
    vMap.WorldRender();
    const int w = static_cast<int>(vMap.H_SIZE);
    const int h = static_cast<int>(vMap.V_SIZE);
    const int PX = 465, PY = 1934;
    const int off = PY * w + PX;
    std::printf("[DIAG] EXTDEMO(%d,%d): VxG=%d VxD=%d Atr=%02X Sur=%d "
                "h=%d hZeroPlast=%d Rnr=%d color=%06X\n",
                PX, PY, vMap.VxGBuf[off], vMap.VxDBuf[off], vMap.AtrBuf[off],
                vMap.SurBuf[off], vMap.SGetAlt(off) >> 5, vMap.hZeroPlast,
                vMap.RnrBuf[off], vMap.getColor32(off) & 0xFFFFFF);
    // A small neighbourhood average (the in-game pixel covers ~4x4 cells).
    unsigned long long r = 0, g = 0, b = 0, n = 0;
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int o2 = ((PY + dy + vMap.V_SIZE) % vMap.V_SIZE) * w +
                           ((PX + dx + vMap.H_SIZE) % vMap.H_SIZE);
            const unsigned int c = vMap.getColor32(o2);
            r += c & 0xFF; g += (c >> 8) & 0xFF; b += (c >> 16) & 0xFF; ++n;
        }
    }
    std::printf("[DIAG] EXTDEMO(%d,%d) 9x9 avg: RGB=(%llu,%llu,%llu)  "
                "(BGR hex %02X%02X%02X)\n",
                PX, PY, b / n, g / n, r / n, b / n, g / n, r / n);
    unsigned long long grey = 0, greyShadow = 0, greyLev = 0, tan = 0;
    int firstX = -1, firstY = -1;
    const int step = w / 1024 + 1;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            const int off2 = y * w + x;
            if (vMap.VxDBuf[off2]) {
                continue;
            }
            if (vMap.RnrBuf[off2] >= 128) {
                ++grey;
                if (vMap.AtrBuf[off2] & 0x20) ++greyShadow;
                if (vMap.AtrBuf[off2] & 0x80) ++greyLev;
                if (firstX < 0) { firstX = x; firstY = y; }
            } else {
                ++tan;
            }
        }
    }
    std::printf("[DIAG] EXTDEMO no-dam sampled: tan-half=%llu grey-half=%llu "
                "(of them At_SHADOW=%llu At_LEVELED=%llu) firstGrey=(%d,%d)\n",
                tan, grey, greyShadow, greyLev, firstX, firstY);
    if (firstX >= 0) {
        const int off2 = firstY * w + firstX;
        std::printf("[DIAG] firstGrey cell: Sur=%d Atr=%02X VxG=%d h=%d\n",
                    vMap.SurBuf[off2], vMap.AtrBuf[off2], vMap.VxGBuf[off2],
                    vMap.SGetAlt(off2) >> 5);
    }
    session.unloadWorld();
    return true;
}

bool runDiagnostic()
{
    inspectWorld("EXTDEMO");
    inspectWorld("MCPDEMO");
    findGreyGeo();
    std::printf("[PASS] game-color diagnostic printed.\n");
    return true;
}

MAP_EDITOR_REGISTER_TEST("GameColorDiagnostic", 999, runDiagnostic);

} // namespace

// ============================================================================
// T4.4 — extended (>256) surface palette: sidecar save/load, painting,
// selective rendering and the vanilla-world guarantees.
//
//   * addPaletteColor()/setExtendedPaletteRgb()/paletteRgb() manage the
//     global extended index space (>=256, Geo segment first);
//   * rebuildExtTables() fills ExtSur2Col32 with the same lighting-gamma
//     formula the base palettes use;
//   * the SetColor brush paints extended indices into SurExtBuf with the
//     same per-layer gating as base colors (ext index refuses the other
//     layer's cells), getHeight buffers stay untouched;
//   * undo/redo restores SurExtBuf bytes through the area snapshots;
//   * setColor32/getColor32 resolve extended cells (surfaceCol32);
//   * saveWorld() writes the output.esur sidecar (magic/version/palette/RLE
//     runs) and reloading the world reproduces colors, entries and painted
//     cells; a world without extended data saves without the sidecar.
//
// Runs after the palette ops (its temporary world copy is created anew).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "BrushTool.h"
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::EditorSession;
using Layer = EditorSession::PaletteLayer;

int mapOffset(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

/// The color the extended table must contain for one lighting step j
/// (identical formula to the base palettes — rebuildExtTables mirrors
/// vrtMap::convertPal2TableSurCol, VMAP.cpp:1364-1372).
unsigned int expectedExtCol32(unsigned char r, unsigned char g,
                              unsigned char b, int j)
{
    const double Lum = 1.5;
    const double Lum0 = 0.05;
    const double dLum = (Lum - Lum0) / MAX_SURFACE_LIGHTING;
    const double w = Lum0 + dLum * j;
    auto sat = [](double v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    const int cb = static_cast<int>(std::lround(sat(b * w)));
    const int cg = static_cast<int>(std::lround(sat(g * w)));
    const int cr = static_cast<int>(std::lround(sat(r * w)));
    return static_cast<unsigned int>(cb) |
           (static_cast<unsigned int>(cg) << 8) |
           (static_cast<unsigned int>(cr) << 16);
}

/// True when every influenced cell of a radius-`radius` filled circle centred
/// at (cx,cy) lies in the requested layer (dam present / absent).
bool patchInLayer(int cx, int cy, int radius, bool wantDam)
{
    const MapEditor::BrushMask stamp = MapEditor::BrushMasks::filledCircle(radius);
    for (int dy = stamp.y; dy < stamp.y + stamp.sy; ++dy) {
        for (int dx = stamp.x; dx < stamp.x + stamp.sx; ++dx) {
            if (stamp.influence[static_cast<std::size_t>(dy - stamp.y) *
                                    stamp.sx +
                                (dx - stamp.x)] == 0) {
                continue;
            }
            const bool isDam = vMap.VxDBuf[mapOffset(cx + dx, cy + dy)] != 0;
            if (isDam != wantDam) {
                return false;
            }
        }
    }
    return true;
}

/// Find the point closest to (cxc,cyc) whose radius-`radius` circle mask lies
/// entirely inside one layer.
bool findLayerPatch(int cxc, int cyc, int radius, bool wantDam,
                    int& outX, int& outY)
{
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    long best = -1;
    for (int y = 32; y < mapH - 32 - radius; y += 4) {
        for (int x = 32; x < mapW - 32 - radius; x += 4) {
            if (!patchInLayer(x, y, radius, wantDam)) {
                continue;
            }
            const long dist = static_cast<long>(x - cxc) * (x - cxc) +
                              static_cast<long>(y - cyc) * (y - cyc);
            if (best < 0 || dist < best) {
                best = dist;
                outX = x;
                outY = y;
            }
        }
    }
    return best >= 0;
}

/// Deserialize the sidecar from disk (little-endian layout — see
/// vrtMap::saveExtendedSurface). Returns false on any structural mismatch.
bool readSidecar(const fs::path& path, unsigned int& kGeo, unsigned int& kDam,
                 std::vector<unsigned char>& rgb,
                 std::vector<std::pair<std::pair<unsigned int, unsigned int>,
                                       unsigned short>>& runs)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char id[4] = {0, 0, 0, 0};
    in.read(id, 4);
    unsigned int version = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!in || std::strncmp(id, "ESUR", 4) != 0 || version != 1) {
        return false;
    }
    in.read(reinterpret_cast<char*>(&kGeo), sizeof(kGeo));
    unsigned int kDamLocal = 0;
    in.read(reinterpret_cast<char*>(&kDamLocal), sizeof(kDamLocal));
    unsigned int runCount = 0;
    in.read(reinterpret_cast<char*>(&runCount), sizeof(runCount));
    if (!in || kGeo > 0x10000 || kDamLocal > 0x10000) {
        return false;
    }
    kDam = kDamLocal;
    rgb.resize(static_cast<size_t>(kGeo + kDam) * 3);
    if (!rgb.empty()) {
        in.read(reinterpret_cast<char*>(&rgb[0]),
                static_cast<std::streamsize>(rgb.size()));
    }
    for (unsigned int i = 0; i < runCount && in; ++i) {
        unsigned int off = 0, len = 0;
        unsigned short idx = 0;
        in.read(reinterpret_cast<char*>(&off), sizeof(off));
        in.read(reinterpret_cast<char*>(&len), sizeof(len));
        in.read(reinterpret_cast<char*>(&idx), sizeof(idx));
        if (!in) {
            return false;
        }
        runs.emplace_back(std::make_pair(off, len), idx);
    }
    return !!in;
}

} // namespace

bool runExtendedPaletteTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Self-contained content root.
    const fs::path loadRoot = ctx.tempRoot / "load_root_esur";
    const fs::path worldDir =
        loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName);
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the extended-palette content root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx",
            loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini",
                      loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");
        MapEditor::Tests::copyDirectoryRecursive(ctx.tempWorldDir, worldDir);
    }

    EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 256 && mapH >= 256,
                 "the world is too small for the extended-palette test.");

    // ------------------------------------------------------------------
    // 65.1: a fresh world carries no extended data and no sidecar.
    // ------------------------------------------------------------------
    TEST_REQUIRE(!vMap.hasExtendedSurface(),
                 "a vanilla world must not report extended surface data.");
    TEST_REQUIRE(session.extendedPaletteSize(Layer::Geo) == 0,
                 "a vanilla world must have no extended Geo entries.");
    TEST_REQUIRE(session.extendedPaletteSize(Layer::Dam) == 0,
                 "a vanilla world must have no extended Dam entries.");
    TEST_REQUIRE(!fs::exists(worldDir / "output.esur"),
                 "a vanilla world folder must not contain a sidecar.");

    // ------------------------------------------------------------------
    // 65.2: extending, painting with selective rendering, undo/redo.
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.addPaletteColor(Layer::Geo, 0x123456) == 256,
                 "the first extended Geo entry must get index 256.");
    TEST_REQUIRE(session.addPaletteColor(Layer::Dam, 0xFEDCBA) == 257,
                 "the first extended Dam entry must get index 257 "
                 "(the global index space follows the Geo segment).");
    TEST_REQUIRE(session.paletteRgb(Layer::Geo, 256) == 0x123456,
                 "the extended Geo entry did not read back.");
    TEST_REQUIRE(session.paletteRgb(Layer::Dam, 257) == 0xFEDCBA,
                 "the extended Dam entry did not read back.");
    TEST_REQUIRE(session.paletteRgb(Layer::Dam, 256) == 0,
                 "a Geo-segment index must be invisible in the Dam layer.");
    TEST_REQUIRE(session.extendedPaletteSize(Layer::Geo) == 1 &&
                     session.extendedPaletteSize(Layer::Dam) == 1,
                 "the extended entry counts did not read back.");
    TEST_REQUIRE(session.hasUnsavedChanges(),
                 "appending a custom entry must count as an unsaved change.");

    // The extended table mirrors the base lighting-gamma formula. Column
    // layout matches Sur2Col32: the layer bit (bit 7) is in the column, the
    // lighting index j is column & 127, and both column halves carry the
    // same ramp (an extended index belongs to one layer only).
    for (int c : {0, 64, 127, 128, 200, 255}) {
        const int j = c & (MAX_SURFACE_LIGHTING - 1);
        TEST_REQUIRE(vMap.extColor32(256, c) ==
                         expectedExtCol32(0x12, 0x34, 0x56, j),
                     "extColor32(256) column %d does not match the gamma "
                     "formula.",
                     c);
        TEST_REQUIRE(vMap.extColor32(257, c) ==
                         expectedExtCol32(0xFE, 0xDC, 0xBA, j),
                     "extColor32(257) column %d does not match the gamma "
                     "formula.",
                     c);
    }

    // Paint: one extended Geo stamp, one extended Dam stamp, plus a refused
    // cross-layer stamp.
    const int cx = mapW / 2;
    const int cy = mapH / 2;
    const int radius = 3;
    int geoX = cx, geoY = cy, damX = cx, damY = cy;
    TEST_REQUIRE(findLayerPatch(cx, cy, radius, false, geoX, geoY),
                 "no all-geo patch found for the extended-palette test.");
    TEST_REQUIRE(findLayerPatch(cx, cy, radius, true, damX, damY),
                 "no all-dam patch found for the extended-palette test.");

    // Capture SurBuf bytes over both stamp regions — extended paint must not
    // touch the base index.
    const int half = radius + 2;
    const int side = 2 * half + 1;
    std::vector<unsigned char> geoSurBefore(
        static_cast<size_t>(side) * side), damSurBefore(
        static_cast<size_t>(side) * side);
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            geoSurBefore[static_cast<size_t>(j) * side + i] =
                vMap.SurBuf[mapOffset(geoX - half + i, geoY - half + j)];
            damSurBefore[static_cast<size_t>(j) * side + i] =
                vMap.SurBuf[mapOffset(damX - half + i, damY - half + j)];
        }
    }

    MapEditor::BrushTool brush(session);
    MapEditor::BrushParams params;
    params.shape = MapEditor::BrushShape::FilledCircle;
    params.radius = radius;
    params.strength = 30;
    params.mode = MapEditor::BrushMode::Geo;
    params.effect = MapEditor::BrushEffect::SetColor;
    params.colorIndex = 256; // extended Geo entry
    TEST_REQUIRE(brush.applyStamp(geoX, geoY, params),
                 "the extended Geo stamp was refused.");
    {
        const MapEditor::BrushMask stamp =
            MapEditor::BrushMasks::filledCircle(radius);
        for (int dy = stamp.y; dy < stamp.y + stamp.sy; ++dy) {
            for (int dx = stamp.x; dx < stamp.x + stamp.sx; ++dx) {
                if (stamp.influence[static_cast<std::size_t>(dy - stamp.y) *
                                        stamp.sx +
                                    (dx - stamp.x)] == 0) {
                    continue;
                }
                const int off = mapOffset(geoX + dx, geoY + dy);
                if (vMap.SurExtBuf[off] != 256) {
                    TEST_REQUIRE(false,
                                 "the Geo stamp must set SurExtBuf=256 at "
                                 "off %d (got %u).",
                                 off, static_cast<unsigned>(vMap.SurExtBuf[off]));
                }
                const int li = dx + half;
                const int lj = dy + half;
                TEST_REQUIRE(vMap.SurBuf[off] ==
                                 geoSurBefore[static_cast<size_t>(lj) * side +
                                              li],
                             "the extended paint must leave SurBuf byte %d "
                             "untouched.",
                             off);
            }
        }
    }

    // Undo/redo must round-trip the extended index bytes.
    const int probeOff = mapOffset(geoX, geoY + 1);
    TEST_REQUIRE(session.undo(), "undo of the extended stamp failed.");
    TEST_REQUIRE(vMap.SurExtBuf[probeOff] == 0,
                 "undo must clear the extended index back to the base "
                 "palette (got %u).",
                 static_cast<unsigned>(vMap.SurExtBuf[probeOff]));
    TEST_REQUIRE(session.redo(), "redo of the extended stamp failed.");
    TEST_REQUIRE(vMap.SurExtBuf[probeOff] == 256,
                 "redo must restore the extended index (got %u).",
                 static_cast<unsigned>(vMap.SurExtBuf[probeOff]));
    TEST_REQUIRE(session.isExtendedDirty(),
                 "an undo swap touching extended cells must flag the "
                 "sidecar dirty.");

    // Extended Dam paint + cross-layer refusal.
    params.mode = MapEditor::BrushMode::Dam;
    params.colorIndex = 257; // extended Dam entry
    TEST_REQUIRE(brush.applyStamp(damX, damY, params),
                 "the extended Dam stamp was refused.");
    {
        const MapEditor::BrushMask stamp =
            MapEditor::BrushMasks::filledCircle(radius);
        for (int dy = stamp.y; dy < stamp.y + stamp.sy; ++dy) {
            for (int dx = stamp.x; dx < stamp.x + stamp.sx; ++dx) {
                if (stamp.influence[static_cast<std::size_t>(dy - stamp.y) *
                                        stamp.sx +
                                    (dx - stamp.x)] == 0) {
                    continue;
                }
                const int off = mapOffset(damX + dx, damY + dy);
                TEST_REQUIRE(vMap.SurExtBuf[off] == 257,
                             "the Dam stamp must set SurExtBuf=257 (off %d).",
                             off);
            }
        }
    }
    params.colorIndex = 256; // Geo entry onto the dam patch — must be refused
    TEST_REQUIRE(brush.applyStamp(damX, damY, params),
                 "the refused cross-layer stamp must still return true.");
    {
        const MapEditor::BrushMask stamp =
            MapEditor::BrushMasks::filledCircle(radius);
        for (int dy = stamp.y; dy < stamp.y + stamp.sy; ++dy) {
            for (int dx = stamp.x; dx < stamp.x + stamp.sx; ++dx) {
                if (stamp.influence[static_cast<std::size_t>(dy - stamp.y) *
                                        stamp.sx +
                                    (dx - stamp.x)] == 0) {
                    continue;
                }
                const int off = mapOffset(damX + dx, damY + dy);
                TEST_REQUIRE(vMap.SurExtBuf[off] == 257,
                             "a Geo-segment paint must not touch dam cells "
                             "(off %d got %u).",
                             off, static_cast<unsigned>(vMap.SurExtBuf[off]));
            }
        }
    }

    // Selective rendering: getColor32 resolves the extended table at the
    // painted cell and the base table otherwise.
    const int geoProbe = mapOffset(geoX, geoY + 1);
    vMap.RnrBuf[geoProbe] = 5;
    TEST_REQUIRE(vMap.getColor32(geoProbe) == vMap.extColor32(256, 5),
                 "getColor32 must resolve the extended index row (got %.6x "
                 "expected %.6x).",
                 static_cast<unsigned>(vMap.getColor32(geoProbe)),
                 static_cast<unsigned>(vMap.extColor32(256, 5)));
    const int plainOff = mapOffset(0, 0);
    TEST_REQUIRE(vMap.hasExtendedSurface() &&
                     session.isExtendedSurfaceUsed(),
                 "painted extended cells must be visible to the session.");

    // Eyedropper round-trips the custom index.
    TEST_REQUIRE(brush.pickColorAt(geoX, geoY + 1),
                 "the eyedropper pick on an extended cell failed.");
    TEST_REQUIRE(brush.lastPickedColorIndex() == 256,
                 "the eyedropper must sample the extended index (got %d).",
                 brush.lastPickedColorIndex());

    // ------------------------------------------------------------------
    // 65.3: save — the sidecar appears, reloads and reproduces everything.
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.saveWorld(),
                 "EditorSession::saveWorld() failed with extended data.");
    TEST_REQUIRE(!session.isExtendedDirty(),
                 "a successful save must clear the extended dirty flag.");
    TEST_REQUIRE(fs::exists(worldDir / "output.esur"),
                 "the save must create the output.esur sidecar.");

    // Parse the sidecar against the in-memory state of the still-loaded
    // world: palette entries match, runs cover exactly the painted cells.
    {
        unsigned int kGeo = 0, kDam = 0;
        std::vector<unsigned char> rgb;
        std::vector<std::pair<std::pair<unsigned int, unsigned int>,
                              unsigned short>> runs;
        TEST_REQUIRE(readSidecar(worldDir / "output.esur", kGeo, kDam, rgb,
                                 runs),
                     "the sidecar could not be parsed.");
        TEST_REQUIRE(kGeo == 1 && kDam == 1,
                     "the sidecar must declare one extended entry per layer "
                     "(got %u/%u).",
                     kGeo, kDam);
        TEST_REQUIRE(rgb.size() == 6 && rgb[0] == 0x12 && rgb[1] == 0x34 &&
                         rgb[2] == 0x56 && rgb[3] == 0xFE && rgb[4] == 0xDC &&
                         rgb[5] == 0xBA,
                     "the sidecar palette bytes are wrong.");
        // Every nonzero SurExtBuf cell must be covered by exactly one run
        // with its index.
        const int bufsz = static_cast<int>(vMap.XS_Buf) * vMap.YS_Buf;
        std::vector<unsigned char> covered(static_cast<size_t>(bufsz), 0);
        for (const auto& run : runs) {
            TEST_REQUIRE(run.first.first < static_cast<unsigned>(bufsz) &&
                             run.first.second > 0 &&
                             run.first.first + run.first.second <=
                                 static_cast<unsigned>(bufsz),
                         "a sidecar run leaves the buffer bounds.");
            for (unsigned int c = 0; c < run.first.second; ++c) {
                const size_t cell =
                    static_cast<size_t>(run.first.first) + c;
                TEST_REQUIRE(!covered[cell],
                             "sidecar runs must not overlap (cell %zu).",
                             cell);
                covered[cell] = 1;
                TEST_REQUIRE(run.second == vMap.SurExtBuf[cell],
                             "sidecar run index %u does not match SurExtBuf "
                             "%u.",
                             static_cast<unsigned>(run.second),
                             static_cast<unsigned>(vMap.SurExtBuf[cell]));
            }
        }
        int nonzero = 0;
        for (int i = 0; i < bufsz; ++i) {
            if (vMap.SurExtBuf[i] != 0) {
                TEST_REQUIRE(covered[static_cast<size_t>(i)],
                             "a painted extended cell is missing from the "
                             "sidecar (cell %d).",
                             i);
                ++nonzero;
            }
        }
        int runCells = 0;
        for (const auto& run : runs) {
            runCells += static_cast<int>(run.first.second);
        }
        TEST_REQUIRE(runCells == nonzero,
                     "the runs must cover exactly the painted cells "
                     "(runs %d, nonzero %d).",
                     runCells, nonzero);
    }

    TEST_REQUIRE(session.loadWorld(0),
                 "the reload of the extended world failed.");
    TEST_REQUIRE(session.extendedPaletteSize(Layer::Geo) == 1 &&
                     session.extendedPaletteSize(Layer::Dam) == 1,
                 "the reload must reproduce the extended entry counts.");
    TEST_REQUIRE(session.paletteRgb(Layer::Geo, 256) == 0x123456 &&
                     session.paletteRgb(Layer::Dam, 257) == 0xFEDCBA,
                 "the reload must reproduce the extended palette bytes.");
    TEST_REQUIRE(vMap.SurExtBuf[mapOffset(geoX, geoY + 1)] == 256 &&
                     vMap.SurExtBuf[mapOffset(damX, damY)] == 257,
                 "the reload must reproduce the painted extended cells.");
    vMap.RnrBuf[mapOffset(geoX, geoY + 1)] = 5;
    TEST_REQUIRE(vMap.getColor32(mapOffset(geoX, geoY + 1)) ==
                     vMap.extColor32(256, 5),
                 "the reload must reproduce the rendered extended color.");
    TEST_REQUIRE(!session.hasUnsavedChanges(),
                 "a reloaded saved world must report no unsaved changes.");
    std::printf("[PASS] extended palette: add colors, gated paint, undo, "
                "sidecar save and reload.\n");

    // ------------------------------------------------------------------
    // 65.4: erasing all extended *paint* leaves only the palette entries.
    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    // 65.4: repainting with base colors erases the extended cells.
    // ------------------------------------------------------------------
    // Undo cannot help here — the reload above restored the painted sidecar
    // runs (and cleared the history). The user erases a custom color by
    // painting a base entry over it: the base SetColor goes through SetTer,
    // which must clear SurExtBuf.
    params.mode = MapEditor::BrushMode::Geo;
    params.colorIndex = 10;
    TEST_REQUIRE(brush.applyStamp(geoX, geoY, params),
                 "the base-palette Geo erase stamp was refused.");
    params.mode = MapEditor::BrushMode::Dam;
    params.colorIndex = 130;
    TEST_REQUIRE(brush.applyStamp(damX, damY, params),
                 "the base-palette Dam erase stamp was refused.");
    {
        const int bufsz = static_cast<int>(vMap.XS_Buf) * vMap.YS_Buf;
        int leftover = 0;
        for (int i = 0; i < bufsz; ++i) {
            if (vMap.SurExtBuf[i] != 0) {
                ++leftover;
            }
        }
        TEST_REQUIRE(leftover == 0,
                     "after repainting with base colors no extended cell may "
                     "remain (got %d).",
                     leftover);
    }
    TEST_REQUIRE(session.paletteRgb(Layer::Geo, 256) == 0x123456,
                 "the custom entries persist outside the undo history.");
    TEST_REQUIRE(session.saveWorld(),
                 "the save after erasing the extended paint failed.");
    {
        unsigned int kGeo2 = 0, kDam2 = 0;
        std::vector<unsigned char> rgb2;
        std::vector<std::pair<std::pair<unsigned int, unsigned int>,
                              unsigned short>> runs2;
        TEST_REQUIRE(readSidecar(worldDir / "output.esur", kGeo2, kDam2, rgb2,
                                 runs2),
                     "the sidecar must survive the erase save "
                     "(the palette entries are still there).");
        TEST_REQUIRE(runs2.empty(),
                     "the sidecar must carry no runs without painted cells.");
    }
    vMap.RnrBuf[mapOffset(geoX, geoY + 1)] = 5;
    TEST_REQUIRE(vMap.SurExtBuf[mapOffset(geoX, geoY + 1)] == 0 &&
                     vMap.SurBuf[mapOffset(geoX, geoY + 1)] == 10 &&
                     vMap.getColor32(mapOffset(geoX, geoY + 1)) ==
                         vMap.Sur2Col32[10][5],
                 "the erased cell must render through the base palette "
                 "again.");
    TEST_REQUIRE(session.loadWorld(0),
                 "the reload after the erase save failed.");
    TEST_REQUIRE(vMap.SurExtBuf[mapOffset(geoX, geoY + 1)] == 0 &&
                     vMap.SurExtBuf[mapOffset(damX, damY)] == 0,
                 "the reload must not resurrect erased extended cells.");
    std::printf("[PASS] erasing extended paint restores the base palette "
                "render.\n");

    return true;
}

MAP_EDITOR_REGISTER_TEST("Extended palette (T4.4)", 65, runExtendedPaletteTests);

bool runVanillaSidecarAbsenceTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Self-contained content root with a fresh copy of the temp world.
    const fs::path loadRoot = ctx.tempRoot / "load_root_nosur";
    const fs::path worldDir =
        loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName);
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the sidecar-absence content root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx",
            loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini",
                      loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");
        MapEditor::Tests::copyDirectoryRecursive(ctx.tempWorldDir, worldDir);
    }

    EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    // A palette-only edit (T4.1 path — inGeo/inDam) must NOT produce a
    // sidecar: only world data of the extended space belongs there.
    TEST_REQUIRE(!fs::exists(worldDir / "output.esur"),
                 "the fresh copy must not ship a sidecar.");
    TEST_REQUIRE(session.saveWorld(),
                 "EditorSession::saveWorld() of the vanilla world failed.");
    TEST_REQUIRE(!fs::exists(worldDir / "output.esur"),
                 "a vanilla save must stay sidecar-free (output.vmp remains "
                 "byte-compatible with the original game).");
    TEST_REQUIRE(!session.isExtendedSurfaceUsed(),
                 "a vanilla world must not report extended surface data.");
    std::printf("[PASS] vanilla worlds stay sidecar-free on save.\n");
    return true;
}

MAP_EDITOR_REGISTER_TEST("Vanilla sidecar absence (T4.4)", 66,
                         runVanillaSidecarAbsenceTests);

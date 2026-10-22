// ============================================================================
// T4.1 — palette panel core: RGB editing, table application, geoPal.xml sync.
//
//   * EditorSession::setPaletteRgb()/paletteRgb() edit the 256-entry RGB
//     buffers per layer (GeoPal / DamPal) with out-of-range and no-world
//     refusals, and flag the session as modified outside the undo history;
//   * applyPalettes() rebuilds the render color tables exactly like
//     vrtMap::convertPal2TableSurCol after loading (Sur2Col32 columns match
//     the game formula for the full lighting sweep; veryLightDam is
//     re-derived with the same "last max wins" rule);
//   * PaletteOps::geoPalXml() serializes the palette as a color table that
//     reloads byte-exactly through the game's truncating float→byte loader;
//   * saveWorld() writes inGeo.act/inDam.act and the synced geoPal.xml only
//     when the palette was edited, and reloading the world reproduces the
//     edited palettes byte-exactly (the decisive check: in _TX3D_LIBRARY_
//     builds the reload reads GeoPal from geoPal.xml, not from inGeo.act).
//
// Runs after the world round-trip (its temporary world copy is reused).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "PaletteOps.h"
#include "WorldFiles.h"
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
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::EditorSession;
using Layer = EditorSession::PaletteLayer;

/// inGeo/inDam file view of a world folder (WorldFiles) is used below for
/// the save-side verification.
int mapOffset(int x, int y)
{
    return y * static_cast<int>(vMap.H_SIZE) + x;
}

/// The Sur2Col32 column the game computes for one palette entry and lighting
/// step j (vrtMap::convertPal2TableSurCol, VMAP.cpp:1364-1372 — the very same
/// numbers the applyPalettes() call produces).
unsigned int expectedCol32(unsigned char r, unsigned char g, unsigned char b, int j)
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

/// The argmax-luminance dam index the game derives (VMAP.cpp:1350-1355 —
/// the comparison is `<=`, so on ties the LAST maximum wins).
unsigned char expectedVeryLightDam(const unsigned char* damPal)
{
    unsigned char best = 0;
    float bestLighting = -1.0f;
    for (int i = 0; i < MAX_SURFACE_TYPE; ++i) {
        const float lighting =
            0.3f * damPal[i * 3 + 0] + 0.59f * damPal[i * 3 + 1] +
            0.11f * damPal[i * 3 + 2];
        if (bestLighting <= lighting) {
            bestLighting = lighting;
            best = static_cast<unsigned char>(i);
        }
    }
    return best;
}

/// Parse every <color value='r,g,b'/> of a geoPal.xml document and truncate
/// each float the way vrtMap::loadGeoDamPal does (GeoPal[i] = float*255).
/// Returns false when the document has no exactly 256 color tags or any
/// component does not round-trip to the given palette bytes.
bool xmlReloadsPalette(const std::string& xml,
                       const unsigned char* pal /* PaletteOps::PaletteBytes */)
{
    static const char tag[] = "\t<color value='";
    size_t pos = 0;
    int count = 0;
    while (true) {
        const size_t begin = xml.find(tag, pos);
        if (begin == std::string::npos) {
            break;
        }
        const size_t end = xml.find("'/>", begin);
        if (end == std::string::npos) {
            return false;
        }
        float v[3] = {0.f, 0.f, 0.f};
        if (std::sscanf(xml.substr(begin + std::strlen(tag),
                                   end - begin - std::strlen(tag))
                            .c_str(),
                        "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) {
            return false;
        }
        for (int c = 0; c < 3; ++c) {
            if (static_cast<int>(v[c] * 255.0) !=
                static_cast<int>(pal[count * 3 + c])) {
                return false;
            }
        }
        pos = end;
        ++count;
    }
    return count == MapEditor::PaletteOps::PaletteEntryCount;
}

bool runPaletteOpsTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    // Self-contained content root (the same pattern the other tests use).
    const fs::path loadRoot = ctx.tempRoot / "load_root_palette";
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the palette-test content root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx",
            loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini",
                      loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.tempWorldDir,
            loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName));
    }

    EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    // ------------------------------------------------------------------
    // 64.1: RGB editing per layer, modified-flag (outside the undo history).
    // ------------------------------------------------------------------
    TEST_REQUIRE(!session.setPaletteRgb(Layer::Geo, -1, 0x010203),
                 "palette index -1 must be refused.");
    TEST_REQUIRE(!session.setPaletteRgb(Layer::Geo, 256, 0x010203),
                 "palette index 256 must be refused.");
    TEST_REQUIRE(!session.isPaletteModified(),
                 "a fresh load must carry no palette modifications.");

    TEST_REQUIRE(session.setPaletteRgb(Layer::Geo, 42, 0xFF4000),
                 "geo entry edit was refused.");
    TEST_REQUIRE(session.paletteRgb(Layer::Geo, 42) == 0xFF4000,
                 "the geo entry did not read back.");
    TEST_REQUIRE(vMap.GeoPal[126] == 255 && vMap.GeoPal[127] == 64 &&
                     vMap.GeoPal[128] == 0,
                 "the geo entry bytes must be plain RGB at +0/+1/+2.");

    TEST_REQUIRE(session.setPaletteRgb(Layer::Dam, 200, 0x00FF40),
                 "dam entry edit was refused.");
    TEST_REQUIRE(session.paletteRgb(Layer::Dam, 200) == 0x00FF40,
                 "the dam entry did not read back.");
    TEST_REQUIRE(session.setPaletteRgb(Layer::Geo, 0, 0x010203) &&
                     session.setPaletteRgb(Layer::Geo, 255, 0xFFFFFF) &&
                     session.setPaletteRgb(Layer::Dam, 255, 0xFFFFFF),
                 "boundary entries (0 / 255) must be editable.");
    TEST_REQUIRE(session.paletteRgb(Layer::Geo, 255) == 0xFFFFFF &&
                     session.paletteRgb(Layer::Dam, 255) == 0xFFFFFF,
                 "the boundary entries did not read back.");

    TEST_REQUIRE(session.isPaletteModified(),
                 "an edited palette must raise the modified flag.");
    TEST_REQUIRE(session.hasUnsavedChanges(),
                 "an edited palette must count as unsaved changes.");
    std::printf("[PASS] per-layer palette edit + modified flag (outside the "
                "undo history).\n");

    // ------------------------------------------------------------------
    // 64.2: applyPalettes() rebuilds the render tables like the game does.
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.applyPalettes(),
                 "applyPalettes() was refused on a loaded world.");

    // Full lighting sweep for the edited geo entry 42 (0xFF4000).
    for (int j = 0; j < MAX_SURFACE_LIGHTING; ++j) {
        TEST_REQUIRE(vMap.Sur2Col32[42][j] ==
                         expectedCol32(255, 64, 0, j),
                     "Sur2Col32 geo column %d does not match the game "
                     "formula (got %.6x).",
                     j, static_cast<unsigned>(vMap.Sur2Col32[42][j]));
    }
    // Dam entry 200 lands in the dam lighting half (columns 128..).
    for (int j = 0; j < MAX_SURFACE_LIGHTING; ++j) {
        TEST_REQUIRE(vMap.Sur2Col32[200][MAX_SURFACE_LIGHTING + j] ==
                         expectedCol32(0, 255, 64, j),
                     "Sur2Col32 dam column %d does not match the game "
                     "formula (got %.6x).",
                     j,
                     static_cast<unsigned>(
                         vMap.Sur2Col32[200][MAX_SURFACE_LIGHTING + j]));
    }
    // veryLightDam is re-derived with the same rule as the game.
    TEST_REQUIRE(vMap.veryLightDam == expectedVeryLightDam(vMap.DamPal),
                 "veryLightDam must be re-derived from the edited DamPal.");
    std::printf("[PASS] applyPalettes() rebuilds the color tables like "
                "convertPal2TableSurCol.\n");

    TEST_REQUIRE(session.cellIsDam(0, 0) ==
                     (vMap.VxDBuf[mapOffset(0, 0)] != 0),
                 "cellIsDam must mirror VxDBuf at (0,0).");

    // ------------------------------------------------------------------
    // 64.3: geoPal.xml serialization round-trips every byte through the
    // game's truncating float → byte loader (the boundary thing).
    // ------------------------------------------------------------------
    const std::string xml = MapEditor::PaletteOps::geoPalXml(vMap.GeoPal);
    TEST_REQUIRE(!xml.empty(), "geoPalXml produced nothing.");
    TEST_REQUIRE(xml.find("<colorTable>") == 0,
                 "the document must start with the colorTable tag.");
    TEST_REQUIRE(xml.find("</colorTable>") == xml.size() - 14,
                 "the document must end with the closing colorTable tag.");
    TEST_REQUIRE(xmlReloadsPalette(xml, vMap.GeoPal),
                 "the serialized XML must truncate back to exact palette "
                 "bytes (incl. 0 and 255).");
    TEST_REQUIRE(MapEditor::PaletteOps::geoPalXml(nullptr).empty(),
                 "a null palette must yield an empty document.");
    std::printf("[PASS] geoPal.xml serialization reloads every byte through "
                "the truncating loader.\n");

    // ------------------------------------------------------------------
    // 64.4: saveWorld() persists the edited palettes; a reload reproduces
    // them byte-exactly — in this build the reload reads the geo palette
    // from geoPal.xml, so the XML sync is what keeps the edits alive.
    // ------------------------------------------------------------------
    const std::vector<unsigned char> geoBefore(vMap.GeoPal, vMap.GeoPal + 768);
    const std::vector<unsigned char> damBefore(vMap.DamPal, vMap.DamPal + 768);

    TEST_REQUIRE(session.saveWorld(), "saveWorld() failed.");

    // The .act files must now match the buffers.
    const fs::path worldDir = loadRoot / "RESOURCE" / "Worlds" /
                              fs::u8path(ctx.worldName);
    MapEditor::WorldFiles files(worldDir);
    MapEditor::GeoDamPalettes pal;
    TEST_REQUIRE(files.readPalettes(pal),
                 "the saved world must carry readable palettes.");
    TEST_REQUIRE(std::equal(pal.geo.begin(), pal.geo.end(), geoBefore.begin()) &&
                     std::equal(pal.dam.begin(), pal.dam.end(), damBefore.begin()),
                 "inGeo.act/inDam.act must match the edited buffers.");

    // The synced XML must truncate back into the edited GeoPal.
    const std::optional<std::string> xmlOnDisk = files.readGeoPalXml();
    TEST_REQUIRE(xmlOnDisk.has_value(), "geoPal.xml must exist after save.");
    TEST_REQUIRE(xmlReloadsPalette(*xmlOnDisk, geoBefore.data()),
                 "the saved geoPal.xml must reload the edited geo palette.");

    // And the flags are cleared after a successful save.
    TEST_REQUIRE(!session.isPaletteModified(),
                 "a save must clear the palette-modified flag.");
    TEST_REQUIRE(!session.hasUnsavedChanges(),
                 "a fresh save must not count as unsaved changes.");

    // Reload: the world comes back with the edited palettes (this build
    // loads GeoPal from geoPal.xml — the byte-exactness proves both the XML
    // quantization and the save pipeline).
    TEST_REQUIRE(session.loadWorld(0), "reloading the world failed.");
    TEST_REQUIRE(std::equal(vMap.GeoPal, vMap.GeoPal + 768, geoBefore.begin()),
                 "the reloaded GeoPal must match the saved palette.");
    TEST_REQUIRE(std::equal(vMap.DamPal, vMap.DamPal + 768, damBefore.begin()),
                 "the reloaded DamPal must match the saved palette.");

    // Refusal without a world (after a deliberate unload).
    session.unloadWorld();
    TEST_REQUIRE(!session.setPaletteRgb(Layer::Geo, 42, 0xFF4000),
                 "palette edits must be refused without a loaded world.");
    TEST_REQUIRE(!session.applyPalettes(),
                 "table application must be refused without a loaded world.");
    std::printf("[PASS] saveWorld() persists the edited palettes and the "
                "reload reproduces them byte-exactly.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Palette panel core (T4.1)", 64, runPaletteOpsTests);

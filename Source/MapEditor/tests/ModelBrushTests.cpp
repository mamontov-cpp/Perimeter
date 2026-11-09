// ============================================================================
// Model brush — ModelBrush::importPly + BrushTool::applyModelBrush.
//
// Two suites in this file (one module):
//
//  95 "ModelBrush PLY import":
//     * ASCII PLY with uchar vertex colours — raster dims, per-cell max
//       height normalized, top-vertex colour, topology-hole cells (height 0,
//       average colour);
//     * binary_little_endian PLY produces the same raster;
//     * a texture named by a header comment is sampled through the UVs
//       (24-bit TGA next to the .ply);
//     * errors: missing file, bad magic, missing x/y/z, big-endian, missing
//       texture file — each with a description.
//
//  96 "ModelBrush application" (on a loaded world):
//     * place mode sets the dam to the scaled model height and paints the
//       quantized palette index where a dam exists (cells at/below the
//       terrain drop to the geo height, dam removed);
//     * add raises the covered average by the local model height, subtract
//       lowers it — below the terrain the dam is removed;
//     * the x/y scales shrink the placement rectangle;
//     * Geo mode refuses; the stamp is one undo entry (byte-exact restore).
//
// Runs after the other brush tests; reuses the shared temp world.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ImageDecal.h"
#include "ModelBrush.h"
#include "WorldImages.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;

// ---------------------------------------------------------------------------
// 95 — PLY import.
// ---------------------------------------------------------------------------

/// The 4-vertex colour model: (0,0,z=0,red) (10,0,z=5,green) (0,10,z=5,blue)
/// (10,10,z=10,white). The 10x10 extent gives a 10x10 raster; the four
/// corner cells carry the vertices, everything else is a hole.
const char* kAsciiColourPly =
    "ply\n"
    "format ascii 1.0\n"
    "comment test model\n"
    "element vertex 4\n"
    "property float x\n"
    "property float y\n"
    "property float z\n"
    "property uchar red\n"
    "property uchar green\n"
    "property uchar blue\n"
    "end_header\n"
    "0 0 0 255 0 0\n"
    "10 0 5 0 255 0\n"
    "0 10 5 0 0 255\n"
    "10 10 10 255 255 255\n";

void writeFile(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

bool checkColourModel(const MapEditor::ModelBrush::ModelRaster& m)
{
    if (m.width != 10 || m.height != 10) {
        std::printf("  unexpected raster size %dx%d\n", m.width, m.height);
        return false;
    }
    auto cell = [&](int x, int y) {
        return static_cast<std::size_t>(y) * m.width + x;
    };
    if (m.normHeight[cell(0, 0)] != 0.0) return false;
    if (std::abs(m.normHeight[cell(9, 0)] - 0.5) > 1e-9) return false;
    if (std::abs(m.normHeight[cell(0, 9)] - 0.5) > 1e-9) return false;
    if (m.normHeight[cell(9, 9)] != 1.0) return false;
    if (m.rgb[cell(0, 0)] != 0xFF0000u) return false;
    if (m.rgb[cell(9, 0)] != 0x00FF00u) return false;
    if (m.rgb[cell(0, 9)] != 0x0000FFu) return false;
    if (m.rgb[cell(9, 9)] != 0xFFFFFFu) return false;
    // A hole: height 0, the average of the four vertex colours
    // ((255+0+0+255)/4 = 127 per channel, integer mean).
    if (m.normHeight[cell(5, 5)] != 0.0) return false;
    if (m.rgb[cell(5, 5)] != 0x7F7F7Fu) return false;
    return true;
}

bool runModelBrushImportTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    const fs::path dir = ctx.tempRoot / "modelbrush";
    fs::create_directories(dir);

    // 1: ASCII import with vertex colours.
    const fs::path asciiPath = dir / "colour_ascii.ply";
    writeFile(asciiPath, kAsciiColourPly);
    {
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(asciiPath);
        TEST_REQUIRE(imported.ok,
                     "the ASCII colour model must import: %s",
                     imported.error.c_str());
        TEST_REQUIRE(checkColourModel(imported.model),
                     "the ASCII raster differs from the expectation.");
    }
    std::printf("[PASS] ASCII PLY import: raster, max heights, top colours, "
                "hole cells.\n");

    // 2: binary_little_endian with the same vertices must match.
    const fs::path binaryPath = dir / "colour_binary.ply";
    {
        std::ofstream out(binaryPath, std::ios::binary);
        out << "ply\n"
               "format binary_little_endian 1.0\n"
               "element vertex 4\n"
               "property float x\n"
               "property float y\n"
               "property float z\n"
               "property uchar red\n"
               "property uchar green\n"
               "property uchar blue\n"
               "end_header\n";
        const float verts[4][3] = {
            {0, 0, 0}, {10, 0, 5}, {0, 10, 5}, {10, 10, 10},
        };
        const unsigned char cols[4][3] = {
            {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
        };
        for (int i = 0; i < 4; ++i) {
            out.write(reinterpret_cast<const char*>(verts[i]), sizeof(float) * 3);
            out.write(reinterpret_cast<const char*>(cols[i]), 3);
        }
    }
    {
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(binaryPath);
        TEST_REQUIRE(imported.ok,
                     "the binary colour model must import: %s",
                     imported.error.c_str());
        TEST_REQUIRE(checkColourModel(imported.model),
                     "the binary raster differs from the ASCII one.");
    }
    std::printf("[PASS] binary_little_endian PLY import matches the ASCII "
                "one.\n");

    // 2a: no texture and no vertex colours -> the model paints WHITE.
    const fs::path plainPath = dir / "plain.ply";
    writeFile(plainPath,
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 4\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "end_header\n"
              "0 0 0\n"
              "10 0 5\n"
              "0 10 5\n"
              "10 10 10\n");
    {
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(plainPath);
        TEST_REQUIRE(imported.ok,
                     "a colour-less model must import: %s",
                     imported.error.c_str());
        const MapEditor::ModelBrush::ModelRaster& m = imported.model;
        TEST_REQUIRE(m.rgb[0] == 0xFFFFFFu &&
                         m.rgb[static_cast<std::size_t>(9)] == 0xFFFFFFu &&
                         m.rgb[static_cast<std::size_t>(9) * 10 + 9] ==
                             0xFFFFFFu,
                     "without texture/vertex colours the model is white.");
        TEST_REQUIRE(std::abs(m.normHeight[static_cast<std::size_t>(9) *
                                                10 + 9] -
                              1.0) < 1e-9,
                     "the colour-less model still carries its heights.");
    }
    std::printf("[PASS] a model without texture/colours imports white.\n");

    // 2b: a cube with faces is a SOLID — the interior columns take the TOP
    // face's height and colour, not "four corner points" (regression: the
    // interior used to be holes when only vertices were sampled). Bottom
    // ring green at z = 0, top ring white at z = 20, 20x20 extent.
    const fs::path cubePath = dir / "cube.ply";
    writeFile(cubePath,
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 8\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "property uchar red\n"
              "property uchar green\n"
              "property uchar blue\n"
              "element face 6\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "-10 -10 0 0 255 0\n"
              "10 -10 0 0 255 0\n"
              "10 10 0 0 255 0\n"
              "-10 10 0 0 255 0\n"
              "-10 -10 20 255 255 255\n"
              "10 -10 20 255 255 255\n"
              "10 10 20 255 255 255\n"
              "-10 10 20 255 255 255\n"
              "4 0 3 2 1\n"
              "4 4 5 6 7\n"
              "4 0 1 5 4\n"
              "4 2 3 7 6\n"
              "4 1 2 6 5\n"
              "4 0 4 7 3\n");
    {
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(cubePath);
        TEST_REQUIRE(imported.ok,
                     "the cube must import: %s", imported.error.c_str());
        const MapEditor::ModelBrush::ModelRaster& m = imported.model;
        TEST_REQUIRE(m.width == 20 && m.height == 20,
                     "unexpected cube raster size %dx%d.", m.width, m.height);
        // Every column of the footprint is under the top face: full height,
        // white — including the interior and the corners.
        for (int y = 0; y < 20; ++y) {
            for (int x = 0; x < 20; ++x) {
                const std::size_t cell = static_cast<std::size_t>(y) * 20 + x;
                TEST_REQUIRE(std::abs(m.normHeight[cell] - 1.0) < 1e-9,
                             "cube cell (%d,%d) must reach the top face.", x,
                             y);
                TEST_REQUIRE(m.rgb[cell] == 0xFFFFFFu,
                             "cube cell (%d,%d) must take the top colour.", x,
                             y);
            }
        }
    }
    std::printf("[PASS] a faced cube imports as a solid (interior columns "
                "take the top face, no holes).\n");

    // 2c: the user's box2 regression — 24 vertices (8 corners x 3 faces with
    // per-face colours), 6 quads. Must stamp as a cube plateau, not four
    // corner points.
    const fs::path box2Path = dir / "box2.ply";
    writeFile(box2Path,
              "ply\n"
              "format ascii 1.0\n"
              "comment Scaled Cube (x20) with 2 red sides, 2 green sides, and white top/bottom\n"
              "element vertex 24\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "property uchar red\n"
              "property uchar green\n"
              "property uchar blue\n"
              "element face 6\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "-20.0 -20.0 -20.0 255 0 0\n"
              " 20.0 -20.0 -20.0 255 0 0\n"
              " 20.0  20.0 -20.0 255 0 0\n"
              "-20.0  20.0 -20.0 255 0 0\n"
              "-20.0 -20.0  20.0 255 0 0\n"
              " 20.0 -20.0  20.0 255 0 0\n"
              " 20.0  20.0  20.0 255 0 0\n"
              "-20.0  20.0  20.0 255 0 0\n"
              "-20.0 -20.0  20.0 0 255 0\n"
              "-20.0 -20.0 -20.0 0 255 0\n"
              "-20.0  20.0 -20.0 0 255 0\n"
              "-20.0  20.0  20.0 0 255 0\n"
              " 20.0 -20.0 -20.0 0 255 0\n"
              " 20.0 -20.0  20.0 0 255 0\n"
              " 20.0  20.0  20.0 0 255 0\n"
              " 20.0  20.0 -20.0 0 255 0\n"
              "-20.0  20.0 -20.0 255 255 255\n"
              " 20.0  20.0 -20.0 255 255 255\n"
              " 20.0  20.0  20.0 255 255 255\n"
              "-20.0  20.0  20.0 255 255 255\n"
              "-20.0 -20.0  20.0 255 255 255\n"
              " 20.0 -20.0  20.0 255 255 255\n"
              " 20.0 -20.0 -20.0 255 255 255\n"
              "-20.0 -20.0 -20.0 255 255 255\n"
              "4 0 3 2 1\n"
              "4 4 5 6 7\n"
              "4 8 11 10 9\n"
              "4 12 15 14 13\n"
              "4 16 19 18 17\n"
              "4 20 23 22 21\n");
    {
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(box2Path);
        TEST_REQUIRE(imported.ok,
                     "box2 must import: %s", imported.error.c_str());
        const MapEditor::ModelBrush::ModelRaster& m = imported.model;
        TEST_REQUIRE(m.width == 40 && m.height == 40,
                     "unexpected box2 raster size %dx%d.", m.width, m.height);
        int fullCells = 0;
        for (std::size_t cell = 0; cell < m.normHeight.size(); ++cell) {
            TEST_REQUIRE(std::abs(m.normHeight[cell] - 1.0) < 1e-9,
                         "box2 cell %zu must reach the top face height.",
                         cell);
            // Two caps coincide at the top height (face 1 red, face 5
            // white); the FIRST face wins the exact tie, so the top reads
            // red.
            TEST_REQUIRE(m.rgb[cell] == 0xFF0000u,
                         "box2 cell %zu must take the first cap's colour.",
                         cell);
            ++fullCells;
        }
        TEST_REQUIRE(fullCells == 40 * 40,
                     "the whole box2 footprint must be covered.");
    }
    std::printf("[PASS] box2 stamps as a cube plateau (all 40x40 columns at "
                "the top height).\n");

    // 3: a texture named by a header comment, sampled through the UVs.
    // Texture 1x2: top row blue, bottom row red (v = 0 is the bottom).
    const fs::path texturePath = dir / "model_tex.tga";
    {
        std::vector<unsigned char> bgr = {
            255, 0, 0,   // top row: blue in BGR
            0, 0, 255,   // bottom row: red in BGR
        };
        TEST_REQUIRE(MapEditor::WorldImages::writeTga24(texturePath, 1, 2, bgr),
                     "could not write the fixture texture.");
    }
    const fs::path uvPath = dir / "textured.ply";
    writeFile(uvPath,
              "ply\n"
              "format ascii 1.0\n"
              "comment TextureFile model_tex.tga\n"
              "element vertex 4\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "property float u\n"
              "property float v\n"
              "end_header\n"
              "0 0 1 0 0\n"
              "10 0 1 1 0\n"
              "0 10 1 0 1\n"
              "10 10 1 1 1\n");
    {
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(uvPath);
        TEST_REQUIRE(imported.ok,
                     "the textured model must import: %s",
                     imported.error.c_str());
        const MapEditor::ModelBrush::ModelRaster& m = imported.model;
        TEST_REQUIRE(m.width == 10 && m.height == 10,
                     "unexpected textured raster size.");
        // Flat z range -> covered cells are full height; the UVs pick the
        // texture rows (v = 0 bottom = red here, v = 1 top = blue).
        TEST_REQUIRE(m.rgb[0] == 0xFF0000u,
                     "cell (0,0) must sample the red texture row.");
        TEST_REQUIRE(m.rgb[static_cast<std::size_t>(9) * 10] == 0x0000FFu,
                     "cell (0,9) must sample the blue texture row.");
    }
    std::printf("[PASS] the commented texture is sampled through the "
                "UVs.\n");

    // 4: errors carry descriptions.
    {
        const MapEditor::ModelBrush::ImportResult missing =
            MapEditor::ModelBrush::importPly(dir / "no_such_file.ply");
        TEST_REQUIRE(!missing.ok && !missing.error.empty(),
                     "a missing file must fail with a description.");
    }
    {
        const fs::path bad = dir / "bad_magic.ply";
        writeFile(bad, "not a ply at all\n");
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(bad);
        TEST_REQUIRE(!imported.ok && !imported.error.empty(),
                     "a bad magic must fail with a description.");
    }
    {
        const fs::path noZ = dir / "no_z.ply";
        writeFile(noZ,
                  "ply\nformat ascii 1.0\nelement vertex 1\n"
                  "property float x\nproperty float y\nend_header\n0 0\n");
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(noZ);
        TEST_REQUIRE(!imported.ok && !imported.error.empty(),
                     "missing z must fail with a description.");
    }
    {
        const fs::path bigEndian = dir / "big_endian.ply";
        writeFile(bigEndian,
                  "ply\nformat binary_big_endian 1.0\n"
                  "element vertex 0\nend_header\n");
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(bigEndian);
        TEST_REQUIRE(!imported.ok && !imported.error.empty(),
                     "big-endian must fail with a description.");
    }
    {
        const fs::path noTexture = dir / "texture_missing.ply";
        writeFile(noTexture,
                  "ply\nformat ascii 1.0\n"
                  "comment TextureFile absent.tga\n"
                  "element vertex 1\n"
                  "property float x\nproperty float y\nproperty float z\n"
                  "property float u\nproperty float v\n"
                  "end_header\n0 0 0 0 0\n");
        const MapEditor::ModelBrush::ImportResult imported =
            MapEditor::ModelBrush::importPly(noTexture);
        TEST_REQUIRE(!imported.ok &&
                         imported.error.find("texture") != std::string::npos,
                     "a missing texture file must fail mentioning it.");
    }
    std::printf("[PASS] import errors carry descriptions.\n");

    return true;
}

// ---------------------------------------------------------------------------
// 96 — application on a loaded world.
// ---------------------------------------------------------------------------

/// The test stamp: a 4x4 ramp along x (0, 1/3, 2/3, 1), palette index 42
/// everywhere. A 4-wide stamp centred at (cx, cy) covers columns
/// cx-2..cx+1 (even width).
MapEditor::ModelBrush::ModelRaster makeRampRaster()
{
    MapEditor::ModelBrush::ModelRaster raster;
    raster.width = 4;
    raster.height = 4;
    raster.normHeight.resize(16);
    raster.rgb.resize(16, 0x808080);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            raster.normHeight[static_cast<std::size_t>(y) * 4 + x] =
                static_cast<double>(x) / 3.0;
        }
    }
    return raster;
}

MapEditor::ImageDecal::DecalImage makeFlatIndices(int width, int height,
                                                  int index)
{
    MapEditor::ImageDecal::DecalImage image;
    image.width = width;
    image.height = height;
    image.indices.assign(static_cast<std::size_t>(width) * height, index);
    return image;
}

bool runModelBrushApplyTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    const fs::path loadRoot = ctx.tempRoot / "load_root_modelbrush";
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        TEST_REQUIRE(!ec, "could not create the load content root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.tempWorldDir, loadRoot / "RESOURCE" / "Worlds" /
                                  fs::u8path(ctx.worldName));
    }

    MapEditor::EditorSession session;
    // Undo coalescing off: these tests expect one undo entry per action.
    session.setUndoCoalescingWindowMs(0);
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the model brush test.");

    // A low spot; the flatten base sits a few voxels above the terrain.
    int cx = mapW / 2;
    int cy = mapH / 2;
    int baseVox = 0;
    {
        bool found = false;
        for (int gy = 32; gy < mapH - 32 && !found; gy += 64) {
            for (int gx = 32; gx < mapW - 32 && !found; gx += 64) {
                int localMax = 0;
                for (int dy = -8; dy <= 8; ++dy) {
                    for (int dx = -8; dx <= 8; ++dx) {
                        localMax = std::max(
                            localMax,
                            static_cast<int>(vMap.SGetAlt(gx + dx, gy + dy)));
                    }
                }
                if ((localMax >> VX_FRACTION) + 12 + 8 + 4 <= 240) {
                    cx = gx;
                    cy = gy;
                    baseVox = (localMax >> VX_FRACTION) + 4;
                    found = true;
                }
            }
        }
        TEST_REQUIRE(found, "no suitably low region for the model test.");
    }

    const auto altAt = [](int x, int y) {
        return static_cast<int>(vMap.SGetAlt(x, y));
    };
    const auto geoAt = [](int x, int y) {
        const int off = y * static_cast<int>(vMap.H_SIZE) + x;
        return (static_cast<int>(vMap.VxGBuf[off]) << VX_FRACTION) |
               (vMap.AtrBuf[off] & VX_FRACTION_MASK);
    };

    MapEditor::BrushTool brush(session);
    brush.setModelBrushData(makeRampRaster(), makeFlatIndices(4, 4, 42));
    BrushParams params;
    params.mode = BrushMode::Dam;
    params.modelBrush = true;
    params.modelMode = 0; // place
    // Ramp targets 0, 4, 8, 12 voxels across columns cx-2..cx+1. The west
    // edge (0 voxels) is below the terrain — it ends at the geo height with
    // the dam removed; the east edge reaches 12 voxels over the base.
    const int modelHeight = baseVox + 12;
    params.modelHeight = modelHeight;

    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the place stamp was refused.");
    // Absolute targets: column k of the ramp aims at
    // round(norm_k * modelHeight) voxels; a target at or below the terrain
    // ends at the geo height with the dam removed (and no paint).
    auto expectColumn = [&](int columnX, double norm) {
        // The brush keeps the target in Terra's 1/32 fixed point: one
        // rounding to the nearest representable step, no whole-voxel
        // quantization (that used to stair-step ramp columns).
        const int target32 = static_cast<int>(std::llround(
            norm * static_cast<double>(modelHeight) * (1 << VX_FRACTION)));
        const int geo = geoAt(columnX, cy);
        const int expected = std::max(geo, target32);
        TEST_REQUIRE(altAt(columnX, cy) == expected,
                     "column at x=%d must reach its target (geo %d, target "
                     "%d fixed = %d voxels).",
                     columnX - cx, geo >> VX_FRACTION, target32,
                     target32 >> VX_FRACTION);
        const int off = cy * mapW + columnX;
        if (target32 > geo) {
            TEST_REQUIRE(vMap.VxDBuf[off] != 0 &&
                             vMap.SurBuf[off] == 42,
                         "a column above the terrain keeps the dam and "
                         "paints index 42.");
        } else {
            TEST_REQUIRE(vMap.VxDBuf[off] == 0,
                         "a column at or below the terrain has no dam.");
        }
    };
    expectColumn(cx - 2, 0.0);
    expectColumn(cx - 1, 1.0 / 3.0);
    expectColumn(cx, 2.0 / 3.0);
    expectColumn(cx + 1, 1.0);
    std::printf("[PASS] place mode sets the scaled heights and paints the "
                "palette colours.\n");

    // Undo/redo restores the rectangle (heights + colours).
    TEST_REQUIRE(session.undo(), "undo after the place stamp failed.");
    TEST_REQUIRE(altAt(cx + 1, cy) != (modelHeight << VX_FRACTION),
                 "undo must revert the east column.");
    TEST_REQUIRE(session.redo(), "redo after the place stamp failed.");
    TEST_REQUIRE(altAt(cx + 1, cy) == (modelHeight << VX_FRACTION),
                 "redo must re-apply the east column.");
    std::printf("[PASS] the model stamp is one undo/redo entry.\n");

    // Add/subtract against a flattened rectangle: a flat 1.0 model.
    MapEditor::ModelBrush::ModelRaster flat;
    flat.width = 4;
    flat.height = 4;
    flat.normHeight.assign(16, 1.0);
    flat.rgb.assign(16, 0x808080);
    brush.setModelBrushData(flat, makeFlatIndices(4, 4, 42));
    params.modelMode = 0;
    params.modelHeight = baseVox + 8; // flatten everything above the terrain
    TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the flatten refused.");
    TEST_REQUIRE(altAt(cx - 2, cy) == ((baseVox + 8) << VX_FRACTION) &&
                     altAt(cx + 1, cy) == ((baseVox + 8) << VX_FRACTION),
                 "the flatten must level the rectangle.");
    params.modelMode = 1; // add: average + 1.0 * modelHeight
    params.modelHeight = 8;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the add refused.");
    TEST_REQUIRE(altAt(cx - 2, cy) == ((baseVox + 16) << VX_FRACTION) &&
                     altAt(cx + 1, cy) == ((baseVox + 16) << VX_FRACTION),
                 "add must raise the covered average by the model height.");
    params.modelMode = 2; // subtract: back down, then below the terrain
    TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the subtract refused.");
    TEST_REQUIRE(altAt(cx, cy) == ((baseVox + 8) << VX_FRACTION),
                 "subtract must lower the rectangle back to the flatten.");
    params.modelHeight = 64; // average - 64 -> below zero: dam removed
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the deep subtract refused.");
    {
        const int off = cy * mapW + cx;
        TEST_REQUIRE(altAt(cx, cy) == geoAt(cx, cy) &&
                         vMap.VxDBuf[off] == 0,
                     "a subtract below zero must remove the dam.");
    }
    std::printf("[PASS] add raises from the covered average; subtract "
                "lowers, removing the dam below the terrain.\n");

    // The x/y scales shrink the placement rectangle: a flat model at a
    // height above the terrain, x-scale 0.5 -> a 2x2 stamp at cx-1..cx.
    brush.setModelBrushData(flat, makeFlatIndices(4, 4, 42));
    params.modelMode = 0;
    params.modelHeight = baseVox + 12;
    params.modelScaleX = 0.5; // 4 -> 2 columns: cx-1, cx
    const int guardAlt = altAt(cx + 1, cy);
    const unsigned char guardSur = vMap.SurBuf[cy * mapW + (cx + 1)];
    const int guardGeo = geoAt(cx + 1, cy);
    TEST_REQUIRE(brush.applyStamp(cx, cy, params), "the scaled place refused.");
    TEST_REQUIRE(altAt(cx, cy) == ((baseVox + 12) << VX_FRACTION) &&
                     altAt(cx - 1, cy) == ((baseVox + 12) << VX_FRACTION),
                 "the scaled stamp must cover a 2x2 square at full height.");
    TEST_REQUIRE(altAt(cx + 1, cy) == guardAlt &&
                     vMap.SurBuf[cy * mapW + (cx + 1)] == guardSur &&
                     geoAt(cx + 1, cy) == guardGeo,
                 "a column outside the scaled rectangle must stay untouched.");
    std::printf("[PASS] the x/y scales shrink the placement rectangle.\n");

    // Geo mode refuses.
    params.modelScaleX = 1.0;
    params.mode = BrushMode::Geo;
    TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                 "the model brush in Geo mode must be refused.");
    std::printf("[PASS] the model brush refuses the Geo mode.\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("ModelBrush PLY import", 95,
                         runModelBrushImportTests);
MAP_EDITOR_REGISTER_TEST("ModelBrush application", 96,
                         runModelBrushApplyTests);

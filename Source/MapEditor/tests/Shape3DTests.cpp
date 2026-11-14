// ============================================================================
// 3D shape brushes — Shape3D.h profile math + BrushTool::applyShape3D.
//
// Two suites in this file (one module):
//
//  93 "Shape3D profiles" (pure math, no world):
//     * cone / oblique cone, pyramid (polygon boundary), truncated cone and
//       truncated pyramid (frustum quadratic), half torus (ring), half
//       ellipsoid (dome) — apex 1, base boundary 0, outside 0;
//     * the upper-part clamps: offset norm / top radius / derived top
//       circumradius stay strictly inside the base radius.
//
//  94 "Shape3D brush application" (on a loaded world):
//     * Set-to-Height stamps the absolute shape (Height = base, Shape Height
//       = amplitude), Raise closes on it from below only (a re-stamp over the
//       formed shape is a refused no-op), Lower with Shape Height 0 flattens
//       the footprint back to the base (the dam is removed where the terrain
//       reaches it) — also a refused no-op on the second pass;
//     * the pyramid base is the polygon mask (a cell inside the circle but
//       outside the triangle is untouched);
//     * the shapes refuse the Geo mode;
//     * the stamp is one undo entry (byte-exact restore).
//
// Runs after the other brush tests; reuses the shared temp world.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "Shape3D.h"
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
#include <vector>

namespace fs = std::filesystem;

namespace {

using MapEditor::BrushEffect;
using MapEditor::BrushMode;
using MapEditor::BrushParams;
using MapEditor::Shape3DKind;
using MapEditor::Shape3DSettings;

bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) < eps;
}

// ---------------------------------------------------------------------------
// 93 — pure profile math.
// ---------------------------------------------------------------------------

bool runShape3DProfileTests()
{
    Shape3DSettings base;
    base.radius = 10.0;
    base.sides = 4;

    // Cone: apex at the centre, linear falloff, zero at and past the base.
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Cone, base, 0, 0), 1.0),
                 "cone apex must be 1.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Cone, base, 5, 0), 0.5),
                 "cone mid-radius must be 0.5.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Cone, base, 10, 0), 0.0),
                 "cone base boundary must be 0.");
    TEST_REQUIRE(MapEditor::shape3DProfile(Shape3DKind::Cone, base, 11, 0) == 0.0,
                 "cone outside the base must be 0.");

    // Oblique cone: the apex follows the upper part offset.
    Shape3DSettings oblique = base;
    oblique.topOffsetX = 5.0;
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Cone, oblique, 5, 0), 1.0),
                 "the oblique cone apex sits at the offset.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Cone, oblique, 0, 0), 0.5),
                 "the base centre is mid-height of the oblique cone.");

    // Pyramid on a square base (vertices at 0/90/180/270 deg): the boundary
    // radius at the vertex direction is R, at the edge-mid direction the
    // apothem/cos(45 deg) construction.
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Pyramid, base, 0, 0), 1.0),
                 "pyramid apex must be 1.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Pyramid, base, 0, 5), 0.5),
                 "half way to a vertex the profile is 0.5.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Pyramid, base, 5, 5), 0.0),
                 "the edge-mid direction sits exactly on the face plane.");

    // Truncated cone (top radius 4): flat top under the cap, linear wall.
    Shape3DSettings trunc = base;
    trunc.topRadius = 4.0;
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::TruncatedCone, trunc, 0, 0), 1.0),
                 "truncated cone centre is 1 (under the top cap).");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::TruncatedCone, trunc, 4, 0), 1.0),
                 "the top rim is still 1.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::TruncatedCone, trunc, 7, 0), 0.5),
                 "half way down the wall the profile is 0.5.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::TruncatedCone, trunc, 10, 0), 0.0),
                 "truncated cone base boundary is 0.");

    // Truncated pyramid on the same square base with a top circumradius 4
    // (top side = 2*R1*sin(45 deg)).
    Shape3DSettings truncP = base;
    truncP.topSide = 2.0 * 4.0 * std::sin(3.14159265358979323846 / 4.0);
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::TruncatedPyramid, truncP, 0, 0), 1.0),
                 "truncated pyramid centre is 1.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::TruncatedPyramid, truncP, 4, 0), 1.0),
                 "the top rim is still 1.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::TruncatedPyramid, truncP, 7, 0), 0.5),
                 "half way down the wall the profile is 0.5.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::TruncatedPyramid, truncP, 5, 5), 0.0),
                 "the face-mid direction reaches the wall at the base.");

    // Half torus (inner radius 4): ring peak at the mid radius, zero in the
    // hole and at the outer radius.
    Shape3DSettings torus = base;
    torus.topRadius = 4.0; // inner radius
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Torus, torus, 7, 0), 1.0),
                 "torus ring peak is 1 at the mid radius.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Torus, torus, 0, 0), 0.0),
                 "torus centre (the hole) is 0.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Torus, torus, 10, 0), 0.0),
                 "torus outer radius is 0.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::Torus, torus, 5.5, 0),
                      std::sqrt(0.75), 1e-9),
                 "torus cross-section is the half-circle.");

    // Half ellipsoid: dome profile.
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::HalfEllipsoid, base, 0, 0), 1.0),
                 "ellipsoid apex must be 1.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::HalfEllipsoid, base, 5, 0),
                      std::sqrt(0.75), 1e-9),
                 "ellipsoid profile is the half-circle.");
    TEST_REQUIRE(near(MapEditor::shape3DProfile(Shape3DKind::HalfEllipsoid, base, 10, 0), 0.0),
                 "ellipsoid base boundary is 0.");

    // Clamps: the upper part stays strictly inside the base.
    Shape3DSettings tooBig = base;
    tooBig.topRadius = 20.0;
    tooBig.topSide = 100.0;
    tooBig.topOffsetX = 100.0;
    const Shape3DSettings clampedC = MapEditor::clampShape3DSettings(
        Shape3DKind::TruncatedCone, tooBig);
    TEST_REQUIRE(near(clampedC.topRadius, 9.5),
                 "the top radius clamps to radius - 0.5.");
    const Shape3DSettings clampedP = MapEditor::clampShape3DSettings(
        Shape3DKind::TruncatedPyramid, tooBig);
    TEST_REQUIRE(clampedP.topSide <= 2.0 * 9.5 + 1e-9,
                 "the top side clamps to keep the top polygon inside the base.");
    const Shape3DSettings clampedO = MapEditor::clampShape3DSettings(
        Shape3DKind::Cone, tooBig);
    TEST_REQUIRE(near(clampedO.topOffsetX, 9.5),
                 "the offset norm clamps to radius - 0.5.");

    std::printf("[PASS] Shape3D profiles: cone/oblique cone, pyramid, both "
                "frusta, half torus, half ellipsoid + the upper-part "
                "clamps.\n");
    return true;
}

// ---------------------------------------------------------------------------
// 94 — application on a loaded world.
// ---------------------------------------------------------------------------

bool runShape3DBrushTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    const fs::path loadRoot = ctx.tempRoot / "load_root_shape3d";
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
                 "the world is too small for the shape test.");

    // A low, flat-enough spot: the absolute base target must sit above the
    // terrain so the Set-to-Height flatten is exact.
    const int radius = 8;
    int cx = mapW / 2;
    int cy = mapH / 2;
    int baseVox = 0;
    {
        bool found = false;
        for (int gy = 32; gy < mapH - 32 && !found; gy += 64) {
            for (int gx = 32; gx < mapW - 32 && !found; gx += 64) {
                int localMax = 0;
                for (int dy = -radius - 2; dy <= radius + 2; ++dy) {
                    for (int dx = -radius - 2; dx <= radius + 2; ++dx) {
                        localMax = std::max(
                            localMax,
                            static_cast<int>(vMap.SGetAlt(gx + dx, gy + dy)));
                    }
                }
                if (localMax + ((10 + 4) << VX_FRACTION) < MAX_VX_HEIGHT &&
                    localMax >> VX_FRACTION <= 200) {
                    cx = gx;
                    cy = gy;
                    baseVox = (localMax >> VX_FRACTION) + 4;
                    found = true;
                }
            }
        }
        TEST_REQUIRE(found, "no suitably low region for the shape test.");
    }
    const int base32 = baseVox << VX_FRACTION;
    const int top32 = (baseVox + 10) << VX_FRACTION;

    const auto altAt = [](int x, int y) {
        return static_cast<int>(vMap.SGetAlt(x, y));
    };

    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.radius = radius;
    params.mode = BrushMode::Dam;
    params.shape3d = Shape3DKind::Cone;
    params.shapeHeight = 10;

    // ------------------------------------------------------------------
    // 1: Set-to-Height with Shape Height 0 flattens the footprint to the
    //    Height base (the absolute basement).
    // ------------------------------------------------------------------
    params.effect = BrushEffect::AlignVariableH;
    params.heightLevel = baseVox;
    params.shapeHeight = 0;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the flatten stamp was refused.");
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy > radius * radius) {
                continue;
            }
            TEST_REQUIRE(altAt(cx + dx, cy + dy) == base32,
                         "the flatten stamp must level the cell (%d,%d) to "
                         "the base.",
                         dx, dy);
        }
    }
    std::printf("[PASS] Set-to-Height with Shape Height 0 flattens the "
                "footprint to the base.\n");

    // ------------------------------------------------------------------
    // 2: Raise builds the cone from the base; a re-stamp over the formed
    //    shape is a refused no-op ("if the computed height is reached — do
    //    nothing").
    // ------------------------------------------------------------------
    params.effect = BrushEffect::ChangeHeight;
    params.direction = 1;
    params.shapeHeight = 10;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the cone raise was refused.");
    TEST_REQUIRE(altAt(cx, cy) == top32,
                 "the cone centre must reach base + shapeHeight.");
    TEST_REQUIRE(altAt(cx + radius, cy) == base32,
                 "the cone base boundary stays at the base height.");
    TEST_REQUIRE(altAt(cx + radius / 2, cy) == base32 + (5 << VX_FRACTION),
                 "the cone wall is linear (mid-radius = half amplitude).");
    TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                 "re-stamping the formed cone must be a refused no-op.");
    std::printf("[PASS] Raise builds the cone from the base; the formed "
                "shape refuses a re-stamp.\n");

    // ------------------------------------------------------------------
    // 3: the stamps are undo/redo entries (byte-exact swap semantics).
    // ------------------------------------------------------------------
    TEST_REQUIRE(session.undo(), "undo after the cone raise failed.");
    TEST_REQUIRE(altAt(cx, cy) == base32,
                 "undo must remove the cone (centre back at the base).");
    TEST_REQUIRE(session.redo(), "redo after the cone raise failed.");
    TEST_REQUIRE(altAt(cx, cy) == top32,
                 "redo must re-apply the cone.");
    std::printf("[PASS] the shape stamp is one undo/redo entry.\n");

    // ------------------------------------------------------------------
    // 4: Lower with Shape Height 0 removes the shape back to the base and
    //    refuses the second pass.
    // ------------------------------------------------------------------
    params.direction = -1;
    params.shapeHeight = 0;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the removal stamp was refused.");
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy > radius * radius) {
                continue;
            }
            TEST_REQUIRE(altAt(cx + dx, cy + dy) == base32,
                         "the removal must return the cell (%d,%d) to the "
                         "base.",
                         dx, dy);
        }
    }
    TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                 "re-removal must be a refused no-op.");
    std::printf("[PASS] Lower with Shape Height 0 removes the shape back to "
                "the base.\n");

    // ------------------------------------------------------------------
    // 4: the pyramid base is the polygon mask — a cell inside the circle but
    //    outside the triangle is untouched.
    // ------------------------------------------------------------------
    const int triOutX = cx;             // 90 deg: between the triangle's
    const int triOutY = cy + radius - 1; // vertices at 0/120/240 deg.
    const int triInX = cx + radius - 1;  // 0 deg: the vertex direction.
    const int triInY = cy;
    const int altOutBefore = altAt(triOutX, triOutY);
    const uint16_t damOutBefore = vMap.VxDBuf[triOutY * mapW + triOutX];
    const int altInBefore = altAt(triInX, triInY);
    params.shape3d = Shape3DKind::Pyramid;
    params.polygonSides = 3;
    params.effect = BrushEffect::AlignVariableH;
    params.direction = 0;
    params.shapeHeight = 0;
    // A DIFFERENT base than the flattened circle: the whole triangle must
    // move, otherwise the no-op precheck refuses the stamp.
    params.heightLevel = baseVox + 3;
    const int triTarget = (baseVox + 3) << VX_FRACTION;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the pyramid stamp was refused.");
    TEST_REQUIRE(altAt(triOutX, triOutY) == altOutBefore &&
                     vMap.VxDBuf[triOutY * mapW + triOutX] == damOutBefore,
                 "a cell outside the triangle base must stay untouched.");
    TEST_REQUIRE(altAt(triInX, triInY) != altInBefore,
                 "a cell inside the triangle base must be levelled.");
    TEST_REQUIRE(altAt(triInX, triInY) == triTarget,
                 "the covered cell levels to the pyramid base.");
    std::printf("[PASS] the pyramid base is the polygon footprint (outside "
                "cells untouched).\n");

    // ------------------------------------------------------------------
    // 5: Geo mode refuses the shapes (the world stays byte-identical).
    // ------------------------------------------------------------------
    params.mode = BrushMode::Geo;
    params.effect = BrushEffect::ChangeHeight;
    params.direction = 1;
    params.shapeHeight = 10;
    const int guardX = cx - radius - 4;
    const int guardY = cy - radius - 4;
    const int guardAlt = altAt(guardX, guardY);
    TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                 "a 3D shape in Geo mode must be refused.");
    TEST_REQUIRE(altAt(guardX, guardY) == guardAlt,
                 "the refused Geo stamp must not touch the world.");
    std::printf("[PASS] the 3D shapes refuse the Geo mode.\n");

    // ------------------------------------------------------------------
    // 7: the whole session history drains through undo (the shape stamps
    //    participate in the common history).
    // ------------------------------------------------------------------
    int undos = 0;
    while (session.undo()) {
        ++undos;
    }
    TEST_REQUIRE(undos >= 4,
                 "the shape stamps must be undoable history entries.");
    std::printf("[PASS] the shape stamps participate in the undo history "
                "(%d entries drained).\n",
                undos);

    return true;
}

// ---------------------------------------------------------------------------
// 119 — Set Color over a 3D solid paints the solid's voxel volume WITHOUT
// touching any heights (applyShape3DPaint).
// ---------------------------------------------------------------------------

bool runShape3DPaintTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    const fs::path loadRoot = ctx.tempRoot / "load_root_shape3dpaint";
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
    session.setUndoCoalescingWindowMs(0);
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");

    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    TEST_REQUIRE(mapW >= 512 && mapH >= 512,
                 "the world is too small for the shape paint test.");

    const int radius = 8;
    int cx = mapW / 2;
    int cy = mapH / 2;
    int baseVox = 0;
    {
        bool found = false;
        for (int gy = 32; gy < mapH - 32 && !found; gy += 64) {
            for (int gx = 32; gx < mapW - 32 && !found; gx += 64) {
                int localMax = 0;
                for (int dy = -radius - 2; dy <= radius + 2; ++dy) {
                    for (int dx = -radius - 2; dx <= radius + 2; ++dx) {
                        localMax = std::max(
                            localMax,
                            static_cast<int>(vMap.SGetAlt(gx + dx, gy + dy)));
                    }
                }
                if (localMax >> VX_FRACTION <= 200) {
                    cx = gx;
                    cy = gy;
                    baseVox = (localMax >> VX_FRACTION) + 4;
                    found = true;
                }
            }
        }
        TEST_REQUIRE(found, "no suitably low region for the shape paint test.");
    }
    const std::uint32_t kPaintRgb = 0x00AA3366;

    const auto altAt = [](int x, int y) {
        return static_cast<int>(vMap.SGetAlt(x, y));
    };
    const auto layerAt = [](int x, int y, int z, std::uint32_t* rgb) {
        return vMap.voxLayers.lookup(
            static_cast<std::uint32_t>(y * static_cast<int>(vMap.H_SIZE) + x),
            z, rgb);
    };

    MapEditor::BrushTool brush(session);
    BrushParams params;
    params.radius = radius;
    params.mode = BrushMode::Dam;
    params.shape3d = Shape3DKind::Cone;
    params.shapeHeight = 10;
    params.effect = BrushEffect::SetColor;
    params.colorRgb = kPaintRgb;
    params.heightLevel = baseVox;
    params.paintInterior3d = true;

    // Heights BEFORE — the paint stamp must not move a single cell.
    std::vector<int> altsBefore;
    for (int y = cy - radius - 1; y <= cy + radius + 1; ++y) {
        for (int x = cx - radius - 1; x <= cx + radius + 1; ++x) {
            altsBefore.push_back(altAt(x, y));
        }
    }

    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the interior-paint stamp was refused.");

    // Heights are byte-identical after the paint stamp.
    {
        std::size_t k = 0;
        for (int y = cy - radius - 1; y <= cy + radius + 1; ++y) {
            for (int x = cx - radius - 1; x <= cx + radius + 1; ++x, ++k) {
                TEST_REQUIRE(altAt(x, y) == altsBefore[k],
                             "the paint stamp must not change heights "
                             "(%d,%d).",
                             x - cx, y - cy);
            }
        }
    }
    std::printf("[PASS] the shape paint stamp leaves all heights unchanged.\n");

    // The interior is painted per the cone profile: the apex column carries
    // [base .. base+10], the boundary column [base .. base].
    {
        std::uint32_t rgb = 0;
        for (int z = baseVox; z <= baseVox + 10; ++z) {
            TEST_REQUIRE(layerAt(cx, cy, z, &rgb) && rgb == kPaintRgb,
                         "the apex column layer %d must carry the paint "
                         "color.",
                         z);
        }
        TEST_REQUIRE(layerAt(cx + radius, cy, baseVox, &rgb) &&
                         rgb == kPaintRgb,
                     "the boundary column must carry the base layer.");
        TEST_REQUIRE(!layerAt(cx, cy, baseVox + 11, &rgb),
                     "above the shape surface nothing is painted.");
        TEST_REQUIRE(!layerAt(cx, cy, baseVox - 1, &rgb),
                     "below the shape base nothing is painted.");
    }
    std::printf("[PASS] the cone interior is painted per the profile.\n");

    // A re-stamp with the same color is a refused no-op (no history entry).
    TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                 "a same-color re-stamp must be a refused no-op.");
    std::printf("[PASS] a same-color re-stamp is a refused no-op.\n");

    // Undo removes the paint (the layer data is part of the snapshot).
    TEST_REQUIRE(session.undo(), "the paint stamp undo failed.");
    {
        std::uint32_t rgb = 0;
        TEST_REQUIRE(!layerAt(cx, cy, baseVox + 10, &rgb) ||
                         rgb != kPaintRgb,
                     "undo must remove the painted layers.");
    }
    TEST_REQUIRE(session.redo(), "the paint stamp redo failed.");
    {
        std::uint32_t rgb = 0;
        TEST_REQUIRE(layerAt(cx, cy, baseVox + 10, &rgb) && rgb == kPaintRgb,
                     "redo must restore the painted layers.");
        TEST_REQUIRE(session.undo(), "the second undo failed.");
    }
    std::printf("[PASS] the paint stamp participates in undo/redo.\n");

    // Slab mode: Paint interior OFF paints only [top-depth+1 .. top].
    params.paintInterior3d = false;
    params.paintDepth = 2;
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the slab-paint stamp was refused.");
    {
        std::uint32_t rgb = 0;
        // Apex top = base + 10: the slab is [base+9 .. base+10].
        TEST_REQUIRE(layerAt(cx, cy, baseVox + 10, &rgb) && rgb == kPaintRgb,
                     "the apex slab top layer must be painted.");
        TEST_REQUIRE(layerAt(cx, cy, baseVox + 9, &rgb) && rgb == kPaintRgb,
                     "the apex slab must span the depth.");
        TEST_REQUIRE(!layerAt(cx, cy, baseVox + 8, &rgb),
                     "below the slab nothing is painted.");
        // Boundary top = base: the slab is [base .. base].
        TEST_REQUIRE(layerAt(cx + radius, cy, baseVox, &rgb) &&
                         rgb == kPaintRgb,
                     "the boundary slab must paint the base layer.");
    }
    std::printf("[PASS] slab mode paints [top-depth+1 .. top] only.\n");

    // An explicit paint height (the H spin / MCP paintHeight / the JS height
    // argument) overrides the Level as the volume base.
    params.paintInterior3d = true;
    params.paintHeight = baseVox + 30;
    params.heightLevel = baseVox; // would be the base without the override
    TEST_REQUIRE(brush.applyStamp(cx, cy, params),
                 "the explicit-base paint stamp was refused.");
    {
        std::uint32_t rgb = 0;
        TEST_REQUIRE(layerAt(cx, cy, baseVox + 30, &rgb) && rgb == kPaintRgb,
                     "the explicit base layer must be painted.");
        TEST_REQUIRE(layerAt(cx, cy, baseVox + 40, &rgb) && rgb == kPaintRgb,
                     "the explicit-base apex (base+30+10) must be painted.");
        TEST_REQUIRE(!layerAt(cx, cy, baseVox + 29, &rgb),
                     "below the explicit base nothing new is painted.");
    }
    params.paintHeight = -1;
    params.paintInterior3d = false;
    std::printf("[PASS] an explicit paint height overrides the Level base.\n");

    // Geo mode refuses the shape paint like the height-moving shapes.
    params.mode = BrushMode::Geo;
    TEST_REQUIRE(!brush.applyStamp(cx, cy, params),
                 "the shape paint in Geo mode must be refused.");

    while (session.undo()) {
    }
    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Shape3D profiles", 93, runShape3DProfileTests);
MAP_EDITOR_REGISTER_TEST("Shape3D brush application", 94,
                         runShape3DBrushTests);
MAP_EDITOR_REGISTER_TEST("Shape3D color paint (no height change)", 119,
                         runShape3DPaintTests);

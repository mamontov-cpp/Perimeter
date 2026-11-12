#pragma once

#include "EditorSession.h"
#include "ImageDecal.h"
#include "ModelBrush.h"
#include "Shape3D.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// ============================================================================
// MapEditorCore — BrushTool (T3.1 framework, T3.2 height tools).
//
// The brush framework all concrete T3.x brushes build upon:
//
//   * BrushParams — the full brush configuration from the plan
//     (shape, radius, strength, Geo/Dam mode, palette colour, effect).
//   * BrushMask / BrushMasks — the stamp geometry: filled/outline
//     circle/square generated with the same ring rounding Terra's own radius
//     tables use (FloorGreedySqrt, docs/architecture.md §7), a line =
//     Bresenham walk with the stamp unioned into one mask.
//   * BrushTool — applies the effect over a mask:
//       1. record the pre-edit snapshot into the session undo stack
//          (EditorSession::recordUndoArea — Terra's UndoDispatcher_
//          PutPreChangedArea is a _SURMAP_-only stub, so the session runs
//          its own history with identical swap semantics);
//       2. route each mask cell through Terra's individualToolzer
//          (tVoxSet — hardness scaling, zeroplast attributes, geo/dam layer
//          handling are all Terra's own behaviour, not reimplemented; the
//          alignment tools use tVoxSetAll, i.e. they ignore the hardness):
//          * ChangeHeight (T3.2) — T2TE_CHANGING_TERRAIN_HEIGHT; the
//            BrushMode Geo/Dam difference is applied around the toolzer
//            (Geo: only VxGBuf, dam-capped; Dam: seeds/removes VxDBuf);
//          * Align4ZP / AlignVariableH (T3.2) — T2TE_ALIGNMENT_* with the
//            target height (hZeroPlast / heightLevel) synced per cell;
//       3. vMap.recalcArea2Grid() + vMap.regRender() over the stamp bbox so
//          the grid and the viewport dirty rectangles update like in the game.
//
// Everything works on map cells (vMap buffer space) and is Qt-free, so the
// headless tests drive it directly.
// ============================================================================

namespace MapEditor {

/// Stamp silhouette drawn under the cursor / used for the mask.
/// The last three are the UNFILLED shapes: their cells form a band of
/// BrushParams::thickness cells instead of the whole area (see BrushMasks).
enum class BrushShape
{
    FilledCircle,   ///< whole disc, rings 0..radius (game `influenceDZ` shape).
    FilledSquare,   ///< solid square, |dx|<=radius and |dy|<=radius.
    CircleOutline,  ///< ring band at radius (Terra's own `maxRad[r]` ring at thickness 1).
    SquareOutline,  ///< square border band at radius.
    FilledPolygon,  ///< solid regular polygon inscribed into the radius circle.
    PolygonOutline, ///< regular-polygon border band at the radius (inset like the other outlines).
    Line,           ///< Bresenham line, circle stamp; endpoints via applyLine().
};

/// Which terrain layer the brush addresses. Geo = the layer under a dam,
/// Dam = the deployed dam layer (higher priority than Geo in the game).
enum class BrushMode
{
    Geo,
    Dam,
};

/// Per-voxel height condition (Dam mode only) that gates the application of
/// ANY brush effect: the effect touches a covered voxel only when the current
/// dam surface height of THAT voxel compares to gateHeight as the condition
/// says. The check is per-voxel, not per-stamp: one stamp can edit some cells
/// and skip their neighbours. None = the modifier is off (the default).
enum class BrushHeightGate
{
    None,   ///< no condition — every covered voxel is edited (default);
    Above,  ///< apply only where the voxel height is STRICTLY ABOVE gateHeight;
    Below,  ///< apply only where the voxel height is STRICTLY BELOW gateHeight.
};

/// What the brush does to the world. T3.1 ships the framework, T3.2 routes
/// the height tools (raise/lower + the two alignment levellers); the
/// remaining values are the T3.3–T3.5 tools — apply() refuses them until
/// their tasks implement the routing.
enum class BrushEffect
{
    None = 0,   ///< No world edit (preview import/only-mask use cases).
    ChangeHeight,   ///< Raise/lower through T2TE_CHANGING_TERRAIN_HEIGHT (T3.2);
                    ///< the per-layer Geo/Dam handling of the T3.2 modes happens
                    ///< around Terra's own toolzer (see BrushMode).
    Align4ZP,       ///< T3.4 (merged): level to the world zeroplast height AND
                    ///< mark the cells that reached it At_ZEROPLAST — the
                    ///< former Align4ZP and PlotZeroplast brushes in one tool
                    ///< (Terra's T2TE_ALIGNMENT_TERRAIN_4ZP +
                    ///< T2TE_PLOTTING_ZEROPLAST per cell). Absolute set like
                    ///< AlignVariableH: one stamp reaches the level from any
                    ///< height, strength does not step it.
    AlignVariableH, ///< T3.2: set to an arbitrary height in one stamp — a
                    ///< full-influence cell moves by the whole remaining
                    ///< distance to params.heightLevel (absolute set, not an
                    ///< incremental step). Routes through Terra's
                    ///< T2TE_ALIGNMENT_TERRAIN_VARIABLE_H.

    // --- routing entry points for later tasks (not applied) ---------------
    SetColor,       ///< T3.3: SurBuf := params.colorIndex, height untouched.
    ImageDecal,     ///< Palette-limited image stamp: SurBuf := the palette index
                    ///< of the matching pixel of the decal image
                    ///< (BrushTool::setDecalImage), Dam layer only. Works like
                    ///< SetColor per pixel; transparent decal pixels are
                    ///< skipped entirely.
    PickColor,      ///< T4.2: eyedropper — sample the Geo/Dam surface pixel
                    ///< (SurBuf index) at the stamp centre into the color
                    ///< brush; no buffer is written.
    PlotSoot,       ///< T3.4: T2TE_PLOTTING_SOOT.
    VeryLightDam,   ///< T3.4: T2TE_PLOTTING_VERY_LIGHT_DAM.
    /// Scripted brush (JavaScript via duktape): applyStamp() runs
    /// params.script once at the stamp centre; the script drives the
    /// editor brush API (raise/lower/set height, zeroplast, soot, very
    /// light dam, palette painting). Dam mode only; the whole evaluation
    /// is ONE undo entry and honours a time limit (see ScriptedBrush).
    Script,

    /// Sculpting — Dam mode only. Two-pass stamp: the average surface
    /// height under the mask is computed first, then every covered cell
    /// moves a fraction (params.sculptEffect, 0..1) of the way towards
    /// that average. One stamp = one undo entry.
    Smooth,
    /// Sculpting — Dam mode only. Like Smooth, but the reference is a
    /// least-squares plane (linear regression z = a*dx + b*dy + c) fitted
    /// over the pre-edit heights under the mask: cells move towards the
    /// plane by the sculptEffect fraction, preserving large-scale slopes
    /// while removing bumps. Falls back to the Smooth average when the
    /// fit is degenerate.
    Flatten,
    /// Sculpting — Dam mode only. The reference is a TRICUBIC (bicubic
    /// Catmull-Rom) interpolation of a coarse lattice built from the
    /// pre-edit heights under the stamp: block-averaged control points at
    /// a fixed stride, interpolated smoothly back at cell resolution.
    /// Removes features smaller than the stride while preserving (curved)
    /// large-scale forms better than a global plane. Same sculptEffect
    /// fraction and toolzer write path as Smooth / Flatten.
    Tricubic,
    /// Sculpting — Dam mode only. Offsets the pre-edit surface along its
    /// VOXEL NORMALS (central-difference gradient): every covered cell
    /// displaces by a fixed step along its normal, the displaced points
    /// rasterize into a target height field (columns hit by several points
    /// keep the HIGHEST z — the growing envelope), and each covered cell
    /// moves the sculptEffect fraction towards the target height at its
    /// column. Grows bumps/slopes outward-and-up.
    Inflate,
    /// Sculpting — Dam mode only. The exact inverse of Inflate: the offset
    /// step is negative and overlapping displaced points keep the LOWEST z
    /// (the shrinking envelope). Shrinks forms inward-and-down.
    Deflate,
    /// Sculpting — Dam mode only. The reference is a normalized GAUSSIAN
    /// BLUR of the pre-edit height field (convolution with an isotropic
    /// kernel of params.blurRadius, sigma = blurRadius/2): every covered
    /// cell moves the sculptEffect fraction towards its blurred height.
    /// Preserves planes exactly (weights sum to 1); kills noise smaller
    /// than the blur radius.
    GaussianBlur,
};

/// The largest stamp radius the editor allows (map cells). The game's own
/// tools are capped by Terra's xRad/yRad/maxRad table length
/// (MAX_RADIUS_CIRCLEARR), but the editor builds its masks analytically from
/// the same ring rounding (BrushMasks::circularRing), so that table length
/// is not a cap here — 512 is a pure editor policy limit.
inline constexpr int kMaxBrushRadius = 512;

/// Regular-polygon stamp vertex-count domain (BrushParams::polygonSides).
inline constexpr int kMinPolygonSides = 3;
inline constexpr int kMaxPolygonSides = 20;

/// Full brush configuration (plan §5 T3.1: shape/radius/strength/mode/
/// colorIndex/effect) + the parameters of the height tools (T3.2).
struct BrushParams
{
    BrushShape shape = BrushShape::FilledCircle;
    int radius = 8;      ///< stamp half-size in map cells (clamped to kMaxBrushRadius).
    double strength = 30.0; ///< 1..100: per-stamp delta in voxels *at full
                            ///< influence* (fractional voxels allowed - converted
                            ///< to the 1/32 fixed point at stamp time).
    BrushMode mode = BrushMode::Geo;   ///< Geo/Dam target layer (rules per effect, T3.2).
    int colorIndex = 0;                ///< legacy palette index (eyedropper side band).
    /// Exact paint color for SetColor, 0x00RRGGBB. Written into the
    /// true-color layer (vrtMap::SurColBuf); the nearest palette entry of
    /// the cell's layer is kept in SurBuf as the surface type. Set by the
    /// color picker, the eyedropper (effective cell color) and the
    /// script/MCP paint calls — never quantized.
    std::uint32_t colorRgb = 0x00FFFFFF;
    /// Layer-paint height in whole voxels (absolute, from 0); -1 = the
    /// column top (the default paint). A height at/above the current
    /// surface clamps to the top (default paint); below — paints internal
    /// voxel layers [height-depth+1 .. height].
    int paintHeight = -1;
    /// How many voxel layers downward a below-surface paint covers (>=1).
    int paintDepth = 1;
    /// 3D solid brushes: paint the interior of the shape — every voxel
    /// level from the shape base up to its surface at the column gets the
    /// brush color (clipped at the highest part).
    bool paintInterior3d = false;
    BrushEffect effect = BrushEffect::ChangeHeight;

    /// Height effect direction: +1 raise, -1 lower, 0 — no height change.
    /// Only the ChangeHeight effect uses it; the alignment effects take the
    /// direction per cell (towards the target level).
    int direction = 1;

    /// AlignVariableH target height in whole voxels (0..255, clamped; 1 voxel
    /// = 1/32 m). Level-to-zeroplast (Align4ZP) reads vMap.hZeroPlast instead.
    /// Target/set height in VOXELS - fractional values allowed. Converted to
    /// Terra's 1/32-voxel fixed point (VX_FRACTION) exactly once, at stamp
    /// time, so scripted/MCP heights like 10.4 land on the nearest supported
    /// fixed-point value instead of truncating to a whole voxel.
    double heightLevel = 32.0;

    /// Stroke width in cells of the UNFILLED shapes
    /// (CircleOutline, SquareOutline, Line). 0 = the full stamp extent (the
    /// pre-thickness behaviour: one-cell ring/border for outlines, full-width
    /// line for the Line shape). A positive value sets the band width:
    /// outlines grow inward from the outer edge so a stamp never edits outside
    /// the radius footprint; the Line shape centres the stroke on the Bresenham
    /// path. Ignored by the filled shapes, the image mask and the decal.
    /// Clamped into [0, maxThickness(radius)].
    int thickness = 0;

    /// Regular-polygon shapes (FilledPolygon / PolygonOutline): the vertex
    /// count, clamped to [kMinPolygonSides, kMaxPolygonSides].
    int polygonSides = 6;
    /// Regular-polygon shapes: the angle offset of the first vertex in
    /// degrees, wrapped into [0,360). 0 points the vertex at +x (map east);
    /// the angle grows towards +y — on screen that is CLOCKWISE, because the
    /// map raster's y axis grows downwards (same convention the 2D preview
    /// and the GPU contour projection draw).
    double polygonAngleDeg = 0.0;

    /// Scripted brush (BrushEffect::Script): the JavaScript source executed
    /// at the stamp centre. Edited in the script editor dialog; empty means
    /// "not configured yet" (the stamp refuses with a clear error).
    std::string script;

    /// Sculpting brushes (Smooth / Flatten): the fraction (0..1) of the
    /// distance towards the reference (average / fitted plane) that one
    /// stamp moves each covered cell. 1.0 reaches the reference in one
    /// stamp, smaller values blend progressively.
    double sculptEffect = 0.5;

    /// GaussianBlur: the convolution radius in cells — strictly LESS than
    /// the stamp radius (BrushTool::clampBlurRadius enforces it); the
    /// kernel sigma is blurRadius/2.
    int blurRadius = 4;

    /// Height gate (Dam mode only, combinable with every effect): the
    /// per-voxel condition restricting which covered voxels the effect
    /// touches. Ignored in Geo mode.
    BrushHeightGate heightGate = BrushHeightGate::None;
    /// The gate comparison height in whole voxels
    /// (0..MAX_VX_HEIGHT_WHOLE, BrushTool::clampGateHeight).
    int gateHeight = 0;

    /// T3.5 (reworked): when true, the loaded image mask (BrushTool::
    /// setImageMask) replaces the shape/radius stamp as the brush geometry
    /// for every terrain effect — the image pixel value is the per-cell
    /// influence. Selecting a shape (FilledCircle..Line) clears the flag, so
    /// the mask source only changes when the user explicitly picks a shape
    /// or loads a new image.
    bool useImageMask = false;

    /// 3D shape brushes (sculpting toolbar): the solid stamped instead of a
    /// flat mask — the footprint is the shape's BASE (circle or regular
    /// polygon at the brush radius), and each covered cell's target is
    /// `Height + profile(cell) * shapeHeight`, where profile is the
    /// normalized shape cross-section (Shape3D.h). Dam mode only; combines
    /// with the Raise/Lower direction (cells move towards the target from
    /// below / above only) and the Set-to-Height brush (absolute base =
    /// Height, both directions). None = the plain 2D stamp geometry.
    Shape3DKind shape3d = Shape3DKind::None;
    /// The shape amplitude in whole voxels (0..MAX_VX_HEIGHT_WHOLE) — the
    /// "affected change height" of the 3D brushes.
    int shapeHeight = 32;
    /// Upper part centre offset in cells (cone / pyramid / truncated cone /
    /// truncated pyramid); clamped to keep the top strictly inside the base.
    double shapeTopOffsetX = 0.0;
    double shapeTopOffsetY = 0.0;
    /// Upper part radius in cells: the truncated cone's top radius or the
    /// torus' inner radius; must stay below the base radius.
    double shapeTopRadius = 0.0;
    /// Upper part side length in cells (truncated pyramid); converted to the
    /// top circumradius through the polygon side count, clamped to stay
    /// below the base.
    double shapeTopSide = 0.0;

    /// Model-heightmap brush (Model toolbar): the imported model raster is
    /// the stamp — a rectangle of the scaled model size. Dam mode only.
    /// Mode 0 "place" sets the dam to the scaled model height and paints
    /// the quantized Dam palette colours (a Set-to-Height style stamp);
    /// mode 1 "add" raises the covered area's average height by the local
    /// model height; mode 2 "subtract" lowers it (the dam is removed where
    /// the result drops to the terrain).
    bool modelBrush = false;
    int modelMode = 0;      ///< 0 place, 1 add, 2 subtract.
    int modelHeight = 32;   ///< whole voxels; the model z range maps onto it.
    double modelScaleX = 1.0; ///< 0..1, the placement size along x.
    double modelScaleY = 1.0; ///< 0..1, the placement size along y.

    bool operator==(const BrushParams& other) const
    {
        return shape == other.shape && radius == other.radius &&
               strength == other.strength && mode == other.mode &&
               colorIndex == other.colorIndex && effect == other.effect &&
               colorRgb == other.colorRgb &&
               paintHeight == other.paintHeight &&
               paintDepth == other.paintDepth &&
               paintInterior3d == other.paintInterior3d &&
               direction == other.direction &&
               heightLevel == other.heightLevel &&
               thickness == other.thickness &&
               polygonSides == other.polygonSides &&
               // Epsilon compare: the angle comes out of wrapping/trig math
               // where bit-exact equality is not guaranteed.
               std::abs(polygonAngleDeg - other.polygonAngleDeg) < 1e-9 &&
               // Same for the sculpt fraction (comes out of spinbox math).
               std::abs(sculptEffect - other.sculptEffect) < 1e-9 &&
               blurRadius == other.blurRadius &&
               heightGate == other.heightGate &&
               gateHeight == other.gateHeight &&
               script == other.script &&
               useImageMask == other.useImageMask &&
               shape3d == other.shape3d &&
               shapeHeight == other.shapeHeight &&
               std::abs(shapeTopOffsetX - other.shapeTopOffsetX) < 1e-9 &&
               std::abs(shapeTopOffsetY - other.shapeTopOffsetY) < 1e-9 &&
               std::abs(shapeTopRadius - other.shapeTopRadius) < 1e-9 &&
               std::abs(shapeTopSide - other.shapeTopSide) < 1e-9 &&
               modelBrush == other.modelBrush &&
               modelMode == other.modelMode &&
               modelHeight == other.modelHeight &&
               std::abs(modelScaleX - other.modelScaleX) < 1e-9 &&
               std::abs(modelScaleY - other.modelScaleY) < 1e-9;
    }
};

/// Brush stamp geometry: bbox offset (mask coordinates relative to the stamp
/// centre; (x,y) = (-(sx/2), -(sy/2)) for the symmetric shapes), size and the
/// per-cell influence in 0..255 (255 = full power; partial influences arrive
/// with the brush-image tool, T3.5).
struct BrushMask
{
    int x = 0;
    int y = 0;
    int sx = 0;
    int sy = 0;
    std::vector<unsigned char> influence;   ///< sx*sy cells, row-major.

    bool empty() const
    {
        return sx <= 0 || sy <= 0 || influence.size() != static_cast<std::size_t>(sx) * sy;
    }
    unsigned char at(int dx, int dy) const
    {
        return influence[static_cast<std::size_t>(dy) * sx + dx];
    }
    /// Convert the mask cell offset into cell coordinates around the stamp centre.
    void translate(int centreX, int centreY)
    {
        x += centreX;
        y += centreY;
    }
};

/// Mask land shape generators (T3.1). All coordinates are map cells; masks are
/// centred at the map cell (x/y offsets are relative to the stamp centre).
/// For the outline shapes, `thickness` controls the band width (cells deep
/// from the outer edge inward); 1 = the original one-cell ring/border.
class BrushMasks
{
public:
    /// Whole disc ring 0..radius — the shape Terra's influenceDZ affects.
    static BrushMask filledCircle(int radius);
    /// Circle band: rings [radius-thickness+1, radius] — thickness=1 is the
    /// one-cell ring exactly at radius (Terra's own `maxRad[r]` ring).
    static BrushMask circleOutline(int radius, int thickness = 1);
    /// Solid square |dx|<=radius, |dy|<=radius.
    static BrushMask filledSquare(int radius);
    /// Square border band: max(|dx|,|dy|) in [radius-thickness+1, radius].
    static BrushMask squareOutline(int radius, int thickness = 1);
    /// Vertices of the regular polygon the polygon shapes are built from:
    /// `sides` points on the circle of `radius` cells, the first vertex at
    /// `angleOffsetDeg` (0 = +x / map east, growing towards +y — clockwise
    /// on screen, the map raster's y axis grows downwards). Qt-free so the
    /// UI preview reuses exactly the same geometry. `sides` is clamped to
    /// [BrushTool::kMinPolygonSides, BrushTool::kMaxPolygonSides].
    static std::vector<std::pair<double, double>>
    regularPolygonVertices(double radius, int sides, double angleOffsetDeg);
    /// Filled regular polygon inscribed into the radius circle (even-odd
    /// fill of the vertex loop), with the vertices rounded to whole cells
    /// like every other mask here.
    static BrushMask filledPolygon(int radius, int sides, double angleOffsetDeg);
    /// Regular-polygon border band: the cells within `thickness` of the
    /// boundary, growing INWARD from the outer edge (plus the sub-cell
    /// fringe straddling the ideal edge — the same inset-band rule the
    /// circle/square outlines use), so a thick stroke never leaves the
    /// radius footprint. thickness <= 0 → the one-cell boundary stroke.
    static BrushMask polygonOutline(int radius, int sides, double angleOffsetDeg,
                                    int thickness);
    /// Mask builder for a BrushShape (Line has no stamps without endpoints —
    /// use stampLine(); returns an empty mask there). Thickness is ignored for
    /// the filled shapes and for Line (which uses its own stamp logic); the
    /// polygon params are used by the polygon shapes only.
    static BrushMask byShape(BrushShape shape, int radius, int thickness = 1,
                             int polygonSides = 6, double polygonAngleDeg = 0.0);

    /// The Line shape: Bresenham from (x0,y0) to (x1,y1), union of
    /// filled-circle stamps of the given radius. Each map cell is counted
    /// exactly once (stamp overlaps do not accumulate the effect within one
    /// line); the influence over a cell is thus one stamp's delta, not a sum.
    /// `strokeRadius` is the radius of the filled-circle stamp used at every
    /// Bresenham step: for the unfilled Line shape it equals (thickness-1)/2
    /// (inset in the brush footprint), while for the filled shapes it equals
    /// the full brush radius (thickness=0, the default).
    static BrushMask lineStamp(int x0, int y0, int x1, int y1, int strokeRadius);

    /// Whether this shape is unfilled (supports BrushParams::thickness).
    static bool isUnfilledShape(BrushShape shape)
    {
        return shape == BrushShape::CircleOutline ||
               shape == BrushShape::SquareOutline ||
               shape == BrushShape::PolygonOutline ||
               shape == BrushShape::Line;
    }

    /// Circle ring number of (dx,dy) — truncating sqrt, exactly the rounding
    /// vrtMap::landPrepare() (LAND.cpp:77) uses for its radius tables.
    static int circularRing(int dx, int dy);
};

// ---------------------------------------------------------------------------
// BrushTool
// ---------------------------------------------------------------------------

class BrushTool
{
public:
    explicit BrushTool(EditorSession& session);

    /// Max possible stamp radius — kMaxBrushRadius (an editor policy cap:
    /// the masks are analytic, Terra's radius table length does not apply).
    static int maxRadius();
    /// Radius clamped into [0, maxRadius()].
    static int clampRadius(int radius);
    /// Strength clamped into [1,100] (percent of the full per-stamp delta).
    static int clampStrength(int strength);
    /// Fractional variant: clamps the per-stamp delta in voxels.
    static double clampStrength(double strength);
    /// Alignment target height clamped into [0, MAX_VX_HEIGHT_WHOLE] whole
    /// voxels — the AlignVariableH target domain (the UI spinbox uses it too).
    static int clampHeightLevel(int level);
    /// Fractional variant: clamps in voxels, keeps the fraction.
    static double clampHeightLevel(double level);
    /// Thickness clamped into [0, maxThickness(radius)]; 0 = "auto" (the
    /// pre-thickness behaviour of the shape).
    static int clampThickness(int thickness, int radius);
    /// Regular-polygon vertex count clamped into
    /// [kMinPolygonSides, kMaxPolygonSides].
    static int clampPolygonSides(int sides);
    /// Regular-polygon first-vertex angle wrapped into [0,360).
    static double clampPolygonAngleDeg(double deg);
    /// Sculpting fraction clamped into [0,1] (Smooth / Flatten).
    static double clampSculptEffect(double effect);
    /// Height-gate comparison height clamped into [0, MAX_VX_HEIGHT_WHOLE]
    /// whole voxels — the same domain as the AlignVariableH target.
    static int clampGateHeight(int level);
    /// Gaussian-blur convolution radius clamped into
    /// [1, stampRadius - 1] — the blur must stay smaller than the stamp.
    static int clampBlurRadius(int blurRadius, int stampRadius);
    /// Largest stroke width the unfilled shapes can take: the stamp's own
    /// extent (2*radius+1 cells) — a thicker band would leave the footprint.
    static int maxThickness(int radius);
    /// The filled-circle stamp radius the Line shape paints with for a brush
    /// radius + stroke thickness: the stroke is centred on the Bresenham path
    /// and never exceeds the brush footprint. thickness 0 → the full stamp.
    static int lineStrokeRadius(int radius, int thickness);
    /// Whether apply() implements this effect (the T3.3–T3.5 tools are not
    /// routed yet; the T3.2 height tools are).
    static bool effectSupported(BrushEffect effect);

    void setParams(const BrushParams& params);
    const BrushParams& params() const { return params_; }

    /// One stamp centred at map cell (cx, cy) with the current parameters.
    /// One call = one undo stack entry. Returns true when the world changed.
    bool applyStamp(int cx, int cy);
    bool applyStamp(int cx, int cy, const BrushParams& params);

    // --- T4.2: eyedropper brush type ---------------------------------------
    /// The PickColor brush type applied at one map cell: sample the surface
    /// palette index (the Geo/Dam pixel, i.e. the SurBuf entry — indices
    /// 0..127 are geo palette colors, 128..255 dam palette colors) without
    /// writing any buffer. The sampled index becomes the color brush's
    /// params().colorIndex, so switching back to SetColor paints it right
    /// away; read lastPickedColorIndex() for the value. One pick = no undo
    /// entry. Returns true when a pixel was sampled.
    bool pickColorAt(int cx, int cy);

    /// Layer-paint height clamp: -1 (top) or 0..MAX_VX_HEIGHT_WHOLE.
    static int clampPaintHeight(int h);
    /// Layer-paint depth clamp: 1..MAX_VX_HEIGHT_WHOLE.
    static int clampPaintDepth(int d);
    /// Nearest palette entry of the given layer for `rgb` (the SurBuf
    /// surface-type side band of an RGB paint). Computed once per
    /// (layer, rgb) and cached — see nearestCache* members.
    unsigned char nearestPaletteIndexFor(bool dam, std::uint32_t rgb);
    /// The last index pickColorAt() sampled (valid after hasPickedColor()).
    unsigned char lastPickedColorIndex() const { return lastPickedColorIndex_; }
    /// Whether pickColorAt() has already sampled at least one pixel.
    bool hasPickedColor() const { return hasPickedColor_; }

    /// The Line shape: apply the stamped mask along a Bresenham path —
    /// one call, one undo entry, cells counted once (see BrushMasks::lineStamp).
    /// Returns true when the world changed.
    bool applyLine(int x0, int y0, int x1, int y1);
    bool applyLine(int x0, int y0, int x1, int y1, const BrushParams& params);

    /// The message of the last failed Script-effect evaluation (empty after
    /// a successful stamp). The UI shows it in a critical message box.
    const std::string& lastScriptError() const { return lastScriptError_; }

    /// Apply an absolute (already positioned) mask. Entry point for later
    /// tools with their own geometry (brush-image T3.5); the public
    /// applyStamp/applyLine funnels across here too.
    bool applyMask(const BrushMask& absoluteMask, const BrushParams& params);

    /// Script-only two-pass height function (the "applyHeightFn" script API).
    /// Dam layer. `heightFn(x, y, h)` is evaluated for EVERY covered cell
    /// against the PRE-EDIT dam surface (h = the current surface in voxels
    /// with 1/32 precision, 0 where no dam) and returns the cell's new
    /// ABSOLUTE height; the targets are then written per cell through the
    /// AlignVariableH mechanism (dam seeding + tVoxSetAll — hardness
    /// ignored, exact in both directions), each converted to the nearest
    /// fixed-point value. Because every read sees the original surface,
    /// covered cells can never add up on each other — the seamless
    /// alternative to overlapping raise/lower stamps. `heightFn` returning
    /// false aborts the op BEFORE anything is written or recorded; when no
    /// cell would change, no undo entry appears. One undo entry per call.
    bool applyHeightFunction(
        const BrushMask& absoluteMask,
        const std::function<bool(int, int, double, double&)>& heightFn);

    // --- T3.5: image brush mask --------------------------------------------
    /// Set the image mask used when params.useImageMask is set. The mask
    /// carries per-cell influence in 0..255 (pixel brightness or
    /// alpha-derived) and replaces the shape stamp as the brush geometry for
    /// every effect. The Qt UI layer converts TGA/PNG/BMP → BrushMask and
    /// calls this.
    void setImageMask(const BrushMask& mask);
    /// The current image mask (empty when no image loaded).
    const BrushMask& imageMask() const { return imageMask_; }
    /// Whether a non-empty image mask is loaded.
    bool hasImageMask() const { return !imageMask_.empty(); }

    // --- ImageDecal brush --------------------------------------------------
    /// Set the palette-index image the ImageDecal effect stamps. The decal is
    /// produced by ImageDecal::prepare() (the Qt UI converts the user image and
    /// writes the new colours into the Dam palette) and carries Dam palette
    /// indices, -1 = transparent. The stamp is centred on the click cell.
    void setDecalImage(const ImageDecal::DecalImage& decal);
    /// The current decal image (empty when none is loaded).
    const ImageDecal::DecalImage& decalImage() const { return decal_; }
    /// Whether a non-empty decal image is loaded.
    bool hasDecalImage() const { return !decal_.empty(); }

    // --- Model-heightmap brush ---------------------------------------------
    /// Set the imported model raster (ModelBrush::importPly) and its palette
    /// quantization (ImageDecal::prepare over the raster colours — the caller
    /// writes paletteWrites into the Dam palette like the decal does). The
    /// raster carries normalized heights + per-cell RGB; the indices carry
    /// the Dam palette entry per cell (all cells opaque).
    void setModelBrushData(const ModelBrush::ModelRaster& raster,
                           const ImageDecal::DecalImage& indices);
    /// The imported model raster (empty when none is imported).
    const ModelBrush::ModelRaster& modelRaster() const { return modelRaster_; }
    /// Whether a non-empty model raster is loaded.
    bool hasModelBrushData() const { return !modelRaster_.empty(); }

    /// The per-cell delta the ChangeHeight effect uses: the direction times
    /// the strength in 1/32 m voxel units scaled by the mask influence
    /// (0..255); rounded half away from zero. Exposed for the tests.
    static int changeHeightDelta(const BrushParams& params, unsigned char influence);

    /// The ChangeHeight magnitude the direction is applied to: strength in
    /// 1/32 m voxel units scaled by the mask influence, rounded half away
    /// from zero. Align4ZP reuses it for its per-cell incremental delta (the
    /// direction there comes from the height geometry); AlignVariableH is an
    /// absolute set and does not use the strength.
    static int heightDeltaMagnitude(const BrushParams& params, unsigned char influence);

private:
    /// Route the effect to Terra and mark the bbox dirty. Shared tail of
    /// applyStamp/applyLine/applyMask: every path reaches here.
    bool applyEffectOverMask(const BrushMask& absoluteMask, const BrushParams& params);

    /// Per-cell effect application (the T3.2 height tools; the T3.3+ switch
    /// cases are refused earlier, in applyMask()).
    void applyEffectToCell(const BrushParams& params, int x, int y, unsigned char influence);

    /// Smooth / Flatten two-pass sculpting (see BrushEffect::Smooth): pass 1
    /// computes the reference (average height or least-squares plane) over
    /// the PRE-edit cells under the mask, pass 2 moves every covered cell a
    /// sculptEffect fraction towards its target — through Terra's own
    /// variable-H toolzer, exactly like the Set Height brush (Dam mode only).
    bool applySculpt(const BrushMask& absoluteMask, const BrushParams& params,
                     int bx, int by, int bxEnd, int byEnd);

    /// 3D shape brushes (see BrushParams::shape3d): two-pass stamp like
    /// applySculpt, but the per-cell reference is the shape profile
    /// (Shape3D.h) scaled by shapeHeight over the Height base. The direction
    /// comes from the brush type the shape combines with: Raise (ChangeHeight
    /// direction > 0) moves cells towards the target from below only, Lower
    /// (direction < 0) from above only, Set-to-Height (AlignVariableH) both
    /// ways. Dam mode only; one stamp = one undo entry.
    bool applyShape3D(const BrushMask& absoluteMask, const BrushParams& params,
                      int bx, int by, int bxEnd, int byEnd);

    /// Model-heightmap brush (see BrushParams::modelBrush): stamps the
    /// imported model raster as a rectangle — "place" sets the dam to the
    /// scaled model height and paints the model's exact 24-bit colors, "add"
    /// raises the covered average by the local model height, "subtract"
    /// lowers it. The resampled stamp data arrives through pendingModel_
    /// (set by the applyStamp rectangle path). Dam mode only; one stamp =
    /// one undo entry.
    bool applyModelBrush(const BrushMask& absoluteMask,
                         const BrushParams& params, int bx, int by, int bxEnd,
                         int byEnd);

    EditorSession& session_;
    BrushParams params_;

    /// T3.5: brush-image mask loaded from file (pixel values → influence 0..255).
    BrushMask imageMask_;

    /// ImageDecal: the RGBA image the effect stamps (0xAARRGGBB per pixel),
    /// plus the absolute map origin of the stamp currently being applied (the
    /// effect handler reads the pixel from (x - decalOriginX_, y - decalOriginY_)).
    ImageDecal::DecalImage decal_;
    int decalOriginX_ = 0;
    int decalOriginY_ = 0;

    /// Model-heightmap brush: the imported raster + its true-color image
    /// (exact RGB per cell, nearest Dam palette entry as the SurBuf side
    /// band). The rectangle stamp path in applyStamp resamples both to the
    /// scale-spin size into pendingModel_.
    ModelBrush::ModelRaster modelRaster_;
    ImageDecal::DecalImage modelIndices_;
    /// Transient: the resampled stamp data for the applyMask call the
    /// rectangle path is about to make (consumed by applyModelBrush).
    ModelBrush::ModelRaster pendingModel_;
    std::vector<int> pendingModelIndices_;
    std::vector<std::uint32_t> pendingModelColors_;
    /// Transient: layer paints collected during the applyMask cell loop and
    /// applied after it (off, z0, z1, 0x00RRGGBB) — keeps the voxel-layer
    /// structure mutation out of the per-cell routing.
    std::vector<std::array<int, 4>> pendingLayerPaints_;

    /// T4.2: the last index pickColorAt() sampled (SurBuf at the click cell).
    unsigned char lastPickedColorIndex_ = 0;
    bool hasPickedColor_ = false;

    /// SetColor side-band cache: the nearest palette entry of (layer, rgb)
    /// is identical for every cell of one stamp, so it is computed once and
    /// reused (a per-cell 256-entry scan would be pure waste).
    int nearestCacheLayer_ = -1; ///< -1 = empty; else PaletteLayer as int.
    std::uint32_t nearestCacheRgb_ = 0;
    unsigned char nearestCacheIndex_ = 0;

    /// Scripted brush: message of the last failed evaluation (see
    /// lastScriptError()).
    std::string lastScriptError_;
};

} // namespace MapEditor

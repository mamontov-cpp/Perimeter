// ============================================================================
// MapEditorCore — BrushTool implementation (T3.1).
//
// Terra headers (terra.h pulls VMAP.h → tools.h with individualToolzer):
// _PERIMETER_ and _TX3D_LIBRARY_ come from CMake, exactly like
// EditorSession.cpp / View3DRenderer.cpp.
// ============================================================================

#include "BrushTool.h"
#include "EditorMath.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <utility>

#include "ScriptedBrush.h"

#include "terra.h"

namespace MapEditor {

// ---------------------------------------------------------------------------
// Geometry: Terra-compatible circle rings
// ---------------------------------------------------------------------------

int BrushMasks::circularRing(int dx, int dy)
{
    // (int)cast truncates towards zero — the same rounding the radius tables
    // are built with in vrtMap::landPrepare() (Source/Terra/LAND.cpp:77),
    // so the ring assignment is identical to Terra's maxRad/xRad/yRad.
    return static_cast<int>(std::sqrt(double(dx) * dx + double(dy) * dy));
}

namespace {

/// Centre-symmetric mask baseline: bbox (-(s/2)..-(s/2)+s-1) filled with zeros.
BrushMask symmetricMask(int side)
{
    BrushMask mask;
    mask.x = -(side / 2);
    mask.y = -(side / 2);
    mask.sx = side;
    mask.sy = side;
    mask.influence.assign(static_cast<std::size_t>(side) * side, 0);
    return mask;
}

/// Set influence=255 for the cell (dx,dy) relative to the bbox origin.
void setCell(BrushMask& mask, int dx, int dy, unsigned char value = 255)
{
    if (dx < 0 || dy < 0 || dx >= mask.sx || dy >= mask.sy) {
        return; // symmetric shapes never go here; line stamps are pre-clipped
    }
    mask.influence[static_cast<std::size_t>(dy) * mask.sx + dx] = value;
}

/// Circle mask over all cells with ring(dx,dy) <= radius, or only the ring band
/// [radius-thickness+1, radius] for the outline. Ring 0 is the centre cell
/// itself, so the outline of radius 0 degenerates to a single centre cell.
/// The band is INSET: the outer edge stays at `radius`, so a thicker stroke
/// never edits outside the stamp footprint the shape has always had.
/// `thickness <= 0` → the original one-cell ring for outlines, whole disc for
/// filled (backward-compatible default).
BrushMask buildCircle(int radius, bool outline, int thickness)
{
    const int side = 2 * radius + 1;
    BrushMask mask = symmetricMask(side);
    const int band = outline ? std::max(1, std::max(0, thickness)) : 0;
    const int innerRing = std::max(0, radius - band + 1);
    for (int dy = 0; dy < side; ++dy) {
        const int y = dy + mask.y;
        for (int dx = 0; dx < side; ++dx) {
            const int x = dx + mask.x;
            const int ring = BrushMasks::circularRing(x, y);
            const bool member = outline ? (ring >= innerRing && ring <= radius)
                                        : (ring <= radius);
            if (member) {
                setCell(mask, dx, dy);
            }
        }
    }
    return mask;
}

/// Square mask: filled |dx|<=radius && |dy|<=radius or the border band
/// max(|dx|,|dy|) in [radius-thickness+1, radius] (inset like buildCircle).
/// `thickness <= 0` → the original one-cell border for outlines, whole square
/// for filled (backward-compatible default).
BrushMask buildSquare(int radius, bool outline, int thickness)
{
    const int side = 2 * radius + 1;
    BrushMask mask = symmetricMask(side);
    const int band = outline ? std::max(1, std::max(0, thickness)) : 0;
    const int innerEdge = std::max(0, radius - band + 1);
    for (int dy = 0; dy < side; ++dy) {
        const int y = dy + mask.y;
        for (int dx = 0; dx < side; ++dx) {
            const int x = dx + mask.x;
            const int edge = std::max(std::abs(x), std::abs(y));
            const bool member = outline ? (edge >= innerEdge && edge <= radius)
                                        : true;
            if (member) {
                setCell(mask, dx, dy);
            }
        }
    }
    return mask;
}

/// Scale a signed height delta by the mask influence (0..255), rounding half
/// away from zero. Full influence returns the delta unchanged, so a shape
/// stamp (influence 255) moves exactly by the requested amount.
int scaleDeltaByInfluence(int delta, unsigned char influence)
{
    if (delta == 0 || influence == 0) {
        return 0;
    }
    const long long scaled = static_cast<long long>(delta) * influence;
    const long long magnitude = scaled < 0 ? -scaled : scaled;
    const long long rounded = (magnitude + 127) / 255;
    return static_cast<int>(scaled < 0 ? -rounded : rounded);
}

} // namespace

BrushMask BrushMasks::filledCircle(int radius) { return buildCircle(radius, false, 0); }
BrushMask BrushMasks::circleOutline(int radius, int thickness) { return buildCircle(radius, true, thickness); }
BrushMask BrushMasks::filledSquare(int radius) { return buildSquare(radius, false, 0); }
BrushMask BrushMasks::squareOutline(int radius, int thickness) { return buildSquare(radius, true, thickness); }

std::vector<std::pair<double, double>>
BrushMasks::regularPolygonVertices(double radius, int sides, double angleOffsetDeg)
{
    sides = BrushTool::clampPolygonSides(sides);
    std::vector<std::pair<double, double>> vertices;
    vertices.reserve(static_cast<std::size_t>(sides));
    const double step = 2.0 * kPi / sides;
    const double start = angleOffsetDeg * kDegToRad;
    for (int i = 0; i < sides; ++i) {
        const double a = start + step * i;
        vertices.emplace_back(radius * std::cos(a), radius * std::sin(a));
    }
    return vertices;
}

namespace {

/// Even-odd point-in-polygon test over the (already cell-rounded) vertex
/// loop, evaluated at the integer cell centre (dx,dy).
bool pointInPolygon(const std::vector<std::pair<double, double>>& vertices,
                    double dx, double dy)
{
    bool inside = false;
    const std::size_t n = vertices.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = vertices[i].first;
        const double yi = vertices[i].second;
        const double xj = vertices[j].first;
        const double yj = vertices[j].second;
        if ((yi > dy) != (yj > dy) &&
            dx < (xj - xi) * (dy - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
    }
    return inside;
}

/// Distance from the point (dx,dy) to the closest point of the polygon
/// boundary (the vertex loop as line segments).
double distanceToPolygonBoundary(
    const std::vector<std::pair<double, double>>& vertices, double dx, double dy)
{
    double best = std::numeric_limits<double>::max();
    const std::size_t n = vertices.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double x0 = vertices[j].first;
        const double y0 = vertices[j].second;
        const double x1 = vertices[i].first;
        const double y1 = vertices[i].second;
        const double ex = x1 - x0;
        const double ey = y1 - y0;
        const double len2 = ex * ex + ey * ey;
        double t = 0.0;
        if (len2 > 0.0) {
            t = ((dx - x0) * ex + (dy - y0) * ey) / len2;
            t = std::clamp(t, 0.0, 1.0);
        }
        const double cx = x0 + ex * t;
        const double cy = y0 + ey * t;
        const double ddx = dx - cx;
        const double ddy = dy - cy;
        best = std::min(best, ddx * ddx + ddy * ddy);
    }
    return std::sqrt(best);
}

/// Shared bbox/raster pass for the polygon masks: evaluates `member(dx,dy)`
/// over the (2r+1)^2 stamp bbox and fills the influence grid.
BrushMask rasterizePolygon(int radius,
                           const std::vector<std::pair<double, double>>& vertices,
                           const std::function<bool(double, double)>& member)
{
    const int side = 2 * radius + 1;
    BrushMask mask = symmetricMask(side);
    for (int dy = 0; dy < side; ++dy) {
        const double y = dy + mask.y;
        for (int dx = 0; dx < side; ++dx) {
            if (member(dx + mask.x, y)) {
                setCell(mask, dx, dy);
            }
        }
    }
    return mask;
}

} // namespace

BrushMask BrushMasks::filledPolygon(int radius, int sides, double angleOffsetDeg)
{
    if (radius < 0) {
        radius = 0;
    }
    // Round the vertices to whole cells like every other mask, then fill.
    // Cells whose centre lands exactly on an edge/vertex (even-odd is
    // degenerate there) join the fill via the sub-cell boundary rule.
    const std::vector<std::pair<double, double>> raw =
        regularPolygonVertices(radius, sides, angleOffsetDeg);
    std::vector<std::pair<double, double>> vertices;
    vertices.reserve(raw.size());
    for (const auto& v : raw) {
        vertices.emplace_back(std::lround(v.first), std::lround(v.second));
    }
    return rasterizePolygon(radius, vertices,
                            [&vertices](double x, double y) {
                                return pointInPolygon(vertices, x, y) ||
                                       distanceToPolygonBoundary(vertices, x, y) < 0.5;
                            });
}

BrushMask BrushMasks::polygonOutline(int radius, int sides, double angleOffsetDeg,
                                     int thickness)
{
    if (radius < 0) {
        radius = 0;
    }
    const int band = std::max(1, thickness);
    const std::vector<std::pair<double, double>> raw =
        regularPolygonVertices(radius, sides, angleOffsetDeg);
    std::vector<std::pair<double, double>> vertices;
    vertices.reserve(raw.size());
    for (const auto& v : raw) {
        vertices.emplace_back(std::lround(v.first), std::lround(v.second));
    }
    // Inset band, the polygon analog of the circle rings [r-t+1, r] / the
    // square edges [r-t+1, r]: cells whose SIGNED distance to the boundary
    // (positive inside) stays below the band width, plus the sub-cell
    // fringe straddling the ideal edge (sd > -1) — a polygon vertex is at
    // most 1/2 cell off the rounded boundary, so the stroke never breaks.
    return rasterizePolygon(radius, vertices,
                            [&vertices, band](double x, double y) {
                                const double dist =
                                    distanceToPolygonBoundary(vertices, x, y);
                                const double sd =
                                    pointInPolygon(vertices, x, y) ? dist : -dist;
                                return sd > -1.0 && sd < band;
                            });
}

BrushMask BrushMasks::byShape(BrushShape shape, int radius, int thickness,
                              int polygonSides, double polygonAngleDeg)
{
    switch (shape) {
    case BrushShape::FilledCircle:  return filledCircle(radius);
    case BrushShape::CircleOutline: return circleOutline(radius, thickness);
    case BrushShape::FilledSquare:  return filledSquare(radius);
    case BrushShape::SquareOutline: return squareOutline(radius, thickness);
    case BrushShape::FilledPolygon:
        return filledPolygon(radius, polygonSides, polygonAngleDeg);
    case BrushShape::PolygonOutline:
        return polygonOutline(radius, polygonSides, polygonAngleDeg, thickness);
    case BrushShape::Line:
        // Line has no stamp without endpoints — lineStamp() builds the
        // mask with its own stamp radius logic.
        return BrushMask{};
    }
    return BrushMask{};
}

/// Effective filled-circle stamp radius for the Line shape: the stroke is
/// centred on the Bresenham path and never exceeds the brush footprint.
/// `thickness <= 0` → full stamp (the pre-thickness behaviour);
/// `thickness N` → stroke radius (N-1)/2, clamped to [0, radius].
int BrushTool::lineStrokeRadius(int radius, int thickness)
{
    if (thickness <= 0) {
        return radius;
    }
    const int effR = (thickness - 1) / 2;
    return std::min(effR, std::max(0, radius));
}

BrushMask BrushMasks::lineStamp(int x0, int y0, int x1, int y1, int stampRadius)
{
    BrushMask mask;
    mask.x = std::min(x0, x1) - stampRadius;
    mask.y = std::min(y0, y1) - stampRadius;
    mask.sx = std::abs(x1 - x0) + 2 * stampRadius + 1;
    mask.sy = std::abs(y1 - y0) + 2 * stampRadius + 1;
    const std::size_t cells = static_cast<std::size_t>(mask.sx) * mask.sy;
    mask.influence.assign(cells, 0);
    if (mask.sx <= 0 || mask.sy <= 0) {
        return mask;
    }

    // Bresenham over the (possibly diagonal) integer path ...
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0;
    int y = y0;

    // ... stamping a filled circle at every step. Cells inside stamp overlaps
    // keep the 255 value — the effect per cell is applied once per apply
    // call, i.e. the line raises every covered cell by one stamp delta.
    const int r = std::max(0, stampRadius);
    const BrushMask stamp = filledCircle(r);
    for (;;) {
        for (int j = 0; j < stamp.sy; ++j) {
            for (int i = 0; i < stamp.sx; ++i) {
                if (stamp.influence[static_cast<std::size_t>(j) * stamp.sx + i]) {
                    const int px = x + stamp.x + i - mask.x;
                    const int py = y + stamp.y + j - mask.y;
                    if (px >= 0 && py >= 0 && px < mask.sx && py < mask.sy) {
                        mask.influence[static_cast<std::size_t>(py) * mask.sx + px] = 255;
                    }
                }
            }
        }
        if (x == x1 && y == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
    return mask;
}

// ---------------------------------------------------------------------------
// BrushTool
// ---------------------------------------------------------------------------

// Keep the params setter/restorer clamping logic in one place.
namespace {

/// The height-gate condition (Dam mode only): does the voxel with the
/// PRE-edit surface height `alt32` (1/32 m units) pass the gate? The gate
/// compares the INDIVIDUAL voxel's height against gateHeight — one stamp may
/// edit some covered cells and skip their neighbours. A voxel exactly at
/// gateHeight fails both conditions (the comparison is strict). None passes
/// every voxel.
bool heightGateAllows(const BrushParams& params, int alt32)
{
    if (params.heightGate == BrushHeightGate::None) {
        return true;
    }
    const int gate32 = BrushTool::clampGateHeight(params.gateHeight)
                       << VX_FRACTION;
    return params.heightGate == BrushHeightGate::Above ? alt32 > gate32
                                                       : alt32 < gate32;
}

/// The gate read at a live cell offset (the generic single-pass path: the
/// cell's own height at the moment it is processed).
bool heightGateAllowsCell(const BrushParams& params, int off)
{
    return heightGateAllows(params, static_cast<int>(vMap.SGetAlt(off)));
}

BrushParams clampedParams(const BrushParams& params)
{
    BrushParams clamped = params;
    clamped.radius = BrushTool::clampRadius(clamped.radius);
    clamped.strength = BrushTool::clampStrength(clamped.strength);
    clamped.heightLevel = BrushTool::clampHeightLevel(clamped.heightLevel);
    clamped.thickness = BrushTool::clampThickness(clamped.thickness, clamped.radius);
    clamped.polygonSides = BrushTool::clampPolygonSides(clamped.polygonSides);
    clamped.polygonAngleDeg = BrushTool::clampPolygonAngleDeg(clamped.polygonAngleDeg);
    clamped.sculptEffect = BrushTool::clampSculptEffect(clamped.sculptEffect);
    clamped.blurRadius = BrushTool::clampBlurRadius(clamped.blurRadius,
                                                    clamped.radius);
    clamped.gateHeight = BrushTool::clampGateHeight(clamped.gateHeight);
    if (clamped.direction != 0) {
        clamped.direction = clamped.direction > 0 ? 1 : -1;
    }
    // 3D shape brushes: amplitude in whole voxels; the geometry fields finite
    // and non-negative (the offset keeps its sign). The shape-specific
    // upper-part clamps need the base radius — those run in
    // clampShape3DSettings() at stamp time.
    clamped.shapeHeight = std::clamp(clamped.shapeHeight, 0,
                                     MAX_VX_HEIGHT_WHOLE);
    const auto finiteLen = [](double v) {
        return std::isfinite(v) ? std::max(0.0, v) : 0.0;
    };
    clamped.shapeTopRadius = finiteLen(clamped.shapeTopRadius);
    clamped.shapeTopSide = finiteLen(clamped.shapeTopSide);
    const auto finiteOffset = [](double v) {
        return std::isfinite(v)
                   ? std::clamp(v, -static_cast<double>(kMaxBrushRadius),
                                static_cast<double>(kMaxBrushRadius))
                   : 0.0;
    };
    clamped.shapeTopOffsetX = finiteOffset(clamped.shapeTopOffsetX);
    clamped.shapeTopOffsetY = finiteOffset(clamped.shapeTopOffsetY);
    // Model brush: amplitude in whole voxels, scales in [0,1], mode 0..2.
    clamped.modelHeight =
        std::clamp(clamped.modelHeight, 0, MAX_VX_HEIGHT_WHOLE);
    clamped.modelScaleX = std::clamp(clamped.modelScaleX, 0.0, 1.0);
    clamped.modelScaleY = std::clamp(clamped.modelScaleY, 0.0, 1.0);
    clamped.modelMode = std::clamp(clamped.modelMode, 0, 2);
    return clamped;
}

} // namespace

BrushTool::BrushTool(EditorSession& session)
    : session_(session)
{
    params_ = clampedParams(params_);
}

int BrushTool::maxRadius()
{
    return kMaxBrushRadius;
}

int BrushTool::clampRadius(int radius)
{
    return std::clamp(radius, 0, maxRadius());
}

int BrushTool::clampStrength(int strength)
{
    return static_cast<int>(clampStrength(static_cast<double>(strength)));
}

double BrushTool::clampStrength(double strength)
{
    return std::clamp(strength, 1.0, 100.0);
}

int BrushTool::clampHeightLevel(int level)
{
    return static_cast<int>(clampHeightLevel(static_cast<double>(level)));
}

double BrushTool::clampHeightLevel(double level)
{
    return std::clamp(level, 0.0, static_cast<double>(MAX_VX_HEIGHT_WHOLE));
}

/// The height a set/alignment brush drives a cell to, in Terra's 1/32-voxel
/// fixed point: the voxel value (whole or fractional) converted ONCE with
/// rounding to the nearest representable step.
static int heightLevelFixedPoint(double levelVox)
{
    const double clamped =
        std::clamp(levelVox, 0.0, static_cast<double>(MAX_VX_HEIGHT_WHOLE));
    return static_cast<int>(std::llround(clamped * (1 << VX_FRACTION)));
}

/// Terra's setHAppoximation() only accepts WHOLE voxels (it shifts, dropping
/// the fraction), but the hAppr member itself is the fixed-point cap the
/// toolzer clamps at - and it is public. Set it to the EXACT fixed-point
/// target so fractional heights converge exactly (Terra stores the fraction
/// natively in AtrBuf's VX_FRACTION bits).
template <toolzer2TerrainEffect T>
static void setExactToolzerTarget(individualToolzer<T>& toolzer, int target32)
{
    toolzer.setHAppoximation(target32 >> VX_FRACTION);
    toolzer.hAppr = target32;
}

int BrushTool::maxThickness(int radius)
{
    return 2 * clampRadius(radius) + 1;
}

int BrushTool::clampThickness(int thickness, int radius)
{
    return std::clamp(thickness, 0, maxThickness(radius));
}

int BrushTool::clampPolygonSides(int sides)
{
    return std::clamp(sides, kMinPolygonSides, kMaxPolygonSides);
}

double BrushTool::clampPolygonAngleDeg(double deg)
{
    double wrapped = std::fmod(deg, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

double BrushTool::clampSculptEffect(double effect)
{
    return std::clamp(effect, 0.0, 1.0);
}

int BrushTool::clampGateHeight(int level)
{
    return clampHeightLevel(level);
}

int BrushTool::clampBlurRadius(int blurRadius, int stampRadius)
{
    return std::clamp(blurRadius, 1, std::max(1, stampRadius - 1));
}

bool BrushTool::effectSupported(BrushEffect effect)
{
    switch (effect) {
    case BrushEffect::None:
    case BrushEffect::ChangeHeight: // T3.2 raise/lower
    case BrushEffect::Align4ZP:     // T3.4: level to zeroplast + mark it
    case BrushEffect::AlignVariableH: // T3.2 smooth/level to height
    case BrushEffect::SetColor:     // T3.3 paint SurBuf with palette index
    case BrushEffect::ImageDecal:   // palette-limited image stamp (Dam only)
    case BrushEffect::PickColor:    // T4.2 eyedropper: sample SurBuf pixel
    case BrushEffect::PlotSoot:        // T3.4 mark soot
    case BrushEffect::VeryLightDam:    // T3.4 very light dam
    case BrushEffect::Script:          // scripted brush (routed in applyStamp)
    case BrushEffect::Smooth:          // sculpting (two-pass, in applyMask)
    case BrushEffect::Flatten:         // sculpting (two-pass, in applyMask)
    case BrushEffect::Tricubic:        // sculpting (two-pass, in applyMask)
    case BrushEffect::Inflate:         // sculpting (two-pass, in applyMask)
    case BrushEffect::Deflate:         // sculpting (two-pass, in applyMask)
    case BrushEffect::GaussianBlur:    // sculpting (two-pass, in applyMask)
        return true;
    default:
        return false;
    }
}

void BrushTool::setParams(const BrushParams& params)
{
    params_ = clampedParams(params);
}

void BrushTool::setImageMask(const BrushMask& mask)
{
    imageMask_ = mask;
}

void BrushTool::setDecalImage(const ImageDecal::DecalImage& decal)
{
    decal_ = decal;
}

void BrushTool::setModelBrushData(const ModelBrush::ModelRaster& raster,
                                  const ImageDecal::DecalImage& indices)
{
    modelRaster_ = raster;
    modelIndices_ = indices;
    pendingModel_ = ModelBrush::ModelRaster();
    pendingModelIndices_.clear();
    pendingModelColors_.clear();
}

int BrushTool::heightDeltaMagnitude(const BrushParams& params, unsigned char influence)
{
    if (influence == 0) {
        return 0;
    }
    // Full delta = strength converted to 1/32 m units (VX_FRACTION),
    // scaled by the mask influence; round half away from zero.
    const int full = static_cast<int>(
        std::llround(clampStrength(params.strength) * (1 << VX_FRACTION)));
    return scaleDeltaByInfluence(full, influence);
}

int BrushTool::changeHeightDelta(const BrushParams& params, unsigned char influence)
{
    if (params.direction == 0 || influence == 0 || !effectSupported(params.effect)) {
        return 0;
    }
    return params.direction < 0 ? -heightDeltaMagnitude(params, influence)
                                : heightDeltaMagnitude(params, influence);
}

bool BrushTool::applyStamp(int cx, int cy)
{
    return applyStamp(cx, cy, params_);
}

bool BrushTool::applyStamp(int cx, int cy, const BrushParams& params)
{
    const BrushParams clamped = clampedParams(params);
    if (clamped.effect == BrushEffect::None) {
        return false; // no world edit requested
    }
    // T4.2: the eyedropper is a point operation — the sample point is the
    // stamp centre cell only (mask/shape/radius are irrelevant), and it
    // writes no buffer, so it never reaches applyMask()/the undo stack.
    if (clamped.effect == BrushEffect::PickColor) {
        return pickColorAt(cx, cy);
    }
    // ImageDecal: the decal image IS the stamp geometry — its pixels are
    // RGBA colors, not an influence mask, and the brush only owns the Dam
    // layer. The stamp is centred on the click cell and covers exactly the
    // image; every fully opaque pixel paints its exact 24-bit color.
    if (clamped.effect == BrushEffect::ImageDecal) {
        if (decal_.empty() || clamped.mode != BrushMode::Dam) {
            return false; // nothing loaded / wrong layer for a dam decal
        }
        decalOriginX_ = cx - decal_.width / 2;
        decalOriginY_ = cy - decal_.height / 2;
        BrushMask mask;
        mask.x = decalOriginX_;
        mask.y = decalOriginY_;
        mask.sx = decal_.width;
        mask.sy = decal_.height;
        mask.influence.assign(static_cast<std::size_t>(mask.sx) * mask.sy, 0);
        for (int dy = 0; dy < mask.sy; ++dy) {
            for (int dx = 0; dx < mask.sx; ++dx) {
                if ((decal_.at(dx, dy) >> 24) == 0xFF) {
                    mask.influence[static_cast<std::size_t>(dy) * mask.sx + dx] = 255;
                }
            }
        }
        return applyMask(mask, clamped);
    }
    // Scripted brush: the script runs once at the stamp centre and drives
    // the editor API itself — the shape/radius params only feed the
    // getCurrent*() calls (the API takes explicit positions and radii).
    // Routed BEFORE the image mask: the script's own geometry wins.
    if (clamped.effect == BrushEffect::Script) {
        lastScriptError_.clear();
        std::string error;
        const bool ok = runScriptedBrush(session_, *this, clamped, cx, cy,
                                         error);
        if (!ok) {
            lastScriptError_ = error;
        }
        return ok;
    }
    // 3D shape brushes: the stamp footprint is the shape's BASE — a filled
    // disc (cone / truncated cone / torus / half ellipsoid) or a filled
    // regular polygon (pyramid / truncated pyramid, reusing the polygon
    // params) at the brush radius. Routed before the 2D shape stamp and the
    // image mask (the solid's own geometry wins).
    if (clamped.shape3d != Shape3DKind::None) {
        BrushMask mask = shape3DIsPolygonal(clamped.shape3d)
            ? BrushMasks::filledPolygon(clamped.radius, clamped.polygonSides,
                                        clamped.polygonAngleDeg)
            : BrushMasks::filledCircle(clamped.radius);
        if (mask.empty()) {
            return false;
        }
        mask.translate(cx, cy);
        return applyMask(mask, clamped);
    }
    // Model-heightmap brush: the stamp is the placement RECTANGLE of the
    // scaled model — the raster and its palette indices are resampled to
    // that size (nearest) and handed to applyMask through pendingModel_.
    if (clamped.modelBrush) {
        if (modelRaster_.empty() || modelIndices_.empty()) {
            return false; // nothing imported yet
        }
        const int stampW = std::max(
            1, static_cast<int>(
                   std::lround(modelRaster_.width * clamped.modelScaleX)));
        const int stampH = std::max(
            1, static_cast<int>(
                   std::lround(modelRaster_.height * clamped.modelScaleY)));
        pendingModel_ = ModelBrush::resample(modelRaster_, stampW, stampH);
        pendingModelIndices_.assign(
            static_cast<std::size_t>(stampW) * stampH, -1);
        pendingModelColors_.assign(
            static_cast<std::size_t>(stampW) * stampH, 0);
        for (int y = 0; y < stampH; ++y) {
            const int sy = std::min(
                modelIndices_.height - 1,
                static_cast<int>(static_cast<long long>(y) *
                                 modelIndices_.height / stampH));
            for (int x = 0; x < stampW; ++x) {
                const int sx = std::min(
                    modelIndices_.width - 1,
                    static_cast<int>(static_cast<long long>(x) *
                                     modelIndices_.width / stampW));
                pendingModelIndices_[static_cast<std::size_t>(y) * stampW + x] =
                    modelIndices_.indexAt(sx, sy);
                pendingModelColors_[static_cast<std::size_t>(y) * stampW + x] =
                    modelIndices_.at(sx, sy);
            }
        }
        BrushMask mask;
        mask.x = -(stampW / 2);
        mask.y = -(stampH / 2);
        mask.sx = stampW;
        mask.sy = stampH;
        mask.influence.assign(static_cast<std::size_t>(stampW) * stampH, 255);
        mask.translate(cx, cy);
        return applyMask(mask, clamped);
    }
    // T3.5: the loaded image mask replaces the shape stamp as the brush
    // geometry for every effect — the image pixel value is the per-cell
    // influence (raised/lowered/levelled/painted through the current effect).
    if (clamped.useImageMask) {
        if (imageMask_.empty()) {
            return false; // no image loaded
        }
        BrushMask mask = imageMask_; // copy, then translate to (cx,cy)
        mask.translate(cx, cy);
        return applyMask(mask, clamped);
    }
    BrushMask mask = BrushMasks::byShape(clamped.shape, clamped.radius,
                                         clamped.thickness, clamped.polygonSides,
                                         clamped.polygonAngleDeg);
    if (mask.empty()) {
        return false;
    }
    mask.translate(cx, cy);
    return applyMask(mask, clamped);
}

bool BrushTool::applyLine(int x0, int y0, int x1, int y1)
{
    return applyLine(x0, y0, x1, y1, params_);
}

bool BrushTool::applyLine(int x0, int y0, int x1, int y1, const BrushParams& params)
{
    const BrushParams clamped = clampedParams(params);
    if (clamped.effect == BrushEffect::None || !effectSupported(clamped.effect)) {
        return false;
    }
    // T4.2: picking is a point operation — stroking a "line" of samples makes
    // no sense, so the Line shape refuses the PickColor effect.
    if (clamped.effect == BrushEffect::PickColor) {
        return false;
    }
    // The decal is a rectangular image stamp with its own geometry: a line
    // stroke has no meaning for it (the viewport does not start one either).
    // The 3D shapes and the model-heightmap brush stamp one solid/rectangle
    // at the click — no line either.
    if (clamped.effect == BrushEffect::ImageDecal ||
        clamped.effect == BrushEffect::Script ||
        clamped.shape3d != Shape3DKind::None ||
        clamped.modelBrush) {
        return false;
    }
    const int stampRadius = BrushTool::lineStrokeRadius(clamped.radius, clamped.thickness);
    BrushMask mask = BrushMasks::lineStamp(x0, y0, x1, y1, stampRadius);
    if (mask.empty()) {
        return false;
    }
    return applyMask(mask, clamped);
}

bool BrushTool::applyMask(const BrushMask& absoluteMask, const BrushParams& params)
{
    if (!session_.isWorldLoaded() || absoluteMask.empty()) {
        return false;
    }
    if (params.effect == BrushEffect::None) {
        return false;
    }
    if (!effectSupported(params.effect)) {
        return false; // routing comes with T3.3–T3.5
    }

    // --- 1. Clip the bbox against the map (the editor view is bounded; ---
    // ---    torus wrap is intentionally not used, like recordUndoArea). ---
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int bx = std::max(0, absoluteMask.x);
    const int by = std::max(0, absoluteMask.y);
    const int bxEnd = std::min(mapW, absoluteMask.x + absoluteMask.sx);
    const int byEnd = std::min(mapH, absoluteMask.y + absoluteMask.sy);
    if (bx >= bxEnd || by >= byEnd) {
        return false; // completely outside the map
    }

    // --- 2. Undo snapshot BEFORE any cell is touched (one entry per apply, ---
    // ---    swap semantics allow overlapping snapshots between strokes). ---
    // Sculpting validates its Dam-only mode here too — a refused stamp must
    // not record an undo entry.
    // 3D shape brushes (cone / pyramid / ...): Dam layer only, like the
    // sculpting types. Their whole flow (no-op precheck -> undo record ->
    // two-pass apply) is self-contained in applyShape3D so an all-refused
    // stamp leaves no empty history entry.
    if (params.shape3d != Shape3DKind::None) {
        if (params.mode != BrushMode::Dam) {
            return false; // 3D shapes own the Dam layer only
        }
        return applyShape3D(absoluteMask, params, bx, by, bxEnd, byEnd);
    }
    // Model-heightmap brush: same self-contained flow through
    // applyModelBrush (place/add/subtract), Dam layer only.
    if (params.modelBrush) {
        if (params.mode != BrushMode::Dam) {
            return false; // the model brush owns the Dam layer only
        }
        return applyModelBrush(absoluteMask, params, bx, by, bxEnd, byEnd);
    }
    if ((params.effect == BrushEffect::Smooth ||
         params.effect == BrushEffect::Flatten ||
         params.effect == BrushEffect::Tricubic ||
         params.effect == BrushEffect::Inflate ||
         params.effect == BrushEffect::Deflate ||
         params.effect == BrushEffect::GaussianBlur) &&
        params.mode != BrushMode::Dam) {
        return false; // sculpting operates on the dam layer only
    }
    // The height gate (Dam mode only): when every covered voxel fails the
    // condition the stamp would change nothing — refuse it BEFORE the undo
    // snapshot so no empty history entry appears.
    if (params.mode == BrushMode::Dam &&
        params.heightGate != BrushHeightGate::None) {
        bool anyPasses = false;
        for (int y = by; y < byEnd && !anyPasses; ++y) {
            for (int x = bx; x < bxEnd && !anyPasses; ++x) {
                const int dx = x - absoluteMask.x;
                const int dy = y - absoluteMask.y;
                if (dx < 0 || dy < 0 || dx >= absoluteMask.sx ||
                    dy >= absoluteMask.sy || !absoluteMask.at(dx, dy)) {
                    continue;
                }
                if (heightGateAllowsCell(params, y * mapW + x)) {
                    anyPasses = true;
                }
            }
        }
        if (!anyPasses) {
            return false;
        }
    }
    session_.recordUndoArea(bx, by, bxEnd - bx, byEnd - by);

    // --- Sculpting (Smooth / Flatten / Tricubic): a two-pass effect — the ---
    // --- reference must be computed over the PRE-edit state, so the       ---
    // --- per-cell routing below cannot serve it.                          ---
    if (params.effect == BrushEffect::Smooth ||
        params.effect == BrushEffect::Flatten ||
        params.effect == BrushEffect::Tricubic ||
        params.effect == BrushEffect::Inflate ||
        params.effect == BrushEffect::Deflate ||
        params.effect == BrushEffect::GaussianBlur) {
        return applySculpt(absoluteMask, params, bx, by, bxEnd, byEnd);
    }

    // --- 3. Route the effect over the visible mask cells. ---
    for (int y = by; y < byEnd; ++y) {
        for (int x = bx; x < bxEnd; ++x) {
            const int dx = x - absoluteMask.x;
            const int dy = y - absoluteMask.y;
            if (dx < 0 || dy < 0 || dx >= absoluteMask.sx || dy >= absoluteMask.sy) {
                continue; // clipped-away mask border
            }
            const unsigned char influence = absoluteMask.at(dx, dy);
            if (influence) {
                applyEffectToCell(params, x, y, influence);
            }
        }
    }

    // --- 4. Grid + viewport dirty rectangles, margin 1 like Terra's tools ---
    // ---    (influenceDZ recalcArea2Grid bugfix comment in tools.h:252).  ---
    const int x0 = std::max(0, bx - 1);
    const int y0 = std::max(0, by - 1);
    const int x1 = std::min(mapW - 1, bxEnd + 1);
    const int y1 = std::min(mapH - 1, byEnd + 1);
    vMap.recalcArea2Grid(x0, y0, x1, y1);
    // The trail-aware wrapper (see EditorSession::regRenderWithShadowTrail):
    // the newly created shadow west of a raised/lowered stamp is rendered by
    // RenderStr beyond the dirty rect, so the rect grows over the shadow
    // trail and the viewport cache repaints those cells in the same frame.
    session_.regRenderWithShadowTrail(x0, y0, x1, y1);

    return true;
}

// ---------------------------------------------------------------------------
// Sculpting (Smooth / Flatten)
//
// Two-pass Dam-layer effect. Pass 1 samples the PRE-edit surface height of
// every covered cell and derives the reference: the plain average (Smooth)
// or a least-squares plane z = a*dx + b*dy + c through the samples
// (Flatten — large-scale slopes survive, local bumps die). Pass 2 moves
// each cell a sculptEffect fraction of the way towards its reference
// height. Height edits go through Terra's own variable-H toolzer
// (setHAppoximation + tVoxSetAll) — the exact mechanism the Set Height
// brush uses — with the same zeroplast-gate clearing and dam seeding.
// ---------------------------------------------------------------------------

namespace {

// Defined with the T3.2 layer helpers below; the sculpt pass seeds dams
// exactly like the other Dam-mode brushes.
int seedDamLayer(int off);

/// Least-squares plane through the samples (dx/dy relative to the mask
/// centre, so the linear terms decouple from the offset). Returns the
/// offset in `offset` (== the plain average) and the slopes in a/b.
/// Degenerate configurations (all samples on a line, too few samples)
/// leave a/b at 0 — the caller then falls back to pure averaging.
struct SculptPlane
{
    double a = 0.0;
    double b = 0.0;
    double offset = 0.0;
};

SculptPlane fitSculptPlane(const std::vector<int>& dxs,
                           const std::vector<int>& dys,
                           const std::vector<int>& alts)
{
    SculptPlane plane;
    const std::size_t n = alts.size();
    if (n == 0) {
        return plane;
    }
    double sz = 0.0;
    for (int alt : alts) {
        sz += alt;
    }
    plane.offset = sz / static_cast<double>(n);

    if (n < 3) {
        return plane; // a/b stay 0 — the average is the only stable answer
    }
    double sxx = 0.0, sxy = 0.0, syy = 0.0, sxz = 0.0, syz = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = static_cast<double>(dxs[i]);
        const double dy = static_cast<double>(dys[i]);
        const double dz = alts[i] - plane.offset;
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
        sxz += dx * dz;
        syz += dy * dz;
    }
    const double det = sxx * syy - sxy * sxy;
    // Near-singular fits (collinear sample positions) fall back to the
    // average: dividing by ~0 would amplify float noise into huge slopes.
    const double scale =
        (sxx + syy) * (sxx + syy) + 1.0; // ~ fourth power of the spread
    if (det <= 1e-9 * scale) {
        return plane;
    }
    plane.a = (sxz * syy - syz * sxy) / det;
    plane.b = (syz * sxx - sxz * sxy) / det;
    return plane;
}

/// Control-lattice stride (cells) for the Tricubic sculpting brush:
/// features smaller than roughly this size are smoothed away.
constexpr int kTricubicStride = 4;

/// Full normal-offset step (whole voxels) for Inflate / Deflate at
/// sculptEffect = 1.0 — the surface offset the reference is built with;
/// the effect fraction scales the actual move towards it.
constexpr int kInflateStepVoxels = 4;

/// 1-D Catmull-Rom cubic through four samples at fractional position t.
/// Reproduces arithmetic (linear) sample sequences EXACTLY at any t.
double catmullRom(const double p[4], double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    return 0.5 * (2.0 * p[1] +
                  (-p[0] + p[2]) * t +
                  (2.0 * p[0] - 5.0 * p[1] + 4.0 * p[2] - p[3]) * t2 +
                  (-p[0] + 3.0 * p[1] - 3.0 * p[2] + p[3]) * t3);
}

/// Node accessor with linear extrapolation past the lattice edges.
double latticeNode(const std::vector<double>& lattice, int nx, int ny, int i,
                   int j)
{
    if (i < 0) {
        return 2.0 * latticeNode(lattice, nx, ny, 0, j) -
               latticeNode(lattice, nx, ny, 1, j);
    }
    if (i >= nx) {
        return 2.0 * latticeNode(lattice, nx, ny, nx - 1, j) -
               latticeNode(lattice, nx, ny, nx - 2, j);
    }
    if (j < 0) {
        return 2.0 * latticeNode(lattice, nx, ny, i, 0) -
               latticeNode(lattice, nx, ny, i, 1);
    }
    if (j >= ny) {
        return 2.0 * latticeNode(lattice, nx, ny, i, ny - 1) -
               latticeNode(lattice, nx, ny, i, ny - 2);
    }
    return lattice[static_cast<std::size_t>(j) * nx + i];
}

/// Separable bicubic (Catmull-Rom) evaluation of a control lattice at
/// bbox-relative coordinates (xr, yr). `posX`/`posY` hold the ACTUAL sample
/// position of every node (block centres — the last, partial window is
/// centred at its midpoint, so the final segment is non-uniform and its t
/// is scaled by the real gap). Segments outside the node range extrapolate
/// along the cubic — exact for the linear fields the reproduction proof
/// relies on.
double bicubicAt(const std::vector<double>& lattice,
                 const std::vector<double>& posX,
                 const std::vector<double>& posY, double xr, double yr)
{
    const int nx = static_cast<int>(posX.size());
    const int ny = static_cast<int>(posY.size());
    const auto segment = [](const std::vector<double>& pos, double v, int n,
                            int& index, double& t) {
        if (n < 2) {
            index = 0;
            t = 0.0;
            return;
        }
        if (v <= pos[0]) {
            index = 0;
            t = (v - pos[0]) / (pos[1] - pos[0]);
            return;
        }
        for (int i = 0; i + 1 < n; ++i) {
            if (v <= pos[i + 1]) {
                index = i;
                t = (v - pos[i]) / (pos[i + 1] - pos[i]);
                return;
            }
        }
        index = n - 2;
        t = (v - pos[n - 2]) / (pos[n - 1] - pos[n - 2]);
    };
    int iu = 0, iv = 0;
    double tu = 0.0, tv = 0.0;
    segment(posX, xr, nx, iu, tu);
    segment(posY, yr, ny, iv, tv);
    double rows[4];
    for (int j = 0; j < 4; ++j) {
        double cols[4];
        for (int i = 0; i < 4; ++i) {
            cols[i] = latticeNode(lattice, nx, ny, iu - 1 + i, iv - 1 + j);
        }
        rows[j] = catmullRom(cols, tu);
    }
    return catmullRom(rows, tv);
}

} // namespace

bool BrushTool::applySculpt(const BrushMask& absoluteMask,
                            const BrushParams& params, int bx, int by,
                            int bxEnd, int byEnd)
{
    // Pass 1: sample the PRE-edit surface heights of the covered cells.
    std::vector<int> offs;
    std::vector<int> dxs;
    std::vector<int> dys;
    std::vector<int> alts;
    std::vector<unsigned char> influences;
    offs.reserve(static_cast<std::size_t>(bxEnd - bx) * (byEnd - by));
    const int cx = (bx + bxEnd - 1) / 2;
    const int cy = (by + byEnd - 1) / 2;
    for (int y = by; y < byEnd; ++y) {
        for (int x = bx; x < bxEnd; ++x) {
            const int dx = x - absoluteMask.x;
            const int dy = y - absoluteMask.y;
            if (dx < 0 || dy < 0 || dx >= absoluteMask.sx ||
                dy >= absoluteMask.sy) {
                continue; // clipped-away mask border
            }
            const unsigned char influence = absoluteMask.at(dx, dy);
            if (!influence) {
                continue;
            }
            const int off = y * static_cast<int>(vMap.H_SIZE) + x;
            offs.push_back(off);
            dxs.push_back(x - cx);
            dys.push_back(y - cy);
            alts.push_back(vMap.SGetAlt(off));
            influences.push_back(influence);
        }
    }
    if (offs.empty()) {
        return false;
    }

    // Reference height per covered cell, computed from the PRE-edit state.
    std::vector<double> references(offs.size());
    if (params.effect == BrushEffect::Tricubic) {
        // Block-average the pre-edit heights of the clip bbox into a coarse
        // control lattice (block values sit at block CENTRES), pad the
        // lattice by one node ring of LINEAR extrapolation, and evaluate
        // the bicubic Catmull-Rom interpolation at every covered cell.
        // Features smaller than the stride die; lines and parabolas pass
        // through exactly (Catmull-Rom reproduction + linear extrapolation
        // keeps planes exact at the ring; plain clamped padding would bend
        // them there).
        const int bw = bxEnd - bx;
        const int bh = byEnd - by;
        std::vector<int> bboxAlts(static_cast<std::size_t>(bw) * bh);
        for (int j = 0; j < bh; ++j) {
            for (int i = 0; i < bw; ++i) {
                bboxAlts[static_cast<std::size_t>(j) * bw + i] =
                    vMap.SGetAlt((by + j) * static_cast<int>(vMap.H_SIZE) +
                                 (bx + i));
            }
        }
        const int nx = (bw + kTricubicStride - 1) / kTricubicStride;
        const int ny = (bh + kTricubicStride - 1) / kTricubicStride;
        if (nx < 2 || ny < 2) {
            // Stamp smaller than the stride — the lattice cannot carry a
            // slope; the plain average is the only stable reference.
            const SculptPlane plane = fitSculptPlane(dxs, dys, alts);
            std::fill(references.begin(), references.end(), plane.offset);
        } else {
            std::vector<double> lattice(static_cast<std::size_t>(nx) * ny);
            std::vector<double> posX(nx);
            std::vector<double> posY(ny);
            for (int j = 0; j < ny; ++j) {
                const int y0 = j * kTricubicStride;
                const int y1 = std::min(bh, y0 + kTricubicStride);
                posY[j] = (y0 + y1 - 1) * 0.5;
                for (int i = 0; i < nx; ++i) {
                    const int x0 = i * kTricubicStride;
                    const int x1 = std::min(bw, x0 + kTricubicStride);
                    if (j == 0) {
                        posX[i] = (x0 + x1 - 1) * 0.5;
                    }
                    long long sum = 0;
                    for (int y = y0; y < y1; ++y) {
                        for (int x = x0; x < x1; ++x) {
                            sum += bboxAlts[static_cast<std::size_t>(y) * bw + x];
                        }
                    }
                    lattice[static_cast<std::size_t>(j) * nx + i] =
                        static_cast<double>(sum) /
                        static_cast<double>((x1 - x0) * (y1 - y0));
                }
            }
            // dxs/dys are mask-centre-relative (see pass 1) — turn them
            // back into bbox-relative cells and evaluate the interpolation.
            for (std::size_t k = 0; k < offs.size(); ++k) {
                references[k] = bicubicAt(lattice, posX, posY,
                                          static_cast<double>(cx + dxs[k] - bx),
                                          static_cast<double>(cy + dys[k] - by));
            }
        }
    } else if (params.effect == BrushEffect::Inflate ||
               params.effect == BrushEffect::Deflate) {
        // Normal-offset reference: every covered cell displaces the
        // pre-edit surface by a fixed step ALONG ITS VOXEL NORMAL
        // (central-difference gradient of the height field, z-up unit
        // normal). Displaced points rasterize into a target height field;
        // columns hit by several points keep the HIGHEST z when inflating
        // (the growing envelope) and the LOWEST when deflating (the
        // shrinking one). A covered cell then moves towards the target
        // height at its OWN column — points sliding sideways along steep
        // slopes carry their height into neighbouring columns.
        const int bw = bxEnd - bx;
        const int bh = byEnd - by;
        std::vector<int> bboxAlts(static_cast<std::size_t>(bw) * bh);
        for (int j = 0; j < bh; ++j) {
            for (int i = 0; i < bw; ++i) {
                bboxAlts[static_cast<std::size_t>(j) * bw + i] =
                    vMap.SGetAlt((by + j) * static_cast<int>(vMap.H_SIZE) +
                                 (bx + i));
            }
        }
        const auto heightAt = [&](int x, int y) {
            return bboxAlts[static_cast<std::size_t>(
                                std::clamp(y, 0, bh - 1)) * bw +
                            std::clamp(x, 0, bw - 1)];
        };
        const bool inflate = params.effect == BrushEffect::Inflate;
        const double step32 =
            static_cast<double>(kInflateStepVoxels) * (1 << VX_FRACTION);
        const int mapW = static_cast<int>(vMap.H_SIZE);
        const int mapH = static_cast<int>(vMap.V_SIZE);
        // Target height field: absolute column -> merged displaced height.
        std::map<std::pair<int, int>, int> target;
        for (std::size_t k = 0; k < offs.size(); ++k) {
            const int xr = cx + dxs[k] - bx;
            const int yr = cy + dys[k] - by;
            const double gx =
                (heightAt(xr + 1, yr) - heightAt(xr - 1, yr)) * 0.5;
            const double gy =
                (heightAt(xr, yr + 1) - heightAt(xr, yr - 1)) * 0.5;
            const double len = std::sqrt(gx * gx + gy * gy + 1.0);
            const double sign = inflate ? 1.0 : -1.0;
            const double dz = sign * step32 / len; // 1/32 units
            // Horizontal offset (cells): the normal's x/y components times
            // the step — one len division only.
            const double dxCells =
                sign * static_cast<double>(kInflateStepVoxels) * (-gx / len);
            const double dyCells =
                sign * static_cast<double>(kInflateStepVoxels) * (-gy / len);
            const int tx = static_cast<int>(std::llround(
                static_cast<double>(cx + dxs[k]) + dxCells));
            const int ty = static_cast<int>(std::llround(
                static_cast<double>(cy + dys[k]) + dyCells));
            if (tx < 0 || ty < 0 || tx >= mapW || ty >= mapH) {
                continue; // displaced off the map — dropped
            }
            const int z = alts[k] + static_cast<int>(std::llround(dz));
            const auto key = std::make_pair(tx, ty);
            const auto it = target.find(key);
            if (it == target.end()) {
                target.emplace(key, z);
            } else if (inflate) {
                it->second = std::max(it->second, z);
            } else {
                it->second = std::min(it->second, z);
            }
        }
        for (std::size_t k = 0; k < offs.size(); ++k) {
            const auto it =
                target.find(std::make_pair(cx + dxs[k], cy + dys[k]));
            references[k] = it != target.end()
                                ? static_cast<double>(it->second)
                                : static_cast<double>(alts[k]);
        }
    } else if (params.effect == BrushEffect::GaussianBlur) {
        // Normalized isotropic Gaussian convolution of the pre-edit
        // heights (kernel radius params.blurRadius, sigma = radius/2) —
        // the reference is the blurred height at the cell. The sample
        // grid extends blurRadius past the bbox so the kernel always
        // reads REAL neighbour heights (edge replication would bend
        // slopes at the rim). Weights sum to 1, so planes reproduce
        // exactly and only features smaller than the radius die.
        const int mapW = static_cast<int>(vMap.H_SIZE);
        const int mapH = static_cast<int>(vMap.V_SIZE);
        const int br = clampBlurRadius(params.blurRadius, kMaxBrushRadius);
        const int gx0 = std::max(0, bx - br);
        const int gy0 = std::max(0, by - br);
        const int gx1 = std::min(mapW, bxEnd + br);
        const int gy1 = std::min(mapH, byEnd + br);
        const int gw = gx1 - gx0;
        const int gh = gy1 - gy0;
        std::vector<int> gridAlts(static_cast<std::size_t>(gw) * gh);
        for (int j = 0; j < gh; ++j) {
            for (int i = 0; i < gw; ++i) {
                gridAlts[static_cast<std::size_t>(j) * gw + i] =
                    vMap.SGetAlt((gy0 + j) * mapW + (gx0 + i));
            }
        }
        const auto heightAt = [&](int absX, int absY) {
            return gridAlts[static_cast<std::size_t>(
                                std::clamp(absY - gy0, 0, gh - 1)) *
                                gw +
                            std::clamp(absX - gx0, 0, gw - 1)];
        };
        if (br < 1) {
            const SculptPlane plane = fitSculptPlane(dxs, dys, alts);
            std::fill(references.begin(), references.end(), plane.offset);
        } else {
            const double sigma = br * 0.5;
            const int side = 2 * br + 1;
            std::vector<double> kernel(static_cast<std::size_t>(side) * side);
            double weightSum = 0.0;
            for (int dy = -br; dy <= br; ++dy) {
                for (int dx = -br; dx <= br; ++dx) {
                    const double w =
                        std::exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma));
                    kernel[static_cast<std::size_t>(dy + br) * side + (dx + br)] =
                        w;
                    weightSum += w;
                }
            }
            for (std::size_t k = 0; k < offs.size(); ++k) {
                const int absX = cx + dxs[k];
                const int absY = cy + dys[k];
                double sum = 0.0;
                for (int dy = -br; dy <= br; ++dy) {
                    for (int dx = -br; dx <= br; ++dx) {
                        sum += kernel[static_cast<std::size_t>(dy + br) * side +
                                      (dx + br)] *
                               heightAt(absX + dx, absY + dy);
                    }
                }
                references[k] = sum / weightSum;
            }
        }
    } else {
        const SculptPlane plane = fitSculptPlane(dxs, dys, alts);
        for (std::size_t k = 0; k < offs.size(); ++k) {
            references[k] =
                params.effect == BrushEffect::Flatten
                    ? plane.offset + plane.a * dxs[k] + plane.b * dys[k]
                    : plane.offset;
        }
    }
    const double effect = clampSculptEffect(params.sculptEffect);

    // Pass 2: move every covered cell an `effect` fraction towards its
    // reference height. Through Terra's variable-H toolzer — the same
    // mechanism the Set Height brush uses (see applyEffectToCell): the
    // toolzer drives the dam layer, caps the step at the target and removes
    // the dam when the surface drops to the geo height. The zeroplast gate
    // is cleared first — a moved cell no longer represents zeroplast (the
    // same rule as Set Height). Dam-free cells are seeded like the other
    // Dam-mode brushes, so the sculpt works on raw geo too.
    static individualToolzer<T2TE_ALIGNMENT_TERRAIN_VARIABLE_H> sculptToolzer;
    for (std::size_t i = 0; i < offs.size(); ++i) {
        // The height gate (Dam mode only): the decision comes from the
        // PRE-edit height of the individual voxel — pass 2 must not let one
        // cell's movement change whether its neighbour is edited.
        if (!heightGateAllows(params, alts[i])) {
            continue;
        }
        const int target32 = std::clamp(
            static_cast<int>(std::llround(references[i])), MIN_VX_HEIGHT,
            MAX_VX_HEIGHT);
        int dV = static_cast<int>(
            std::llround((references[i] - alts[i]) * effect));
        dV = scaleDeltaByInfluence(dV, influences[i]);
        if (dV == 0) {
            continue;
        }
        const int off = offs[i];
        if ((vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST) {
            vMap.AtrBuf[off] = static_cast<unsigned char>(
                vMap.AtrBuf[off] & ~At_ZPMASK);
        }
        seedDamLayer(off);
        setExactToolzerTarget(sculptToolzer, target32);
        sculptToolzer.tVoxSetAll(off, dV);
    }

    // Grid + viewport dirty rectangles, margin 1 like the generic path.
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int x0 = std::max(0, bx - 1);
    const int y0 = std::max(0, by - 1);
    const int x1 = std::min(mapW - 1, bxEnd + 1);
    const int y1 = std::min(mapH - 1, byEnd + 1);
    vMap.recalcArea2Grid(x0, y0, x1, y1);
    session_.regRenderWithShadowTrail(x0, y0, x1, y1);
    return true;
}

bool BrushTool::applyShape3D(const BrushMask& absoluteMask,
                             const BrushParams& params, int bx, int by,
                             int bxEnd, int byEnd)
{
    // The direction comes from the brush type the shape combines with (the
    // sculpting-toolbar shapes do not change the Raise / Lower / Set-to-Height
    // buttons): Set-to-Height moves both ways, Raise only closes on the
    // target from below, Lower only from above.
    const bool setMode = params.effect == BrushEffect::AlignVariableH;
    const int dir = setMode ? 0 : (params.direction < 0 ? -1 : 1);

    Shape3DSettings shape;
    shape.radius = params.radius;
    shape.sides = params.polygonSides;
    shape.angleDeg = params.polygonAngleDeg;
    shape.topOffsetX = params.shapeTopOffsetX;
    shape.topOffsetY = params.shapeTopOffsetY;
    shape.topRadius = params.shapeTopRadius;
    shape.topSide = params.shapeTopSide;
    shape = clampShape3DSettings(params.shape3d, shape);

    // Pass 1 (PRE-edit): collect the covered cells, their heights and their
    // shape targets. The target is absolute — the Height base plus the shape
    // profile (Shape3D.h) scaled by the Shape Height amplitude — kept in
    // Terra's 1/32-voxel fixed point end to end: rounding once at the very
    // end avoids the whole-voxel stair-stepping a per-cell lround would put
    // on smooth profiles, and setExactToolzerTarget caps the toolzer at the
    // exact fractional target.
    const int shapeHeightVox =
        std::clamp(params.shapeHeight, 0, MAX_VX_HEIGHT_WHOLE);
    const double baseVox = clampHeightLevel(params.heightLevel);
    const int cx = (bx + bxEnd - 1) / 2;
    const int cy = (by + byEnd - 1) / 2;
    std::vector<int> offs;
    std::vector<int> alts;
    std::vector<int> targets;
    std::vector<unsigned char> influences;
    for (int y = by; y < byEnd; ++y) {
        for (int x = bx; x < bxEnd; ++x) {
            const int mdx = x - absoluteMask.x;
            const int mdy = y - absoluteMask.y;
            if (mdx < 0 || mdy < 0 || mdx >= absoluteMask.sx ||
                mdy >= absoluteMask.sy) {
                continue; // clipped-away mask border
            }
            const unsigned char influence = absoluteMask.at(mdx, mdy);
            if (!influence) {
                continue;
            }
            const double profile =
                shape3DProfile(params.shape3d, shape,
                               static_cast<double>(x - cx),
                               static_cast<double>(y - cy));
            const double targetVox = baseVox + profile * shapeHeightVox;
            const int target32 = std::clamp(
                static_cast<int>(std::llround(targetVox * (1 << VX_FRACTION))),
                0, MAX_VX_HEIGHT);
            const int off = y * static_cast<int>(vMap.H_SIZE) + x;
            offs.push_back(off);
            alts.push_back(vMap.SGetAlt(off));
            targets.push_back(target32);
            influences.push_back(influence);
        }
    }

    // No-op precheck (before the undo record): a cell can move only when it
    // passes the height gate (PRE-edit height) and the direction cap — a
    // re-stamp over an already-formed shape, or a raise over higher ground,
    // changes nothing and must not even enter the history.
    bool anyMoves = false;
    for (std::size_t k = 0; k < offs.size() && !anyMoves; ++k) {
        if (!heightGateAllows(params, alts[k])) {
            continue;
        }
        const int target32 = targets[k];
        if (dir > 0 && alts[k] < target32) {
            anyMoves = true;
        } else if (dir < 0 && alts[k] > target32) {
            anyMoves = true;
        } else if (dir == 0 && alts[k] != target32) {
            anyMoves = true;
        }
    }
    if (!anyMoves) {
        return false;
    }

    session_.recordUndoArea(bx, by, bxEnd - bx, byEnd - by);

    // Pass 2: move every covered cell the whole remaining distance towards
    // its target, through Terra's variable-H toolzer — the same mechanism as
    // the Set Height brush and the sculpting pass 2: the toolzer drives the
    // dam layer, caps the step at the target and removes the dam when the
    // surface drops back to the geo height (Shape Height 0 flattens the
    // footprint down to the Height base — "remove dam"). The zeroplast gate
    // is cleared first: a moved cell no longer represents zeroplast. Dam-free
    // cells are seeded like the other Dam-mode brushes, so the shapes build
    // on raw geo too. tVoxSetAll: the shape placement must not depend on the
    // GRIDAT hardness (same rule as Set Height / sculpting).
    static individualToolzer<T2TE_ALIGNMENT_TERRAIN_VARIABLE_H> shapeToolzer;
    for (std::size_t k = 0; k < offs.size(); ++k) {
        // The height gate (Dam mode only): the decision comes from the
        // PRE-edit height of the individual voxel — pass 2 must not let one
        // cell's movement change whether its neighbour is edited.
        if (!heightGateAllows(params, alts[k])) {
            continue;
        }
        const int target32 = targets[k];
        int dV = target32 - alts[k];
        if (dir > 0 && dV <= 0) {
            continue; // raise: only cells below the target move
        }
        if (dir < 0 && dV >= 0) {
            continue; // lower: only cells above the target move
        }
        dV = scaleDeltaByInfluence(dV, influences[k]);
        if (dV == 0) {
            continue;
        }
        const int off = offs[k];
        if ((vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST) {
            vMap.AtrBuf[off] = static_cast<unsigned char>(
                vMap.AtrBuf[off] & ~At_ZPMASK);
        }
        seedDamLayer(off);
        setExactToolzerTarget(shapeToolzer, target32);
        shapeToolzer.tVoxSetAll(off, dV);
    }

    // Grid + viewport dirty rectangles, margin 1 like the generic path.
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int x0 = std::max(0, bx - 1);
    const int y0 = std::max(0, by - 1);
    const int x1 = std::min(mapW - 1, bxEnd + 1);
    const int y1 = std::min(mapH - 1, byEnd + 1);
    vMap.recalcArea2Grid(x0, y0, x1, y1);
    session_.regRenderWithShadowTrail(x0, y0, x1, y1);
    return true;
}

bool BrushTool::applyModelBrush(const BrushMask& absoluteMask,
                                const BrushParams& params, int bx, int by,
                                int bxEnd, int byEnd)
{
    if (pendingModel_.empty() ||
        pendingModelIndices_.size() !=
            static_cast<std::size_t>(pendingModel_.width) *
                pendingModel_.height) {
        return false; // the rectangle path did not run (defensive)
    }
    // Modes: 0 place — absolute set to the scaled model height + palette
    // colours; 1 add — the covered average raised by the local model height;
    // 2 subtract — the covered average lowered (the dam is removed where the
    // result drops to the terrain). Targets are whole voxels, like the 3D
    // shape brushes (Terra's toolzer caps at whole-voxel granularity).
    const int mode = params.modelMode;
    const int heightVox =
        std::clamp(params.modelHeight, 0, MAX_VX_HEIGHT_WHOLE);
    const int stampW = pendingModel_.width;
    const int stampH = pendingModel_.height;
    const int mx = bx + (bxEnd - bx - stampW) / 2; // stamp bbox == mask bbox
    const int my = by + (byEnd - by - stampH) / 2;

    // Pass 1 (PRE-edit): collect the covered cells, heights, model samples
    // and (after the average is known) the targets.
    std::vector<int> offs;
    std::vector<int> alts;
    std::vector<int> targets;
    std::vector<int> indices;
    std::vector<std::uint32_t> colors;
    std::vector<double> norms;
    double altSum = 0.0;
    std::size_t altCount = 0;
    for (int y = by; y < byEnd; ++y) {
        for (int x = bx; x < bxEnd; ++x) {
            const int mdx = x - absoluteMask.x;
            const int mdy = y - absoluteMask.y;
            if (mdx < 0 || mdy < 0 || mdx >= absoluteMask.sx ||
                mdy >= absoluteMask.sy) {
                continue;
            }
            if (!absoluteMask.at(mdx, mdy)) {
                continue;
            }
            const int off = y * static_cast<int>(vMap.H_SIZE) + x;
            const int alt = vMap.SGetAlt(off);
            // Clipped at the map border the bbox can be smaller than the
            // stamp — clamp the sample into the raster.
            const int sy = std::clamp(y - my, 0, stampH - 1);
            const int sx = std::clamp(x - mx, 0, stampW - 1);
            const std::size_t sample = static_cast<std::size_t>(sy) * stampW + sx;
            offs.push_back(off);
            alts.push_back(alt);
            indices.push_back(pendingModelIndices_[sample]);
            colors.push_back(pendingModelColors_[sample]);
            norms.push_back(pendingModel_.normHeight[sample]);
            targets.push_back(0); // filled below (modes 1/2 need the average)
            altSum += static_cast<double>(alt);
            ++altCount;
        }
    }
    if (offs.empty() || altCount == 0) {
        return false;
    }
    // Targets in Terra's 1/32 fixed point throughout: averageAlt is already
    // fixed point, the model fraction scales in voxel units — one rounding
    // at the end lands each cell on the nearest representable step instead
    // of a whole voxel (no stair-stepping on smooth model slopes).
    const double averageAlt = altSum / static_cast<double>(altCount);
    const double heightScale =
        static_cast<double>(heightVox) * (1 << VX_FRACTION);
    for (std::size_t k = 0; k < offs.size(); ++k) {
        const double norm = norms[k];
        if (mode == 0) {
            targets[k] = std::clamp(
                static_cast<int>(std::llround(norm * heightScale)), 0,
                MAX_VX_HEIGHT);
        } else if (mode == 1) {
            targets[k] = std::clamp(
                static_cast<int>(std::llround(averageAlt + norm * heightScale)),
                0, MAX_VX_HEIGHT);
        } else {
            // Subtract: a negative result removes the dam (clamped at 0 —
            // the toolzer removes the dam on its way down to the terrain).
            targets[k] = std::clamp(
                static_cast<int>(std::llround(averageAlt - norm * heightScale)),
                0, MAX_VX_HEIGHT);
        }
    }

    // No-op precheck (before the undo record): gate (PRE-edit height) plus
    // the mode's direction cap — a re-placement over the formed model, or an
    // add over higher ground, changes nothing and must not enter the history.
    const int dir = mode == 0 ? 0 : (mode == 1 ? 1 : -1);
    bool anyMoves = false;
    for (std::size_t k = 0; k < offs.size() && !anyMoves; ++k) {
        if (!heightGateAllows(params, alts[k])) {
            continue;
        }
        const int target32 = targets[k];
        if (dir > 0 && alts[k] < target32) {
            anyMoves = true;
        } else if (dir < 0 && alts[k] > target32) {
            anyMoves = true;
        } else if (dir == 0 && alts[k] != target32) {
            anyMoves = true;
        }
    }
    if (!anyMoves) {
        return false;
    }

    session_.recordUndoArea(bx, by, bxEnd - bx, byEnd - by);

    // Pass 2: move the cells through Terra's variable-H toolzer (the same
    // mechanism as Set Height / the 3D shapes: dam seeding, whole-voxel cap,
    // dam removal at the geo height, zeroplast gate cleared, hardness
    // ignored). "place" also paints the model's exact 24-bit color where a
    // dam exists after the move.
    static individualToolzer<T2TE_ALIGNMENT_TERRAIN_VARIABLE_H> modelToolzer;
    for (std::size_t k = 0; k < offs.size(); ++k) {
        if (!heightGateAllows(params, alts[k])) {
            continue;
        }
        const int target32 = targets[k];
        int dV = target32 - alts[k];
        if (dir > 0 && dV <= 0) {
            continue;
        }
        if (dir < 0 && dV >= 0) {
            continue;
        }
        dV = scaleDeltaByInfluence(dV, 255);
        if (dV == 0) {
            continue;
        }
        const int off = offs[k];
        if ((vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST) {
            vMap.AtrBuf[off] = static_cast<unsigned char>(
                vMap.AtrBuf[off] & ~At_ZPMASK);
        }
        seedDamLayer(off);
        setExactToolzerTarget(modelToolzer, targets[k]);
        modelToolzer.tVoxSetAll(off, dV);
        if (mode == 0 && indices[k] >= 0 && vMap.VxDBuf[off] != 0) {
            // True-color: the model's exact 24-bit color goes into the
            // color layer; SurBuf keeps the nearest palette entry (surface
            // type/materials).
            if ((colors[k] >> 24) == 0xFF) {
                vMap.putTrueColor(
                    off, static_cast<unsigned char>((colors[k] >> 16) & 0xFF),
                    static_cast<unsigned char>((colors[k] >> 8) & 0xFF),
                    static_cast<unsigned char>(colors[k] & 0xFF));
            }
            vMap.SurBuf[off] = static_cast<unsigned char>(indices[k]);
        }
    }

    // Grid + viewport dirty rectangles, margin 1 like the generic path.
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int x0 = std::max(0, bx - 1);
    const int y0 = std::max(0, by - 1);
    const int x1 = std::min(mapW - 1, bxEnd + 1);
    const int y1 = std::min(mapH - 1, byEnd + 1);
    vMap.recalcArea2Grid(x0, y0, x1, y1);
    session_.regRenderWithShadowTrail(x0, y0, x1, y1);
    return true;
}


//   * Geo — only VxGBuf is driven (dam height caps the raise, the dam buffer
//     itself is never written), keeping the game invariant "dam ≥ geo";
//   * Dam — the dam layer is seeded (exactly like Terra's
//     T2TE_PLOTTING_VERY_LIGHT_DAM) when the tool raises a dam-free cell, and
//     every further change goes through Terra's own toolzer which removes the
//     dam when the surface falls to the geo height.
// The write orderings replicate Terra's tVoxSetAll (tools.h:496-674) cell
// behaviour: SurBuf re-derivation via GetGeoType, whole voxel + fraction bit
// storage, AtrBuf carrying the fraction bits.
// ---------------------------------------------------------------------------

namespace {

/// Terra's tVoxSet hardness attenuation for dV < 0 (tools.h:488-495):
/// `dV = dV * (hardness << hardness) / 24`, INTEGER ops in this order.
/// Replicated verbatim so the editor digs exactly like the game.
int applyHardness(int off, int dV)
{
    if (dV >= 0) {
        return dV;
    }
    const int hardness = vMap.GABuf[vMap.offsetBuf2offsetGBuf(off)] &
                         GRIDAT_MASK_HARDNESS;
    return (dV * (hardness << hardness)) / 24;
}

// ---------------------------------------------------------------------------
// T3.2 — BrushMode layer helpers for the height tools.
//
// Terra's change-height toolzer always edits the active (top) layer: the dam
// when VxDBuf != 0, the geo otherwise. The editor's Geo/Dam modes cut across
// that, so the layer selection happens around the Terra toolzer:

/// ChangeHeight in Geo mode.
///
/// Dam-free cells drive VxGBuf only (Terra's geo-branch write order:
/// SurBuf = GetGeoType, whole voxel into VxGBuf, fraction bits into AtrBuf).
///
/// Dam cells must never rewrite the dam layer's surface state while moving
/// the geo under it:
///   * SurBuf stays untouched — it is the material of the *top* layer (the
///     dam); GetGeoType on a dam cell would repaint the dam in ground
///     texture, which read like the raised geo was drawn on top of the dam.
///   * AtrBuf stays untouched on a lower — the 3 fraction bits are shared
///     between both layers and describe the top surface, so storing the
///     lowered geo's remainder there visibly shifted the dam top.
///   * A raise lifts the dam together with the geo ("dam >= geo" moves up
///     as a unit): the geo part stays below the dam, the dam part rises by
///     the same delta and the dam thickness is preserved. Because both
///     layer tops share the same fraction, shifting both by the same dV
///     keeps every remainder consistent.
///   * A lower is the exact inverse: both layers shift down by dV, so a
///     raise+lower pair with the same parameters restores the original
///     state byte-exact (the dam thickness is preserved either way).
void geoHeightChange(int off, int dV)
{
    dV = applyHardness(off, dV);
    const int fraction = vMap.AtrBuf[off] & VX_FRACTION_MASK;
    const int geoTop = (static_cast<int>(vMap.VxGBuf[off]) << VX_FRACTION) +
                       fraction;
    const int damTop = (static_cast<int>(vMap.VxDBuf[off]) << VX_FRACTION) +
                       fraction;

    if (vMap.VxDBuf[off] == 0) {
        // Dam-free cell: the geo is the surface — full precision write.
        const int clamped = std::clamp(geoTop + dV, MIN_VX_HEIGHT, MAX_VX_HEIGHT);
        vMap.SurBuf[off] = vMap.GetGeoType(off, clamped);
        vMap.VxGBuf[off] = static_cast<unsigned char>(clamped >> VX_FRACTION);
        vMap.AtrBuf[off] = static_cast<unsigned char>(clamped & VX_FRACTION_MASK);
        return;
    }

    if (dV >= 0) {
        // Raise: geo + dam slide up together, the dam stays on top. A
        // pre-existing geo height above the dam (corrupt state) is capped.
        const int newDam = std::min(damTop + dV, MAX_VX_HEIGHT);
        const int newGeo = std::min(geoTop + dV, newDam);
        vMap.VxDBuf[off] = static_cast<unsigned char>(newDam >> VX_FRACTION);
        vMap.VxGBuf[off] = static_cast<unsigned char>(newGeo >> VX_FRACTION);
        // Same dV shift on both tops keeps the shared fraction exact.
        vMap.AtrBuf[off] = static_cast<unsigned char>(newDam & VX_FRACTION_MASK);
    } else {
        // Lower: the exact inverse of the raise — geo + dam slide down
        // together, so a raise+lower pair with the same parameters
        // restores the original state byte-exact and the dam stays on top
        // (a pre-existing geo height above the dam is capped at the new
        // dam top, like in the raise branch).
        const int newDam = std::max(damTop + dV, MIN_VX_HEIGHT);
        const int newGeo = std::min(std::max(geoTop + dV, MIN_VX_HEIGHT), newDam);
        vMap.VxDBuf[off] = static_cast<unsigned char>(newDam >> VX_FRACTION);
        vMap.VxGBuf[off] = static_cast<unsigned char>(newGeo >> VX_FRACTION);
        // Same dV shift on both tops keeps the shared fraction exact.
        vMap.AtrBuf[off] = static_cast<unsigned char>(newDam & VX_FRACTION_MASK);
    }
}

/// Seed a very light dam at the geo height (exactly like Terra's
/// T2TE_PLOTTING_VERY_LIGHT_DAM) so that a dam-mode tool can drive it.
/// Returns the height bump the seeding introduced in 1/32 m units: a
/// zero-height geo cannot carry a dam (VxDBuf == 0 means "no dam"), so the
/// layer starts one voxel up (bump = 1 << VX_FRACTION); an existing dam
/// returns 0.
int seedDamLayer(int off)
{
    if (vMap.VxDBuf[off] != 0) {
        return 0;
    }
    const int seed = vMap.VxGBuf[off] ? vMap.VxGBuf[off] : 1;
    const int bump = (seed != vMap.VxGBuf[off]) ? (1 << VX_FRACTION) : 0;
    vMap.VxDBuf[off] = static_cast<unsigned char>(seed);
    vMap.SurBuf[off] = vMap.veryLightDam;
    return bump;
}

/// ChangeHeight in Dam mode: seed the dam layer when needed, then let Terra's
/// toolzer edit the active dam layer.
void damHeightChange(int off, int dV)
{
    if (vMap.VxDBuf[off] == 0) {
        if (dV <= 0) {
            // Nothing to lower, and lowering the geo does not belong to the
            // Dam mode (the Geo mode owns VxGBuf).
            return;
        }
        const int bump = seedDamLayer(off);
        if (bump >= dV) {
            // A sub-voxel raise cannot create a whole-voxel dam layer.
            return;
        }
        dV -= bump; // compensate the seed bump exactly
    }
    // The toolzers below are function-local statics ON PURPOSE: they carry
    // no cross-call state that matters (setHAppoximation() is (re)set before
    // every tVoxSet), and the statics avoid reallocating Terra's internal
    // tables per brush cell. BrushTool is strictly single-threaded (UI
    // thread, see EditorSession) — do not use these from worker threads.
    static individualToolzer<T2TE_CHANGING_TERRAIN_HEIGHT> heightToolzer;
    heightToolzer.tVoxSet(off, dV);
}

// (the anonymous namespace of the layer helpers closes here — a member
// function cannot be defined inside it)
} // namespace

bool BrushTool::applyHeightFunction(
    const BrushMask& absoluteMask,
    const std::function<bool(int, int, double, double&)>& heightFn)
{
    if (!session_.isWorldLoaded() || absoluteMask.empty()) {
        return false;
    }
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int bx = std::max(0, absoluteMask.x);
    const int by = std::max(0, absoluteMask.y);
    const int bxEnd = std::min(mapW, absoluteMask.x + absoluteMask.sx);
    const int byEnd = std::min(mapH, absoluteMask.y + absoluteMask.sy);
    if (bx >= bxEnd || by >= byEnd) {
        return false; // completely outside the map
    }

    // --- Pass 1: evaluate the function over the PRE-EDIT surface. Every ---
    // --- read happens before ANY write, so covered cells never see each  ---
    // --- other's new heights — adjacent radius-1 stamps / cells cannot  ---
    // --- add up the way overlapping raise/lower stamps do. A callback   ---
    // --- failure aborts before anything is written or recorded.         ---
    struct CellTarget
    {
        int off;
        int target; ///< Terra 1/32 fixed point
    };
    std::vector<CellTarget> targets;
    for (int y = by; y < byEnd; ++y) {
        for (int x = bx; x < bxEnd; ++x) {
            const int dx = x - absoluteMask.x;
            const int dy = y - absoluteMask.y;
            if (dx < 0 || dy < 0 || dx >= absoluteMask.sx ||
                dy >= absoluteMask.sy || !absoluteMask.at(dx, dy)) {
                continue; // clipped-away mask border
            }
            const int off = y * mapW + x;
            // Same surface read the API's getDamHeight reports (0 where no
            // dam), but keeping the fraction bits — the function sees the
            // exact 1/32-precision height.
            const int alt = vMap.VxDBuf[off] != 0
                ? static_cast<int>(vMap.SGetAlt(off))
                : 0;
            double targetVox = 0.0;
            if (!heightFn(x, y,
                          static_cast<double>(alt) / (1 << VX_FRACTION),
                          targetVox)) {
                return false; // the script failed — nothing written yet
            }
            const int target32 = heightLevelFixedPoint(targetVox);
            if (target32 != alt) {
                targets.push_back({off, target32});
            }
        }
    }
    if (targets.empty()) {
        return false; // nothing would change — no empty undo entry
    }

    // --- Undo snapshot before the first write (one entry per call). ---
    session_.recordUndoArea(bx, by, bxEnd - bx, byEnd - by);

    // --- Pass 2: write each target through the AlignVariableH mechanism ---
    // --- (Dam layer: zeroplast-gate clearing + seeding + tVoxSetAll —    ---
    // --- hardness ignored, exact in both directions), with the per-cell  ---
    // --- target synced before every call like the alignment brush does.  ---
    for (const CellTarget& t : targets) {
        const int off = t.off;
        const int wholeZP = static_cast<int>(vMap.hZeroPlast);
        if ((t.target >> VX_FRACTION) != wholeZP &&
            (vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST) {
            // The alignment toolzers early-return on At_ZEROPLAST cells;
            // a function target different from hZeroPlast means the cell
            // no longer represents zeroplast (same rule as Set Height).
            vMap.AtrBuf[off] = static_cast<unsigned char>(
                vMap.AtrBuf[off] & ~At_ZPMASK);
        }
        seedDamLayer(off);
        const int dV = t.target - static_cast<int>(vMap.SGetAlt(off));
        if (dV == 0) {
            continue;
        }
        static individualToolzer<T2TE_ALIGNMENT_TERRAIN_VARIABLE_H> alignHToolzer;
        setExactToolzerTarget(alignHToolzer, t.target);
        alignHToolzer.tVoxSetAll(off, dV);
    }

    // Grid + viewport dirty rectangles, margin 1 like applyMask.
    const int x0 = std::max(0, bx - 1);
    const int y0 = std::max(0, by - 1);
    const int x1 = std::min(mapW - 1, bxEnd + 1);
    const int y1 = std::min(mapH - 1, byEnd + 1);
    vMap.recalcArea2Grid(x0, y0, x1, y1);
    session_.regRenderWithShadowTrail(x0, y0, x1, y1);
    return true;
}

namespace {

/// Alignment (Align4ZP / AlignVariableH) in Geo mode over a dam cell.
///
/// Terra's alignment toolzers always edit the top (dam) layer, but the Geo
/// mode owns VxGBuf only — before the fix such cells were skipped entirely,
/// which read as "level to height does nothing over dams". This helper moves
/// the geo top towards `target` by `dV` under the dam, capping at the dam
/// top so the "dam >= geo" invariant holds; the dam's whole voxel and the
/// surface material stay untouched (the shared fraction bits follow the geo,
/// the same trade-off Terra's own dam-lowering branch makes when it rewrites
/// the geo under a falling dam).
void geoAlignUnderDam(int off, int target, int dV)
{
    const int fraction = vMap.AtrBuf[off] & VX_FRACTION_MASK;
    const int geoTop = (static_cast<int>(vMap.VxGBuf[off]) << VX_FRACTION) +
                       fraction;
    const int damTop = (static_cast<int>(vMap.VxDBuf[off]) << VX_FRACTION) +
                       fraction;

    int v = geoTop;
    if (dV > 0) {
        if (geoTop >= target || geoTop >= damTop) {
            return; // already at the target or capped by the dam
        }
        v = std::min(geoTop + dV, std::min(target, damTop));
    } else if (dV < 0) {
        if (geoTop <= target) {
            return;
        }
        v = std::max(geoTop + dV, target);
        v = std::min(v, damTop);
    } else {
        return;
    }
    v = std::max(v, MIN_VX_HEIGHT);
    vMap.VxGBuf[off] = static_cast<unsigned char>(v >> VX_FRACTION);
    vMap.AtrBuf[off] = static_cast<unsigned char>(v & VX_FRACTION_MASK);
    if (v == target) {
        // Terra's alignment toolzers mark the reached target At_LEVELED.
        vMap.AtrBuf[off] = static_cast<unsigned char>(
            (vMap.AtrBuf[off] & ~At_ZPMASK) | At_LEVELED);
    }
}

} // namespace

void BrushTool::applyEffectToCell(const BrushParams& params, int x, int y, unsigned char influence)
{
    const int off = y * static_cast<int>(vMap.H_SIZE) + x;
    // The height gate (Dam mode only, combinable with every effect): skip the
    // voxel when its own dam height fails the condition. Checked per voxel —
    // the stamp still edits the neighbours that pass.
    if (params.mode == BrushMode::Dam &&
        !heightGateAllowsCell(params, off)) {
        return;
    }
    switch (params.effect) {
    case BrushEffect::ChangeHeight: {
        // Terra's own change-height toolzer: hardness scaling, At_*
        // attributes and the geo/dam layer split (VxGBuf/VxDBuf/SurBuf
        // consistency) happen inside tVoxSet (tools.h:488-674); the
        // BrushMode layer selection is applied around it (see the helpers).
        const int dV = changeHeightDelta(params, influence);
        if (params.mode == BrushMode::Geo) {
            geoHeightChange(off, dV);
        } else {
            damHeightChange(off, dV);
        }
        break;
    }
    case BrushEffect::Align4ZP:
    case BrushEffect::AlignVariableH: {
        // Terra's alignment toolzers level the surface towards hAppr, cap it
        // at the target and skip At_ZEROPLAST cells — the game behaviour
        // (tools.h:545-631). Align4ZP marks the reached cells At_LEVELED;
        // the variable-H toolzer writes only the fraction bits. The dam
        // layer is driven by the toolzer as well: raising with a dam present
        // edits VxDBuf, dropping below the geo height removes the dam.
        // Both effects ignore the GRIDAT hardness (tVoxSetAll, not tVoxSet).
        // The target height is synced before every cell — a static toolzer
        // would otherwise carry the hZeroPlast of a previously loaded world.
        const bool damCell = vMap.VxDBuf[off] != 0;
        // The target in Terra's 1/32 fixed point: hZeroPlast is whole by
        // definition; a fractional heightLevel (scripted/MCP brushes) is
        // converted once to the nearest supported fixed-point value.
        const int target =
            params.effect == BrushEffect::Align4ZP
                ? static_cast<int>(vMap.hZeroPlast) << VX_FRACTION
                : heightLevelFixedPoint(params.heightLevel);

        if (params.effect == BrushEffect::Align4ZP &&
            (vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST) {
            break; // already zeroplast — the alignment toolzer skips it too
        }

        // Both alignment effects are absolute sets: one stamp moves every
        // covered cell by the whole remaining distance to the target (scaled
        // by the mask influence), so even a much lower dam or a geo-only cell
        // reaches the chosen height in a single click — the toolzer caps the
        // overshoot at the target. The Dam mode seeds its layer first: the
        // distance must be measured from the seeded dam top (the zero-geo
        // seed sits one voxel up), otherwise the stamp stops one voxel short
        // of the target. (Align4ZP used to take Terra's game-like
        // strength-scaled step instead: over low geo the brush then reached
        // the zeroplast height only after repeated stamps, and only the
        // overshooting stamp marked At_ZEROPLAST.)
        //
        // Terra's alignment toolzers have an early-return gate:
        // `if (AtrBuf & At_ZPMASK == At_ZEROPLAST) return 0`
        // (tools.h:546, 590). A cell marked At_ZEROPLAST is silently
        // skipped — the dam height never moves. The user-level rule is:
        // when the brush target differs from hZeroPlast, the dam should
        // be levelled to the target and the zeroplast mark removed (the
        // cell no longer represents zeroplast). Clear the ZP gate here
        // so the toolzer can drive the dam layer.
        if (params.effect == BrushEffect::AlignVariableH &&
            params.mode == BrushMode::Dam) {
            const int wholeZP = static_cast<int>(vMap.hZeroPlast);
            if ((target >> VX_FRACTION) != wholeZP &&
                (vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST) {
                vMap.AtrBuf[off] = static_cast<unsigned char>(
                    vMap.AtrBuf[off] & ~At_ZPMASK);
            }
        }
        if (params.mode == BrushMode::Dam) {
            seedDamLayer(off);
        }

        // Full remaining distance; influence 255 (every shape stamp) is
        // exact, a partial image-mask influence moves part of the way.
        const int dV = scaleDeltaByInfluence(
            target - static_cast<int>(vMap.SGetAlt(off)), influence);

        if (params.mode == BrushMode::Geo && damCell) {
            // Geo mode owns VxGBuf only: Terra's toolzer would edit the top
            // (dam) layer, so the geo under the dam is aligned here instead
            // — capped at the dam top, the dam itself stays untouched.
            geoAlignUnderDam(off, target, dV);
            if (params.effect == BrushEffect::Align4ZP) {
                // Mark only when the cell's top surface sits exactly at the
                // zeroplast height (the toolzer checks that itself).
                static individualToolzer<T2TE_PLOTTING_ZEROPLAST> plotZPToolzer;
                setExactToolzerTarget(plotZPToolzer, target);
                plotZPToolzer.tVoxSet(off, 1);
            }
            break;
        }
        if (params.effect == BrushEffect::Align4ZP) {
            static individualToolzer<T2TE_ALIGNMENT_TERRAIN_4ZP> alignZPToolzer;
            setExactToolzerTarget(alignZPToolzer, target);
            // tVoxSetAll, not tVoxSet: the zeroplast brush ignores the GRIDAT
            // hardness. tVoxSet attenuates a lowering by
            // `dV*(hardness<<hardness)/24` — 0/24, 2/24, 8/24, 24/24 for
            // hardness 0..3. On hard ground the dam then either did not move
            // at all (hardness 0) or moved by a fraction of the step, so a
            // stamp over cells of different heights left them uneven and
            // above hZeroPlast. Setting the surface to the zeroplast level
            // must not depend on hardness. (The Dam-mode dam seeding now
            // happens above, before the distance measurement.)
            alignZPToolzer.tVoxSetAll(off, dV);
            // The zeroplast marking of the former PlotZeroplast brush: the
            // merged brush levels to hZeroPlast and marks every cell that
            // sits at that height (Terra's own plotCircleZL behaviour).
            static individualToolzer<T2TE_PLOTTING_ZEROPLAST> plotZPToolzer;
            setExactToolzerTarget(plotZPToolzer, target);
            plotZPToolzer.tVoxSet(off, 1);
        } else {
            // AlignVariableH drives the (seeded) dam or the geo layer to the
            // absolute target. tVoxSetAll (not tVoxSet) is used so the GRIDAT
            // hardness attenuation does not leave a lowering stamp short of
            // the chosen height: "set to height" is exact in both directions.
            static individualToolzer<T2TE_ALIGNMENT_TERRAIN_VARIABLE_H> alignHToolzer;
            setExactToolzerTarget(alignHToolzer, target);
            alignHToolzer.tVoxSetAll(off, dV);
        }
        break;
    }
    case BrushEffect::SetColor: {
        // T3.3/T4.3 — paint the surface buffer with the selected palette
        // index, honouring the brush layer. SurBuf is a direct palette index
        // of the cell's layer (T4.1), and the renderer resolves the color
        // through the *layer's* palette (RnrBuf bit 7 is derived from
        // VxDBuf at render time, LAND.cpp:1461-1462), so painting a dam
        // index on a geo cell (or vice versa) would show an arbitrary color
        // from the wrong palette. Layer selection therefore gates the paint:
        //   Geo mode recolors geo cells only (VxDBuf == 0),
        //   Dam mode recolors existing dam cells only (VxDBuf != 0) —
        // the color brush never seeds or removes a dam; the merged
        // zeroplast brush / VeryLightDam own dam creation. Height buffers
        // stay untouched.
        const bool isDamCell = vMap.VxDBuf[off] != 0;
        if ((params.mode == BrushMode::Dam) != isDamCell) {
            break; // wrong layer for this cell — skip, keep SurBuf as-is
        }
        vMap.SetTer(x, y, static_cast<unsigned char>(params.colorIndex));
        // A palette-index paint makes the cell follow the palette again:
        // any direct RGB override recorded earlier is dropped.
        vMap.clearTrueColor(off);
        break;
    }
    case BrushEffect::ImageDecal: {
        // Works exactly like SetColor, one pixel at a time, but paints the
        // pixel's exact 24-bit color into the true-color layer instead of a
        // palette entry; the nearest Dam palette index still goes into SurBuf
        // (surface type/materials, palette fallback). Two extra rules:
        //   * the decal owns the Dam layer only — geo cells are skipped;
        //   * transparent decal pixels are never applied (the mask already
        //     dropped them; the alpha check keeps a stale origin harmless).
        const int dx = x - decalOriginX_;
        const int dy = y - decalOriginY_;
        if (dx < 0 || dy < 0 || dx >= decal_.width || dy >= decal_.height) {
            break;
        }
        const std::uint32_t pixel = decal_.at(dx, dy);
        if ((pixel >> 24) != 0xFF) {
            break;
        }
        if (vMap.VxDBuf[off] == 0) {
            break; // Dam layer only
        }
        vMap.putTrueColor(off, static_cast<unsigned char>((pixel >> 16) & 0xFF),
                          static_cast<unsigned char>((pixel >> 8) & 0xFF),
                          static_cast<unsigned char>(pixel & 0xFF));
        vMap.SetTer(x, y, decal_.indexAt(dx, dy));
        break;
    }
    case BrushEffect::PlotSoot: {
        // T3.4 — T2TE_PLOTTING_SOOT toolzer: mark cell as soot (At_SOOT)
        // when the mask has influence and the cell is not zeroplast.
        {
            static individualToolzer<T2TE_PLOTTING_SOOT> sootToolzer;
            sootToolzer.tVoxSet(off, 1);
        }
        break;
    }
    case BrushEffect::VeryLightDam: {
        // T3.4 — T2TE_PLOTTING_VERY_LIGHT_DAM toolzer: seed a dam at geo
        // height and paint the surface with the very light dam index.
        {
            static individualToolzer<T2TE_PLOTTING_VERY_LIGHT_DAM> vldToolzer;
            vldToolzer.tVoxSet(off, 1);
        }
        break;
    }
    // applyMask() refuses unsupported effects, so these cases are unreachable.
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// T4.2 — eyedropper brush type (PickColor).
//
// SurBuf carries the pixel the renderer shows: indices 0..127 are Geo palette
// colors, 128..255 Dam palette colors (the same split the SetColor brush
// paints with). The sampled index feeds params().colorIndex so the common
// brush interface round-trips the pick: the click samples the Geo/Dam pixel
// of the layer that is actually displayed at that cell.
// ---------------------------------------------------------------------------

bool BrushTool::pickColorAt(int cx, int cy)
{
    if (!session_.isWorldLoaded()) {
        return false;
    }
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    if (cx < 0 || cy < 0 || cx >= mapW || cy >= mapH) {
        return false; // sky / off-map pick — nothing to sample
    }
    const int off = cy * mapW + cx;
    lastPickedColorIndex_ = vMap.SurBuf[off];
    hasPickedColor_ = true;
    // The pick result belongs to the color brush: keep it in the shared
    // parameters so switching the brush type back to SetColor paints the
    // sampled color without any extra UI round-trip.
    params_.colorIndex = lastPickedColorIndex_;
    return true;
}

} // namespace MapEditor

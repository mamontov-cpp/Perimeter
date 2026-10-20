// ============================================================================
// MapEditorCore — BrushTool implementation (T3.1).
//
// Terra headers (terra.h pulls VMAP.h → tools.h with individualToolzer):
// _PERIMETER_ and _TX3D_LIBRARY_ come from CMake, exactly like
// EditorSession.cpp / View3DRenderer.cpp.
// ============================================================================

#include "BrushTool.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

/// Circle mask over all cells with ring(dx,dy) <= radius, or only the ring ==
/// radius cells for the outline. Ring 0 is the centre cell itself, so the
/// outline of radius 0 degenerates to a single centre cell.
BrushMask buildCircle(int radius, bool outline)
{
    const int side = 2 * radius + 1;
    BrushMask mask = symmetricMask(side);
    for (int dy = 0; dy < side; ++dy) {
        const int y = dy + mask.y;
        for (int dx = 0; dx < side; ++dx) {
            const int x = dx + mask.x;
            const int ring = BrushMasks::circularRing(x, y);
            const bool member = outline ? (ring == radius) : (ring <= radius);
            if (member) {
                setCell(mask, dx, dy);
            }
        }
    }
    return mask;
}

/// Square mask: filled |dx|<=radius && |dy|<=radius or the one-cell border.
BrushMask buildSquare(int radius, bool outline)
{
    const int side = 2 * radius + 1;
    BrushMask mask = symmetricMask(side);
    for (int dy = 0; dy < side; ++dy) {
        const int y = dy + mask.y;
        for (int dx = 0; dx < side; ++dx) {
            const int x = dx + mask.x;
            const bool border = std::max(std::abs(x), std::abs(y)) == radius;
            const bool member = outline ? border : true;
            if (member) {
                setCell(mask, dx, dy);
            }
        }
    }
    return mask;
}

} // namespace

BrushMask BrushMasks::filledCircle(int radius) { return buildCircle(radius, false); }
BrushMask BrushMasks::circleOutline(int radius) { return buildCircle(radius, true); }
BrushMask BrushMasks::filledSquare(int radius) { return buildSquare(radius, false); }
BrushMask BrushMasks::squareOutline(int radius) { return buildSquare(radius, true); }

BrushMask BrushMasks::byShape(BrushShape shape, int radius)
{
    switch (shape) {
    case BrushShape::FilledCircle:  return filledCircle(radius);
    case BrushShape::CircleOutline: return circleOutline(radius);
    case BrushShape::FilledSquare:  return filledSquare(radius);
    case BrushShape::SquareOutline: return squareOutline(radius);
    case BrushShape::Line:
        // Line needs two endpoints — BrushTool::applyLine() builds the mask
        // with lineStamp(); a shapeless line stamp is empty on purpose.
        return BrushMask{};
    }
    return BrushMask{};
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
    const int r = BrushTool::clampRadius(stampRadius);
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

BrushParams clampedParams(const BrushParams& params)
{
    BrushParams clamped = params;
    clamped.radius = BrushTool::clampRadius(clamped.radius);
    clamped.strength = BrushTool::clampStrength(clamped.strength);
    if (clamped.direction != 0) {
        clamped.direction = clamped.direction > 0 ? 1 : -1;
    }
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
    return MAX_RADIUS_CIRCLEARR;
}

int BrushTool::clampRadius(int radius)
{
    return std::clamp(radius, 0, maxRadius());
}

int BrushTool::clampStrength(int strength)
{
    return std::clamp(strength, 1, 100);
}

bool BrushTool::effectSupported(BrushEffect effect)
{
    switch (effect) {
    case BrushEffect::None:
    case BrushEffect::ChangeHeight:
        return true;
    // The remaining effects are routed by T3.2–T3.5.
    default:
        return false;
    }
}

void BrushTool::setParams(const BrushParams& params)
{
    params_ = clampedParams(params);
}

int BrushTool::changeHeightDelta(const BrushParams& params, unsigned char influence)
{
    if (params.direction == 0 || influence == 0 || !effectSupported(params.effect)) {
        return 0;
    }
    // Full delta = strength in 1/32 m units (VX_FRACTION), scaled by the mask
    // influence; round half away from zero.
    const long long full = static_cast<long long>(clampStrength(params.strength)) << VX_FRACTION;
    const long long magnitude = (full * influence + 127) / 255;
    return params.direction < 0 ? -static_cast<int>(magnitude)
                                : static_cast<int>(magnitude);
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
    BrushMask mask = BrushMasks::byShape(clamped.shape, clamped.radius);
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
    const int stampRadius = clampRadius(clamped.radius);
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
        return false; // routing comes with T3.2–T3.5
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
    session_.recordUndoArea(bx, by, bxEnd - bx, byEnd - by);

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
    vMap.regRender(x0, y0, x1, y1);

    return true;
}

void BrushTool::applyEffectToCell(const BrushParams& params, int x, int y, unsigned char influence)
{
    switch (params.effect) {
    case BrushEffect::ChangeHeight: {
        // Terra's own change-height toolzer: hardness scaling, At_* attributes
        // and the geo/dam layer split (VxGBuf/VxDBuf/SurBuf consistency)
        // happen inside tVoxSet (tools.h:488-674).
        static individualToolzer<T2TE_CHANGING_TERRAIN_HEIGHT> heightToolzer;
        heightToolzer.tVoxSet(y * static_cast<int>(vMap.H_SIZE) + x,
                              changeHeightDelta(params, influence));
        break;
    }
    // T3.2–T3.5 route further toolzers here; applyMask() refuses them until
    // then (effectSupported), so these cases cannot be reached yet.
    default:
        break;
    }
}

} // namespace MapEditor

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
    clamped.heightLevel = BrushTool::clampHeightLevel(clamped.heightLevel);
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

int BrushTool::clampHeightLevel(int level)
{
    return std::clamp(level, 0, MAX_VX_HEIGHT_WHOLE);
}

bool BrushTool::effectSupported(BrushEffect effect)
{
    switch (effect) {
    case BrushEffect::None:
    case BrushEffect::ChangeHeight: // T3.2 raise/lower
    case BrushEffect::Align4ZP:     // T3.4: level to zeroplast + mark it
    case BrushEffect::AlignVariableH: // T3.2 smooth/level to height
    case BrushEffect::SetColor:     // T3.3 paint SurBuf with palette index
    case BrushEffect::PickColor:    // T4.2 eyedropper: sample SurBuf pixel
    case BrushEffect::EraseZeroplast:  // T3.4 remove zeroplast flag
    case BrushEffect::PlotSoot:        // T3.4 mark soot
    case BrushEffect::VeryLightDam:    // T3.4 very light dam
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

int BrushTool::heightDeltaMagnitude(const BrushParams& params, unsigned char influence)
{
    if (influence == 0) {
        return 0;
    }
    // Full delta = strength in 1/32 m units (VX_FRACTION), scaled by the mask
    // influence; round half away from zero.
    const long long full = static_cast<long long>(clampStrength(params.strength)) << VX_FRACTION;
    const long long magnitude = (full * influence + 127) / 255;
    return static_cast<int>(magnitude);
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
    // T4.2: picking is a point operation — stroking a "line" of samples makes
    // no sense, so the Line shape refuses the PickColor effect.
    if (clamped.effect == BrushEffect::PickColor) {
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
    // The trail-aware wrapper (see EditorSession::regRenderWithShadowTrail):
    // the newly created shadow west of a raised/lowered stamp is rendered by
    // RenderStr beyond the dirty rect, so the rect grows over the shadow
    // trail and the viewport cache repaints those cells in the same frame.
    session_.regRenderWithShadowTrail(x0, y0, x1, y1);

    return true;
}

// ---------------------------------------------------------------------------
// T3.2 — BrushMode layer helpers for the height tools.
//
// Terra's change-height toolzer always edits the active (top) layer: the dam
// when VxDBuf != 0, the geo otherwise. The editor's Geo/Dam modes cut across
// that, so the layer selection happens around the Terra toolzer:
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
    static individualToolzer<T2TE_CHANGING_TERRAIN_HEIGHT> heightToolzer;
    heightToolzer.tVoxSet(off, dV);
}

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
        // at the target, mark At_LEVELED when the target is reached and skip
        // At_ZEROPLAST cells — the game behaviour (tools.h:545-631). The dam
        // layer is driven by the toolzer as well: raising with a dam present
        // edits VxDBuf, dropping below the geo height removes the dam.
        // The target height is synced before every cell — a static toolzer
        // would otherwise carry the hZeroPlast of a previously loaded world.
        const bool damCell = vMap.VxDBuf[off] != 0;
        const int whole = params.effect == BrushEffect::Align4ZP
            ? static_cast<int>(vMap.hZeroPlast)
            : clampHeightLevel(params.heightLevel);
        const int target = whole << VX_FRACTION;
        // Direction per cell: towards the target. The magnitude is the same
        // strength-scaled per-stamp delta ChangeHeight uses; the toolzer caps
        // the result at the target on both sides.
        const int magnitude = heightDeltaMagnitude(params, influence);
        const int dV = vMap.SGetAlt(off) < target ? magnitude : -magnitude;
        if (params.effect == BrushEffect::Align4ZP &&
            (vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST) {
            break; // already zeroplast — the alignment toolzer skips it too
        }
        if (params.mode == BrushMode::Geo && damCell) {
            // Geo mode owns VxGBuf only: Terra's toolzer would edit the top
            // (dam) layer, so the geo under the dam is aligned here instead
            // — capped at the dam top, the dam itself stays untouched.
            geoAlignUnderDam(off, target, dV);
            if (params.effect == BrushEffect::Align4ZP) {
                // Mark only when the cell's top surface sits exactly at the
                // zeroplast height (the toolzer checks that itself).
                static individualToolzer<T2TE_PLOTTING_ZEROPLAST> plotZPToolzer;
                plotZPToolzer.setHAppoximation(whole);
                plotZPToolzer.tVoxSet(off, 1);
            }
            break;
        }
        if (params.effect == BrushEffect::Align4ZP) {
            // The former PlotZeroplast Dam-mode behaviour: seed the dam at
            // the geo height first so the toolzer drives the dam layer (the
            // zeroplast belongs to the dam domain — the UI offers this brush
            // in Dam mode only).
            if (params.mode == BrushMode::Dam) {
                seedDamLayer(off);
            }
            static individualToolzer<T2TE_ALIGNMENT_TERRAIN_4ZP> alignZPToolzer;
            alignZPToolzer.setHAppoximation(whole);
            alignZPToolzer.tVoxSet(off, dV);
            // The zeroplast marking of the former PlotZeroplast brush: the
            // merged brush levels to hZeroPlast and marks every cell that
            // sits at that height (Terra's own plotCircleZL behaviour).
            static individualToolzer<T2TE_PLOTTING_ZEROPLAST> plotZPToolzer;
            plotZPToolzer.setHAppoximation(whole);
            plotZPToolzer.tVoxSet(off, 1);
        } else {
            static individualToolzer<T2TE_ALIGNMENT_TERRAIN_VARIABLE_H> alignHToolzer;
            alignHToolzer.setHAppoximation(whole);
            alignHToolzer.tVoxSet(off, dV);
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
        break;
    }
    case BrushEffect::EraseZeroplast: {
        // T3.4 — eraseCircleZL analogue: remove zeroplast flag via the
        // T2TE_ERASE_ZEROPLAST toolzer (clears At_NOTPURESURFACE).
        {
            static individualToolzer<T2TE_ERASE_ZEROPLAST> eraseZPToolzer;
            eraseZPToolzer.tVoxSet(off, 1);
        }
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

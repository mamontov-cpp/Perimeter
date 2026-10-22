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
    case BrushEffect::Align4ZP:     // T3.2 level to zeroplast
    case BrushEffect::AlignVariableH: // T3.2 smooth/level to height
    case BrushEffect::SetColor:     // T3.3 paint SurBuf with palette index
    case BrushEffect::PickColor:    // T4.2 eyedropper: sample SurBuf pixel
    case BrushEffect::PlotZeroplast:   // T3.4 level + mark zeroplast
    case BrushEffect::EraseZeroplast:  // T3.4 remove zeroplast flag
    case BrushEffect::PlotSoot:        // T3.4 mark soot
    case BrushEffect::VeryLightDam:    // T3.4 very light dam
    case BrushEffect::ImageBrush:      // T3.5 image-driven terrain edit
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
    // T3.5: image brush uses the loaded image mask as the stamp geometry.
    if (clamped.effect == BrushEffect::ImageBrush) {
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
    vMap.regRender(x0, y0, x1, y1);

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

/// ChangeHeight in Geo mode: VxGBuf only, dam-capped raise.
void geoHeightChange(int off, int dV)
{
    dV = applyHardness(off, dV);
    const unsigned char fraction = vMap.AtrBuf[off] & VX_FRACTION_MASK;
    const int v = (static_cast<int>(vMap.VxGBuf[off]) << VX_FRACTION) +
                  fraction + dV;
    const int ceiling = vMap.VxDBuf[off] != 0
        ? (static_cast<int>(vMap.VxDBuf[off]) << VX_FRACTION) + fraction
        : MAX_VX_HEIGHT;
    const int clamped = std::clamp(v, MIN_VX_HEIGHT, ceiling);
    vMap.SurBuf[off] = vMap.GetGeoType(off, clamped);
    vMap.VxGBuf[off] = static_cast<unsigned char>(clamped >> VX_FRACTION);
    vMap.AtrBuf[off] = static_cast<unsigned char>(clamped & VX_FRACTION_MASK);
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
        // Seed the dam exactly like T2TE_PLOTTING_VERY_LIGHT_DAM:
        // dam height = geo height, surface = very light dam index.
        // A zero-height geo cannot carry a dam (VxDBuf == 0 means "no dam"),
        // so the layer starts one voxel up and the delta shrinks by the
        // same bump — the net surface change stays exactly `dV`.
        const int seed = vMap.VxGBuf[off] ? vMap.VxGBuf[off] : 1;
        if (seed != vMap.VxGBuf[off]) {
            if (dV < (1 << VX_FRACTION)) {
                // A sub-voxel raise cannot create a whole-voxel dam layer.
                return;
            }
            dV -= 1 << VX_FRACTION; // compensate the seed bump exactly
        }
        vMap.VxDBuf[off] = static_cast<unsigned char>(seed);
        vMap.SurBuf[off] = vMap.veryLightDam;
    }
    static individualToolzer<T2TE_CHANGING_TERRAIN_HEIGHT> heightToolzer;
    heightToolzer.tVoxSet(off, dV);
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
        if (params.mode == BrushMode::Geo && vMap.VxDBuf[off]) {
            break; // Geo mode drives VxGBuf only — dam cells stay untouched
        }
        const int whole = params.effect == BrushEffect::Align4ZP
            ? static_cast<int>(vMap.hZeroPlast)
            : clampHeightLevel(params.heightLevel);
        const int target = whole << VX_FRACTION;
        // Direction per cell: towards the target. The magnitude is the same
        // strength-scaled per-stamp delta ChangeHeight uses; the toolzer caps
        // the result at the target on both sides.
        const int magnitude = heightDeltaMagnitude(params, influence);
        const int dV = vMap.SGetAlt(off) < target ? magnitude : -magnitude;
        if (params.effect == BrushEffect::Align4ZP) {
            static individualToolzer<T2TE_ALIGNMENT_TERRAIN_4ZP> alignZPToolzer;
            alignZPToolzer.setHAppoximation(whole);
            alignZPToolzer.tVoxSet(off, dV);
        } else {
            static individualToolzer<T2TE_ALIGNMENT_TERRAIN_VARIABLE_H> alignHToolzer;
            alignHToolzer.setHAppoximation(whole);
            alignHToolzer.tVoxSet(off, dV);
        }
        break;
    }
    case BrushEffect::SetColor: {
        // T3.3 — paint the surface buffer with the selected palette index
        // without touching the height buffers (VxGBuf, VxDBuf, AtrBuf).
        // The caller validates that colorIndex is within [0, 255]; the game
        // uses 0..127 for geo surface and 128..255 for dam surface.
        vMap.SetTer(x, y, static_cast<unsigned char>(params.colorIndex));
        break;
    }
    case BrushEffect::PlotZeroplast: {
        // T3.4 — flashCircleLeveling + plotCircleZL:
        //   1. set height to hZeroPlast (Dam mode: seed dam at geo height first,
        //      then level the dam to hZeroPlast, like the game's leveller);
        //   2. mark At_ZEROPLAST when the cell reached the target altitude.
        if (params.mode == BrushMode::Dam && vMap.VxDBuf[off] == 0) {
            // Seed the dam at the geo height so that the toolzer can drive it.
            const int seed = vMap.VxGBuf[off] ? vMap.VxGBuf[off] : 1;
            vMap.VxDBuf[off] = static_cast<unsigned char>(seed);
            vMap.SurBuf[off] = vMap.veryLightDam;
        }
        {
            static individualToolzer<T2TE_ALIGNMENT_TERRAIN_4ZP> alignZPToolzer;
            alignZPToolzer.setHAppoximation(static_cast<int>(vMap.hZeroPlast));
            const int dV = 1; // non-zero — the toolzer caps at hZeroPlast
            alignZPToolzer.tVoxSet(off, dV);
        }
        {
            static individualToolzer<T2TE_PLOTTING_ZEROPLAST> plotZPToolzer;
            plotZPToolzer.setHAppoximation(static_cast<int>(vMap.hZeroPlast));
            plotZPToolzer.tVoxSet(off, 1);
        }
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
    case BrushEffect::ImageBrush: {
        // T3.5 — image-driven terrain edit: the image pixel value (0..255)
        // acts as per-cell influence; the delta magnitude is strength scaled
        // by that influence (same formula ChangeHeight uses). The BrushMode
        // Geo/Dam layer selection applies identically.
        const int dV = changeHeightDelta(params, influence);
        if (params.mode == BrushMode::Geo) {
            geoHeightChange(off, dV);
        } else {
            damHeightChange(off, dV);
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

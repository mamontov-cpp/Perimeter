#pragma once

#include "EditorSession.h"
#include "ImageDecal.h"

#include <cstdint>
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
enum class BrushShape
{
    FilledCircle,   ///< whole disc, rings 0..radius (game `influenceDZ` shape).
    FilledSquare,   ///< solid square, |dx|<=radius and |dy|<=radius.
    CircleOutline,  ///< one-cell ring at radius (Terra's own `maxRad[r]` ring).
    SquareOutline,  ///< one-cell square border.
    Line,           ///< Bresenham line, circle stamp; endpoints via applyLine().
};

/// Which terrain layer the brush addresses. Geo = the layer under a dam,
/// Dam = the deployed dam layer (higher priority than Geo in the game).
enum class BrushMode
{
    Geo,
    Dam,
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
                    ///< T2TE_PLOTTING_ZEROPLAST per cell).
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
    EraseZeroplast, ///< T3.4: T2TE_ERASE_ZEROPLAST.
    PlotSoot,       ///< T3.4: T2TE_PLOTTING_SOOT.
    VeryLightDam,   ///< T3.4: T2TE_PLOTTING_VERY_LIGHT_DAM.
};

/// Full brush configuration (plan §5 T3.1: shape/radius/strength/mode/
/// colorIndex/effect) + the parameters of the height tools (T3.2).
struct BrushParams
{
    BrushShape shape = BrushShape::FilledCircle;
    int radius = 8;      ///< stamp half-size in map cells (clamped to the Terra radius tables).
    int strength = 30;   ///< 1..100: per-stamp delta in 1/32 m units *at full influence*.
    BrushMode mode = BrushMode::Geo;   ///< Geo/Dam target layer (rules per effect, T3.2).
    int colorIndex = 0;                ///< palette index for SetColor (0..255), geosurface<128, dam>=128.
    BrushEffect effect = BrushEffect::ChangeHeight;

    /// Height effect direction: +1 raise, -1 lower, 0 — no height change.
    /// Only the ChangeHeight effect uses it; the alignment effects take the
    /// direction per cell (towards the target level).
    int direction = 1;

    /// AlignVariableH target height in whole voxels (0..255, clamped; 1 voxel
    /// = 1/32 m). Level-to-zeroplast (Align4ZP) reads vMap.hZeroPlast instead.
    int heightLevel = 32;

    /// T3.5 (reworked): when true, the loaded image mask (BrushTool::
    /// setImageMask) replaces the shape/radius stamp as the brush geometry
    /// for every terrain effect — the image pixel value is the per-cell
    /// influence. Selecting a shape (FilledCircle..Line) clears the flag, so
    /// the mask source only changes when the user explicitly picks a shape
    /// or loads a new image.
    bool useImageMask = false;

    bool operator==(const BrushParams& other) const
    {
        return shape == other.shape && radius == other.radius &&
               strength == other.strength && mode == other.mode &&
               colorIndex == other.colorIndex && effect == other.effect &&
               direction == other.direction &&
               heightLevel == other.heightLevel &&
               useImageMask == other.useImageMask;
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
class BrushMasks
{
public:
    /// Whole disc ring 0..radius — the shape Terra's influenceDZ affects.
    static BrushMask filledCircle(int radius);
    /// One-cell ring exactly at radius — Terra's own maxRad[r] offsets.
    static BrushMask circleOutline(int radius);
    /// Solid square |dx|<=radius, |dy|<=radius.
    static BrushMask filledSquare(int radius);
    /// Square border max(|dx|,|dy|)==radius.
    static BrushMask squareOutline(int radius);
    /// Mask builder for a BrushShape (Line has no stamps without endpoints —
    /// use stampLine(); returns an empty mask there).
    static BrushMask byShape(BrushShape shape, int radius);

    /// The Line shape: Bresenham from (x0,y0) to (x1,y1), union of
    /// filled-circle stamps of the given radius. Each map cell is counted
    /// exactly once (stamp overlaps do not accumulate the effect within one
    /// line); the influence over a cell is thus one stamp's delta, not a sum.
    static BrushMask lineStamp(int x0, int y0, int x1, int y1, int stampRadius);

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

    /// Max possible stamp radius — Terra's radius tables length.
    static int maxRadius();
    /// Radius clamped into [0, maxRadius()].
    static int clampRadius(int radius);
    /// Strength clamped into [1,100] (percent of the full per-stamp delta).
    static int clampStrength(int strength);
    /// Alignment target height clamped into [0, MAX_VX_HEIGHT_WHOLE] whole
    /// voxels — the AlignVariableH target domain (the UI spinbox uses it too).
    static int clampHeightLevel(int level);
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
    /// The last index pickColorAt() sampled (valid after hasPickedColor()).
    unsigned char lastPickedColorIndex() const { return lastPickedColorIndex_; }
    /// Whether pickColorAt() has already sampled at least one pixel.
    bool hasPickedColor() const { return hasPickedColor_; }

    /// The Line shape: apply the stamped mask along a Bresenham path —
    /// one call, one undo entry, cells counted once (see BrushMasks::lineStamp).
    /// Returns true when the world changed.
    bool applyLine(int x0, int y0, int x1, int y1);
    bool applyLine(int x0, int y0, int x1, int y1, const BrushParams& params);

    /// Apply an absolute (already positioned) mask. Entry point for later
    /// tools with their own geometry (brush-image T3.5); the public
    /// applyStamp/applyLine funnels across here too.
    bool applyMask(const BrushMask& absoluteMask, const BrushParams& params);

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

    EditorSession& session_;
    BrushParams params_;

    /// T3.5: brush-image mask loaded from file (pixel values → influence 0..255).
    BrushMask imageMask_;

    /// ImageDecal: the palette-index image the effect stamps, plus the
    /// absolute map origin of the stamp currently being applied (the effect
    /// handler reads the pixel from (x - decalOriginX_, y - decalOriginY_)).
    ImageDecal::DecalImage decal_;
    int decalOriginX_ = 0;
    int decalOriginY_ = 0;

    /// T4.2: the last index pickColorAt() sampled (SurBuf at the click cell).
    unsigned char lastPickedColorIndex_ = 0;
    bool hasPickedColor_ = false;
};

} // namespace MapEditor

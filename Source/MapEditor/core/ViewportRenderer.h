#pragma once

#include <cstdint>
#include <vector>

#include "ColorMipmap.h"

// ============================================================================
// MapEditorCore — ViewportRenderer (T2.1).
//
// Software rasteriser for the 2D top-down 1:1 viewport:
//
//   * Owns an ARGB32 pixel cache (0xFFRRGGBB, row-major, H_SIZE x V_SIZE).
//     The layout matches QImage::Format_ARGB32, so the Qt widget (T2.2) can
//     wrap the buffer zero-copy instead of keeping a second copy in QImage.
//     MapEditorCore stays Qt-free and headless-testable this way — this is a
//     deliberate refinement of plan §3.4 ("кэш в QImage").
//   * Base colors come from vrtMap::getColor32(), which already resolves
//     At_LEVELED / At_ZEROPLAST through Tex2Col32 / SurZP2Col32.
//   * Dirty rectangles: regRender() pushes boxes into vMap.changedAreas and
//     vMap.renderAreas; update() runs vMap.renderQuant() (lighting recompute),
//     repaints the dirty boxes and clears vMap.changedAreas — the same
//     consume-and-clear contract terUniverse::quant() keeps for the game.
//   * Layer filters: Composite (exact getColor32), Geo (geo-palette half of
//     the color tables — the layer under a dam), Dam (dam cells highlighted).
//   * Overlays: GABuf flags — zeroplast, leveled, non-diggable hardness.
//
// Precondition for bindWorld(): a world must be fully loaded AND lit, i.e.
// EditorSession::loadWorld() (fullLoad + WorldRender). Raw Terra loads must
// call vMap.WorldRender() themselves, otherwise RnrBuf holds garbage.
// ============================================================================

namespace MapEditor {

/// Which terrain layer the base raster shows.
enum class LayerFilter
{
    Composite,  ///< Exactly vrtMap::getColor32() (geo + dam + leveled/zeroplast).
    Geo,        ///< Geo palette everywhere (dam cells show the geo layer under it).
    Dam,        ///< Dam cells as-is; cells without a dam are dimmed to 25%.
};

/// Overlay flags (bitmask) — GABuf cell markers painted over the base raster.
enum OverlayFlags : std::uint32_t
{
    OverlayNone        = 0,
    OverlayZeroplast   = 1u << 0, ///< GRIDAT_ZEROPLAST / GRIDAT_ZEROPLAST_IS_PRESENT.
    OverlayLeveled     = 1u << 1, ///< GRIDAT_LEVELED.
    OverlayNonDiggable = 1u << 2, ///< GRIDAT_MASK_HARDNESS < 3 (0 = undiggable).
};

/// Map-space rectangle in cell coordinates (x/y/w/h form — unlike sRectS,
/// whose dx/dy are inclusive extents).
struct DirtyRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool operator==(const DirtyRect& other) const
    {
        return x == other.x && y == other.y && w == other.w && h == other.h;
    }
};

class ViewportRenderer
{
public:
    /// Overlay tints and alpha levels. Part of the rendering contract:
    /// tests recompute the expected pixels from these constants.
    static constexpr std::uint32_t kZeroplastTint = 0x00E040E0; ///< magenta
    static constexpr std::uint32_t kLeveledTint   = 0x0040C040; ///< green
    static constexpr std::uint32_t kHardTint      = 0x00E02020; ///< red
    static constexpr int kZeroplastAlphaFull    = 96;  ///< all 16 cells zeroplast
    static constexpr int kZeroplastAlphaPartial = 40;  ///< GRIDAT_ZEROPLAST_IS_PRESENT only
    static constexpr int kLeveledAlpha = 72;
    /// Indexed by GRIDAT_MASK_HARDNESS value 0..3; 3 = diggable = no tint.
    static constexpr int kHardAlpha[4] = {96, 64, 32, 0};

    static constexpr std::uint32_t kOpaque = 0xFF000000u;

    /// Alpha-blend a tint over a base pixel: per channel
    /// (base*(256-a) + tint*a) >> 8, alpha forced to opaque.
    static constexpr std::uint32_t blendTint(std::uint32_t base,
                                             std::uint32_t tint,
                                             int alpha)
    {
        const unsigned a =
            static_cast<unsigned>(alpha < 0 ? 0 : (alpha > 256 ? 256 : alpha));
        const unsigned inv = 256u - a;
        const unsigned b =
            (((base & 0xFFu) * inv + (tint & 0xFFu) * a) >> 8) & 0xFFu;
        const unsigned g =
            ((((base >> 8) & 0xFFu) * inv + ((tint >> 8) & 0xFFu) * a) >> 8) & 0xFFu;
        const unsigned r =
            ((((base >> 16) & 0xFFu) * inv + ((tint >> 16) & 0xFFu) * a) >> 8) & 0xFFu;
        return kOpaque | (r << 16) | (g << 8) | b;
    }

    /// Quarter-brightness version of a pixel (used by the Dam filter for
    /// cells that have no dam). Per-channel shift with a carry-safe mask.
    static constexpr std::uint32_t dimColor(std::uint32_t col)
    {
        return kOpaque | ((col >> 2) & 0x003F3F3Fu);
    }

    ViewportRenderer() = default;

    /// Attach to the currently loaded world: validates the Terra buffers,
    /// allocates the cache and paints the full map. Returns false when no
    /// world is loaded (state stays unbound).
    bool bindWorld();
    /// Detach and free the cache. Layer settings are kept.
    void unbindWorld();
    bool isBound() const;

    /// The update pipeline; safe to call every frame:
    ///   1. vMap.renderQuant() — recompute lighting (RnrBuf/At_SHADOW) for
    ///      regions marked via regRender(); clears vMap.renderAreas;
    ///   2. collect dirty rectangles — a full-map rect after bind/settings
    ///      changes, otherwise the boxes in vMap.changedAreas; clears
    ///      vMap.changedAreas;
    ///   3. repaint them into the cache.
    /// Returns the repainted rectangles (clipped to the map); empty when
    /// nothing was dirty. Unbinds itself if the world was unloaded/replaced.
    std::vector<DirtyRect> update();

    /// Mark the whole map dirty (repainted by the next update()).
    void invalidateAll();

    /// Repaint a single rectangle (clipped) into the cache immediately.
    void renderRect(const DirtyRect& rect);

    /// Change the layer filter; returns true if it changed (which marks the
    /// whole map dirty).
    bool setLayerFilter(LayerFilter filter);
    LayerFilter layerFilter() const { return filter_; }

    /// The in-game look toggle: cells without a dam render as darkened
    /// shades of grey (the game dims the un-terraformed geo surface; the
    /// dam areas keep their palette colors). Viewport-only — cache.tga /
    /// map.tga generation is unaffected. Returns true when the state (and
    /// thus the whole map + mip pyramid) changed.
    bool setGeoDimmed(bool dimmed);
    bool isGeoDimmed() const { return geoDimmed_; }

    /// Change the overlay set; returns true if it changed (whole map dirty).
    bool setOverlays(std::uint32_t overlays);
    std::uint32_t overlays() const { return overlays_; }

    /// ARGB32 cache, row-major, width*height entries (Format_ARGB32 layout).
    const std::uint32_t* pixels() const { return cache_.empty() ? nullptr : cache_.data(); }
    std::uint32_t* pixels() { return cache_.empty() ? nullptr : cache_.data(); }
    int width() const { return width_; }
    int height() const { return height_; }

    /// Base-layer pixel at a map cell (layer filter applied, overlay NOT
    /// applied), opaque 0xFFRRGGBB; kOpaque when unbound/out of range.
    /// Handy for tests and for the eyedropper.
    std::uint32_t colorAt(int x, int y) const;

    /// Composite color of a single cell (0x00RRGGBB) WITHOUT a bound cache:
    /// exactly what the LayerFilter::Composite branch of colorAt() returns for
    /// the cell, i.e. the pixel WorldImages::renderToBgr()/cache.tga/map.tga
    /// store for it. Static on purpose — the minimap panel re-samples single
    /// cells of its own downscaled raster after every terrain edit and must not
    /// bind a second full-size cache (bindWorld() drains vMap.changedAreas,
    /// which would starve the viewport's repaint). Returns false when no world
    /// is loaded or the cell is outside the map.
    static bool compositeColorAt(int x, int y, std::uint32_t &rgb);

    /// --- Color/height mip pyramid (post-T3.3, smooth 3D view) -----------
    ///
    /// Build the pyramid lazily: ViewportWidget only pays the memory and
    /// build cost when the Smooth render style turns on the 3D view (see
    /// View3DRenderer::renderSmooth). No-op when already current; also
    /// dropped/rebuilt automatically when the world is unbound or replaced.
    /// const + mutable state on purpose — the smooth renderer reads the
    /// pyramid through a const source pointer and populates it on demand.
    void ensureMips() const;
    bool hasMips() const { return mipsBuilt_; }
    /// The pyramid itself (valid only when hasMips() is true). Level 0 holds
    /// the pure composite base colors (vrtMap::getColor32, no overlays, no
    /// layer filtering) and the effective voxel heights (VxD else VxG) —
    /// exactly the inputs vrtMap::viewV2D point-samples.
    const ColorMipmap &colorMipmap() const { return mips_; }

    /// --- Geo-only pyramid (dam-frozen 3D scroll) -------------------------
    ///
    /// A second pyramid holding the geo layer only: colors resolved through
    /// the geo half of the color tables (the palette under any dam) and the
    /// geo voxel heights (VxGBuf, dam ignored). The "freeze dam" 3D mode
    /// samples it for ground points outside the one real map copy, so the
    /// world repeats with geo only while the dam stays in its single
    /// original place (docs/architecture.md).
    void ensureGeoMips() const;
    bool hasGeoMips() const { return geoMipsBuilt_; }
    const ColorMipmap &geoMipmap() const { return geoMips_; }

private:
    /// True while vMap still holds the same, fully loaded world the cache
    /// was built for.
    bool worldAlive() const;
    /// Paint GABuf-derived overlays over a base pixel.
    std::uint32_t applyOverlay(std::uint32_t col, int x, int y) const;
    /// Refresh the pyramid base cells + upper levels for a rect (used by
    /// renderRect/update when the pyramid has been built).
    void refreshMipRegion(int x0, int y0, int x1, int y1) const;
    /// Composite base color + effective height of a cell — the mip source.
    /// Kept next to colorAt(): same inputs, just without the layer filters.
    std::uint32_t mipBaseColor(int x, int y) const;
    static unsigned char mipEffectiveHeight(int x, int y);

    std::vector<std::uint32_t> cache_;
    int width_ = 0;
    int height_ = 0;
    bool fullInvalid_ = false;
    LayerFilter filter_ = LayerFilter::Composite;
    std::uint32_t overlays_ = OverlayNone;
    bool geoDimmed_ = false;
    mutable ColorMipmap mips_;     ///< Built on demand (ensureMips), see above.
    mutable bool mipsBuilt_ = false;
    mutable ColorMipmap geoMips_;  ///< Geo-only pyramid (ensureGeoMips).
    mutable bool geoMipsBuilt_ = false;
};

} // namespace MapEditor

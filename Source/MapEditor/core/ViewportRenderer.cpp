// ============================================================================
// MapEditorCore — ViewportRenderer implementation (T2.1).
// ============================================================================

#include "ViewportRenderer.h"

// Terra / XTool headers — needed for vMap and the color tables.
// _PERIMETER_ and _TX3D_LIBRARY_ are defined by CMake.
#include "terra.h"

#include <algorithm>
#include <cstdio>

namespace MapEditor {

namespace {

/// All buffers the raster needs. VxG/VxD/Sur/Atr/Rnr/GA are allocated
/// together by mem4BufAlloc(), LvdTex comes from loadLeveledTexture(); the
/// explicit checks mirror the guards WorldImages::renderToBgr() relies on.
bool worldBuffersPresent()
{
    return vMap.VxGBuf != nullptr && vMap.VxDBuf != nullptr &&
           vMap.AtrBuf != nullptr && vMap.SurBuf != nullptr &&
           vMap.RnrBuf != nullptr && vMap.GABuf != nullptr &&
           vMap.LvdTex != nullptr && vMap.H_SIZE > 0 && vMap.V_SIZE > 0;
}

/// vrtMap::getColor32() (VMAP.h:648) with an explicit lighting index instead
/// of reading RnrBuf directly. Same branches — At_ZEROPLAST via
/// SurZP2Col32, At_LEVELED via the Tex2Col32/Sur2Col32 blend, plain terrain
/// via Sur2Col32 — so the Geo filter can force the geo-palette half of the
/// tables (Rnr & 0x7F) and show the layer that lies under a dam.
unsigned int color32WithLighting(int off, unsigned int lighting)
{
    const unsigned int atr = vMap.AtrBuf[off];
    if (atr & At_LEVELED) {
        if ((atr & At_ZPMASK) == At_ZEROPLAST) {
            return vMap.SurZP2Col32[lighting][vMap.SurBuf[off]];
        }
        unsigned int texOff = off & vMap.LvdTex_clip_mask_x;
        texOff |= (off >> vMap.LvdTex_SPEC_SHIFT_X) & vMap.LvdTex_spec_clip_mask_y;
        const unsigned int mask = 0x00FEFEFF;
        const unsigned int tex = vMap.Tex2Col32[lighting][vMap.LvdTex[texOff]];
        return ((tex & mask) + (vMap.Sur2Col32[vMap.SurBuf[off]][lighting] & mask)) >> 1;
    }
    return vMap.Sur2Col32[vMap.SurBuf[off]][lighting];
}

} // namespace

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

bool ViewportRenderer::bindWorld()
{
    unbindWorld();

    if (!worldBuffersPresent()) {
        std::fprintf(stderr,
                     "ViewportRenderer: no fully loaded world to bind "
                     "(call EditorSession::loadWorld first).\n");
        return false;
    }

    width_ = static_cast<int>(vMap.H_SIZE);
    height_ = static_cast<int>(vMap.V_SIZE);
    cache_.assign(static_cast<std::size_t>(width_) * height_, kOpaque);

    // Paint everything right away so pixels() is valid after bind; update()
    // also runs renderQuant() and drains any stale dirty areas left by the
    // load.
    fullInvalid_ = true;
    update();
    return true;
}

void ViewportRenderer::unbindWorld()
{
    cache_.clear();
    width_ = 0;
    height_ = 0;
    fullInvalid_ = false;
}

bool ViewportRenderer::isBound() const
{
    return !cache_.empty() && width_ > 0 && height_ > 0;
}

bool ViewportRenderer::worldAlive() const
{
    return isBound() && worldBuffersPresent() &&
           static_cast<int>(vMap.H_SIZE) == width_ &&
           static_cast<int>(vMap.V_SIZE) == height_;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

bool ViewportRenderer::setLayerFilter(LayerFilter filter)
{
    if (filter_ == filter) {
        return false;
    }
    filter_ = filter;
    fullInvalid_ = true;
    return true;
}

bool ViewportRenderer::setOverlays(std::uint32_t overlays)
{
    if (overlays_ == overlays) {
        return false;
    }
    overlays_ = overlays;
    fullInvalid_ = true;
    return true;
}

void ViewportRenderer::invalidateAll()
{
    if (isBound()) {
        fullInvalid_ = true;
    }
}

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------

std::uint32_t ViewportRenderer::colorAt(int x, int y) const
{
    if (!worldAlive()) {
        return kOpaque;
    }
    x = std::min(std::max(x, 0), width_ - 1);
    y = std::min(std::max(y, 0), height_ - 1);
    const int off = vMap.offsetBuf(x, y);

    switch (filter_) {
    case LayerFilter::Geo:
        // The geo layer is the geo-palette half (Rnr bits 0..6) of the color
        // tables: Rnr's bit 7 marks a dam cell and switches rows 128..255 to
        // DamPal (convertPal2TableSurCol, VMAP.cpp:1322). SurBuf holds one
        // shared material index for both palettes — PutAltDam() never
        // rewrites it (SetTer is commented out, VMAP.h:864) — so clearing
        // bit 7 recovers the exact geo color even under a dam.
        return kOpaque | color32WithLighting(off, vMap.RnrBuf[off] & 0x7Fu);
    case LayerFilter::Dam: {
        const unsigned int col = vMap.getColor32(off);
        return vMap.VxDBuf[off] != 0 ? (kOpaque | col) : dimColor(col);
    }
    case LayerFilter::Composite:
    default:
        return kOpaque | vMap.getColor32(off);
    }
}

std::uint32_t ViewportRenderer::applyOverlay(std::uint32_t col, int x, int y) const
{
    if (overlays_ == OverlayNone) {
        return col;
    }

    const int offG = vMap.offsetBuf2offsetGBuf(vMap.offsetBuf(x, y));
    const unsigned short ga = vMap.GABuf[offG];

    // Zeroplast is more specific than "leveled" (initGrid sets both for a
    // fully-zeroplast 4x4 block), so a block showing zeroplast does not also
    // get the leveled tint.
    bool zeroplastShown = false;
    if (overlays_ & OverlayZeroplast) {
        if (ga & GRIDAT_ZEROPLAST) {
            col = blendTint(col, kZeroplastTint, kZeroplastAlphaFull);
            zeroplastShown = true;
        } else if (ga & GRIDAT_ZEROPLAST_IS_PRESENT) {
            col = blendTint(col, kZeroplastTint, kZeroplastAlphaPartial);
            zeroplastShown = true;
        }
    }
    if (!zeroplastShown && (overlays_ & OverlayLeveled) && (ga & GRIDAT_LEVELED)) {
        col = blendTint(col, kLeveledTint, kLeveledAlpha);
    }
    if (overlays_ & OverlayNonDiggable) {
        // 0 = undiggable (MissionEdit's "make hard", tVoxSet scales digging
        // by h<<h so 0 digs nothing); 3 = normal diggable terrain, alpha 0.
        const unsigned int hardness = ga & GRIDAT_MASK_HARDNESS;
        col = blendTint(col, kHardTint, kHardAlpha[hardness]);
    }
    return col;
}

// ---------------------------------------------------------------------------
// Repainting
// ---------------------------------------------------------------------------

void ViewportRenderer::renderRect(const DirtyRect& rect)
{
    if (!worldAlive() || rect.w <= 0 || rect.h <= 0) {
        return;
    }
    const int x0 = std::max(rect.x, 0);
    const int y0 = std::max(rect.y, 0);
    const int x1 = std::min(rect.x + rect.w, width_);
    const int y1 = std::min(rect.y + rect.h, height_);

    for (int y = y0; y < y1; ++y) {
        std::uint32_t* row = cache_.data() + static_cast<std::size_t>(y) * width_;
        for (int x = x0; x < x1; ++x) {
            row[x] = applyOverlay(colorAt(x, y), x, y);
        }
    }
}

std::vector<DirtyRect> ViewportRenderer::update()
{
    std::vector<DirtyRect> out;
    if (!worldAlive()) {
        // The world was unloaded or replaced underneath us (EditorSession
        // reloads into the same global vMap); drop the stale cache.
        if (isBound()) {
            unbindWorld();
        }
        return out;
    }

    // 1. Recompute lighting (RnrBuf + At_SHADOW) for regions marked with
    //    regRender(); no-op when renderAreas is empty, clears renderAreas.
    vMap.renderQuant();

    // 2. Collect dirty rectangles.
    if (fullInvalid_) {
        out.push_back({0, 0, width_, height_});
        fullInvalid_ = false;
        vMap.changedAreas.clear(); // covered by the full repaint
    } else {
        for (const sRectS& r : vMap.changedAreas) {
            // regRender()/renderBox() store in-bounds boxes (wrapping rects
            // are split) and sRectS dx/dy are inclusive extents; clip
            // defensively against the degenerate full-row/full-column boxes
            // renderBox() produces for x1==x2 / y1==y2 requests.
            DirtyRect d{r.x, r.y, static_cast<int>(r.dx) + 1,
                        static_cast<int>(r.dy) + 1};
            const int x0 = std::max(d.x, 0);
            const int y0 = std::max(d.y, 0);
            const int x1 = std::min(d.x + d.w, width_);
            const int y1 = std::min(d.y + d.h, height_);
            if (x1 > x0 && y1 > y0) {
                out.push_back({x0, y0, x1 - x0, y1 - y0});
            }
        }
        vMap.changedAreas.clear();
    }

    // 3. Repaint.
    for (const DirtyRect& r : out) {
        renderRect(r);
    }
    return out;
}

} // namespace MapEditor

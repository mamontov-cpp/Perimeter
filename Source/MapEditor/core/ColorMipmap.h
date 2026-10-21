#pragma once

#include <cstdint>
#include <vector>

// ============================================================================
// MapEditorCore — ColorMipmap (post-T3.3, smooth 3D view).
//
// Per-cell mip pyramid of the voxel surface raster. Why it exists:
// vrtMap::viewV2D (the oblique 3D view) samples ONE ground cell per screen
// column/row — fine when a cell spans several pixels, but near the horizon a
// single pixel covers dozens of ground cells, so distant structures (e.g. the
// dams at the map corners seen from steep cameras) alias into blocky speckle
// — the classic minification problem GPUs solve with mipmapping.
//
// The pyramid stores both channels the voxel renderer consumes:
//   * colors  — level 0 filled by ViewportRenderer from the same source the
//     chunky view and the 2D cache use (vrtMap::getColor32(), 0x00RRGGBB
//     channel order as returned — no alpha),
//   * effective voxel heights — VxDBuf cell where non-zero, else VxGBuf
//     (exactly the lvD/lvG choice viewV2D makes), 0..255.
// Each upper level halves the resolution and keeps the rounded 2x2 mean of
// the level below (edge cells average the children that exist).
//
// The smooth 3D renderer picks, per frustum row, the level whose kernel
// width (2^level cells) covers the ground footprint of one screen pixel and
// bilinearly samples the pyramid at 16.16 fixed-point original-cell
// coordinates — the same coordinate domain viewV2D walks. Level 0 with zero
// fractions degenerates to the plain nearest-cell value, so the near field
// keeps the familiar look while far field becomes filtered.
//
// Worlds are powers of two (analyzeINI guarantees H_SIZE/V_SIZE), but odd
// sizes are handled anyway: upper-level dims are ceil-halved and sampling
// wraps via modulo when a level dimension is not a power of two.
// ============================================================================

namespace MapEditor {

class ColorMipmap
{
public:
    ColorMipmap() = default;

    /// (Re)allocate the pyramid for a width x height base raster; the caller
    /// fills level 0 through setBaseCell() and then refreshes the upper
    /// levels with updateRegion(fullRect).
    void build(int width, int height);
    /// Free everything (also used before a rebuild after a world change).
    void clear();
    bool empty() const { return levels_.empty(); }

    int levelCount() const { return static_cast<int>(levels_.size()); }
    int levelWidth(int level) const { return levels_[level].width; }
    int levelHeight(int level) const { return levels_[level].height; }

    /// Base-level (level 0) write: color as returned by vrtMap::getColor32()
    /// (0x00RRGGBB channel order, no alpha) and the effective voxel height
    /// (VxD else VxG). Does NOT refresh the upper levels — batch that with
    /// updateRegion() over the touched rect (per-cell refresh would rewalk
    /// the whole pyramid chain for every pixel).
    void setBaseCell(int x, int y, std::uint32_t color, unsigned char effHeight);

    /// Recompute every level >= 1 cell whose 2x2 children intersect the base
    /// rect [x0..x1) x [y0..y1) (clipped to the base raster). Cheap for
    /// brush-sized rects; the whole base rect is also fine (full rebuild).
    void updateRegion(int x0, int y0, int x1, int y1);

    /// Bilinear wrap-sample the pyramid at (u, v) in 16.16 fixed-point
    /// ORIGINAL-cell coordinates. Negative coordinates wrap the same way
    /// vrtMap::XCYCL/YCYCL do. Colors come back in the 0x00RRGGBB channel
    /// order of level 0 (no alpha).
    std::uint32_t sampleColor(int level, int u, int v) const;
    /// Same sample for the effective voxel heights (0..255).
    unsigned char sampleHeight(int level, int u, int v) const;

    /// Direct cell access — tests and debugging.
    std::uint32_t colorCell(int level, int x, int y) const;
    unsigned char heightCell(int level, int x, int y) const;

private:
    struct Level
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint32_t> colors;    ///< 0x00RRGGBB channel order.
        std::vector<unsigned char> heights;   ///< 0..255 voxel heights.
    };

    const Level &level(int index) const { return levels_[index]; }

    std::vector<Level> levels_;
};

} // namespace MapEditor

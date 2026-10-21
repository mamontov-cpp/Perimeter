// ============================================================================
// MapEditorCore — ColorMipmap implementation (post-T3.3, smooth 3D view).
// ============================================================================

#include "ColorMipmap.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace MapEditor {

namespace {

/// Rounded mean with the exact integer arithmetic the pixel blending uses:
/// (sum + count/2) / count — same rounding family as blendTint() in
/// ViewportRenderer, so pyramid means and screen-space blends agree.
inline int roundedMean(int sum, int count)
{
    return (sum + count / 2) / count;
}

/// Per-channel blend of 4 colors with weights summing to 1 << 16.
/// Input channel order: 0x00RRGGBB (no alpha — matches getColor32()).
inline std::uint32_t blendColors4(const std::uint32_t *colors, const int *weights)
{
    unsigned b = 0, g = 0, r = 0;
    for (int i = 0; i < 4; ++i) {
        b += (colors[i] & 0xFFu) * static_cast<unsigned>(weights[i]);
        g += ((colors[i] >> 8) & 0xFFu) * static_cast<unsigned>(weights[i]);
        r += ((colors[i] >> 16) & 0xFFu) * static_cast<unsigned>(weights[i]);
    }
    return (b >> 16) | ((g >> 16) << 8) | ((r >> 16) << 16);
}

inline unsigned char blendHeights4(const unsigned char *heights, const int *weights)
{
    unsigned sum = 0;
    for (int i = 0; i < 4; ++i) {
        sum += static_cast<unsigned>(heights[i]) * static_cast<unsigned>(weights[i]);
    }
    return static_cast<unsigned char>(sum >> 16);
}

/// Bilinear corner indices + 8-bit fractions at a pyramid level, from a
/// 16.16 original-cell coordinate. Levels may halve an odd dimension away,
/// so the wrap uses a power-of-two mask when possible and modulo otherwise.
struct SamplePoint
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int fa = 0, fb = 0;   ///< 8-bit fractions inside the level cell (0..255).
};

SamplePoint makeSamplePoint(int u, int v, int level, const ColorMipmap &mip)
{
    const int levelW = mip.levelWidth(level);
    const int levelH = mip.levelHeight(level);
    const int maskX = (levelW & (levelW - 1)) == 0 ? levelW - 1 : 0;
    const int maskY = (levelH & (levelH - 1)) == 0 ? levelH - 1 : 0;

    // The coordinate needs to move into level-k space: divide the position
    // by 2^k (arithmetically, so the fraction travels along), keeping the
    // 16.16 fixed-point format. (u >> level) does both at once.
    const int uk = u >> level;
    const int vk = v >> level;

    SamplePoint p;
    int ix = uk >> 16;
    int iy = vk >> 16;
    if (maskX) {
        ix &= maskX;
        p.x1 = (ix + 1) & maskX;
    } else {
        ix = ix % levelW;
        if (ix < 0) ix += levelW;
        p.x1 = ix + 1 == levelW ? 0 : ix + 1;
    }
    if (maskY) {
        iy &= maskY;
        p.y1 = (iy + 1) & maskY;
    } else {
        iy = iy % levelH;
        if (iy < 0) iy += levelH;
        p.y1 = iy + 1 == levelH ? 0 : iy + 1;
    }
    p.x0 = ix;
    p.y0 = iy;
    p.fa = (uk >> 8) & 255;   ///< low 8 bits of the level-cell fraction.
    p.fb = (vk >> 8) & 255;
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

void ColorMipmap::build(int width, int height)
{
    clear();
    if (width <= 0 || height <= 0) {
        return;
    }
    int w = width;
    int h = height;
    while (true) {
        Level lv;
        lv.width = w;
        lv.height = h;
        lv.colors.assign(static_cast<std::size_t>(w) * h, 0u);
        lv.heights.assign(static_cast<std::size_t>(w) * h, 0u);
        levels_.push_back(std::move(lv));
        if (w == 1 && h == 1) {
            break;
        }
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
}

void ColorMipmap::clear()
{
    levels_.clear();
}

void ColorMipmap::setBaseCell(int x, int y, std::uint32_t color, unsigned char effHeight)
{
    assert(!levels_.empty());
    Level &base = levels_.front();
    assert(x >= 0 && x < base.width && y >= 0 && y < base.height);
    const std::size_t off = static_cast<std::size_t>(y) * base.width + x;
    base.colors[off] = color;
    base.heights[off] = effHeight;
}

void ColorMipmap::updateRegion(int x0, int y0, int x1, int y1)
{
    if (levels_.empty()) {
        return;
    }
    const Level &base = levels_.front();
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, base.width);
    y1 = std::min(y1, base.height);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    // Ascending levels: cell (cx, cy) at level k averages the children that
    // exist at level k-1. A level-k cell is touched exactly when its
    // children intersect the base rect — i.e. when it does itself.
    for (int k = 1; k < static_cast<int>(levels_.size()); ++k) {
        const Level &lo = levels_[k - 1];
        Level &up = levels_[k];
        const int cx0 = x0 >> k;
        const int cx1 = (x1 - 1) >> k;
        const int cy0 = y0 >> k;
        const int cy1 = (y1 - 1) >> k;
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                // Children live at level k-1: cell (cx, cy) at level k covers
                // the 2x2 block [(cx*2)..(cx*2+1)] x [(cy*2)..(cy*2+1)] there.
                const int childX = cx << 1;
                const int childY = cy << 1;
                int rSum = 0, gSum = 0, bSum = 0, hSum = 0;
                int count = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int chX = childX + dx;
                        const int chY = childY + dy;
                        if (chX >= lo.width || chY >= lo.height) {
                            continue;
                        }
                        const std::size_t chOff =
                            static_cast<std::size_t>(chY) * lo.width + chX;
                        const std::uint32_t c = lo.colors[chOff];
                        rSum += static_cast<int>((c >> 16) & 0xFFu);
                        gSum += static_cast<int>((c >> 8) & 0xFFu);
                        bSum += static_cast<int>(c & 0xFFu);
                        hSum += lo.heights[chOff];
                        ++count;
                    }
                }
                assert(count > 0);
                const std::size_t off =
                    static_cast<std::size_t>(cy) * up.width + cx;
                up.colors[off] = static_cast<std::uint32_t>(
                    (roundedMean(rSum, count) << 16) |
                    (roundedMean(gSum, count) << 8) |
                    roundedMean(bSum, count));
                up.heights[off] =
                    static_cast<unsigned char>(roundedMean(hSum, count));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

std::uint32_t ColorMipmap::sampleColor(int level, int u, int v) const
{
    const Level &lv = this->level(level);
    const SamplePoint p = makeSamplePoint(u, v, level, *this);

    // Weights of the four corners; wa+wb = 256 per axis so the total is
    // exactly 1 << 16.
    const int wa = 256 - p.fa;
    const int wb = 256 - p.fb;
    const int weights[4] = { wa * wb, p.fa * wb, wa * p.fb, p.fa * p.fb };

    const std::size_t w = static_cast<std::size_t>(lv.width);
    const std::size_t off00 = static_cast<std::size_t>(p.y0) * w + p.x0;
    const std::size_t off10 = static_cast<std::size_t>(p.y0) * w + p.x1;
    const std::size_t off01 = static_cast<std::size_t>(p.y1) * w + p.x0;
    const std::size_t off11 = static_cast<std::size_t>(p.y1) * w + p.x1;
    const std::uint32_t colors[4] = {
        lv.colors[off00], lv.colors[off10], lv.colors[off01], lv.colors[off11],
    };
    return blendColors4(colors, weights);
}

unsigned char ColorMipmap::sampleHeight(int level, int u, int v) const
{
    const Level &lv = this->level(level);
    const SamplePoint p = makeSamplePoint(u, v, level, *this);

    const int wa = 256 - p.fa;
    const int wb = 256 - p.fb;
    const int weights[4] = { wa * wb, p.fa * wb, wa * p.fb, p.fa * p.fb };

    const std::size_t w = static_cast<std::size_t>(lv.width);
    const std::size_t off00 = static_cast<std::size_t>(p.y0) * w + p.x0;
    const std::size_t off10 = static_cast<std::size_t>(p.y0) * w + p.x1;
    const std::size_t off01 = static_cast<std::size_t>(p.y1) * w + p.x0;
    const std::size_t off11 = static_cast<std::size_t>(p.y1) * w + p.x1;
    const unsigned char heights[4] = {
        lv.heights[off00], lv.heights[off10], lv.heights[off01], lv.heights[off11],
    };
    return blendHeights4(heights, weights);
}

std::uint32_t ColorMipmap::colorCell(int level, int x, int y) const
{
    const Level &lv = this->level(level);
    return lv.colors[static_cast<std::size_t>(y) * lv.width + x];
}

unsigned char ColorMipmap::heightCell(int level, int x, int y) const
{
    const Level &lv = this->level(level);
    return lv.heights[static_cast<std::size_t>(y) * lv.width + x];
}

} // namespace MapEditor

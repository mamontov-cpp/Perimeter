// ============================================================================
// MapEditorCore — View3DRenderer implementation (T2.3).
// ============================================================================

#include "View3DRenderer.h"

// Terra / XTool headers — vMap, the camera members and viewV2D itself.
// _PERIMETER_ and _TX3D_LIBRARY_ are defined by CMake.
#include "terra.h"

#include "ColorMipmap.h"
#include "ViewportRenderer.h"

#include <algorithm>
#include <cmath>

static_assert(MAX_XSIZE_VX_WINDOW == MapEditor::View3DRenderer::kMaxWindowSize,
              "kMaxWindowSize must track Terra's MAX_XSIZE_VX_WINDOW "
              "(VMAP.h:125) — viewV2D indexes its lasty/lastc arrays with it.");

namespace MapEditor {

// ---------------------------------------------------------------------------
// Clamps
// ---------------------------------------------------------------------------

int View3DRenderer::clampFocus(int focusC)
{
    return std::clamp(focusC, kMinFocusC, kMaxFocusC);
}

int View3DRenderer::clampCameraDZ(int cameraDZ)
{
    return std::clamp(cameraDZ,
                      static_cast<int>(MIN_CAMERADZ_VX_WINDOW),
                      static_cast<int>(MAX_CAMERADZ_VX_WINDOW));
}

double View3DRenderer::stableLeanDeg(double leanDeg, int focusC, int yside)
{
    // viewV2D scans frustum rows d ∈ [-yside-200, yside] and divides by
    // (d + tan(lean)*focusC) (VMAP.cpp:2079-2083). For the editor's upright
    // angles (lean in 90°..180°) tan is negative and the only zero is
    // d0 = |tan(lean)| * focusC > 0 — keep it past the scanned range with a
    // margin. tan(180° - x) = -tan(x), so the condition
    //     tan(180° - lean) * focusC > yside + margin
    // is lean < 180° - atan((yside + margin) / focusC).
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kMarginCells = 16.0;
    const double safeCells = static_cast<double>(yside) + kMarginCells;
    const double tanRequired = safeCells / std::max(1.0, static_cast<double>(focusC));
    const double maxLean = 180.0 - std::atan(tanRequired) * (180.0 / kPi);

    // Belt & braces: never fall below kMinLeanDeg even for pathological
    // focus/height pairs — the geometry check above shows maxLean >= 114° for
    // the smallest allowed focus (yside <= 320, focus >= 150), but clamp
    // defensively anyway.
    const double upperBound = std::max(kMinLeanDeg,
                                       std::min(kMaxLeanDeg, maxLean));
    return std::clamp(leanDeg, kMinLeanDeg, upperBound);
}

// ---------------------------------------------------------------------------
// World availability — the same buffer set ViewportRenderer (T2.1) relies on.
// ---------------------------------------------------------------------------

bool View3DRenderer::isWorldAvailable()
{
    return vMap.VxGBuf != nullptr && vMap.VxDBuf != nullptr &&
           vMap.AtrBuf != nullptr && vMap.SurBuf != nullptr &&
           vMap.RnrBuf != nullptr && vMap.H_SIZE > 0 && vMap.V_SIZE > 0;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

void View3DRenderer::resizeWindow(int windowWidth, int windowHeight)
{
    const int w =
        std::clamp(windowWidth, kMinWindowSize, kMaxWindowSize) & ~1; // even:
        // a 3-bytes-per-2-pixels RGB16 row keeps QImage rows 4-byte aligned.
    const int h = std::clamp(windowHeight, kMinWindowSize, kMaxWindowSize);
    if (w == width_ && h == height_) {
        return;
    }
    width_ = w;
    height_ = h;
    buffer_.assign(static_cast<std::size_t>(w) * h, 0u);
}

void View3DRenderer::setCamera(const View3DCamera &camera)
{
    camera_ = camera;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void View3DRenderer::render(int cx, int cy)
{
    if (!isWorldAvailable() || buffer_.empty()) {
        return;
    }

    // Push the camera into vrtMap's members (their only consumer is viewV2D;
    // the 2D modes do not read them, so aliasing is not a concern). The lean
    // member is int-degrees (VMAP.h:413) — round the editor-safe double into
    // it; turnAngleAroundZ is a float in radians.
    constexpr double kPi = 3.14159265358979323846;
    const int yside = height_ / 2;
    vMap.turnAngleAroundZ =
        static_cast<float>(camera_.turnAngleDeg * kPi / 180.0);
    vMap.cameraAngleLean = static_cast<int>(
        stableLeanDeg(camera_.leanDeg, camera_.focusC, yside) + 0.5);
    vMap.cameraDZ = clampCameraDZ(camera_.cameraDZ);
    vMap.focusC = clampFocus(camera_.focusC);

    // The window is centred on the map cell; xside/yside = half-window so the
    // d loop covers the full buffer height (viewV2D adds its own 200-row
    // near-field margin, VMAP.cpp:2078). xc/yc are unused by the
    // implementation — it fills GRAF_BUF from (0,0).
    // Post-T3.3: Smooth style drives the mip-filtered re-render of the same
    // projection instead of Terra's point-sampled loop.
    if (smooth_) {
        renderSmooth(cx, cy);
        return;
    }
    vMap.viewV2D(cx, cy, width_ / 2, height_ / 2, width_ / 2, height_ / 2,
                 buffer_.data(), width_, height_);
}

// ---------------------------------------------------------------------------
// Smooth (mip-filtered) oblique voxels — post-T3.3
// ---------------------------------------------------------------------------

namespace {

/// 0x00RRGGBB (the getColor32 channel order stored in the pyramid) to the
/// RGB565 layout Sur2Col uses (QImage::Format_RGB16): rrrrrggg gggbbbbb.
/// Truncation matches convertPal2TableSurCol (VMAP.cpp:1367-1369), where
/// (cr<<8)&0xF800 keeps exactly the (r>>3)<<10 bits.
inline unsigned short packRGB565(std::uint32_t color)
{
    const unsigned b = color & 0xFFu;
    const unsigned g = (color >> 8) & 0xFFu;
    const unsigned r = (color >> 16) & 0xFFu;
    return static_cast<unsigned short>(
        (r << 8) & 0xF800u | (g << 3) & 0x07E0u | (b >> 3) & 0x001Fu);
}

/// Smallest pyramid level whose kernel width (2^level cells) covers a ground
/// footprint of `footprint` cells — the mip choice per frustum row, capped at
/// `maxLevel` (the coarsiest pyramid level).
inline int mipLevelForFootprint(int footprint, int maxLevel)
{
    int level = 0;
    while (level < maxLevel && (1 << level) < footprint) {
        ++level;
    }
    return level;
}

} // namespace

void View3DRenderer::renderSmooth(int cx, int cy)
{
    // Without a bound pyramid source the smooth style degrades to the exact
    // chunky path — keeps viewV2D-based tests valid and the widget safe.
    if (!colorSource_ || !colorSource_->isBound()) {
        vMap.viewV2D(cx, cy, width_ / 2, height_ / 2, width_ / 2, height_ / 2,
                     buffer_.data(), width_, height_);
        return;
    }
    colorSource_->ensureMips();
    if (!colorSource_->hasMips()) {
        vMap.viewV2D(cx, cy, width_ / 2, height_ / 2, width_ / 2, height_ / 2,
                     buffer_.data(), width_, height_);
        return;
    }
    const ColorMipmap &mips = colorSource_->colorMipmap();

    short *vp = reinterpret_cast<short *>(buffer_.data());
    const int GB_MAXX = width_;
    const int GB_MAXY = height_;
    const int yside = GB_MAXY / 2;
    const int maxLevel = mips.levelCount() - 1;

    // Last drawn pixel per column (viewV2D's overdraw guard, VMAP.cpp:2053):
    // the scan walks near→far rows, so each column only paints its new top
    // segment above what nearer rows already drew.
    int lasty[kMaxWindowSize];
    for (int i = 0; i < GB_MAXX; ++i) {
        lasty[i] = GB_MAXY;
    }

    // Camera → world projection: the expressions below are copied verbatim
    // from viewV2D (VMAP.cpp:2066-2108) so both styles always agree on the
    // geometry; only the per-column ground sampling differs (mip pyramid
    // instead of the lvD/lvG + Sur2Col point reads).
    const int x0 = vMap.XCYCL(cx) << 16;
    const int y0 = vMap.YCYCL(cy) << 16;

    const float tga = xm::tan(vMap.cameraAngleLean * 3.14 / 180);
    const int dh = xm::round(65536 *
        (xm::sin((vMap.cameraAngleLean - 90) * 3.14 / 180) * vMap.focusC /
         (vMap.focusC + vMap.cameraDZ)));

    // Frustum-row ground distance in 16.16 cells (same float math viewV2D's
    // DY keeps; needed per row and for the next row to size the footprint).
    const auto rowDY = [tga](int d) -> int {
        return xm::round(xm::sqrt(
            xm::pow(d * (vMap.focusC + vMap.cameraDZ) / (d + tga * vMap.focusC), 2) +
            xm::pow(tga * d * (vMap.focusC + vMap.cameraDZ) /
                    (d + tga * vMap.focusC), 2)) * 65536);
    };

    for (int d = -yside - 200; d < yside; ++d) {
        int DY = rowDY(d);
        if (d < 0) {
            DY = -DY;
        }
        const int DX = xm::round(
            65536 * (width_ / 2) *
            (vMap.focusC + (vMap.cameraDZ -
                (d * (vMap.focusC + vMap.cameraDZ) /
                 (d + tga * vMap.focusC)))) / vMap.focusC);

        const int xxOr = x0 + static_cast<int>(
            (-DX) * xm::cos(vMap.turnAngleAroundZ) -
            (-DY) * xm::sin(vMap.turnAngleAroundZ));
        const int yyOr = y0 + static_cast<int>(
            (-DY) * xm::cos(vMap.turnAngleAroundZ) +
            (-DX) * xm::sin(vMap.turnAngleAroundZ));
        const int xx1r = x0 + static_cast<int>(
            (+DX) * xm::cos(vMap.turnAngleAroundZ) -
            (-DY) * xm::sin(vMap.turnAngleAroundZ));
        const int yy1r = y0 + static_cast<int>(
            (-DY) * xm::cos(vMap.turnAngleAroundZ) +
            (+DX) * xm::sin(vMap.turnAngleAroundZ));

        const int sx = (xx1r - xxOr) / GB_MAXX;
        const int sy = (yy1r - yyOr) / GB_MAXX;

        // Mip level from this row's ground footprint per pixel: columns
        // advance |sx|>>16 cells along the scan, rows |DY(d+1)-DY|>>16 cells
        // forward. Both explode toward the horizon — without the pyramid the
        // corners there alias into the blocky speckle fixed after T3.3.
        int footX = (sx < 0 ? -sx : sx) >> 16;
        // The next row's ground distance must carry the same sign flip the
        // main loop applies (d < 0 rows negate DY); otherwise |DYnext − DY|
        // degenerates to ≈ 2·|DY| and the whole near half of the frame
        // samples the coarsest pyramid levels (the flat-patch bug).
        int DYnext = rowDY(d + 1);
        if (d + 1 < 0) {
            DYnext = -DYnext;
        }
        int footY = (DYnext - DY) >> 16;
        if (footY < 0) {
            footY = -footY;
        }
        if (footX < footY) {
            footX = footY;
        }
        const int level = mipLevelForFootprint(footX, maxLevel);

        int xx0 = xxOr;
        int yy0 = yyOr;
        for (int i = 0; i < GB_MAXX; ++i) {
            // Bilinear mip sample: effective height (kernel-averaged at
            // coarse levels, which smooths distant silhouettes) and the
            // filtered cell color for the constant-per-column strip.
            const int h = mips.sampleHeight(level, xx0, yy0);
            const unsigned short col =
                packRGB565(mips.sampleColor(level, xx0, yy0));

            const int y = GB_MAXY / 2 - d - (dh * h >> 16);

            if (y < lasty[i]) {
                short *b = vp + lasty[i] * GB_MAXX + i;
                int a = lasty[i];
                if (a > GB_MAXY - 1) {
                    b -= (a - (GB_MAXY - 1)) * GB_MAXX;
                    a = GB_MAXY - 1;
                }
                const int yClamped = y < 0 ? 0 : y;
                while (yClamped < a) {
                    *b = col;
                    b -= GB_MAXX;
                    --a;
                }
                lasty[i] = yClamped;
            }

            xx0 += sx;
            yy0 += sy;
        }
    }

    // Sky: everything above the last drawn pixel of each column
    // (viewV2D:2203-2212) — 0 is the RGB565 sky color.
    for (int i = 0; i < GB_MAXX; ++i) {
        if (lasty[i] > 0) {
            short *b = vp + lasty[i] * GB_MAXX + i;
            while (lasty[i] > 0) {
                *b = 0;
                b -= GB_MAXX;
                --lasty[i];
            }
        }
    }
}

} // namespace MapEditor

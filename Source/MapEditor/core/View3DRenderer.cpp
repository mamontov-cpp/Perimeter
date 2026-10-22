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
#include <vector>

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

    // Previous frustum-row's ground sample position per column: strips paint
    // the interval between this row's anchor (P(d) at its bottom) and the
    // previous row's anchor (P(d-1) at its top), so adjacent strips tile the
    // depth axis without the one-sample-per-row steps that read as stacked
    // "layers" on distant mountains.
    std::vector<int> prevXXArr(GB_MAXX, 0);
    std::vector<int> prevYYArr(GB_MAXX, 0);
    bool havePrev = false;

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

        // Fractional mip level from this row's ground footprint per pixel
        // (GPU-style): columns advance |sx|>>16 cells along the scan, rows
        // |DY(d+1)-DY|>>16 cells forward — both explode toward the horizon,
        // and without the pyramid the corners there alias into the blocky
        // speckle fixed after T3.3. The floor(log2(foot)) level blends into
        // the next coarser one by the remainder (fW/256): adjacent rows then
        // change kernels continuously, instead of painting level-jump rings
        // around distant mountains.
        int foot = (sx < 0 ? -sx : sx) >> 16;
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
        if (foot < footY) {
            foot = footY;
        }
        int k0 = 0;
        int k1 = 0;
        int fW = 0;  // 0..256 blend weight of the coarser level k1
        if (foot > 1) {
            const float lFoot =
                xm::log(static_cast<float>(foot)) * 1.4426950408889634f;
            k0 = std::min(std::max(static_cast<int>(lFoot), 0), maxLevel);
            if (k0 < maxLevel) {
                k1 = k0 + 1;
                fW = static_cast<int>((lFoot - static_cast<float>(k0)) * 255.0f);
            }
        }
        // Depth advance below one cell: neighboring rows sample nearly the
        // same ground point, so the previous-row samples would be redundant.
        const bool subCellStep = footY == 0;

        int xx0 = xxOr;
        int yy0 = yyOr;
        for (int i = 0; i < GB_MAXX; ++i) {
            const bool usePrev = havePrev && !subCellStep;
            const int xa = usePrev ? prevXXArr[i] : xx0;
            const int ya = usePrev ? prevYYArr[i] : yy0;

            // Trilinear sampling helpers: kernel-averaged levels smooth
            // distant silhouettes and terrain detail, the fractional blend
            // toward the next coarser level keeps the kernel growth
            // continuous with depth (discrete levels per row would paint
            // level-jump rings around distant mountains).
            const auto heightAt = [&mips, k0, k1, fW](int x, int yy) -> int {
                const int v = mips.sampleHeight(k0, x, yy);
                return fW ? (v * (256 - fW) +
                             mips.sampleHeight(k1, x, yy) * fW + 128) >> 8
                          : v;
            };
            const auto colorAt =
                [&mips, k0, k1, fW](int x, int yy) -> unsigned int {
                const unsigned int v = mips.sampleColor(k0, x, yy);
                if (!fW) {
                    return v;
                }
                const unsigned int w = mips.sampleColor(k1, x, yy);
                return (((v >> 16 & 0xFFu) * unsigned(256 - fW) +
                         (w >> 16 & 0xFFu) * unsigned(fW) + 128u) >> 8 << 16) |
                       (((v >> 8 & 0xFFu) * unsigned(256 - fW) +
                         (w >> 8 & 0xFFu) * unsigned(fW) + 128u) >> 8 << 8) |
                       (((v & 0xFFu) * unsigned(256 - fW) +
                         (w & 0xFFu) * unsigned(fW) + 128u) >> 8);
            };

            // Heights are averaged between the two row anchors so the
            // silhouette edges follow the same depth filter as the colors.
            const int hCur = heightAt(xx0, yy0);
            const int hPrev = usePrev ? heightAt(xa, ya) : hCur;
            const int h = (hCur + hPrev + 1) >> 1;

            // Strip colors: cFar anchors this row's ground position (the
            // strip's bottom), cNear the previous row's (its top). Instead of
            // painting one slab of cFar, the strip's rows slide linearly
            // between the anchors — the linear un-projection of each screen
            // row back onto the ground between the two anchors — so the
            // depth profile is continuous across adjacent strips and the
            // far mountains lose their stacked "layers" look.
            const unsigned int cFar = colorAt(xx0, yy0);
            const unsigned int cNear = usePrev ? colorAt(xa, ya) : cFar;
            const int rF = (cFar >> 16) & 0xFF, gF = (cFar >> 8) & 0xFF,
                      bF = cFar & 0xFF;
            const int rN = (cNear >> 16) & 0xFF, gN = (cNear >> 8) & 0xFF,
                      bN = cNear & 0xFF;

            const int y = GB_MAXY / 2 - d - (dh * h >> 16);

            if (y < lasty[i]) {
                short *b = vp + lasty[i] * GB_MAXX + i;
                int a = lasty[i];
                if (a > GB_MAXY - 1) {
                    b -= (a - (GB_MAXY - 1)) * GB_MAXX;
                    a = GB_MAXY - 1;
                }
                const int yClamped = y < 0 ? 0 : y;
                // One blended color per painted screen row: the row's ground
                // position slides linearly from the previous row's anchor
                // (top) down to this row's (just above the bottom), so the
                // depth profile is continuous across adjacent strips.
                const int rows = a - yClamped;
                while (yClamped < a) {
                    const int wNear = (a - yClamped) * 65536 / rows;
                    const int rr = (rN * wNear + rF * (65536 - wNear)
                                    + 32768) >> 16;
                    const int gg = (gN * wNear + gF * (65536 - wNear)
                                    + 32768) >> 16;
                    const int bb = (bN * wNear + bF * (65536 - wNear)
                                    + 32768) >> 16;
                    *b = static_cast<short>(
                        (rr << 8) & 0xF800u | (gg << 3) & 0x07E0u |
                        (bb >> 3) & 0x001Fu);
                    b -= GB_MAXX;
                    --a;
                }
                lasty[i] = yClamped;
            }

            prevXXArr[i] = xx0;
            prevYYArr[i] = yy0;
            xx0 += sx;
            yy0 += sy;
        }
        havePrev = true;
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

// ---------------------------------------------------------------------------
// Ground pick (inverse projection for 3D brush painting)
// ---------------------------------------------------------------------------

bool View3DRenderer::pickGroundCell(int cx, int cy, int col, int row,
                                    int &mapX, int &mapY) const
{
    // Preconditions: must have been rendered (buffer + camera set) and the
    // requested pixel must be inside the window.
    if (buffer_.empty() || width_ <= 0 || height_ <= 0) {
        return false;
    }
    if (col < 0 || col >= width_ || row < 0 || row >= height_) {
        return false;
    }
    if (!isWorldAvailable()) {
        return false;
    }

    const int xside = width_ / 2;
    const int yside = height_ / 2;
    const int GB_MAXX = width_;
    const int GB_MAXY = height_;

    // Same clamped camera values render() pushes into vMap, taken from our own
    // camera_ so the pick does not depend on vMap's current contents.
    const int focusC = clampFocus(camera_.focusC);
    const int cameraDZ = clampCameraDZ(camera_.cameraDZ);
    const double lean = stableLeanDeg(camera_.leanDeg, focusC, yside);
    const double leanRad = lean * 3.14159265358979323846 / 180.0;
    const double turnRad = camera_.turnAngleDeg * 3.14159265358979323846 / 180.0;

    const float tga = static_cast<float>(std::tan(leanRad));
    const float cosT = static_cast<float>(std::cos(turnRad));
    const float sinT = static_cast<float>(std::sin(turnRad));

    const int dh = static_cast<int>(xm::round(65536.0 *
        (std::sin(leanRad - 3.14159265358979323846 / 2.0) * focusC /
         static_cast<double>(focusC + cameraDZ))));

    const int x0 = vMap.XCYCL(cx) << 16;
    const int y0 = vMap.YCYCL(cy) << 16;

    const unsigned char *lvG = vMap.VxGBuf;
    const unsigned char *lvD = vMap.VxDBuf;

    // Walk the frustum rows near → far exactly like viewV2D (VMAP.cpp:2078)
    // and return the first row whose ground strip covers `row`: nearer strips
    // paint lower on the screen and occlude the distant ones, which is what
    // the rendered frame shows.
    for (int d = -yside - 200; d < yside; ++d) {
        const double denom = d + tga * focusC;
        if (std::fabs(denom) < 1e-6) {
            continue; // projection singularity (excluded by stableLeanDeg)
        }

        const double near = static_cast<double>(d) * (focusC + cameraDZ) / denom;
        const double tanNear = static_cast<double>(tga) * near;
        int DY = static_cast<int>(
            xm::round(std::sqrt(near * near + tanNear * tanNear) * 65536.0));
        if (d < 0) {
            DY = -DY;
        }
        const int DX = static_cast<int>(xm::round(
            65536.0 * xside * (focusC + (cameraDZ - near)) / focusC));

        // The row endpoints on the ground, 16.16 cells (viewV2D:2096-2099).
        const int xxLeft = x0 + static_cast<int>(
            (-DX) * cosT - (-DY) * sinT);
        const int yyLeft = y0 + static_cast<int>(
            (-DY) * cosT + (-DX) * sinT);
        const int xxRight = x0 + static_cast<int>(
            (+DX) * cosT - (-DY) * sinT);
        const int yyRight = y0 + static_cast<int>(
            (-DY) * cosT + (+DX) * sinT);

        // Per-column ground advance along the row (viewV2D:2108).
        const int sx = (xxRight - xxLeft) / GB_MAXX;
        const int sy = (yyRight - yyLeft) / GB_MAXX;

        const int xx = xxLeft + sx * col;
        const int yy = yyLeft + sy * col;

        // Bilinear height sample of the ground point (viewV2D:2112-2128).
        const int u0 = (xx >> 16) & vMap.clip_mask_x;
        const int a = (xx >> 8) & 255;
        const int v0r = (yy >> 16) & vMap.clip_mask_y;
        const int b = (yy >> 8) & 255;
        const int u1 = (u0 + 1) & vMap.clip_mask_x;
        const int v1r = (v0r + 1) & vMap.clip_mask_y;

        const int off0 = v0r * vMap.XS_Buf;
        const int off1 = v1r * vMap.XS_Buf;

        int h0 = lvD[u0 + off0]; if (!h0) h0 = lvG[u0 + off0];
        int h1 = lvD[u1 + off0]; if (!h1) h1 = lvG[u1 + off0];
        int h2 = lvD[u0 + off1]; if (!h2) h2 = lvG[u0 + off1];
        int h3 = lvD[u1 + off1]; if (!h3) h3 = lvG[u1 + off1];

        h0 = (h0 << 8) + a * (h1 - h0);
        h2 = (h2 << 8) + a * (h3 - h2);
        const int h = ((h0 << 8) + b * (h2 - h0)) >> 16;

        // Screen row of this strip's top edge (viewV2D:2173).
        const int y = GB_MAXY / 2 - d - (dh * h >> 16);
        if (y <= row) {
            mapX = vMap.XCYCL(xx >> 16);
            mapY = vMap.YCYCL(yy >> 16);
            return true;
        }
    }

    // Sky — no surface under the pixel.
    mapX = mapY = -1;
    return false;
}

} // namespace MapEditor

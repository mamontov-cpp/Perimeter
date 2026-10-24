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
    // Kept raw (unwrapped) for the grid overlay's identical projection walk;
    // viewV2D/renderSmooth wrap the centre themselves (XCYCL/YCYCL).
    lastCx_ = cx;
    lastCy_ = cy;

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
    //
    // Zeroplast demotion: viewV2D paints At_ZEROPLAST cells with the blue
    // SurZP2Col tint — in the game that tint belongs to the powered core's
    // field (the 3D scene's SetZeroplastColor), which does not exist in the
    // editor. The zero-plastic cells are therefore temporarily demoted to
    // plain leveled for the viewV2D passes (chunky style and the smooth
    // fallback), so they render like every other leveled cell — own color
    // with the leveledSurfaceTexture on top (docs/context.md). The smooth
    // path with a bound pyramid reads the already-corrected 2D cache instead.
    struct ZpCell { int off; unsigned char atr; };
    std::vector<ZpCell> zeroplastCells;
    if (vMap.AtrBuf && vMap.H_SIZE > 0 && vMap.V_SIZE > 0) {
        const std::size_t cells =
            static_cast<std::size_t>(vMap.H_SIZE) * vMap.V_SIZE;
        for (std::size_t off = 0; off < cells; ++off) {
            if ((vMap.AtrBuf[off] & At_ZPMASK) == At_ZEROPLAST) {
                zeroplastCells.push_back({static_cast<int>(off),
                                          vMap.AtrBuf[off]});
                vMap.AtrBuf[off] = static_cast<unsigned char>(
                    (vMap.AtrBuf[off] & ~At_ZPMASK) | At_LEVELED);
            }
        }
    }

    if (colorSource_ && colorSource_->isBound()) {
        // Both styles share the corrected geometry: Smooth filters through
        // the mip levels, Chunky point-samples level 0 over the same walk.
        // Terra's viewV2D (legacy geometry) only serves as the no-source
        // fallback below.
        renderSmooth(cx, cy, !smooth_);
    } else {
        viewV2DChunky(cx, cy);
    }

    for (const ZpCell &cell : zeroplastCells) {
        vMap.AtrBuf[cell.off] = cell.atr;
    }
}

// ---------------------------------------------------------------------------
// Chunky path — viewV2D with the in-game geo dimming
// ---------------------------------------------------------------------------

void View3DRenderer::viewV2DChunky(int cx, int cy)
{
    if (!geoDimmed_) {
        vMap.viewV2D(cx, cy, width_ / 2, height_ / 2, width_ / 2, height_ / 2,
                     buffer_.data(), width_, height_);
        return;
    }
    // viewV2D samples Terra's own 16-bit tables (Sur2Col[sur][lighting],
    // Tex2Col[lighting][tex] — the +128 rows are the dam layer,
    // convertPal2TableSurCol VMAP.cpp:1370-1375). The dimmed game look
    // greys the geo (non-dam) halves: save both tables, grey rows 0..127,
    // render, restore byte-exact. Terra itself is never modified.
    std::vector<unsigned short> surSave(
        &vMap.Sur2Col[0][0],
        &vMap.Sur2Col[0][0] + MAX_SURFACE_TYPE * (MAX_SURFACE_LIGHTING * 2));
    std::vector<unsigned short> texSave(
        &vMap.Tex2Col[0][0],
        &vMap.Tex2Col[0][0] + (MAX_SURFACE_LIGHTING * 2) * MAX_SURFACE_TYPE);
    const auto grey565 = [](unsigned short v) {
        const int r8 = ((v >> 11) & 31) << 3;
        const int g8 = ((v >> 5) & 63) << 2;
        const int b8 = (v & 31) << 3;
        const int lum = (r8 * 76 + g8 * 150 + b8 * 29) >> 8;
        const int half = lum >> 1;
        return static_cast<unsigned short>(
            ((half >> 3) << 11) | (((half >> 2) & 63) << 5) | (half >> 3));
    };
    for (int i = 0; i < MAX_SURFACE_TYPE; ++i) {
        for (int j = 0; j < MAX_SURFACE_LIGHTING; ++j) {
            vMap.Sur2Col[i][j] = grey565(vMap.Sur2Col[i][j]);
        }
    }
    for (int j = 0; j < MAX_SURFACE_LIGHTING; ++j) {
        for (int i = 0; i < MAX_SURFACE_TYPE; ++i) {
            vMap.Tex2Col[j][i] = grey565(vMap.Tex2Col[j][i]);
        }
    }
    vMap.viewV2D(cx, cy, width_ / 2, height_ / 2, width_ / 2, height_ / 2,
                 buffer_.data(), width_, height_);
    std::copy(surSave.begin(), surSave.end(), &vMap.Sur2Col[0][0]);
    std::copy(texSave.begin(), texSave.end(), &vMap.Tex2Col[0][0]);
}

// ---------------------------------------------------------------------------
// Smooth (mip-filtered) oblique voxels — post-T3.3
// ---------------------------------------------------------------------------

void View3DRenderer::renderSmooth(int cx, int cy, bool pointSample)
{
    // Without a bound pyramid source there is nothing to sample — degrade
    // to Terra's viewV2D (legacy geometry; the geo dimming of the chunky
    // tables applies there as well).
    if (!colorSource_ || !colorSource_->isBound()) {
        viewV2DChunky(cx, cy);
        return;
    }
    colorSource_->ensureMips();
    if (!colorSource_->hasMips()) {
        viewV2DChunky(cx, cy);
        return;
    }
    const ColorMipmap &mips = colorSource_->colorMipmap();
    // Freeze-dam mode: repeats of the torus render through the geo-only
    // pyramid (built on demand) so only the geo layer scrolls; the one real
    // map copy keeps the composite look with its single dam.
    const ColorMipmap *geoMips = nullptr;
    if (damFrozen_) {
        colorSource_->ensureGeoMips();
        if (colorSource_->hasGeoMips()) {
            geoMips = &colorSource_->geoMipmap();
        }
    }

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
    // Offset from the wrapped scan origin back to the caller's unwrapped
    // camera position (a whole number of map sizes) — the freeze-dam test
    // tells the one real map copy from its torus repeats with it.
    const long long wrapOffX =
        (static_cast<long long>(cx) << 16) - x0;
    const long long wrapOffY =
        (static_cast<long long>(cy) << 16) - y0;
    const long long mapSpanX =
        static_cast<long long>(vMap.H_SIZE) << 16;
    const long long mapSpanY =
        static_cast<long long>(vMap.V_SIZE) << 16;
    const auto inRealMap = [&](int xx, int yy) {
        if (!geoMips) {
            return true; // no freeze — everything is the real copy
        }
        const long long ux = xx + wrapOffX;
        const long long uy = yy + wrapOffY;
        return ux >= 0 && ux < mapSpanX && uy >= 0 && uy < mapSpanY;
    };

    const float tga = xm::tan(vMap.cameraAngleLean * 3.14 / 180);
    const int dh = xm::round(65536 *
        (xm::sin((vMap.cameraAngleLean - 90) * 3.14 / 180) * vMap.focusC /
         (vMap.focusC + vMap.cameraDZ)));

    // Frustum-row ground distance in 16.16 cells. AHEAD rows (d > 0) keep
    // Terra's own perspective-correct mapping |near| = d(F+Z)/(d0-d).
    // BEHIND rows (d < 0) deliberately deviate from viewV2D: the original
    // maps them with a pole at near = F+Z, which inverts perspective on the
    // whole below-nadir half (far-behind ground occupies MORE rows and a
    // NARROWER lateral span — features spread apart toward the screen
    // bottom). The editor mirrors the ahead side instead: the nadir is the
    // screen centre, ground recedes symmetrically to the front horizon
    // (+d0) and the rear horizon (-d0), so both halves compress with
    // distance (docs/architecture.md §5.15).
    const double horizonD = -tga * vMap.focusC; // d0 = |tga|*F > 0
    const auto rowAbsNearOfD = [&](double d) -> double {
        const double dd = d < 0.0 ? -d : d;
        const double denom = horizonD - dd;
        if (denom <= 1e-6) {
            return 1e30; // at/beyond the horizon — no ground
        }
        return dd * (vMap.focusC + vMap.cameraDZ) / denom;
    };
    const auto rowDY = [&](int d) -> int {
        const double n = rowAbsNearOfD(d);
        return xm::round(xm::sqrt(n * n * (1.0 + tga * tga)) * 65536);
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
        // Beyond either horizon (|d| >= d0) no ground maps to this row.
        if (std::fabs(static_cast<double>(d)) >= horizonD - 1.0) {
            havePrev = false; // the ground gap breaks strip continuity
            continue;
        }
        const double rowNearAbs = rowAbsNearOfD(d);
        // Mirrored sign: near is negative on BOTH sides, so the lateral
        // span (F+Z-near) grows with distance on both sides of the nadir.
        const double nearSigned = -rowNearAbs;
        int DY = rowDY(d);
        if (d < 0) {
            DY = -DY;
        }
        const int DX = xm::round(
            65536 * (width_ / 2) *
            (vMap.focusC + vMap.cameraDZ - nearSigned) / vMap.focusC);

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
        if (!pointSample && foot > 1) {
            const float lFoot =
                xm::log(static_cast<float>(foot)) * 1.4426950408889634f;
            k0 = std::min(std::max(static_cast<int>(lFoot), 0), maxLevel);
            if (k0 < maxLevel) {
                k1 = k0 + 1;
                fW = static_cast<int>((lFoot - static_cast<float>(k0)) * 255.0f);
            }
        }
        // pointSample (Chunky style): k0 = k1 = 0, fW = 0 — pure level-0
        // bilinear sampling over the same corrected geometry.
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
            // level-jump rings around distant mountains). The pyramid is a
            // parameter: freeze-dam mode samples the geo-only pyramid for
            // anchors on torus repeats, the composite one for the real map.
            const auto heightAt = [k0, k1, fW](const ColorMipmap &m,
                                               int x, int yy) -> int {
                const int v = m.sampleHeight(k0, x, yy);
                return fW ? (v * (256 - fW) +
                             m.sampleHeight(k1, x, yy) * fW + 128) >> 8
                          : v;
            };
            const auto colorAt = [k0, k1,
                                  fW](const ColorMipmap &m, int x,
                                      int yy) -> unsigned int {
                const unsigned int v = m.sampleColor(k0, x, yy);
                if (!fW) {
                    return v;
                }
                const unsigned int w = m.sampleColor(k1, x, yy);
                return (((v >> 16 & 0xFFu) * unsigned(256 - fW) +
                         (w >> 16 & 0xFFu) * unsigned(fW) + 128u) >> 8 << 16) |
                       (((v >> 8 & 0xFFu) * unsigned(256 - fW) +
                         (w >> 8 & 0xFFu) * unsigned(fW) + 128u) >> 8 << 8) |
                       (((v & 0xFFu) * unsigned(256 - fW) +
                         (w & 0xFFu) * unsigned(fW) + 128u) >> 8);
            };

            // Heights are averaged between the two row anchors so the
            // silhouette edges follow the same depth filter as the colors.
            // Each anchor samples the pyramid of its own map copy (composite
            // for the real one, geo-only for a repeat when the dam is
            // frozen — strips that straddle the wrap blend the transition).
            const ColorMipmap &mipCur =
                inRealMap(xx0, yy0) ? mips : *geoMips;
            const ColorMipmap &mipPrev = (usePrev && geoMips &&
                                          !inRealMap(xa, ya))
                                             ? *geoMips
                                             : mips;
            const int hCur = heightAt(mipCur, xx0, yy0);
            const int hPrev = usePrev ? heightAt(mipPrev, xa, ya) : hCur;
            const int h = (hCur + hPrev + 1) >> 1;

            // Strip colors: cFar anchors this row's ground position (the
            // strip's bottom), cNear the previous row's (its top). Instead of
            // painting one slab of cFar, the strip's rows slide linearly
            // between the anchors — the linear un-projection of each screen
            // row back onto the ground between the two anchors — so the
            // depth profile is continuous across adjacent strips and the
            // far mountains lose their stacked "layers" look.
            const unsigned int cFar = colorAt(mipCur, xx0, yy0);
            const unsigned int cNear =
                usePrev ? colorAt(mipPrev, xa, ya) : cFar;
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
// Grid overlay (post-render pass, same projection walk as renderSmooth)
// ---------------------------------------------------------------------------

void View3DRenderer::drawGridOverlay(int stepX, int stepY,
                                     unsigned short color)
{
    if (buffer_.empty() || width_ <= 0 || height_ <= 0 ||
        stepX <= 0 || stepY <= 0 || !isWorldAvailable()) {
        return;
    }
    const int xside = width_ / 2;
    const int yside = height_ / 2;
    const int GB_MAXX = width_;
    const int GB_MAXY = height_;

    // The same clamped camera values renderSmooth renders with.
    const int focusC = clampFocus(camera_.focusC);
    const int cameraDZ = clampCameraDZ(camera_.cameraDZ);
    const double lean = stableLeanDeg(camera_.leanDeg, focusC, yside);
    const double leanRad = lean * 3.14159265358979323846 / 180.0;
    const double tga = std::tan(leanRad);
    const double turnRad =
        camera_.turnAngleDeg * 3.14159265358979323846 / 180.0;
    const double cosT = std::cos(turnRad);
    const double sinT = std::sin(turnRad);

    // The grid is a transparent plane of lines overlaying the frame: every
    // grid line is walked in map space and FORWARD-PROJECTED through the
    // same camera the frame renders with. Unlike crossing enumeration over
    // the scan rows (the previous attempt), this has no degenerate
    // orientations — lines parallel to the scan direction (horizontal map
    // lines at rotation 0, vertical at 90) rendered not at all there.
    const double F = focusC;
    const double Z = cameraDZ;
    const double tgaF = tga * F;            // negative (lean in 100..170)
    const double absCosLean = -std::cos(leanRad); // |cos| = 1/sqrt(1+tga^2)
    const double planeH = static_cast<double>(vMap.hZeroPlast);
    const int dh = static_cast<int>(xm::round(65536.0 *
        (std::sin(leanRad - 3.14159265358979323846 / 2.0) * F /
         (F + Z))));
    const double planeRowOff = (dh * planeH) / 65536.0;
    const double ccx = lastCx_;             // unwrapped camera look-at (cells)
    const double ccy = lastCy_;

    // Ground point of the reference plane (unwrapped map cells) -> screen
    // pixel. Straight inversion of the renderer's row math, which is
    // SYMMETRIC around the camera nadir (screen centre): a point at
    // camera-basis distance |forward| has |near| = |forward|*|cos(lean)|,
    // paints frustum row d = sign * |near|*d0/(F+Z+|near|) (d0 = |tga|*F,
    // the horizon row; ground recedes to the front horizon above the nadir
    // and to the rear horizon below it), and the column runs linearly
    // across the row's lateral extent +/-halfSpan = xside*(F+Z+|near|)/F —
    // exactly the anchors renderSmooth steps between.
    const auto project = [&](double px, double py, double &col,
                             double &row) {
        const double dpx = px - ccx;
        const double dpy = py - ccy;
        const double lateral = dpx * cosT + dpy * sinT;
        const double forward = -dpx * sinT + dpy * cosT;
        const double aNear = std::fabs(forward) * absCosLean;
        const double s = forward < 0.0 ? -1.0 : 1.0;
        const double d = s * aNear * (-tgaF) / (F + Z + aNear);
        row = yside - d - planeRowOff;
        const double halfSpan = xside * (F + Z + aNear) / F;
        col = halfSpan > 1e-6
            ? (lateral + halfSpan) / (2.0 * halfSpan) * GB_MAXX
            : GB_MAXX / 2.0; // at the horizon everything meets the centre
        return true;
    };

    // 2x2-thick point plot (the "at least two pixels bold" requirement).
    const auto plotPoint = [&](int x, int y) {
        for (int yy = y; yy <= y + 1; ++yy) {
            if (yy < 0 || yy >= GB_MAXY) {
                continue;
            }
            for (int xx = x; xx <= x + 1; ++xx) {
                if (xx >= 0 && xx < GB_MAXX) {
                    buffer_[static_cast<std::size_t>(yy) * GB_MAXX + xx] =
                        color;
                }
            }
        }
    };
    // Straight segment between two projected points (Liang-Barsky clip to
    // the frame, then Bresenham with 2x2 blocks).
    const auto drawSeg = [&](double ax, double ay, double bx, double by) {
        double t0 = 0.0;
        double t1 = 1.0;
        const double dx = bx - ax;
        const double dy = by - ay;
        // Clip window slightly inside the frame; the 2x2 blocks then stay
        // whole pixels. Edges: x >= -1, x <= GB_MAXX, y >= -1, y <= GB_MAXY
        // in the parametric form p = a + t*(b-a).
        const double edges[4][2] = {
            {-dx, ax + 1.0}, {dx, double(GB_MAXX) - ax},
            {-dy, ay + 1.0}, {dy, double(GB_MAXY) - ay}};
        for (const auto &edge : edges) {
            const double denom = edge[0];
            const double num = edge[1];
            if (denom == 0.0) {
                if (num < 0.0) {
                    return; // parallel and outside
                }
                continue;
            }
            const double t = num / denom;
            if (denom < 0.0) {
                if (t > t1) {
                    return;
                }
                if (t > t0) {
                    t0 = t;
                }
            } else {
                if (t < t0) {
                    return;
                }
                if (t < t1) {
                    t1 = t;
                }
            }
        }
        if (t1 <= t0) {
            return;
        }
        int x0 = static_cast<int>(std::lround(ax + dx * t0));
        int y0 = static_cast<int>(std::lround(ay + dy * t0));
        const int x1 = static_cast<int>(std::lround(ax + dx * t1));
        const int y1 = static_cast<int>(std::lround(ay + dy * t1));
        const int sx = (x1 > x0) - (x1 < x0);
        const int sy = (y1 > y0) - (y1 < y0);
        const int dxAbs = std::abs(x1 - x0);
        const int dyAbs = std::abs(y1 - y0);
        int err = dxAbs - dyAbs;
        for (;;) {
            plotPoint(x0, y0);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int e2 = 2 * err;
            if (e2 > -dyAbs) {
                err -= dyAbs;
                x0 += sx;
            }
            if (e2 < dxAbs) {
                err += dxAbs;
                y0 += sy;
            }
        }
    };

    // The visible ground wedge in camera basis (the rows the frame scans),
    // transformed to a map-space bounding box the line walks cover. The
    // projection is symmetric around the nadir: both the far (front) and the
    // near (rear) edges are capped just under their horizon rows — rows
    // within ~3% of a horizon differ by <1px on screen, and the caps keep
    // the walk bounded.
    const double horizonD = -tgaF; // d0 = |tga|*F
    const auto absNearOfD = [&](double d) {
        const double dd = d < 0.0 ? -d : d;
        const double denom = horizonD - dd;
        return denom > 1e-6 ? dd * (F + Z) / denom : 1e30;
    };
    const auto rowAbsNear = [&](double n) {
        return std::sqrt(n * n * (1.0 + tga * tga));
    };
    const double horizonCap = horizonD * 32.0 / 33.0;
    const double dFar = std::min(yside - 1.0, horizonCap);
    const double dNear = std::max(-(yside + 200.0), -horizonCap);
    const double farNear = absNearOfD(dFar);
    const double nearNear = absNearOfD(dNear);
    // The near (behind-nadir) edge at negative, the far (ahead) edge at
    // positive forward distance; the lateral half-span grows with distance
    // symmetrically on both sides.
    const auto spanAt = [&](double n) { return xside * (F + Z + n) / F; };
    const double corners[4][2] = {
        {spanAt(nearNear), -rowAbsNear(nearNear)},
        {-spanAt(nearNear), -rowAbsNear(nearNear)},
        {spanAt(farNear), rowAbsNear(farNear)},
        {-spanAt(farNear), rowAbsNear(farNear)},
    };
    double minX = 1e30, maxX = -1e30, minY = 1e30, maxY = -1e30;
    for (const auto &c : corners) {
        const double mx = ccx + c[0] * cosT - c[1] * sinT;
        const double my = ccy + c[1] * cosT + c[0] * sinT;
        minX = std::min(minX, mx);
        maxX = std::max(maxX, mx);
        minY = std::min(minY, my);
        maxY = std::max(maxY, my);
    }

    // Walk every grid line of both families across the bbox with adaptive
    // stepping (aim a few screen px per segment); off-screen segments clip
    // away inside drawSeg, on-screen ones draw as thick white runs.
    const auto walkLine = [&](bool fixedX, double fixed, double lo,
                              double hi) {
        double t = lo;
        double step = 4.0;
        double colA = 0.0;
        double rowA = 0.0;
        bool hasA = fixedX ? project(fixed, t, colA, rowA)
                           : project(t, fixed, colA, rowA);
        while (t < hi) {
            const double next = std::min(t + step, hi);
            double colB = 0.0;
            double rowB = 0.0;
            const bool hasB = fixedX ? project(fixed, next, colB, rowB)
                                     : project(next, fixed, colB, rowB);
            if (hasA && hasB) {
                drawSeg(colA, rowA, colB, rowB);
            }
            if (hasB) {
                const double dist = std::hypot(colB - colA, rowB - rowA);
                if (dist > 8.0) {
                    step = std::max(0.25, step * 0.5);
                } else if (dist < 3.0) {
                    step = std::min(64.0, step * 2.0);
                }
            }
            t = next;
            colA = colB;
            rowA = rowB;
            hasA = hasB;
        }
    };
    for (double kx = std::floor(minX / stepX); kx * stepX <= maxX; ++kx) {
        walkLine(true, kx * stepX, minY, maxY);
    }
    for (double ky = std::floor(minY / stepY); ky * stepY <= maxY; ++ky) {
        walkLine(false, ky * stepY, minX, maxX);
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

    // Walk the frustum rows near → far exactly like the renderer above
    // (including the mirrored behind-nadir mapping — see renderSmooth) and
    // return the first row whose ground strip covers `row`: nearer strips
    // paint lower on the screen and occlude the distant ones, which is what
    // the rendered frame shows.
    const double horizonD = -tga * focusC; // d0 = |tga|*F > 0
    const auto rowAbsNearOfD = [&](double d) -> double {
        const double dd = d < 0.0 ? -d : d;
        const double denom = horizonD - dd;
        if (denom <= 1e-6) {
            return -1.0; // at/beyond the horizon — no ground
        }
        return dd * (focusC + cameraDZ) / denom;
    };
    for (int d = -yside - 200; d < yside; ++d) {
        const double rowNearAbs = rowAbsNearOfD(d);
        if (rowNearAbs < 0.0) {
            continue; // beyond the (front or rear) horizon
        }
        const double nearSigned = -rowNearAbs;
        const double tanNear = static_cast<double>(tga) * rowNearAbs;
        int DY = static_cast<int>(
            xm::round(std::sqrt(rowNearAbs * rowNearAbs +
                                tanNear * tanNear) * 65536.0));
        if (d < 0) {
            DY = -DY;
        }
        const int DX = static_cast<int>(xm::round(
            65536.0 * xside * (focusC + (cameraDZ - nearSigned)) / focusC));

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
        h2 = (h2 << 8) + b * (h3 - h2);
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

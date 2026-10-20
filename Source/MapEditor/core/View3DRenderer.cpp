// ============================================================================
// MapEditorCore — View3DRenderer implementation (T2.3).
// ============================================================================

#include "View3DRenderer.h"

// Terra / XTool headers — vMap, the camera members and viewV2D itself.
// _PERIMETER_ and _TX3D_LIBRARY_ are defined by CMake.
#include "terra.h"

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
    vMap.viewV2D(cx, cy, width_ / 2, height_ / 2, width_ / 2, height_ / 2,
                 buffer_.data(), width_, height_);
}

} // namespace MapEditor

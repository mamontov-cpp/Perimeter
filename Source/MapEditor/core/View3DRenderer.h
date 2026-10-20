#pragma once

#include <cstdint>
#include <vector>

// ============================================================================
// MapEditorCore — View3DRenderer (T2.3).
//
// Software-rasterised oblique (tilted) view of the voxel surface on top of
// vrtMap::viewV2D (Source/Terra/VMAP.cpp:2041 — the legacy SurMap-style voxel
// window, unused by the game itself):
//
//   * The camera state mirrors vrtMap's own camera members
//     (turnAngleAroundZ/focusC/cameraDZ/cameraAngleLean, VMAP.h:407-413) but
//     in editor-friendly units: rotation and tilt in degrees, focus and
//     camera lift as map cells. render() writes them into vMap by value,
//     then asks viewV2D to fill an internal RGB565 buffer.
//   * The buffer layout is exactly QImage::Format_RGB16 (RGB565, low bits to
//     high: blue, green, red — convertPal2TableSurCol builds Sur2Col in the
//     same order, VMAP.cpp:1367-1370), so the Qt widget wraps it zero-copy,
//     matching the T2.1/T2.2 zero-copy pipeline.
//   * Window width is clamped to Terra's own limit: viewV2D keeps per-column
//     scratch arrays sized MAX_XSIZE_VX_WINDOW = 640 (VMAP.h:125), so
//     GB_MAXX must never exceed it.
//   * viewV2D divides by (d + tan(lean)*focusC) while scanning the frustum
//     rows d ∈ [-yside-200, yside]. For upright camera angles (lean in
//     90°..180°, tan negative) the only zero sits at
//     d0 = |tan(lean)| * focusC; stableLeanDeg() clamps the tilt so d0 stays
//     outside the scanned range, otherwise the interpolation degenerates.
//
// Precondition for render(): a world is fully loaded AND lit, i.e.
// EditorSession::loadWorld() (fullLoad + WorldRender), exactly like
// ViewportRenderer (T2.1). There is no cache here — the camera moves mean
// every frame is a fresh full-render anyway.
// ============================================================================

namespace MapEditor {

/// Camera knobs of the oblique 3D view, in editor units.
struct View3DCamera
{
    /// Rotation around the map Z axis, degrees 0..360 (turnAngleAroundZ).
    double turnAngleDeg = 0.0;
    /// Camera tilt in degrees (cameraAngleLean). The game default is 140
    /// (VMAP.cpp:218); render() clamps it into the stable band.
    double leanDeg = 140.0;
    /// Camera lift above the ground plane in cells (cameraDZ). The Terra
    /// limits are MIN_CAMERADZ_VX_WINDOW / MAX_CAMERADZ_VX_WINDOW
    /// (-500 .. 1500, VMAP.h:127-128).
    int cameraDZ = 100;
    /// Focal length in cells (focusC): smaller = closer view. Default 600
    /// (VMAP.cpp:214).
    int focusC = 600;
};

class View3DRenderer
{
public:
    /// Terra's own voxel-window width limit (MAX_XSIZE_VX_WINDOW, VMAP.h:125);
    /// static_assert'ed against the constant in the .cpp. Doubles as the
    /// window-height cap — viewV2D has no explicit height limit, capping both
    /// keeps the window square-ish and the frametime predictable.
    static constexpr int kMaxWindowSize = 640;
    /// Never render a window smaller than this (avoids degenerate geometry).
    static constexpr int kMinWindowSize = 128;

    /// Focus bounds. 150 = close-up; 8000 shows most of a 2048² map.
    static constexpr int kMinFocusC = 150;
    static constexpr int kMaxFocusC = 8000;

    /// Editor-facing tilt range (degrees). render() may tighten the upper
    /// bound for the current focus/window via stableLeanDeg().
    static constexpr double kMinLeanDeg = 100.0;
    static constexpr double kMaxLeanDeg = 170.0;

    /// Clamp an editor-facing focus value into [kMinFocusC, kMaxFocusC].
    static int clampFocus(int focusC);
    /// Clamp cameraDZ into Terra's own CAMERADZ window constants.
    static int clampCameraDZ(int cameraDZ);
    /// Clamp the tilt into the band that renders cleanly. yside is half the
    /// window height the renderer will scan (buffer height / 2).
    static double stableLeanDeg(double leanDeg, int focusC, int yside);

    View3DRenderer() = default;

    /// "Is a world right now loaded?" — render() is a no-op otherwise. The
    /// check is fresh on every frame, so world reloads need no re-binding.
    static bool isWorldAvailable();

    /// Resize the internal RGB565 buffer (width clamped to kMaxWindowSize and
    /// kept even for a 4-byte-aligned QImage row). Keeps camera settings.
    void resizeWindow(int windowWidth, int windowHeight);
    int windowWidth() const { return width_; }
    int windowHeight() const { return height_; }

    /// Replace the camera state (no clamping beyond render-time guarantees).
    void setCamera(const View3DCamera &camera);
    const View3DCamera &camera() const { return camera_; }

    /// Render one frame centred on the map cell (cx, cy) — out-of-range
    /// coordinates wrap the same way vrtMap::XCYCL/YCYCL do. Nothing happens
    /// without a loaded world.
    void render(int cx, int cy);

    /// RGB565 pixels, row-major (QImage::Format_RGB16 layout), windowWidth()
    /// entries per row; null until the first resizeWindow().
    const unsigned short *pixels() const
    {
        return buffer_.empty() ? nullptr : buffer_.data();
    }

private:
    std::vector<unsigned short> buffer_;
    int width_ = 0;
    int height_ = 0;
    View3DCamera camera_{};
};

} // namespace MapEditor

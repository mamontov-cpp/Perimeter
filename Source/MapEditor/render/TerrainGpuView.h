// ============================================================================
// MapEditorRender — TerrainGpuView.
//
// Owns the game scene stack the GPU oblique viewport renders through:
//
//   EditorSokolRender (device)  ← cVisGeneric ← cScene ← cCamera + cTileMap
//
// The game's real terrain renderer (Source/Render/tilemap, compiled
// unmodified in MapEditorRender) draws the world: a cTileMap fed by an
// EditorTerraInterface (colors from the editor's 2D raster cache so layer
// filters / geo dimming match the TopDown view, heights straight from vMap).
// Rendering lands in whatever GL framebuffer the widget passes per frame
// (the QOpenGLWidget FBO) — the sokol swapchain is retargeted each frame.
//
// The view is a real perspective camera mirroring the CPU oblique view's
// knobs (View3DCamera): depression angle = lean − 90°, camera height above
// the zeroplast plane = focus + lift, focal length (pixels) = focus, azimuth
// from the turn angle. With that mapping the nadir ground coverage per pixel
// is (focus+lift)/focus cells — the same formula the chunky CPU view uses,
// so minimap view-rect math and zoom limits stay valid in GPU mode.
//
// Single-threaded: all calls happen on the Qt UI thread (paintGL/mouse),
// which the render stack accepts because MTConfig::multithreading() is
// stubbed false (see shim/game_stubs.cpp).
// ============================================================================

#pragma once

#include <cstdint>

class cInterfaceRenderDevice;
class cVisGeneric;
class cScene;
class cCamera;
class cTileMap;
// Global scope on purpose (the game's TerraInterface) — a `class TerraInterface`
// inside namespace MapEditor would shadow it.
class TerraInterface;

namespace MapEditor {

class ViewportRenderer;

class TerrainGpuView
{
public:
    TerrainGpuView() = default;
    ~TerrainGpuView();
    TerrainGpuView(const TerrainGpuView&) = delete;
    TerrainGpuView& operator=(const TerrainGpuView&) = delete;

    /// Tile color source (not owned). Swapping sources re-marks the whole
    /// tilemap texture dirty.
    void setColorSource(ViewportRenderer* colors);

    bool isReady() const { return tileMap_ != nullptr; }

    /// Camera knobs — the View3DCamera state translated by the widget:
    /// height stays in world units (cells), while the focal length is in
    /// DEVICE pixels (widget px * devicePixelRatio) so the ground scale
    /// cells-per-widget-px == (height - lift)/focal stays DPR-independent.
    struct CameraState
    {
        double centerX = 0;        ///< look-at x, map cells
        double centerY = 0;        ///< look-at y, map cells
        double turnAngleDeg = 0;   ///< azimuth (yaw)
        double leanDeg = 140;      ///< tilt; 90 = nadir, 180 = horizon
        double heightCells = 700;  ///< camera height above zeroplast plane
        double focalDevPx = 600;   ///< focal length, device pixels
    };
    void setCamera(const CameraState& state) { cameraState_ = state; }

    /// Create the device and scene stack. MUST be called with the target
    /// OpenGL 4.1 core context current (QOpenGLWidget::initializeGL). width/
    /// height are the initial framebuffer size in device pixels. Returns
    /// false (state stays inert) when the world is not loaded or sokol/GL
    /// setup fails.
    bool init(int width, int height);

    /// Tear down everything (device included). Must be called with the GL
    /// context current.
    void shutdown();

    /// Brush stamp: cells in [x0..x1]x[y0..y1] (inclusive) changed — queues
    /// the tilemap texture/geometry rebuild for the next frame.
    void updateCells(int x0, int y0, int x1, int y1);

    /// The world was (re)loaded: rebuild the tilemap from scratch. Same GL
    /// context; call from initializeGL or right after a reload while the
    /// context of the viewport is current.
    void reloadWorld();

    /// Render the current camera view. framebuffer is the target GL FBO
    /// (QOpenGLWidget::defaultFramebufferObject(), never 0); width/height
    /// the FBO size in device pixels.
    void renderFrame(uint32_t framebuffer, int width, int height);

    /// Screen position (device pixels, y down) → ground cell under it by
    /// ray-marching the terrain. Returns false when the ray misses the map
    /// (or the view is not ready).
    bool pickCell(double sxPx, double syPx, int& cellX, int& cellY) const;

    /// World point → screen position in device pixels (y down). Returns
    /// false when the view is not ready.
    bool projectToScreen(double cellX, double cellY, double z,
                         double& sxPx, double& syPx) const;

    /// Ground footprint of the current view: where the rays through the four
    /// screen corners hit the horizontal plane z = planeZ, in world cells.
    /// Order follows the screen: top-left, top-right, bottom-right, bottom-left
    /// (top = the far edge near the horizon). Unlike the axis-aligned rect a
    /// per-corner ray cast gives, this keeps the camera azimuth and the
    /// perspective spread — the far edge covers much more ground than the near
    /// one, so the quad comes out as a rotated trapezoid.
    /// A corner whose ray never reaches the plane (at/above the horizon) is
    /// clamped to a far point along it, so the polygon stays finite.
    /// Returns false when the view is not ready.
    bool groundFootprint(double planeZ, double outX[4], double outY[4]) const;

    /// Project a ground contour through the camera: `count` map-cell points
    /// (cellX, cellY pairs in `cellXY`), each taken at the terrain height under
    /// it, to screen device pixels (y down) in `outSx`/`outSy`. Points that miss
    /// the terrain grid or fall behind the camera are dropped — the return value
    /// is the number of valid points written (0 when the view is not ready).
    /// Used by the brush preview, which must show the true perspective image of
    /// the stamp footprint rather than a heuristic ellipse.
    int projectGroundContour(const double* cellXY, int count,
                             double* outSx, double* outSy) const;

    /// Zeroplast plane height in world units (meters), for the grid plane.
    float zeroplastZ() const;

    /// When true the sokol tilemap sampler uses nearest filtering (chunky
    /// voxel look, no bilinear). When false (default) the default linear
    /// sampler is used (smooth). Call once per frame before renderFrame().
    void setNearestFiltering(bool nearest) { nearestFiltering_ = nearest; }

    bool isNearestFiltering() const { return nearestFiltering_; }

private:
    /// (Re)create scene/camera/tilemap around the current device.
    bool buildScene();
    void destroyScene();
    void applyCamera();

    cInterfaceRenderDevice* device_ = nullptr;
    cVisGeneric* visGeneric_ = nullptr;
    cScene* scene_ = nullptr;
    cCamera* camera_ = nullptr;
    cTileMap* tileMap_ = nullptr;
    /// Owned between buildScene() calls — the tilemap takes the reference
    /// over (dtor Releases it), the pointer is only for recreating.
    ::TerraInterface* terra_ = nullptr;

    ViewportRenderer* colors_ = nullptr;
    CameraState cameraState_;
    bool nearestFiltering_ = false;
};

} // namespace MapEditor

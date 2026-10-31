#pragma once

#include <QColor>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QSize>
#include <QString>
#include <cstdint>
#include <vector>

#include "View3DRenderer.h"
#include "ViewportRenderer.h"

class QOpenGLTextureBlitter;
class QOpenGLFunctions;

namespace MapEditor {

class EditorSession;
class ViewportRenderer;
class BrushTool;
class TerrainGpuView;

// ============================================================================
// MapEditor UI — ViewportWidget (T2.2, T2.3).
//
// Qt viewport over a world raster with two view modes:
//
//   * TopDown (T2.2) — the 2D top-down raster produced by ViewportRenderer
//     (T2.1):
//       - Pan  — middle-button or right-button drag.
//       - Zoom — mouse wheel, centred on the cursor, clamped to the plan
//         range "1:1 … fit" (fit = whole map visible, 1:1 = one map cell per
//         screen pixel; no interpolation beyond 1:1).
//       - Double-click — back to fit.
//   * Oblique (T2.3) — the tilted voxel window of View3DRenderer over
//     vrtMap::viewV2D (window width clamped to MAX_XSIZE_VX_WINDOW=640):
//       - Pan  — middle/right drag; the camera walks on the ground plane.
//       - Orbit — left drag: horizontal moves turn the camera around Z,
//         vertical moves tilt it.
//       - Zoom — mouse wheel, mapped to the focal length (focusC), the
//         centre map cell stays in place.
//     The 3D camera itself is driven both interactively here and from the
//     MainWindow view toolbar (setObliqueCamera/adjust* methods + the
//     obliqueCameraChanged signal). Overlays (grid/brush/markers) still come
//     from the 2D pipeline — they are TopDown-only in T2.3.
//
//   * Renderer style (chunky / smooth — post-T3.3 fix): the world data is
//     one color per map cell; how that data is sampled on the screen is a
//     separate toggle the user drives from the View toolbar:
//       - Chunky — the classic look: one cell = one solid pixel, cell
//         boundaries are hard edges (TopDown zoom stays at/below 1:1, the
//         oblique 3D frame is upscaled nearest-neighbor).
//       - Smooth — the in-game camera look: neighboring cells are blended
//         bilinearly (exact same data the game feeds the GPU as a filtered
//         terrain texture, Source/Game/CopyToGraph.cpp GetTileColor).
//         TopDown zoom may then go above 1:1 so the blending is visible, and
//         the oblique 3D frame is upscaled bilinearly as before.
//
//   * Cursor → map cell conversion (screenToMap) and a coordinate/zoom readout
//     (TopDown mode).
//
//   * Brush editing (T3.1, tools through T3.5) — with a BrushTool attached and
//     brush editing enabled, left-button press/drag stamps the brush in BOTH
//     view modes (undo/redo and the dirty-rect pipeline run inside the tool).
//     TopDown maps the cursor to a cell linearly; Oblique ray-picks the voxel
//     surface through View3DRenderer::pickGroundCell(). Alt+left in Oblique
//     still orbits. The Line form is a press→drag→release two-point stroke.
//     The preview outline follows the cursor (screen-space ring in 3D).
//
//   * Editor interaction modes (View / Brush / Eyedropper) — orthogonal to the
//     view mode and supported in BOTH (2D and 3D):
//       - View      — navigation: left drag pans (TopDown) or orbits
//                     (Oblique), middle/right drag pans, wheel zooms —
//                     the full camera control.
//       - Brush     — the click in the world applies the selected brush: left
//                     press/drag stamps (both view modes), Alt+left orbits in
//                     Oblique ("limited" camera change), middle/right drag
//                     pans, wheel zooms.
//       - Eyedropper — like Brush, but the click/drag samples the color: the
//                     PickColor brush type reads the Geo/Dam pixel (SurBuf
//                     index) at the cursor cell and reports colorPicked (the
//                     picked index lands in BrushTool::params().colorIndex).
//     Alt+left in Oblique still orbits; middle/right drags still pan.
//
// The two CPU-rendered base rasters (the ARGB32 map cache of ViewportRenderer
// and the RGB565 frame of View3DRenderer) are kept on the GPU as persistent GL
// textures of a QOpenGLWidget: a full upload on (re)creation and a
// glTexSubImage2D per dirty rectangle afterwards, drawn through
// QOpenGLTextureBlitter with linear/nearest sampling per the render style —
// the same filtered-terrain-texture technique the game itself uses
// (Source/Game/CopyToGraph.cpp GetTileColor). All overlays (grid, brush
// preview, markers, spawns, HUD) are drawn by QPainter on top of the same
// framebuffer. All vMap access happens in the UI thread (Terra is not
// thread-safe).
// ============================================================================

/// Marker on the map (owned by T7 — the widget only renders what it is given).
struct MapMarker
{
    QString label;
    QString type = QStringLiteral("point"); // point | zone | unit | building
    int mapX = 0;
    int mapY = 0;
    int radius = 0; // zone radius in map cells (0 = plain dot)
    QColor color{Qt::yellow};
};

/// Battle-map spawn point (owned by T8 — the widget only renders it).
struct SpawnPoint
{
    QString label;
    int playerIndex = 0;
    int mapX = 0;
    int mapY = 0;
    QColor color{Qt::red};
};

/// Shape of the brush stamp drawn under the cursor.
enum class BrushPreviewShape
{
    Circle,
    Square,
    Polygon,   ///< regular polygon — vertices from BrushMasks::regularPolygonVertices
};

class ViewportWidget : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget *parent = nullptr);
    ~ViewportWidget() override;

    // -----------------------------------------------------------------------
    // Session / renderer binding
    // -----------------------------------------------------------------------

    /// Attach to a session. Creates and binds an internal ViewportRenderer if
    /// a world is currently loaded, otherwise clears the view. Safe to call
    /// again after every load/unload to rebind.
    void setSession(EditorSession *session);

    EditorSession *session() const { return session_; }
    bool hasWorld() const { return renderer_ != nullptr; }

    /// Rebind / drop the raster after the world was loaded or unloaded while
    /// this session pointer stayed the same.
    void reloadWorld();

    /// The renderer the view draws through (nullptr when no world is shown).
    /// Used by later tasks (layer filters, overlays, eyedropper).
    ViewportRenderer *renderer() const { return renderer_; }

    // -----------------------------------------------------------------------
    // Brush editing (T3.1)
    // -----------------------------------------------------------------------

    /// Attach the brush tool the Brush mode paints with (and the Eyedropper
    /// mode samples through the PickColor brush type). Pass nullptr to return
    /// the viewport to a plain navigate-only widget.
    void setBrushTool(BrushTool *brush);
    BrushTool *brushTool() const { return brushTool_; }

    // -----------------------------------------------------------------------
    // Editor interaction mode (View / Brush / Eyedropper)
    // -----------------------------------------------------------------------

    /// What a mouse click in the world does (independent of the 2D/3D view
    /// mode).
    enum class EditorMode
    {
        View,       ///< navigation only: drag pans (TopDown) / orbits (Oblique).
        Brush,      ///< left press/drag applies the brush stamp (both view modes).
        Eyedropper, ///< left press/drag samples the Geo/Dam pixel color (T4.2).
    };

    /// Switch what a click in the world does. The mode is orthogonal to the
    /// 2D/3D view mode: navigation keeps working in every mode (middle/right
    /// drag pan, wheel zoom; View mode adds left-drag pan/orbit, Brush and
    /// Eyedropper limit the camera to Alt+left orbit / pan / zoom).
    void setEditorMode(EditorMode mode);
    EditorMode editorMode() const { return editorMode_; }
    /// True when the current mode paints with the brush (mode Brush with a
    /// tool attached) — the viewport's internal paint-path guard.
    bool isBrushEditingEnabled() const
    {
        return editorMode_ == EditorMode::Brush && brushTool_ != nullptr;
    }

    // -----------------------------------------------------------------------
    // View state
    // -----------------------------------------------------------------------

    /// Which representation the widget shows.
    enum class ViewMode
    {
        TopDown, ///< Flat 1:1 palette view with overlays (T2.1/T2.2).
        Oblique, ///< Tilted 3D voxel window over viewV2D (T2.3).
    };

    ViewMode viewMode() const { return viewMode_; }
    /// Switch the visible mode. Each mode keeps its own camera state.
    void setViewMode(ViewMode mode);
    void toggleViewMode()
    {
        setViewMode(viewMode_ == ViewMode::TopDown ? ViewMode::Oblique
                                                   : ViewMode::TopDown);
    }

    // ---- TopDown state -----------------------------------------------------

    /// How the per-cell raster is sampled on the screen.
    enum class RenderStyle
    {
        Chunky, ///< Nearest-neighbor: solid cell blocks, zoom ≤ 1:1 (TopDown).
        Smooth, ///< Bilinear: in-game-like filtered look; zoom up to
                /// kMaxSmoothZoom in TopDown, bilinear 3D upscale.
    };

    /// Upper zoom bound for the TopDown view in Smooth style — a close-up
    /// shows soft gradients like the game camera, magnification above this
    /// adds nothing (the data is one color per cell).
    static constexpr double kMaxSmoothZoom = 4.0;

    RenderStyle renderStyle() const { return renderStyle_; }
    /// Switch the sampling style. In TopDown Chunky mode zooms above 1:1 are
    /// clamped back (with the view centre kept stable); Smooth keeps them.
    void setRenderStyle(RenderStyle style);

    /// Zoom: 1.0 = 1 map cell per screen pixel, fitScale() = whole map visible.
    double zoom() const { return zoom_; }
    /// Lower zoom bound for the current map/widget size.
    double fitScale() const { return fitScale_; }

    /// Centre the map on the widget without changing the zoom.
    void centerOnMap();
    /// Fit the whole map into the widget.
    void zoomToFit();
    /// One zoom step for the toolbar actions; works in both modes
    /// (factor > 1 zooms in, < 1 zooms out).
    void zoomStep(double factor);

    // ---- Oblique (3D) state -------------------------------------------------

    /// Read-only camera state of the 3D view (editor units, see View3DCamera).
    const View3DCamera &obliqueCamera() const { return obliqueCamera_; }

    /// Replace the whole camera state (clamped inside).
    void setObliqueCamera(const View3DCamera &camera);

    /// Fine-grained camera helpers for the view toolbar; each one clamps the
    /// affected fields and emits obliqueCameraChanged() when something moved.
    /// In the game camera mode (see setGameCameraMode) the rotation/tilt
    /// helpers drive the game psi/theta instead of the editor knobs.
    void adjustObliqueRotation(double deltaDeg); ///< ± degrees around Z.
    void adjustObliqueTilt(double deltaDeg);     ///< ± degrees of lean.
    void adjustObliqueLift(int deltaCells);      ///< ± cameraDZ cells.
    void adjustObliqueFocus(int deltaCells);     ///< ± focal length cells.

    /// The rotation (azimuth) of the ACTIVE 3D camera in degrees [0,360) —
    /// the editor turnAngleDeg, or the game camera psi in the game camera
    /// mode.
    double rotationDeg() const;
    /// Set the rotation of the active 3D camera to an absolute value
    /// (wrapped into [0,360) like the orbit drag does). Routes to the game
    /// psi in the game camera mode; no-op (no signal) when the value is
    /// already there.
    void setRotationDeg(double deg);

    /// Back to the default camera and map centre.
    void resetObliqueView();

    // ---- Game camera mode (the main game's camera model) -------------------

    /// Camera of the game model (terCameraType CameraCoordinate,
    /// Source/Game/CameraManager.cpp) the GPU oblique view uses when the game
    /// camera mode is on. psi/theta/distance are the raw game values, the
    /// FOV is the game's HardwareCameraFocus (0.8) — set in TerrainGpuView.
    struct GameCameraState
    {
        double centerX = 0;      ///< look-at x, map cells
        double centerY = 0;      ///< look-at y, map cells
        double psiDeg = 0;       ///< azimuth (CameraCoordinate psi)
        double thetaDeg = 0;     ///< polar tilt (0 = nadir, like the game)
        double distance = 300;   ///< game zoom (CameraCoordinate distance)
    };

    bool isGameCameraMode() const { return gameCameraMode_; }
    /// True when the GPU terrain path rendered at least one frame — the game
    /// camera model is unavailable without it (the CPU voxel renderer has no
    /// frustum/FOV model). The same state gpuViewReadyChanged() announces.
    bool gpuViewReady() const { return lastGpuViewReady_; }

    /// Switch the Oblique camera between the editor model and the main game's
    /// model (psi/theta/distance + the game FOV). Entering the mode aims the
    /// camera at `presetCenter` with the game start-of-battle defaults
    /// (psi=0, theta=0 — nadir, distance=300); re-entering re-applies the
    /// preset. Returns false when the GPU path is not available.
    bool setGameCameraMode(bool gameMode, const QPointF &presetCenter);

    // -----------------------------------------------------------------------
    // Current view rectangle (T2.4)
    // -----------------------------------------------------------------------

    /// The map region the viewport is looking at, in map cells:
    ///   - TopDown: the actual visible cell range;
    ///   - Oblique: the approximate ground footprint of the 3D window around
    ///     the look-at centre (the oblique projection is not a linear map
    ///     mapping, so the footprint is derived from the viewV2D basis).
    /// Both are clipped to the map; empty when nothing is loaded/visible.
    /// MinimapWidget (T2.4) draws this rectangle; a change is announced
    /// through the viewRectChanged signal after every repaint that moved it
    /// (pan, zoom, orbit, mode switch, resize, world load/unload).
    QRect currentViewRect() const;

    /// The same region as a polygon in map cells — what MinimapWidget draws.
    ///   - TopDown: the four corners of currentViewRect().
    ///   - Oblique + GPU view: the ground footprint of the frustum, one ray
    ///     per screen corner intersected with the zeroplast plane, so the
    ///     outline keeps the camera azimuth and the perspective spread (a
    ///     rotated trapezoid, wide at the far edge).
    ///   - Oblique CPU fallback: the ground rectangle of the affine viewV2D
    ///     basis, rotated by the turn angle.
    /// Empty when nothing is loaded/visible. Changes are announced through
    /// viewPolygonChanged() at the same points as viewRectChanged().
    QPolygonF currentViewPolygon() const;

    /// Centre the viewport on the given map cell (wrapped like XCYCL/YCYCL).
    /// In Oblique mode it moves the look-at centre — the minimap click
    /// navigation (T2.4).
    void centerOnCell(const QPoint &cell);

    // -----------------------------------------------------------------------
    // Overlays
    // -----------------------------------------------------------------------

    void setBrushPreview(BrushPreviewShape shape, int radius, int innerRadius = 0);
    BrushPreviewShape brushPreviewShape() const { return brushShape_; }
    int brushPreviewRadius() const { return brushRadius_; }
    int brushPreviewInnerRadius() const { return brushPreviewInnerRadius_; }
    void setBrushPreviewVisible(bool visible);
    /// Regular-polygon preview geometry (BrushPreviewShape::Polygon): the
    /// vertex count and the first-vertex angle offset in degrees — the same
    /// values BrushParams carries, so the outline shows exactly the stamp.
    void setBrushPreviewPolygon(int sides, double angleDeg);

    void setGridVisible(bool visible);
    bool isGridVisible() const { return gridVisible_; }

    /// Grid cell size in map cells — the editable steps of the grid overlay
    /// (both 2D lines and the 3D surface overlay draw them). Clamped to
    /// [1, 1024]; the defaults are Terra's own kmGrid step (4).
    void setGridStep(int cellsX, int cellsY);
    int gridStepX() const { return gridStepX_; }
    int gridStepY() const { return gridStepY_; }

    /// The in-game look: cells without a dam are dimmed to grey shades in
    /// the 2D raster and the smooth 3D pyramid (game-like contrast between
    /// the terraformed dam areas and the raw geo). Viewport-only.
    void setGeoDimmed(bool dimmed);
    bool isGeoDimmed() const { return geoDimmed_; }

    /// Freeze-dam 3D scroll: the torus repeats render geo-only, so only the
    /// geo layer scrolls while the dam stays in its single original place
    /// (see View3DRenderer::setDamFrozen). Smooth 3D style only.
    void setDamFrozen(bool frozen);
    bool isDamFrozen() const { return damFrozen_; }

    void setMarkers(const std::vector<MapMarker> &markers);
    const std::vector<MapMarker> &markers() const { return markers_; }

    void setSpawnPoints(const std::vector<SpawnPoint> &spawns);
    const std::vector<SpawnPoint> &spawnPoints() const { return spawns_; }

    /// Map cell under the cursor (-1,-1 when outside the map).
    QPoint cursorCell() const { return cursorCell_; }

    /// Map cell under an arbitrary widget position in the current view mode:
    /// TopDown uses the linear screenToMap(); Oblique ray-picks the rendered
    /// voxel surface through View3DRenderer::pickGroundCell(). Returns
    /// (-1,-1) for sky / off-window positions.
    QPoint cellAtWidgetPos(const QPoint &widgetPos) const;

    /// Trigger a fresh 3D render on the next paint event (used by undo/redo).
    void setNeeds3DRedraw() { needs3DRedraw_ = true; }

    // -----------------------------------------------------------------------
    // Coordinate conversion (public: reused by tool panels / tests)
    // -----------------------------------------------------------------------

    /// Top-left corner of a map cell in widget coordinates.
    QPointF mapToScreen(double mapX, double mapY) const;
    /// Map cell containing widget coordinates (sx, sy); no clamping.
    QPoint screenToMap(int sx, int sy) const;

signals:
    /// Emitted when the cursor moves over a different map cell.
    void cursorMapPosition(int mapX, int mapY);
    /// Emitted after setViewMode().
    void viewModeChanged(MapEditor::ViewportWidget::ViewMode mode);
    /// Emitted after setRenderStyle().
    void renderStyleChanged(MapEditor::ViewportWidget::RenderStyle style);
    /// Emitted after every successful 3D camera change (toolbar, wheel,
    /// orbit, pan) so MainWindow can refresh its readout/labels. Also fired
    /// when the GAME camera mode rotates/resets — the payload is the
    /// (unchanged) editor camera there, so consumers needing the effective
    /// azimuth must read rotationDeg().
    void obliqueCameraChanged(const MapEditor::View3DCamera &camera);
    /// Emitted after a repaint in which currentViewRect() changed (pan, zoom,
    /// orbit, mode switch, resize, world load/unload) — main consumers are
    /// the minimap panel (T2.4) and tool panel readouts.
    void viewRectChanged(const QRect &rect);
    /// Emitted after a repaint when currentViewPolygon() changed —
    /// the minimap panel consumes this for a rotated/trapezoidal 3D zone
    /// outline instead of an axis-aligned rect.
    void viewPolygonChanged(const QPolygonF &poly);
    /// Emitted right after a brush stamp changed the world (T3.1) so the
    /// MainWindow can refresh the modified state in the status bar.
    void worldEdited();
    /// Emitted after a repaint that repainted terrain cells into the 2D cache
    /// (a brush stamp, undo/redo, an image import, a palette edit). Lighting
    /// has been recomputed (renderQuant) and the cache is final at this point,
    /// so the overview panels can re-sample the world from the live terrain.
    /// Consumed by MinimapWidget — map.tga only catches up on save.
    void terrainRepainted();
    /// Emitted after setEditorMode() — MainWindow keeps its mode buttons in
    /// sync with it (the interactions can also switch the mode internally).
    void editorModeChanged(MapEditor::ViewportWidget::EditorMode mode);
    /// Emitted when the eyedropper is triggered (T3.3/T4.2) — provides the
    /// map cell coordinates and the current SurBuf palette index at that cell.
    void eyedropperPicked(int mapX, int mapY, unsigned char surIndex);
    /// Emitted once after the first successful GPU terrain view init (or
    /// after a reload that drops it back to the CPU path). MainWindow
    /// hides the freeze-dam action (which only makes sense for the CPU path)
    /// when the GPU path is available.
    void gpuViewReadyChanged(bool gpuReady);
    /// Emitted after setGameCameraMode() — MainWindow keeps the two camera
    /// preset actions (like in game / like in editor) in sync with it.
    void gameCameraModeChanged(bool gameMode);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateFitScale();
    void clampZoom();

    /// Push geo dimming + the centred 3D look-at into a freshly bound
    /// renderer (called from setSession/reloadWorld after bindWorld()).
    void applyViewportSettings();

    // --- Hardware-accelerated base rasters (QOpenGLWidget) -----------------
    //
    // The CPU renderers keep producing the two pixel buffers (the ARGB32 map
    // cache of ViewportRenderer and the RGB565 oblique frame of
    // View3DRenderer); their presentation is GPU-driven:
    //   * drawTopDownGL / drawObliqueGL upload the buffers into persistent GL
    //     textures — full upload on (re)creation, glTexSubImage2D per dirty
    //     rectangle for map edits afterwards — and draw them with
    //     QOpenGLTextureBlitter (linear/nearest sampling per the render
    //     style).
    //   * Everything above the base raster (grid, brush preview, markers,
    //     spawns, HUD) is QPainter as before, drawn on the same framebuffer
    //     after the native GL pass.

    /// Draw the TopDown base raster: refresh the map texture from `dirty`
    /// (full upload when the texture was just created), clear and blit.
    /// Returns false when there is nothing drawable (no bound world).
    bool drawTopDownGL(QOpenGLFunctions *gl,
                       const std::vector<DirtyRect> &dirty);
    /// (Re)render the oblique frame when needed (needs3DRedraw_), bake the
    /// grid overlay, upload it into the frame texture and blit. Returns false
    /// when there is nothing drawable (no bound world / no rendered frame).
    bool drawObliqueGL(QOpenGLFunctions *gl);
    /// Create the map texture when missing/resize (returns true then — the
    /// caller must upload the full image).
    bool ensureMapTexture(QOpenGLFunctions *gl, int mapW, int mapH);
    /// Create the frame texture when missing/resize and upload the current
    /// RGB565 frame of view3d_ in full.
    void uploadFrameTexture(QOpenGLFunctions *gl);
    /// Delete the GL blitter and both textures (context must be current).
    void releaseGLResources();

    void drawGrid(QPainter &painter);
    /// GPU-mode grid: zeroplast-plane grid lines projected through the sokol
    /// camera (the CPU 3D view bakes the same grid into its frame).
    void drawGrid3D(QPainter &painter);
    void drawBrushPreview(QPainter &painter);
    void drawMarkers(QPainter &painter);
    void drawSpawnPoints(QPainter &painter);
    void drawHud(QPainter &painter);
    QRect visibleCellRange() const;

    /// Rendered 3D buffer size drawObliqueGL() uses (widget size clamped to
    /// Terra's window limits) — shared with currentViewRect() so the minimap
    /// footprint matches what the next 3D paint will show.
    QSize obliqueBufferSize() const;

    /// Compare currentViewRect() with the last emitted one and emit
    /// viewRectChanged() when it moved (called from paintGL, so every
    /// pan/zoom/orbit/mode/resize change is caught, whatever triggered it).
    void emitViewRectIfChanged();

    /// Pan of the 3D camera over the ground plane after a screen-drag of the
    /// given pixel deltas — inverse view-basis mapping, see the .cpp.
    void panObliqueBy(double dxCursorPx, double dyCursorPx);

    /// Game camera mode pan: drag-the-terrain — pick the ground cell under
    /// the cursor (device-pixel widget position) and keep it under the
    /// cursor while the drag lasts. Uses the real GPU camera, so the
    /// perspective/FOV does not matter.
    void panObliqueGame(const QPointF &widgetPos);
    /// Game camera helpers behind adjustObliqueRotation/adjustObliqueTilt
    /// and zoomStep when the game camera mode is on.
    void adjustGameRotation(double deltaDeg);
    void adjustGameTilt(double deltaDeg);
    void adjustGameDistance(double factor);

    // --- GPU oblique path (TerrainGpuView) ---------------------------------
    //
    // When the MapEditorRender target is linked (MAP_EDITOR_GPU_VIEW) the
    // Oblique view renders through the game's sokol terrain renderer into
    // this widget's FBO; the CPU voxel renderer (view3d_) then only serves
    // as the fallback when GL/sokol setup fails. setSession/reloadWorld run
    // without a current GL context, so the GPU scene (re)build is deferred
    // to the next paintGL (flag below).

    /// True when the GPU path currently renders the Oblique view.
    bool gpuActive() const;
    /// Rebuild/delete the GPU scene at the next paintGL (context current).
    void scheduleGpuRebuild() { gpuRebuildPending_ = true; }
    /// Immediate rebuild — must be called with the GL context current
    /// (initializeGL / paintGL).
    void rebuildGpuView();
    /// Push obliqueCamera_ + obliqueCenter_ into the GPU view (no GL calls —
    /// safe from mouse handlers).
    void applyGpuCamera();
    /// The lean the oblique math uses: the CPU voxel renderer clamps it into
    /// its stable band (View3DRenderer::stableLeanDeg), the GPU camera does
    /// not. Shared by currentViewRect/pan/preview/grid math.
    double obliqueMathLeanDeg() const;
    /// Ground cells per widget pixel across the view (x): (F+Z)/F scaled by
    /// the CPU buffer/widget ratio in the chunky window, plain (F+Z)/F in
    /// GPU mode.
    double obliqueCellsPerWidgetPx() const;

    /// Cursor shape for the current editor mode: cross over a brush/pipette
    /// working area, arrow for pure navigation.
    Qt::CursorShape paintCursorShape() const
    {
        return editorMode_ == EditorMode::View ? Qt::ArrowCursor
                                               : Qt::CrossCursor;
    }

    /// The pick action of the Eyedropper mode at one cell: run the PickColor
    /// brush type through BrushTool (the common brush interface — the sample
    /// lands in BrushTool::params().colorIndex) and emit eyedropperPicked().
    /// Returns true when a color was sampled.
    bool pickColorAtCell(const QPoint &cell);

    /// Convert a widget position into a valid, on-map cell in the current
    /// view mode; (-1,-1) when the point is outside the map or nothing is
    /// loaded. Shared by the paint/pick press paths.
    QPoint onMapCellAtWidgetPos(const QPoint &widgetPos) const;

    /// Screen-space ground scale of the 3D view: widget pixels per ground cell
    /// across the view (x) and along the view direction (y, the kv-scaled
    /// depth axis). Used by the 3D brush preview ring.
    QPointF obliquePxPerCell() const;

    /// GPU oblique path: the stamp footprint projected through the perspective
    /// camera — the boundary of the disc/square of `brushRadius_` cells centred
    /// on cursorCell_, sampled at the terrain height. Widget pixels; empty when
    /// the GPU view is not ready or too few points projected.
    QPolygonF projectedBrushContour() const;

    EditorSession *session_ = nullptr;
    ViewportRenderer *renderer_ = nullptr;

    // T3.1 — brush painting: left-drag applies stamps (both view modes) when
    // the editor mode is Brush.
    BrushTool *brushTool_ = nullptr;
    /// What a click in the world does (View / Brush / Eyedropper).
    EditorMode editorMode_ = EditorMode::Brush;
    bool brushPainting_ = false;
    QPoint lastPaintCell_{-1, -1};
    /// Eyedropper drag: left held down re-picks at every new cell (the brush
    /// mode stamps along the drag, the eyedropper samples along it).
    bool picking_ = false;
    QPoint lastPickCell_{-1, -1};
    /// Line-shape stroke: the press marks lineStart_, the release draws one
    /// Bresenham line from the start to the release cell (single undo entry).
    bool linePending_ = false;
    QPoint lineStart_{-1, -1};
    /// Widget position of the line-stroke press — the 3D preview has no
    /// cell→screen mapping, so the preview line runs between the two cursor
    /// positions.
    QPoint lineStartWidgetPos_{-1, -1};
    /// Widget position of the cursor — the 3D preview ring is drawn in screen
    /// space (the oblique projection has no linear cell→screen mapping).
    QPoint cursorWidgetPos_{-1, -1};

    /// Oblique mode: true when the 3D buffer needs a fresh render (camera
    /// changed, terrain stamped, window resized). When false, paintEvent()
    /// blits the cached RGB565 frame without calling viewV2D() again.
    bool needs3DRedraw_ = true;

    ViewMode viewMode_ = ViewMode::TopDown;
    View3DRenderer *view3d_ = nullptr;

    /// GPU oblique path — the game's sokol terrain renderer drawing into this
    /// widget's FBO. Null when the MapEditorRender target is absent or GL/
    /// sokol setup failed; the CPU renderer (view3d_) is the fallback then.
    TerrainGpuView *gpuView_ = nullptr;
    /// setSession/reloadWorld happen without a current GL context — the GPU
    /// scene (re)build is deferred to the next paintGL.
    bool gpuRebuildPending_ = false;
    /// Whether the GPU scene was ready the last time we checked in paintGL.
    /// Used to emit gpuViewReadyChanged() only on transitions (one-shot
    /// reveal/hide of the freeze-dam action in MainWindow).
    bool lastGpuViewReady_ = false;

    // --- GL presentation state (created in initializeGL, see above) --------
    QOpenGLTextureBlitter *glBlitter_ = nullptr;
    /// ARGB32 map cache texture (GL_RGBA/GL_BGRA upload).
    unsigned int mapTexture_ = 0;
    QSize mapTextureSize_;
    /// RGB565 oblique frame texture (GL_RGB/GL_UNSIGNED_SHORT_5_6_5 upload).
    unsigned int frameTexture_ = 0;
    QSize frameTextureSize_;

    /// How the per-cell raster is sampled on the screen (post-T3.3 fix).
    RenderStyle renderStyle_ = RenderStyle::Smooth;

    double zoom_ = 1.0;
    double fitScale_ = 1.0;
    QPointF panOffset_;

    // 3D camera and look-at; wrap of obliqueCenter_ happens at render time.
    View3DCamera obliqueCamera_{};
    QPointF obliqueCenter_;

    // Game camera mode (setGameCameraMode): the game camera model the GPU
    // view renders with instead of obliqueCamera_/obliqueCenter_.
    GameCameraState gameCamera_;
    bool gameCameraMode_ = false;
    /// Ground cell picked under the cursor when the game-mode pan started
    /// (drag-the-terrain anchor; invalid until the first successful pick).
    QPointF gamePanGrabCell_{-1, -1};
    bool gamePanGrabValid_ = false;

    BrushPreviewShape brushShape_ = BrushPreviewShape::Circle;
    int brushRadius_ = kFineGridCell;
    /// Inner radius of the band for the outline shapes; 0 = no inner ring.
    int brushPreviewInnerRadius_ = 0;
    /// Regular-polygon preview geometry (BrushPreviewShape::Polygon).
    int brushPolygonSides_ = 6;
    double brushPolygonAngleDeg_ = 0.0;
    bool brushPreviewVisible_ = false;
    bool gridVisible_ = false;
    /// Editable grid cell size in map cells (kFineGridCell = Terra's kmGrid).
    int gridStepX_ = kFineGridCell;
    int gridStepY_ = kFineGridCell;
    bool geoDimmed_ = false;
    bool damFrozen_ = false;

    QPoint cursorCell_{-1, -1};
    QPoint lastCursorCell_{-1, -1};

    bool panning_ = false;
    QPointF panStart_;
    /// Orbit drag (left button, Oblique mode).
    bool orbiting_ = false;

    /// View rectangle emitted through viewRectChanged the last time
    /// (guard against repeated identical emissions).
    QRect lastEmittedViewRect_;
    /// Polygon emitted through viewPolygonChanged the last time.
    QPolygonF lastEmittedViewPolygon_;

    std::vector<MapMarker> markers_;
    std::vector<SpawnPoint> spawns_;

    /// Default grid step in map cells: 1 << kmGrid (VMAP.h:89).
    static constexpr int kFineGridCell = 4;
    /// Screen pixels a grid line must occupy before it is worth drawing.
    static constexpr double kMinGridSpacingPx = 4.0;
    /// Margin kept around the map when fitting.
    static constexpr int kFitMarginPx = 12;
    /// Wheel zoom step / default toolbar zoom factor.
    static constexpr double kZoomStep = 1.25;
    /// Degrees the orbit drag moves for one screen pixel.
    static constexpr double kOrbitDegPerPx = 0.3;

    // Game camera mode constants — the main game's own values
    // (Source/Game/CameraManager.cpp, Source/Scripts/Config.prm).
    /// HardwareCameraFocus — the game FOV knob (clip-space focal length).
    static constexpr double kGameCameraFocus = 0.8;
    /// CAMERA_ZOOM_MIN / CAMERA_ZOOM_MAX (the terCameraType distance clamp).
    static constexpr double kGameDistanceMin = 300.0;
    static constexpr double kGameDistanceMax = 5000.0;
    /// CAMERA_THETA_MAX of the restricted game camera (PI/2.85), degrees.
    static constexpr double kGameThetaMaxDeg = 63.1579;
};

} // namespace MapEditor


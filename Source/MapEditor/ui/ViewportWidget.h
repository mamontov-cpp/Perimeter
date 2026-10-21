#pragma once

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "View3DRenderer.h"

namespace MapEditor {

class EditorSession;
class ViewportRenderer;
class BrushTool;

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
//   * Cursor → map cell conversion (screenToMap) and a coordinate/zoom readout
//     (TopDown mode).
//
//   * Brush editing (T3.1) — with a BrushTool attached and brush editing
//     enabled, left-button press/drag in TopDown mode stamps the brush
//     (undo/redo and dirty-rect pipeline run inside the tool); the preview
//     outline follows the cursor.
//
// Both base rasters are painted from zero-copy QImages: the ARGB32 cache of
// ViewportRenderer and the RGB16 (565) frame of View3DRenderer.
// All vMap access happens in the UI thread (Terra is not thread-safe).
// ============================================================================

/// Marker on the map (owned by T7 — the widget only renders what it is given).
struct MapMarker
{
    QString label;
    int mapX = 0;
    int mapY = 0;
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
};

class ViewportWidget : public QWidget
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

    /// Attach the brush tool that left-button drag applies in TopDown mode.
    /// Pass nullptr (or disable below) to return the viewport to a plain
    /// navigate-only widget.
    void setBrushTool(BrushTool *brush);
    BrushTool *brushTool() const { return brushTool_; }
    /// Brush painting on/off (checked state of the UI toggle).
    void setBrushEditingEnabled(bool enabled);
    bool isBrushEditingEnabled() const
    {
        return brushEditingEnabled_ && brushTool_ != nullptr;
    }

    /// T3.3 — eyedropper mode: when enabled, a right-click in TopDown mode
    /// reads the SurBuf index under the cursor and emits eyedropperPicked().
    void setEyedropperEnabled(bool enabled);
    bool isEyedropperEnabled() const { return eyedropperEnabled_; }

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
    void adjustObliqueRotation(double deltaDeg); ///< ± degrees around Z.
    void adjustObliqueTilt(double deltaDeg);     ///< ± degrees of lean.
    void adjustObliqueLift(int deltaCells);      ///< ± cameraDZ cells.
    void adjustObliqueFocus(int deltaCells);     ///< ± focal length cells.

    /// Back to the default camera and map centre.
    void resetObliqueView();

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

    /// Centre the viewport on the given map cell (wrapped like XCYCL/YCYCL).
    /// In Oblique mode it moves the look-at centre — the minimap click
    /// navigation (T2.4).
    void centerOnCell(const QPoint &cell);

    // -----------------------------------------------------------------------
    // Overlays
    // -----------------------------------------------------------------------

    void setBrushPreview(BrushPreviewShape shape, int radius);
    BrushPreviewShape brushPreviewShape() const { return brushShape_; }
    int brushPreviewRadius() const { return brushRadius_; }
    void setBrushPreviewVisible(bool visible);

    void setGridVisible(bool visible);
    bool isGridVisible() const { return gridVisible_; }

    void setMarkers(const std::vector<MapMarker> &markers);
    const std::vector<MapMarker> &markers() const { return markers_; }

    void setSpawnPoints(const std::vector<SpawnPoint> &spawns);
    const std::vector<SpawnPoint> &spawnPoints() const { return spawns_; }

    /// Map cell under the cursor (-1,-1 when outside the map).
    QPoint cursorCell() const { return cursorCell_; }

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
    /// Emitted after every successful 3D camera change (toolbar, wheel,
    /// orbit, pan) so MainWindow can refresh its readout/labels.
    void obliqueCameraChanged(const MapEditor::View3DCamera &camera);
    /// Emitted after a repaint in which currentViewRect() changed (pan, zoom,
    /// orbit, mode switch, resize, world load/unload) — main consumers are
    /// the minimap panel (T2.4) and tool panel readouts.
    void viewRectChanged(const QRect &rect);
    /// Emitted right after a brush stamp changed the world (T3.1) so the
    /// MainWindow can refresh the modified state in the status bar.
    void worldEdited();
    /// Emitted when the eyedropper is triggered (T3.3) — provides the
    /// map cell coordinates and the current SurBuf palette index at that cell.
    void eyedropperPicked(int mapX, int mapY, unsigned char surIndex);

protected:
    void paintEvent(QPaintEvent *event) override;
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

    void drawBaseRaster(QPainter &painter);
    void draw3DRaster(QPainter &painter);
    void drawGrid(QPainter &painter);
    void drawBrushPreview(QPainter &painter);
    void drawMarkers(QPainter &painter);
    void drawSpawnPoints(QPainter &painter);
    void drawHud(QPainter &painter);
    QRect visibleCellRange() const;

    /// Rendered 3D buffer size draw3DRaster() uses (widget size clamped to
    /// Terra's window limits) — shared with currentViewRect() so the minimap
    /// footprint matches what the next 3D paint will show.
    QSize obliqueBufferSize() const;

    /// Compare currentViewRect() with the last emitted one and emit
    /// viewRectChanged() when it moved (called from paintEvent, so every
    /// pan/zoom/orbit/mode/resize change is caught, whatever triggered it).
    void emitViewRectIfChanged();

    /// Pan of the 3D camera over the ground plane after a screen-drag of the
    /// given pixel deltas — inverse view-basis mapping, see the .cpp.
    void panObliqueBy(double dxCursorPx, double dyCursorPx);

    EditorSession *session_ = nullptr;
    ViewportRenderer *renderer_ = nullptr;

    // T3.1 — brush painting: left-drag applies stamps (TopDown mode).
    BrushTool *brushTool_ = nullptr;
    bool brushEditingEnabled_ = true;
    bool brushPainting_ = false;
    QPoint lastPaintCell_{-1, -1};

    // T3.3 — eyedropper: right-click reads SurBuf at cursor.
    bool eyedropperEnabled_ = false;

    ViewMode viewMode_ = ViewMode::TopDown;
    View3DRenderer *view3d_ = nullptr;

    double zoom_ = 1.0;
    double fitScale_ = 1.0;
    QPointF panOffset_;

    // 3D camera and look-at; wrap of obliqueCenter_ happens at render time.
    View3DCamera obliqueCamera_{};
    QPointF obliqueCenter_;

    BrushPreviewShape brushShape_ = BrushPreviewShape::Circle;
    int brushRadius_ = kFineGridCell;
    bool brushPreviewVisible_ = false;
    bool gridVisible_ = true;

    QPoint cursorCell_{-1, -1};
    QPoint lastCursorCell_{-1, -1};

    bool panning_ = false;
    QPointF panStart_;
    /// Orbit drag (left button, Oblique mode).
    bool orbiting_ = false;

    /// View rectangle emitted through viewRectChanged the last time
    /// (guard against repeated identical emissions).
    QRect lastEmittedViewRect_;

    std::vector<MapMarker> markers_;
    std::vector<SpawnPoint> spawns_;

    /// Grid steps in map cells: 1 << kmGrid and 1 << kmGridChA (VMAP.h:89,121).
    static constexpr int kFineGridCell = 4;
    static constexpr int kCoarseGridCell = 64;
    /// Screen pixels a grid line must occupy before it is worth drawing.
    static constexpr double kMinGridSpacingPx = 4.0;
    /// Margin kept around the map when fitting.
    static constexpr int kFitMarginPx = 12;
    /// Wheel zoom step / default toolbar zoom factor.
    static constexpr double kZoomStep = 1.25;
    /// Degrees the orbit drag moves for one screen pixel.
    static constexpr double kOrbitDegPerPx = 0.3;
};

} // namespace MapEditor


#pragma once

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "View3DRenderer.h"

namespace MapEditor {

class EditorSession;
class ViewportRenderer;

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
    void viewModeChanged(MapEditor::ViewportWidget::ViewMode mode);;    /// Emitted after every successful 3D camera change (toolbar, wheel,
    /// orbit, pan) so MainWindow can refresh its readout/labels.
    void obliqueCameraChanged(const MapEditor::View3DCamera &camera);

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

    /// Pan of the 3D camera over the ground plane after a screen-drag of the
    /// given pixel deltas — inverse view-basis mapping, see the .cpp.
    void panObliqueBy(double dxCursorPx, double dyCursorPx);

    EditorSession *session_ = nullptr;
    ViewportRenderer *renderer_ = nullptr;

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


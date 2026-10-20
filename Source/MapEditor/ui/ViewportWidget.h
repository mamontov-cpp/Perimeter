#pragma once

#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <vector>

namespace MapEditor {

class EditorSession;
class ViewportRenderer;

// ============================================================================
// MapEditor UI — ViewportWidget (T2.2).
//
// Qt viewport that shows the 2D top-down raster produced by ViewportRenderer
// (T2.1) and adds editor navigation/preview on top of it:
//
//   * Pan  — middle-button or right-button drag.
//   * Zoom — mouse wheel, centred on the cursor, clamped to the plan range
//            "1:1 … fit" (fit = whole map visible, 1:1 = one map cell per
//            screen pixel; no interpolation beyond 1:1).
//   * Double-click — back to fit.
//
//   * Cursor → map cell conversion (screenToMap) and a coordinate/zoom readout.
//   * Brush preview — filled-outline circle/square stamp at the cursor cell.
//   * Grid — fine (kmGrid, 4 cells) and coarse (kmGridChA, 64 cells) lines,
//     drawn only where they are actually resolvable on screen.
//   * Markers and spawn points — overlay dots/labels (data is fed in from
//     T7/T8; the widget itself only draws them).
//
// The base raster is painted by wrapping ViewportRenderer's ARGB32 cache in a
// zero-copy QImage and letting QPainter scale it — no per-cell fillRect loop.
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

    /// Zoom: 1.0 = 1 map cell per screen pixel, fitScale() = whole map visible.
    double zoom() const { return zoom_; }
    /// Lower zoom bound for the current map/widget size.
    double fitScale() const { return fitScale_; }

    /// Centre the map on the widget without changing the zoom.
    void centerOnMap();
    /// Fit the whole map into the widget.
    void zoomToFit();

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
    void drawGrid(QPainter &painter);
    void drawBrushPreview(QPainter &painter);
    void drawMarkers(QPainter &painter);
    void drawSpawnPoints(QPainter &painter);
    void drawHud(QPainter &painter);
    QRect visibleCellRange() const;

    EditorSession *session_ = nullptr;
    ViewportRenderer *renderer_ = nullptr;

    double zoom_ = 1.0;
    double fitScale_ = 1.0;
    QPointF panOffset_;

    BrushPreviewShape brushShape_ = BrushPreviewShape::Circle;
    int brushRadius_ = kFineGridCell;
    bool brushPreviewVisible_ = false;
    bool gridVisible_ = true;

    QPoint cursorCell_{-1, -1};
    QPoint lastCursorCell_{-1, -1};

    bool panning_ = false;
    QPointF panStart_;

    std::vector<MapMarker> markers_;
    std::vector<SpawnPoint> spawns_;

    /// Grid steps in map cells: 1 << kmGrid and 1 << kmGridChA (VMAP.h:89,121).
    static constexpr int kFineGridCell = 4;
    static constexpr int kCoarseGridCell = 64;
    /// Screen pixels a grid line must occupy before it is worth drawing.
    static constexpr double kMinGridSpacingPx = 4.0;
    /// Margin kept around the map when fitting.
    static constexpr int kFitMarginPx = 12;
    /// Wheel zoom step.
    static constexpr double kZoomStep = 1.25;
};

} // namespace MapEditor


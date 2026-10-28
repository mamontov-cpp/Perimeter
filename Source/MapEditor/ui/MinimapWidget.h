#pragma once

#include <QImage>
#include <QOpenGLWidget>
#include <QPointF>
#include <QPolygonF>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <vector>

class QOpenGLTextureBlitter;
class QOpenGLFunctions;

namespace MapEditor {

class EditorSession;

// ============================================================================
// MapEditor UI — MinimapWidget (T2.4).
//
// Small overview panel of the whole world:
//
//   * Image — the map.tga radar/minimap texture of the current world (the
//     file the game shell uses). When the file is missing or in a format the
//     editor never produces (fresh / never-saved worlds), the widget falls
//     back to a live render through WorldImages::renderToBgr() — the same
//     raster the viewport cache shows.
//   * View polygon — the outline of the region the central ViewportWidget is
//     looking at, pushed in via setViewPolygon() from the viewport's
//     viewPolygonChanged signal. In TopDown it is the real visible cell range;
//     in the 3D modes it is the ground footprint of the frustum — a rotated
//     trapezoid that follows the camera azimuth and shows how much more ground
//     the far edge covers than the near one.
//   * Click navigation — clicking the minimap recentres the viewport on the
//     clicked map cell (MainWindow wires cellClicked → ViewportWidget::
//     centerOnCell).
//
// The widget never touches Terra itself: map pixels go through
// WorldImages (MapEditorCore), world geometry (map size) through WorldFiles.
//
// Presentation is GPU-accelerated (QOpenGLWidget): the minimap image lives in
// a persistent GL texture uploaded only when the image changes, drawn scaled
// by QOpenGLTextureBlitter — a pan of the main viewport repaints the view
// rectangle every frame, and the old raster path re-scaled the whole (up to
// 4096²) source image on every such paint. The frame/view-rectangle drawing
// on top stays QPainter.
// ============================================================================

class MinimapWidget : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit MinimapWidget(QWidget *parent = nullptr);
    ~MinimapWidget() override;

    /// Bind the session and (re)load the minimap image of the current world.
    /// Safe to call when no game directory/world is available — the panel
    /// then shows a hint instead of the image.
    void setSession(EditorSession *session);

    /// Re-read map.tga (the file is regenerated on save — keep the panel in
    /// sync after EditorSession::saveWorld() succeeded).
    void reload();

    /// True when a minimap image is loaded.
    bool hasMinimapImage() const { return !minimapImage_.isNull(); }

    /// Size of the world in map cells (from world.ini / world raster).
    QSize mapSize() const { return mapSize_; }

    /// Outline of the viewport's current view, as a polygon of map cells
    /// (a rotated/trapezoidal quad in the 3D modes); an empty polygon hides
    /// the outline.
    void setViewPolygon(const QPolygonF &poly);
    QPolygonF viewPolygon() const { return viewPolygon_; }

    /// Axis-aligned convenience for the 2D view — forwards to
    /// setViewPolygon(). An empty/invalid rect hides the outline.
    void setViewRect(const QRect &rect);
    QRect viewRect() const { return viewPolygon_.boundingRect().toAlignedRect(); }

    /// Letter-boxed map rectangle inside the widget (autotest diagnostics).
    QRectF debugMapArea() const { return mapArea_; }

signals:
    /// Emitted on a left-click inside the map area with the clicked map cell
    /// (clamped to the world size). Consumed by MainWindow to navigate the
    /// viewport (ViewportWidget::centerOnCell).
    void cellClicked(int mapX, int mapY);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    /// Map widget position of a map cell (top-left of its 1x1 area).
    QPointF mapCellToWidget(double mapX, double mapY) const;
    /// Map cell under a widget point, or none (-1, -1).
    QPoint widgetToMapCell(const QPointF &pos) const;
    /// Recompute the letter-boxed rectangle of the map inside the widget.
    void updateMapArea();

    /// Create the GL texture when missing and upload the current minimap
    /// image (called from paintGL — needs the current GL context). Returns
    /// false when the image is not loaded or the blitter is unavailable.
    bool ensureMinimapTexture(QOpenGLFunctions *gl);
    /// Delete the GL blitter and the texture (context must be current).
    void releaseGLResources();

    /// Set the minimap data: image (BGR buffer), world size in cells; sizes
    /// the widget's layout rectangle accordingly.
    void setMinimapData(unsigned imageWidth, unsigned imageHeight,
                        const std::vector<unsigned char> &bgr,
                        const QSize &worldSize);

    EditorSession *session_ = nullptr;

    QImage minimapImage_; ///< map.tga pixels (or the live fallback render).
    QSize mapSize_;       ///< world size in map cells.
    QPolygonF viewPolygon_; ///< viewport's current view, in map cells.

    QRectF mapArea_;      ///< letter-boxed map rectangle inside the widget.

    // --- GL presentation state (created in initializeGL) -------------------
    QOpenGLTextureBlitter *glBlitter_ = nullptr;
    /// Minimap image texture, valid while minimapTextureStale_ is false.
    unsigned int minimapTexture_ = 0;
    bool minimapTextureStale_ = true;
};

} // namespace MapEditor

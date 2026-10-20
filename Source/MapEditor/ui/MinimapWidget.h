#pragma once

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QWidget>
#include <vector>

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
//   * View rectangle — the yellow outline of the region the central
//     ViewportWidget is looking at, pushed in via setViewRect() from the
//     viewport's viewRectChanged signal (TopDown: the real visible cell
//     range; Oblique: the approximate 3D window footprint).
//   * Click navigation — clicking the minimap recentres the viewport on the
//     clicked map cell (MainWindow wires cellClicked → ViewportWidget::
//     centerOnCell).
//
// The widget never touches Terra itself: map pixels go through
// WorldImages (MapEditorCore), world geometry (map size) through WorldFiles.
// ============================================================================

class MinimapWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MinimapWidget(QWidget *parent = nullptr);

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

    /// Outline of the viewport's current view, in map cells; an empty rect
    /// hides the outline.
    void setViewRect(const QRect &rect);
    QRect viewRect() const { return viewRect_; }

signals:
    /// Emitted on a left-click inside the map area with the clicked map cell
    /// (clamped to the world size). Consumed by MainWindow to navigate the
    /// viewport (ViewportWidget::centerOnCell).
    void cellClicked(int mapX, int mapY);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    /// Map widget position of a map cell (top-left of its 1x1 area).
    QPointF mapCellToWidget(double mapX, double mapY) const;
    /// Map cell under a widget point, or none (-1, -1).
    QPoint widgetToMapCell(const QPointF &pos) const;
    /// Recompute the letter-boxed rectangle of the map inside the widget.
    void updateMapArea();

    /// Set the minimap data: image (BGR buffer), world size in cells; sizes
    /// the widget's layout rectangle accordingly.
    void setMinimapData(unsigned imageWidth, unsigned imageHeight,
                        const std::vector<unsigned char> &bgr,
                        const QSize &worldSize);

    EditorSession *session_ = nullptr;

    QImage minimapImage_; ///< map.tga pixels (or the live fallback render).
    QSize mapSize_;       ///< world size in map cells.
    QRect viewRect_;      ///< viewport view rectangle, in map cells.

    QRectF mapArea_;      ///< letter-boxed map rectangle inside the widget.
};

} // namespace MapEditor

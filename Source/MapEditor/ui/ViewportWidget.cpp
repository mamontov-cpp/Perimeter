// ============================================================================
// MapEditor UI — ViewportWidget implementation (T2.2).
// ============================================================================

#include "ViewportWidget.h"

#include "EditorSession.h"
#include "ViewportRenderer.h"

#include <QPainter>
#include <QWheelEvent>
#include <algorithm>

namespace MapEditor {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ViewportWidget::ViewportWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

ViewportWidget::~ViewportWidget()
{
    delete renderer_;
}

// ---------------------------------------------------------------------------
// Session binding
// ---------------------------------------------------------------------------

void ViewportWidget::setSession(EditorSession *session)
{
    session_ = session;
    delete renderer_;
    renderer_ = nullptr;

    if (session_ && session_->isWorldLoaded()) {
        renderer_ = new ViewportRenderer();
        if (renderer_->bindWorld()) {
            updateFitScale();
            zoomToFit();
            update();
        }
    } else {
        updateFitScale();
        update();
    }
}

void ViewportWidget::reloadWorld()
{
    if (!session_ || !session_->isWorldLoaded()) {
        delete renderer_;
        renderer_ = nullptr;
        updateFitScale();
        update();
        return;
    }

    if (!renderer_ || !renderer_->isBound()) {
        delete renderer_;
        renderer_ = new ViewportRenderer();
        if (renderer_->bindWorld()) {
            updateFitScale();
            zoomToFit();
            update();
        }
    }
}

// ---------------------------------------------------------------------------
// View state
// ---------------------------------------------------------------------------

void ViewportWidget::updateFitScale()
{
    if (!renderer_ || renderer_->width() <= 0 || renderer_->height() <= 0) {
        fitScale_ = 1.0;
        return;
    }
    int mw = renderer_->width();
    int mh = renderer_->height();
    double scaleX = static_cast<double>(width()  - 2 * kFitMarginPx) / mw;
    double scaleY = static_cast<double>(height() - 2 * kFitMarginPx) / mh;
    fitScale_ = std::max(0.001, std::min(scaleX, scaleY));
}

void ViewportWidget::clampZoom()
{
    // The upper bound is 1:1 (plan T2.2 "1:1…fit"); the lower bound is fit.
    if (zoom_ > 1.0) {
        zoom_ = 1.0;
    }
    if (!renderer_ || fitScale_ <= 0.0) {
        return;
    }
    if (zoom_ < fitScale_) {
        // Keep the map centred when clamped out to fit.
        zoom_ = fitScale_;
        panOffset_ = QPointF(
            (width()  - renderer_->width()  * zoom_) / 2.0,
            (height() - renderer_->height() * zoom_) / 2.0);
    }
}

void ViewportWidget::centerOnMap()
{
    if (!renderer_) return;
    double scaledW = renderer_->width() * zoom_;
    double scaledH = renderer_->height() * zoom_;
    panOffset_ = QPointF(
        (width()  - scaledW) / 2.0,
        (height() - scaledH) / 2.0);
    update();
}

void ViewportWidget::zoomToFit()
{
    updateFitScale();
    zoom_ = fitScale_;
    centerOnMap();
    update();
}

// ---------------------------------------------------------------------------
// Overlays
// ---------------------------------------------------------------------------

void ViewportWidget::setBrushPreview(BrushPreviewShape shape, int radius)
{
    brushShape_ = shape;
    brushRadius_ = qMax(1, radius);
    update();
}

void ViewportWidget::setBrushPreviewVisible(bool visible)
{
    brushPreviewVisible_ = visible;
    update();
}

void ViewportWidget::setGridVisible(bool visible)
{
    gridVisible_ = visible;
    update();
}

void ViewportWidget::setMarkers(const std::vector<MapMarker> &markers)
{
    markers_ = markers;
    update();
}

void ViewportWidget::setSpawnPoints(const std::vector<SpawnPoint> &spawns)
{
    spawns_ = spawns;
    update();
}

// ---------------------------------------------------------------------------
// Coordinate conversion
// ---------------------------------------------------------------------------

QPointF ViewportWidget::mapToScreen(double mapX, double mapY) const
{
    return panOffset_ + QPointF(mapX * zoom_, mapY * zoom_);
}

QPoint ViewportWidget::screenToMap(int sx, int sy) const
{
    if (zoom_ <= 0.0 || !renderer_) return {-1, -1};
    int mx = static_cast<int>((sx - panOffset_.x()) / zoom_);
    int my = static_cast<int>((sy - panOffset_.y()) / zoom_);
    return {mx, my};
}

// ---------------------------------------------------------------------------
// Visible cell range helper
// ---------------------------------------------------------------------------

QRect ViewportWidget::visibleCellRange() const
{
    if (!renderer_) return {0, 0, 0, 0};
    int w = renderer_->width();
    int h = renderer_->height();
    int col0 = qMax(0, static_cast<int>(-panOffset_.x() / zoom_));
    int row0 = qMax(0, static_cast<int>(-panOffset_.y() / zoom_));
    int col1 = qMin(w,  static_cast<int>((width()  - panOffset_.x()) / zoom_ + 1));
    int row1 = qMin(h,  static_cast<int>((height() - panOffset_.y()) / zoom_ + 1));
    return {qMin(col0, col1), qMin(row0, row1),
            qMax(0, col1 - col0), qMax(0, row1 - row0)};
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void ViewportWidget::paintEvent(QPaintEvent * /*event*/)
{
    // Drain dirty rects / recompute lighting before painting, so the cache is
    // current when we blit it.
    if (renderer_) {
        renderer_->update();
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Background
    painter.fillRect(rect(), QColor(32, 32, 32));

    if (!renderer_ || !renderer_->isBound()) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No world loaded.\n"
                            "Use File → Load World to open a map."));
        return;
    }

    // --- 1. Zero-copy QImage from renderer cache ---
    drawBaseRaster(painter);

    // --- 2. Grid ---
    if (gridVisible_) drawGrid(painter);

    // --- 3. Brush preview ---
    if (brushPreviewVisible_) drawBrushPreview(painter);

    // --- 4. Markers ---
    drawMarkers(painter);

    // --- 5. Spawn points ---
    drawSpawnPoints(painter);

    // --- 6. HUD (cursor info, zoom) ---
    drawHud(painter);
}

// ---------------------------------------------------------------------------
// Base raster: zero-copy QImage
// ---------------------------------------------------------------------------

void ViewportWidget::drawBaseRaster(QPainter &painter)
{
    const std::uint32_t *pixels = renderer_->pixels();
    if (!pixels) return;

    int mw = renderer_->width();
    int mh = renderer_->height();

    // Wrap the renderer cache zero-copy. Layout matches QImage::Format_ARGB32
    // (0xFFRRGGBB, row-major).
    const QImage image(
        reinterpret_cast<const uchar *>(pixels),
        mw, mh, mw * 4,
        QImage::Format_ARGB32);

    // Position and size of the whole map in widget coords.
    QRectF targetRect(panOffset_, QSizeF(mw * zoom_, mh * zoom_));
    QRectF sourceRect(0, 0, mw, mh);

    painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
    painter.setClipRect(rect());
    painter.drawImage(targetRect, image, sourceRect);
}

// ---------------------------------------------------------------------------
// Grid overlay
// ---------------------------------------------------------------------------

void ViewportWidget::drawGrid(QPainter &painter)
{
    if (!renderer_ || !renderer_->isBound()) return;

    const QRect vis = visibleCellRange();
    const int x1 = vis.x() + vis.width();
    const int y1 = vis.y() + vis.height();
    const int mapH = renderer_->height();
    const int mapW = renderer_->width();

    // Fine grid (kmGrid = 4 cells). Draw only once lines are resolvable.
    if (zoom_ * kFineGridCell >= kMinGridSpacingPx) {
        painter.setPen(QColor(70, 70, 70, 90));
        // Align to the grid origin, not to the first visible cell.
        const int x0f = (vis.x() / kFineGridCell) * kFineGridCell;
        for (int x = x0f; x <= x1; x += kFineGridCell) {
            QPointF p1 = mapToScreen(x, 0);
            QPointF p2 = mapToScreen(x, mapH);
            painter.drawLine(p1, p2);
        }
        const int y0f = (vis.y() / kFineGridCell) * kFineGridCell;
        for (int y = y0f; y <= y1; y += kFineGridCell) {
            QPointF p1 = mapToScreen(0, y);
            QPointF p2 = mapToScreen(mapW, y);
            painter.drawLine(p1, p2);
        }
    }

    // Coarse grid (kmGridChA = 64 cells): the always-visible reference frame.
    painter.setPen(QColor(110, 110, 110, 130));
    const int x0c = (vis.x() / kCoarseGridCell) * kCoarseGridCell;
    for (int x = x0c; x <= x1; x += kCoarseGridCell) {
        QPointF p1 = mapToScreen(x, 0);
        QPointF p2 = mapToScreen(x, mapH);
        painter.drawLine(p1, p2);
    }
    const int y0c = (vis.y() / kCoarseGridCell) * kCoarseGridCell;
    for (int y = y0c; y <= y1; y += kCoarseGridCell) {
        QPointF p1 = mapToScreen(0, y);
        QPointF p2 = mapToScreen(mapW, y);
        painter.drawLine(p1, p2);
    }
}

// ---------------------------------------------------------------------------
// Brush preview
// ---------------------------------------------------------------------------

void ViewportWidget::drawBrushPreview(QPainter &painter)
{
    if (!renderer_ || !renderer_->isBound()) return;
    if (!brushPreviewVisible_) return;

    QPoint cell = cursorCell_;
    if (cell.x() < 0 || cell.y() < 0 ||
        cell.x() >= renderer_->width() || cell.y() >= renderer_->height()) {
        return;
    }

    // Center of the cell in widget coords.
    QPointF center = mapToScreen(cell.x() + 0.5, cell.y() + 0.5);
    double screenR = brushRadius_ * zoom_;

    painter.setPen(QPen(QColor(255, 255, 0, 180), 1.5, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);

    if (brushShape_ == BrushPreviewShape::Circle) {
        painter.drawEllipse(center, screenR, screenR);
    } else {
        QRectF r(center.x() - screenR, center.y() - screenR,
                 screenR * 2, screenR * 2);
        painter.drawRect(r);
    }
}

// ---------------------------------------------------------------------------
// Markers
// ---------------------------------------------------------------------------

void ViewportWidget::drawMarkers(QPainter &painter)
{
    if (markers_.empty() || !renderer_->isBound()) return;

    for (const MapMarker &m : markers_) {
        QPointF pos = mapToScreen(m.mapX, m.mapY);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m.color);
        painter.drawEllipse(pos, 5, 5);

        painter.setPen(QColor(255, 255, 255));
        painter.setBrush(Qt::NoBrush);
        painter.drawText(pos.x() + 7, pos.y() - 3, m.label);
    }
}

// ---------------------------------------------------------------------------
// Spawn points
// ---------------------------------------------------------------------------

void ViewportWidget::drawSpawnPoints(QPainter &painter)
{
    if (spawns_.empty() || !renderer_->isBound()) return;

    for (const SpawnPoint &s : spawns_) {
        QPointF pos = mapToScreen(s.mapX, s.mapY);
        painter.setPen(QPen(s.color, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(pos, 7, 7);

        painter.setPen(s.color);
        painter.setBrush(Qt::NoBrush);
        painter.drawText(pos.x() + 9, pos.y() - 3, s.label);
    }
}

// ---------------------------------------------------------------------------
// HUD — cursor cell + zoom readout
// ---------------------------------------------------------------------------

void ViewportWidget::drawHud(QPainter &painter)
{
    if (!renderer_->isBound()) return;

    QString info;
    if (cursorCell_.x() >= 0 && cursorCell_.y() >= 0 &&
        cursorCell_.x() < renderer_->width() &&
        cursorCell_.y() < renderer_->height()) {
        info = QString(tr("X: %1  Y: %2")).arg(cursorCell_.x()).arg(cursorCell_.y());
    } else {
        info = tr("Outside map");
    }
    info += QString(tr("  |  Zoom: %1%").arg(qRound(zoom_ * 100)));

    QRect hudRect = rect().adjusted(4, 0, -4, -4);
    painter.setPen(QColor(210, 210, 210));
    painter.setRenderHint(QPainter::TextAntialiasing, false);
    painter.drawText(hudRect, Qt::AlignRight | Qt::AlignBottom | Qt::TextHideMnemonic, info);
    painter.setRenderHint(QPainter::TextAntialiasing);
}

// ---------------------------------------------------------------------------
// Mouse events — pan & zoom
// ---------------------------------------------------------------------------

void ViewportWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        panning_ = true;
        panStart_ = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ViewportWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (panning_) {
        panOffset_ += event->position() - panStart_;
        panStart_ = event->position();
        update();
        event->accept();
        return;
    }

    // Compute the cell under cursor and emit only when it changes.
    QPoint pos = event->position().toPoint();
    QPoint cell = screenToMap(pos.x(), pos.y());
    if (cell != cursorCell_) {
        cursorCell_ = cell;
        emit cursorMapPosition(cell.x(), cell.y());
        // Update brush preview position.
        if (brushPreviewVisible_) update();
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (panning_) {
        panning_ = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ViewportWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    zoomToFit();
}

void ViewportWidget::leaveEvent(QEvent *)
{
    // Only drop the cursor cell; brush-preview visibility stays under the tool
    // panel's control (T3) — drawBrushPreview already skips off-map cells.
    cursorCell_ = {-1, -1};
    update();
}

void ViewportWidget::wheelEvent(QWheelEvent *event)
{
    if (!renderer_ || !session_ || !session_->isWorldLoaded()) {
        return;
    }

    // Wheel forward (positive delta) → zoom in.
    const double factor = event->angleDelta().y() > 0
                              ? kZoomStep
                              : 1.0 / kZoomStep;

    // Map cursor to cell before zoom so the cell under cursor stays stable.
    QPoint pos = event->position().toPoint();
    QPoint cell = screenToMap(pos.x(), pos.y());

    // Apply zoom.
    zoom_ *= factor;
    clampZoom();

    // Adjust pan: the map cell under cursor should stay at cursor position.
    QPointF screenAfter = mapToScreen(cell.x(), cell.y());
    panOffset_ -= (screenAfter - QPointF(pos));

    update();
    event->accept();
}

void ViewportWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateFitScale();
    clampZoom();
    update();
}

} // namespace MapEditor


// ============================================================================
// MapEditor UI — ViewportWidget implementation (T2.2).
// ============================================================================

#include "ViewportWidget.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ViewportRenderer.h"

#include <QPainter>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

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
    delete view3d_;
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
// Brush editing (T3.1)
// ---------------------------------------------------------------------------

void ViewportWidget::setBrushTool(BrushTool *brush)
{
    brushTool_ = brush;
}

void ViewportWidget::setBrushEditingEnabled(bool enabled)
{
    brushEditingEnabled_ = enabled;
    if (!brushEditingEnabled_) {
        brushPainting_ = false;
    }
}

/// Apply one brush stamp at the cell under the cursor; returns true when the
/// world changed. Same guard set used by press and move.
static bool applyBrushStampHere(ViewportWidget &viewport, const QPoint &cell)
{
    MapEditor::BrushTool *brush = viewport.brushTool();
    if (!brush || !viewport.isBrushEditingEnabled()
            || !viewport.session() || !viewport.session()->isWorldLoaded()) {
        return false;
    }
    const MapEditor::ViewportRenderer *renderer = viewport.renderer();
    if (!renderer || !renderer->isBound()
            || cell.x() < 0 || cell.y() < 0
            || cell.x() >= renderer->width() || cell.y() >= renderer->height()) {
        return false;
    }
    const bool changed = brush->applyStamp(cell.x(), cell.y());
    if (changed) {
        emit viewport.worldEdited();
        viewport.update();
    }
    return changed;
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
    if (viewMode_ == ViewMode::Oblique) {
        resetObliqueView();
        return;
    }
    updateFitScale();
    zoom_ = fitScale_;
    centerOnMap();
    update();
}

// ---------------------------------------------------------------------------
// View mode switching (T2.3)
// ---------------------------------------------------------------------------

void ViewportWidget::setViewMode(ViewMode mode)
{
    if (viewMode_ == mode) {
        return;
    }
    viewMode_ = mode;
    // Reset stale pointer/drag state so the mouse does not drag the new mode
    // with a half-finished gesture.
    orbiting_ = false;
    emit viewModeChanged(viewMode_);
    update();
}

void ViewportWidget::zoomStep(double factor)
{
    if (!session_ || !session_->isWorldLoaded()) {
        return;
    }
    if (viewMode_ == ViewMode::Oblique) {
        adjustObliqueFocus(factor > 1.0 ? -100 : 100);
    } else {
        // Zoom around the widget centre.
        const QPointF widgetCenter(width() / 2.0, height() / 2.0);
        const QPoint cell = screenToMap(qRound(widgetCenter.x()),
                                        qRound(widgetCenter.y()));
        zoom_ *= factor;
        clampZoom();
        if (cell.x() >= 0 && cell.y() >= 0) {
            const QPointF screenAfter = mapToScreen(cell.x(), cell.y());
            panOffset_ -= (screenAfter - widgetCenter);
        }
        update();
    }
}

// ---------------------------------------------------------------------------
// Oblique (3D) camera helpers (T2.3)
// ---------------------------------------------------------------------------

namespace {

/// Wrap any angle into [0, 360).
double wrapDegrees(double deg)
{
    double wrapped = std::fmod(deg, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

} // namespace

void ViewportWidget::setObliqueCamera(const View3DCamera &camera)
{
    obliqueCamera_.turnAngleDeg = wrapDegrees(camera.turnAngleDeg);
    obliqueCamera_.leanDeg = camera.leanDeg;
    obliqueCamera_.cameraDZ = View3DRenderer::clampCameraDZ(camera.cameraDZ);
    obliqueCamera_.focusC = View3DRenderer::clampFocus(camera.focusC);
    emit obliqueCameraChanged(obliqueCamera_);
    update();
}

void ViewportWidget::adjustObliqueRotation(double deltaDeg)
{
    const double next = wrapDegrees(obliqueCamera_.turnAngleDeg + deltaDeg);
    if (next == obliqueCamera_.turnAngleDeg) {
        return;
    }
    obliqueCamera_.turnAngleDeg = next;
    emit obliqueCameraChanged(obliqueCamera_);
    update();
}

void ViewportWidget::adjustObliqueTilt(double deltaDeg)
{
    obliqueCamera_.leanDeg += deltaDeg;
    emit obliqueCameraChanged(obliqueCamera_);
    update();
}

void ViewportWidget::adjustObliqueLift(int deltaCells)
{
    const int next = View3DRenderer::clampCameraDZ(
        obliqueCamera_.cameraDZ + deltaCells);
    if (next == obliqueCamera_.cameraDZ) {
        return;
    }
    obliqueCamera_.cameraDZ = next;
    emit obliqueCameraChanged(obliqueCamera_);
    update();
}

void ViewportWidget::adjustObliqueFocus(int deltaCells)
{
    const int next = View3DRenderer::clampFocus(
        obliqueCamera_.focusC + deltaCells);
    if (next == obliqueCamera_.focusC) {
        return;
    }
    obliqueCamera_.focusC = next;
    emit obliqueCameraChanged(obliqueCamera_);
    update();
}

void ViewportWidget::resetObliqueView()
{
    obliqueCamera_ = View3DCamera{};
    obliqueCenter_ = renderer_ ? QPointF(renderer_->width() / 2.0,
                                         renderer_->height() / 2.0)
                               : QPointF();
    setViewMode(ViewMode::Oblique); // no-op when already Oblique
    emit obliqueCameraChanged(obliqueCamera_);
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
// Current view rectangle (T2.4)
// ---------------------------------------------------------------------------

namespace {

/// Depth ("forward") scaling of the viewV2D projection: rendered rows map to
/// ground distances with the slope kv
///     kv = (focusC + cameraDZ) * sqrt(1 + tan²lean) / (|tan lean| * focusC)
/// clamped like panObliqueBy() (a knee-soft heuristic — the real projection
/// is non-linear along rows; it makes the north-south ground extent feel
/// comparable to the east-west one). Shared with currentViewRect().
double obliqueForwardScale(const MapEditor::View3DCamera &camera,
                           double stableLeanDeg)
{
    const double tanLean = std::tan(qDegreesToRadians(stableLeanDeg));
    if (std::fabs(tanLean) <= 1e-3) {
        return 1.0;
    }
    const double kv =
        (camera.focusC + camera.cameraDZ) * std::sqrt(1.0 + tanLean * tanLean) /
        (std::fabs(tanLean) * camera.focusC);
    return std::clamp(kv, 1.0, 12.0);
}

} // namespace

QSize ViewportWidget::obliqueBufferSize() const
{
    // Buffer width — widget width clamped by Terra's window limit;
    // height keeps the widget aspect.
    const int bufferWidth = std::min(width(), View3DRenderer::kMaxWindowSize);
    const double aspect =
        height() / static_cast<double>(qMax(1, width()));
    const int bufferHeight =
        std::min(View3DRenderer::kMaxWindowSize,
                 qMax(View3DRenderer::kMinWindowSize,
                      qRound(bufferWidth * aspect)));
    return {bufferWidth, bufferHeight};
}

QRect ViewportWidget::currentViewRect() const
{
    if (!renderer_) {
        return {};
    }
    if (viewMode_ == ViewMode::TopDown) {
        return visibleCellRange();
    }

    // Oblique: the 3D window is not a linear map projection — approximate the
    // ground footprint by the viewV2D axes at the look-at centre
    // (basis from VMAP.cpp:2096-2099): each rendered column spans
    // (focusC + cameraDZ) / focusC ground cells, each row the same distance
    // scaled by the kv slope of the projection. Good enough for the minimap
    // outline; the rectangle is clipped to the map (single-map wrap area —
    // the real 3D window can span the seam, the minimap shows the centre
    // region).
    const QSize buffer = obliqueBufferSize();
    const double leanDeg = View3DRenderer::stableLeanDeg(
        obliqueCamera_.leanDeg, obliqueCamera_.focusC, buffer.height() / 2);
    const double kv = obliqueForwardScale(obliqueCamera_, leanDeg);
    const double cellsPerPx =
        (obliqueCamera_.focusC + obliqueCamera_.cameraDZ) /
        static_cast<double>(obliqueCamera_.focusC);

    const double groundWidth = buffer.width() * cellsPerPx;
    const double groundHeight = buffer.height() * kv * cellsPerPx;
    const int x0 = qRound(obliqueCenter_.x() - groundWidth / 2.0);
    const int y0 = qRound(obliqueCenter_.y() - groundHeight / 2.0);
    return QRect(x0, y0, qRound(groundWidth), qRound(groundHeight))
        .intersected(QRect(0, 0, renderer_->width(), renderer_->height()));
}

void ViewportWidget::centerOnCell(const QPoint &cell)
{
    if (!session_ || !session_->isWorldLoaded() || !renderer_) {
        return;
    }
    // Wrap the requested cell like the map axes do.
    const int mapW = renderer_->width();
    const int mapH = renderer_->height();
    const int px = ((cell.x() % mapW) + mapW) % mapW;
    const int py = ((cell.y() % mapH) + mapH) % mapH;

    if (viewMode_ == ViewMode::Oblique) {
        obliqueCenter_ = QPointF(px, py);
    } else {
        panOffset_ = QPointF(width() / 2.0 - px * zoom_,
                             height() / 2.0 - py * zoom_);
        cursorCell_ = {px, py};
    }
    update();
}

void ViewportWidget::emitViewRectIfChanged()
{
    if (!session_ || !session_->isWorldLoaded() || !renderer_) {
        // World dropped — make sure the minimap hides the rectangle once.
        if (!lastEmittedViewRect_.isEmpty()) {
            lastEmittedViewRect_ = {};
            emit viewRectChanged(lastEmittedViewRect_);
        }
        return;
    }
    const QRect viewRect = currentViewRect();
    if (viewRect != lastEmittedViewRect_) {
        lastEmittedViewRect_ = viewRect;
        emit viewRectChanged(lastEmittedViewRect_);
    }
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

    if (viewMode_ == ViewMode::Oblique) {
        draw3DRaster(painter);
        drawHud(painter);
        emitViewRectIfChanged();
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

    // --- 7. Announce the view rectangle for the minimap (T2.4) ---
    emitViewRectIfChanged();
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
// Base raster: oblique 3D view (T2.3)
// ---------------------------------------------------------------------------

void ViewportWidget::draw3DRaster(QPainter &painter)
{
    // Resize the brain-free window: full widget size, clamped by Terra's own
    // MAX_XSIZE_VX_WINDOW (the widget then scales the frame back up). The
    // same buffer size drives currentViewRect(), so the minimap footprint
    // matches the frame about to be drawn.
    if (!view3d_) {
        view3d_ = new View3DRenderer();
    }
    const QSize buffer = obliqueBufferSize();
    view3d_->resizeWindow(buffer.width(), buffer.height());

    // Camera + look-at (map cell coordinates wrapped to the map size).
    view3d_->setCamera(obliqueCamera_);
    const int mapW = renderer_->width();
    const int mapH = renderer_->height();
    const int cx =
        ((static_cast<int>(std::floor(obliqueCenter_.x())) % mapW) + mapW) % mapW;
    const int cy =
        ((static_cast<int>(std::floor(obliqueCenter_.y())) % mapH) + mapH) % mapH;
    view3d_->render(cx, cy);
    if (view3d_->pixels() == nullptr) {
        return;
    }

    // Zero-copy RGB16 (565) frame — exactly QImage::Format_RGB16 layout,
    // since Sur2Col cubes that pixel order (VMAP.cpp:1367-1370).
    const QImage image(
        reinterpret_cast<const uchar *>(view3d_->pixels()),
        view3d_->windowWidth(), view3d_->windowHeight(),
        view3d_->windowWidth() * 2,
        QImage::Format_RGB16);
    painter.setClipRect(rect());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(rect(), image, QRectF(0, 0, view3d_->windowWidth(),
                                            view3d_->windowHeight()));
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
    if (viewMode_ == ViewMode::Oblique) {
        // The 3D camera line-up; lean is the value viewV2D will actually use
        // (may be clamped by the stable band, see View3DRenderer).
        const double lean = View3DRenderer::stableLeanDeg(
            obliqueCamera_.leanDeg, obliqueCamera_.focusC,
            std::max(1, view3d_ ? view3d_->windowHeight() / 2 : height() / 2));
        info = tr("3D  |  Rot: %1\u00B0  Tilt: %2\u00B0  Lift: %3  Focus: %4")
                   .arg(qRound(obliqueCamera_.turnAngleDeg))
                   .arg(qRound(lean))
                   .arg(obliqueCamera_.cameraDZ)
                   .arg(obliqueCamera_.focusC);
    } else {
        if (cursorCell_.x() >= 0 && cursorCell_.y() >= 0 &&
            cursorCell_.x() < renderer_->width() &&
            cursorCell_.y() < renderer_->height()) {
            info = QString(tr("X: %1  Y: %2")).arg(cursorCell_.x()).arg(cursorCell_.y());
        } else {
            info = tr("Outside map");
        }
        info += QString(tr("  |  Zoom: %1%").arg(qRound(zoom_ * 100)));
    }

    QRect hudRect = rect().adjusted(4, 0, -4, -4);
    painter.setPen(QColor(210, 210, 210));
    painter.setRenderHint(QPainter::TextAntialiasing, false);
    painter.drawText(hudRect, Qt::AlignRight | Qt::AlignBottom | Qt::TextHideMnemonic, info);
    painter.setRenderHint(QPainter::TextAntialiasing);
}

// ---------------------------------------------------------------------------
// Mouse events — pan / orbit / zoom
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
    if (viewMode_ == ViewMode::Oblique && event->button() == Qt::LeftButton) {
        // Orbit drag: horizontal — rotation around Z, vertical — tilt.
        orbiting_ = true;
        panStart_ = event->position();
        setCursor(Qt::SizeAllCursor);
        event->accept();
        return;
    }
    if (viewMode_ == ViewMode::TopDown && event->button() == Qt::LeftButton) {
        // Brush painting (T3.1): press applies one stamp at the cursor cell,
        // a following drag stamps along the path (mouseMoveEvent).
        const QPoint pos = event->position().toPoint();
        const QPoint cell = screenToMap(pos.x(), pos.y());
        if (isBrushEditingEnabled()
                && cell.x() >= 0 && cell.y() >= 0
                && renderer_ && renderer_->isBound()
                && cell.x() < renderer_->width() && cell.y() < renderer_->height()) {
            brushPainting_ = true;
            lastPaintCell_ = cell;
            setCursor(Qt::CrossCursor);
            if (applyBrushStampHere(*this, cell)) {
                update();
            }
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void ViewportWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (panning_) {
        const QPointF delta = event->position() - panStart_;
        if (viewMode_ == ViewMode::Oblique) {
            panObliqueBy(delta.x(), delta.y());
        } else {
            panOffset_ += delta;
        }
        panStart_ = event->position();
        update();
        event->accept();
        return;
    }
    if (orbiting_) {
        const QPointF delta = event->position() - panStart_;
        if (delta.x() != 0.0) {
            adjustObliqueRotation(-delta.x() * kOrbitDegPerPx);
        }
        if (delta.y() != 0.0) {
            // Drag up — camera looks more along the horizon; drag down —
            // more top-down.
            adjustObliqueTilt(delta.y() * kOrbitDegPerPx);
        }
        panStart_ = event->position();
        event->accept();
        return;
    }

    if (viewMode_ == ViewMode::Oblique) {
        // The oblique view is not a linear map projection — screen-space cell
        // lookup is undefined there; keep the last TopDown cell in the panels.
        return;
    }

    // Compute the cell under cursor and emit only when it changes.
    QPoint pos = event->position().toPoint();
    QPoint cell = screenToMap(pos.x(), pos.y());
    if (cell != cursorCell_) {
        cursorCell_ = cell;
        emit cursorMapPosition(cell.x(), cell.y());
        // Continue painting or update the brush preview position.
        if (brushPainting_ && cell != lastPaintCell_) {
            lastPaintCell_ = cell;
            applyBrushStampHere(*this, cell);
        } else if (brushPreviewVisible_) {
            update();
        }
    } else if (brushPainting_ && cell != lastPaintCell_) {
        // Re-entered the previous drag cell after panning the view beneath.
        lastPaintCell_ = cell;
        applyBrushStampHere(*this, cell);
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (panning_) {
        panning_ = false;
        setCursor(brushPainting_ ? Qt::CrossCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (brushPainting_) {
        brushPainting_ = false;
        setCursor(isBrushEditingEnabled() ? Qt::CrossCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (orbiting_) {
        orbiting_ = false;
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

    if (viewMode_ == ViewMode::Oblique) {
        // Focal length: wheel in = closer (focusC shrinks). cameraDZ follows
        // proportionally so the horizon height stays readable while zooming.
        event->accept();
        zoomStep(factor);
        return;
    }

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

// ---------------------------------------------------------------------------
// Oblique ground-plane panning (T2.3)
//
// viewV2D's camera basis (derived from the projection in VMAP.cpp:2096-2099):
// the look-at centre maps to the screen centre, screen-right runs along the
// ground direction (cos θ, sin θ) and "away from the camera" runs along
// (sin θ, -cos θ), where θ = turnAngleAroundZ. A grab-drag of (dx, dy) screen
// pixels therefore moves the camera by
//     -dx * right + dy * forward * kv
// ground cells. The horizontal scale is (focusC + cameraDZ) / focusC cells
// per rendered pixel (the d=0 row of the projection), the forward factor kv
// uses the same projection's row slope at d≈0:
//     kv = (focusC + cameraDZ) * sqrt(1 + tan²lean) / (|tan lean| * focusC)
// (a knee-soft heuristic — the real projection is non-linear along rows; it
// makes north-south drags and east-west drags feel comparable).
// ---------------------------------------------------------------------------

void ViewportWidget::panObliqueBy(double dxCursorPx, double dyCursorPx)
{
    if (!view3d_ || view3d_->windowWidth() <= 0 || width() <= 0) {
        return;
    }

    const double focus = obliqueCamera_.focusC;
    const double lift = obliqueCamera_.cameraDZ;
    // Screen→ground scale: the drag happened on widget pixels while the
    // projection constants are per rendered pixel.
    const double cellsPerPxX =
        (focus + lift) / focus * view3d_->windowWidth() / width();

    const double leanDeg = View3DRenderer::stableLeanDeg(
        obliqueCamera_.leanDeg, obliqueCamera_.focusC,
        view3d_->windowHeight() / 2);
    const double forwardScale = obliqueForwardScale(obliqueCamera_, leanDeg);
    const double theta = qDegreesToRadians(obliqueCamera_.turnAngleDeg);
    const double cosT = std::cos(theta);
    const double sinT = std::sin(theta);

    // Δcenter = -dx * right + dy * forward * kv (screen y grows downwards).
    const double moveX = (-dxCursorPx * cosT + dyCursorPx * forwardScale * sinT) * cellsPerPxX;
    const double moveY = (-dxCursorPx * sinT - dyCursorPx * forwardScale * cosT) * cellsPerPxX;
    obliqueCenter_ += QPointF(moveX, moveY);
}

} // namespace MapEditor


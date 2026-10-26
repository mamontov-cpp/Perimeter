// ============================================================================
// MapEditor UI — MinimapWidget implementation (T2.4).
// ============================================================================

#include "MinimapWidget.h"

#include "EditorSession.h"
#include "WorldFiles.h"
#include "WorldImages.h"

#include <QMouseEvent>
#include <QOpenGLFunctions>
#include <QOpenGLTextureBlitter>
#include <QPainter>
#include <QPaintEvent>
#include <QtMath>
#include <algorithm>
#include <utility>

namespace MapEditor {

namespace {

constexpr int kMarginPx = 4;      ///< letter-box margin inside the widget.
constexpr int kMinDrawSize = 16;  ///< too small — show the hint text instead.

} // namespace

MinimapWidget::MinimapWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(140, 140);
    setMouseTracking(true);
}

MinimapWidget::~MinimapWidget()
{
    if (isValid()) {
        makeCurrent();
        releaseGLResources();
        doneCurrent();
    }
}

void MinimapWidget::initializeGL()
{
    // Free the GL-backed state together with the context; the next
    // initializeGL (context recreation) rebuilds everything.
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        releaseGLResources();
    }, Qt::DirectConnection);

    glBlitter_ = new QOpenGLTextureBlitter();
    if (!glBlitter_->create()) {
        qWarning("MinimapWidget: QOpenGLTextureBlitter creation failed, "
                 "the minimap image will not be drawn");
        delete glBlitter_;
        glBlitter_ = nullptr;
    }
}

void MinimapWidget::releaseGLResources()
{
    if (glBlitter_) {
        delete glBlitter_;
        glBlitter_ = nullptr;
    }
    if (minimapTexture_ != 0 && context() && context()->functions()) {
        context()->functions()->glDeleteTextures(1, &minimapTexture_);
    }
    minimapTexture_ = 0;
    minimapTextureStale_ = true;
}

// ---------------------------------------------------------------------------
// Session binding / image loading
// ---------------------------------------------------------------------------

void MinimapWidget::setSession(EditorSession *session)
{
    session_ = session;
    reload();
}

void MinimapWidget::reload()
{
    minimapImage_ = QImage();
    mapSize_ = QSize();

    if (!session_) {
        update();
        return;
    }

    const auto worldFiles = session_->currentWorldFiles();
    if (!worldFiles) {
        update();
        return;
    }

    // Preferred source: the world's map.tga (what the game shell shows as
    // the radar/minimap, plan T2.4). Regenerated on every save — reload()
    // is called after saveWorld() succeeded.
    std::vector<unsigned char> bgr;
    unsigned imageWidth = 0;
    unsigned imageHeight = 0;
    bool loaded = false;
    const std::filesystem::path mapTga =
        worldFiles->filePath(WorldFiles::MapTgaFile);
    if (std::filesystem::exists(mapTga)) {
        loaded = WorldImages::loadTga24(mapTga, imageWidth, imageHeight, bgr);
    }

    // Fallback: a fresh world (new-world wizard, never saved) has no map.tga
    // yet and a stale/foreign one may not parse — render the same raster the
    // on-screen viewport cache shows instead.
    if (!loaded && session_->isWorldLoaded()) {
        loaded = WorldImages::renderToBgr(bgr, imageWidth, imageHeight);
    }

    if (!loaded) {
        update();
        return;
    }

    // World size in map cells drives the rectangle geometry: map.tga may have
    // an arbitrary size (any power-of-two-ish downscale of the raster).
    QSize worldSize;
    if (const auto power = worldFiles->readMapPower()) {
        worldSize = QSize(1 << power->first, 1 << power->second);
    } else {
        // No world.ini data — assume the raster maps 1:1 onto the cells.
        worldSize = QSize(static_cast<int>(imageWidth),
                          static_cast<int>(imageHeight));
    }

    setMinimapData(imageWidth, imageHeight, bgr, worldSize);
}

void MinimapWidget::setMinimapData(unsigned imageWidth, unsigned imageHeight,
                                  const std::vector<unsigned char> &bgr,
                                  const QSize &worldSize)
{
    if (imageWidth == 0 || imageHeight == 0 ||
        worldSize.width() <= 0 || worldSize.height() <= 0 ||
        bgr.size() < static_cast<std::size_t>(imageWidth) * imageHeight * 3) {
        update();
        return;
    }

    // BGR byte triples → QImage::Format_RGB32 (little-endian byte order:
    // blue, green, red, opaque).
    QImage image(static_cast<int>(imageWidth), static_cast<int>(imageHeight),
                 QImage::Format_RGB32);
    for (unsigned y = 0; y < imageHeight; ++y) {
        uchar *dst = image.scanLine(static_cast<int>(y));
        const unsigned char *src = bgr.data() +
            static_cast<std::size_t>(y) * imageWidth * 3;
        for (unsigned x = 0; x < imageWidth; ++x) {
            dst[x * 4 + 0] = src[x * 3 + 0];                   // B
            dst[x * 4 + 1] = src[x * 3 + 1];                   // G
            dst[x * 4 + 2] = src[x * 3 + 2];                   // R
            dst[x * 4 + 3] = 0xFF;                             // A
        }
    }

    minimapImage_ = std::move(image);
    mapSize_ = worldSize;
    minimapTextureStale_ = true;
    updateMapArea();
    update();
}

// ---------------------------------------------------------------------------
// View rectangle (T2.4)
// ---------------------------------------------------------------------------

void MinimapWidget::setViewRect(const QRect &rect)
{
    if (viewRect_ == rect) {
        return;
    }
    viewRect_ = rect;
    update();
}

// ---------------------------------------------------------------------------
// Geometry / coordinate conversion
// ---------------------------------------------------------------------------

void MinimapWidget::updateMapArea()
{
    if (mapSize_.isEmpty()) {
        mapArea_ = QRectF();
        return;
    }
    const QRectF available = rect().adjusted(kMarginPx, kMarginPx,
                                             -kMarginPx, -kMarginPx);
    const double scale = std::min(available.width() / mapSize_.width(),
                                  available.height() / mapSize_.height());
    const double drawWidth = mapSize_.width() * scale;
    const double drawHeight = mapSize_.height() * scale;
    mapArea_ = QRectF((width() - drawWidth) / 2.0,
                      (height() - drawHeight) / 2.0,
                      drawWidth, drawHeight);
}

QPointF MinimapWidget::mapCellToWidget(double mapX, double mapY) const
{
    if (mapSize_.isEmpty()) {
        return {};
    }
    return mapArea_.topLeft() +
           QPointF(mapX, mapY) * (mapArea_.width() / mapSize_.width());
}

QPoint MinimapWidget::widgetToMapCell(const QPointF &pos) const
{
    if (mapArea_.isEmpty() || !mapArea_.contains(pos)) {
        return {-1, -1};
    }
    const double relX = (pos.x() - mapArea_.left()) /
                        mapArea_.width() * mapSize_.width();
    const double relY = (pos.y() - mapArea_.top()) /
                        mapArea_.height() * mapSize_.height();
    return {qBound(0, qFloor(relX), mapSize_.width() - 1),
            qBound(0, qFloor(relY), mapSize_.height() - 1)};
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void MinimapWidget::resizeEvent(QResizeEvent * /*event*/)
{
    updateMapArea();
    update();
}

void MinimapWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || mapArea_.isEmpty()) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint cell = widgetToMapCell(event->position());
    if (cell.x() >= 0) {
        emit cellClicked(cell.x(), cell.y());
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

bool MinimapWidget::ensureMinimapTexture(QOpenGLFunctions *gl)
{
    if (!glBlitter_ || minimapImage_.isNull()) {
        return false;
    }
    if (minimapTexture_ != 0 && !minimapTextureStale_) {
        return true;
    }
    if (minimapTexture_ != 0) {
        gl->glDeleteTextures(1, &minimapTexture_);
        minimapTexture_ = 0;
    }
    gl->glGenTextures(1, &minimapTexture_);
    gl->glBindTexture(GL_TEXTURE_2D, minimapTexture_);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Format_RGB32: byte order B,G,R,A — GL_BGRA feeds the channels through.
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     minimapImage_.width(), minimapImage_.height(), 0,
                     GL_BGRA, GL_UNSIGNED_BYTE,
                     reinterpret_cast<const GLvoid *>(minimapImage_.constBits()));
    minimapTextureStale_ = false;
    return true;
}

void MinimapWidget::paintGL()
{
    QOpenGLFunctions *gl = context()->functions();

    // Native pass FIRST, before QPainter is constructed: texture upload +
    // clear + map raster blit, the same discipline ViewportWidget uses (raw
    // GL calls while a QPainter is active are only legal inside
    // beginNativePainting — and doing them lazily below used to leave the
    // texture upload in exactly that illegal window).
    const bool drawMap = !mapSize_.isEmpty() && width() >= kMinDrawSize
                         && height() >= kMinDrawSize
                         && ensureMinimapTexture(gl);
    if (drawMap) {
        // The map raster is stretched over the whole letter-boxed map
        // rectangle: the view rectangle must align with world geometry (map
        // cells), not with whatever aspect map.tga happens to have. GPU
        // path: the image texture is uploaded once (setMinimapData marks it
        // stale) and sampled linearly — the old raster path re-scaled the
        // whole source image on every paint.
        gl->glClearColor(32.0f / 255.0f, 32.0f / 255.0f, 32.0f / 255.0f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        gl->glDisable(GL_DEPTH_TEST);
        gl->glDisable(GL_BLEND);
        glBlitter_->bind();
        glBlitter_->blit(minimapTexture_,
                         QOpenGLTextureBlitter::targetTransform(
                             mapArea_, rect()),
                         QOpenGLTextureBlitter::OriginTopLeft);
        glBlitter_->release();
    }

    QPainter painter(this);

    if (!drawMap) {
        painter.fillRect(rect(), QColor(32, 32, 32));
        // Overflowing world size (e.g. image not loaded) — hint text instead.
        if (mapSize_.isEmpty()) {
            painter.setPen(QColor(160, 160, 160));
            painter.drawText(rect(), Qt::AlignCenter,
                             tr("No minimap.\n"
                                "Load a world to see the overview here."));
        }
        return;
    }

    // Frame around the map.
    painter.setPen(QColor(90, 90, 90));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(mapArea_.adjusted(-0.5, -0.5, 0.5, 0.5));

    // Current view rectangle of the viewport (T2.4), clamped to the map.
    if (!viewRect_.isEmpty() && !mapSize_.isEmpty()) {
        const QRect clamped = viewRect_.intersected(QRect(QPoint(0, 0), mapSize_));
        if (!clamped.isEmpty()) {
            const QRectF widgetRect(mapCellToWidget(clamped.x(), clamped.y()),
                QSizeF(clamped.width() * mapArea_.width() / mapSize_.width(),
                       clamped.height() * mapArea_.height() / mapSize_.height()));
            painter.setPen(QPen(QColor(255, 255, 0), 2));
            painter.setBrush(QColor(255, 255, 0, 36));
            painter.drawRect(widgetRect);
        }
    }
}

} // namespace MapEditor

// ============================================================================
// MapEditor UI — ViewportWidget implementation (T2.2).
// ============================================================================

#include "ViewportWidget.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "ViewportRenderer.h"

#ifdef MAP_EDITOR_GPU_VIEW
#include "TerrainGpuView.h"
#else
// The MapEditorRender target is not built (OPTION_SOKOL off or sokol deps
// missing): the GPU oblique path is disabled and the CPU voxel renderer
// (T2.3) stays authoritative. The stub keeps the widget code identical.
namespace MapEditor {
class TerrainGpuView
{
public:
    struct CameraState
    {
        double centerX = 0;
        double centerY = 0;
        double turnAngleDeg = 0;
        double leanDeg = 140;
        double heightCells = 700;
        double focalDevPx = 600;
    };
    bool isReady() const { return false; }
    void setColorSource(ViewportRenderer *) {}
    void setCamera(const CameraState &) {}
    bool init(int, int) { return false; }
    void shutdown() {}
    void updateCells(int, int, int, int) {}
    void reloadWorld() {}
    void renderFrame(unsigned, int, int) {}
    bool pickCell(double, double, int &, int &) const { return false; }
    bool projectToScreen(double, double, double, double &, double &) const
    {
        return false;
    }
    bool projectToScreenZ(double, double, double, double &, double &,
                          double &) const { return false; }
    bool groundFootprint(double, double out[4], double outY[4]) const
    {
        static_cast<void>(out);
        static_cast<void>(outY);
        return false;
    }
    float zeroplastZ() const { return 0.0f; }
    float nearPlaneZ() const { return 1.0f; }
    int projectGroundContour(const double*, int, double*, double*) const { return 0; }
};
} // namespace MapEditor
#endif

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLTextureBlitter>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

#include "EditorTrace.h"

namespace MapEditor {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ViewportWidget::ViewportWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // The GPU oblique path needs a desktop GL context sokol's GLCORE backend
    // can run on (GL >= 3.3/4.1). Request it on THIS widget only — the
    // default application format (and the MinimapWidget) stays untouched, so
    // the CPU-rendered rasters keep their plain blit contexts. Without sokol
    // (MAP_EDITOR_GPU_VIEW off) the request is harmless: the CPU fallback
    // renders in whatever context Qt hands out.
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    if (format.renderableType() == QSurfaceFormat::OpenGL
        && (format.version() < qMakePair(4, 1))) {
        format.setVersion(4, 1);
        setFormat(format);
    }
}

ViewportWidget::~ViewportWidget()
{
    editorTrace("ViewportWidget: dtor start (isValid=%d, gpuView=%p)",
                isValid() ? 1 : 0, static_cast<void*>(gpuView_));
    // GL objects must die with a current context; skip silently when the
    // widget's window is already gone (destruction order) — the context
    // release frees the textures anyway.
    if (isValid()) {
        editorTrace("ViewportWidget: dtor makeCurrent");
        makeCurrent();
        editorTrace("ViewportWidget: dtor deleting gpuView_");
        delete gpuView_;
        gpuView_ = nullptr;
        editorTrace("ViewportWidget: dtor gpuView deleted, releasing GL resources");
        releaseGLResources();
        editorTrace("ViewportWidget: dtor doneCurrent");
        doneCurrent();
    }
    editorTrace("ViewportWidget: dtor deleting renderer_=%p", static_cast<void*>(renderer_));
    delete renderer_;
    editorTrace("ViewportWidget: dtor deleting view3d_=%p", static_cast<void*>(view3d_));
    delete view3d_;
    editorTrace("ViewportWidget: dtor done");
}

void ViewportWidget::initializeGL()
{
    // The context may be destroyed/recreated behind our back (window
    // teardown, format changes) — free the GL-backed state with it, the
    // (re)creation below rebuilds everything.
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        editorTrace("ViewportWidget: aboutToBeDestroyed hook");
        releaseGLResources();
        delete gpuView_;
        gpuView_ = nullptr;
    }, Qt::DirectConnection);

    editorTrace("ViewportWidget: initializeGL start");
    glBlitter_ = new QOpenGLTextureBlitter();
    if (!glBlitter_->create()) {
        qWarning("ViewportWidget: QOpenGLTextureBlitter creation failed, "
                 "the base rasters will not be drawn");
        delete glBlitter_;
        glBlitter_ = nullptr;
    }

    // Build (or rebuild) the GPU scene now that the context is current —
    // same deferred path paintGL uses.
    gpuRebuildPending_ = true;
    rebuildGpuView();
    editorTrace("ViewportWidget: initializeGL done");
}

void ViewportWidget::releaseGLResources()
{
    if (glBlitter_) {
        delete glBlitter_;
        glBlitter_ = nullptr;
    }
    if (QOpenGLFunctions *gl = context()->functions()) {
        if (mapTexture_ != 0) {
            gl->glDeleteTextures(1, &mapTexture_);
        }
        if (frameTexture_ != 0) {
            gl->glDeleteTextures(1, &frameTexture_);
        }
    }
    mapTexture_ = 0;
    mapTextureSize_ = QSize();
    frameTexture_ = 0;
    frameTextureSize_ = QSize();
}

// ---------------------------------------------------------------------------
// Session binding
// ---------------------------------------------------------------------------

void ViewportWidget::setSession(EditorSession *session)
{
    editorTrace("ViewportWidget: setSession start (session=%p, worldLoaded=%d)",
                static_cast<void*>(session),
                (session && session->isWorldLoaded()) ? 1 : 0);
    session_ = session;
    delete renderer_;
    renderer_ = nullptr;

    // The GPU scene feeds from the renderer — rebuild it around whatever the
    // new session provides (deferred, no GL context here).
    editorTrace("ViewportWidget: setSession scheduling GPU rebuild");
    scheduleGpuRebuild();

    if (session_ && session_->isWorldLoaded()) {
        editorTrace("ViewportWidget: setSession creating renderer");
        renderer_ = new ViewportRenderer();
        editorTrace("ViewportWidget: setSession bindWorld");
        if (renderer_->bindWorld()) {
            editorTrace("ViewportWidget: setSession applyViewportSettings");
            applyViewportSettings();
            editorTrace("ViewportWidget: setSession updateFitScale");
            updateFitScale();
            editorTrace("ViewportWidget: setSession zoomToFit");
            zoomToFit();
            editorTrace("ViewportWidget: setSession update");
            update();
        } else {
            editorTrace("ViewportWidget: setSession bindWorld FAILED");
        }
    } else {
        editorTrace("ViewportWidget: setSession no world — updateFitScale");
        updateFitScale();
        update();
    }
    editorTrace("ViewportWidget: setSession done");
}

void ViewportWidget::reloadWorld()
{
    if (!session_ || !session_->isWorldLoaded()) {
        delete renderer_;
        renderer_ = nullptr;
        updateFitScale();
        needs3DRedraw_ = true;
        scheduleGpuRebuild();
        update();
        return;
    }

    if (!renderer_ || !renderer_->isBound()) {
        delete renderer_;
        renderer_ = new ViewportRenderer();
        if (renderer_->bindWorld()) {
            applyViewportSettings();
            updateFitScale();
            zoomToFit();
            needs3DRedraw_ = true;
            scheduleGpuRebuild();
            update();
        }
    }
}

/// Push the widget-level view settings into a freshly bound renderer: the
/// geo dimming (the cache paints in bindWorld before the flag exists) and
/// the 3D look-at centre — a new world opens with the camera at its centre,
/// not at the (0,0) corner.
void ViewportWidget::applyViewportSettings()
{
    if (!renderer_) {
        return;
    }
    renderer_->setGeoDimmed(geoDimmed_);
    obliqueCenter_ = QPointF(renderer_->width() / 2.0,
                             renderer_->height() / 2.0);
    needs3DRedraw_ = true;
}

// ---------------------------------------------------------------------------
// Brush editing (T3.1)
// ---------------------------------------------------------------------------

void ViewportWidget::setBrushTool(BrushTool *brush)
{
    brushTool_ = brush;
}

void ViewportWidget::setEditorMode(EditorMode mode)
{
    if (editorMode_ == mode) {
        return;
    }
    editorMode_ = mode;
    // An in-flight drag belongs to the old mode: drop it so the mouse does
    // not continue a brush stroke as a pick (or vice versa) after a switch.
    brushPainting_ = false;
    linePending_ = false;
    lineStart_ = {-1, -1};
    lineStartWidgetPos_ = {-1, -1};
    picking_ = false;
    lastPickCell_ = {-1, -1};
    setCursor(paintCursorShape());
    emit editorModeChanged(editorMode_);
    update();
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
        viewport.setNeeds3DRedraw();
        viewport.update();
    }
    return changed;
}

/// The Eyedropper-mode click at `cell`: run the PickColor brush type through
/// BrushTool (the pick sample lands in BrushTool::params().colorIndex) and
/// announce the sampled Geo/Dam pixel. Returns true when a color was picked.
bool ViewportWidget::pickColorAtCell(const QPoint &cell)
{
    MapEditor::BrushTool *brush = brushTool_;
    if (!brush || !session_ || !session_->isWorldLoaded()
            || editorMode_ != EditorMode::Eyedropper) {
        return false;
    }
    const MapEditor::ViewportRenderer *r = renderer();
    if (!r || !r->isBound()
            || cell.x() < 0 || cell.y() < 0
            || cell.x() >= r->width() || cell.y() >= r->height()) {
        return false;
    }
    // The mode is authoritative for what a click does: the effect is forced
    // to PickColor here, so a mode switched from the UI picks even if the
    // brush parameters were not re-synced yet. BrushTool::pickColorAt also
    // updates params().colorIndex with the sampled index.
    MapEditor::BrushParams pickParams = brush->params();
    pickParams.effect = MapEditor::BrushEffect::PickColor;
    if (!brush->applyStamp(cell.x(), cell.y(), pickParams)) {
        return false;
    }
    // No world edit happened — no worldEdited / 3D redraw, only the pick.
    emit eyedropperPicked(cell.x(), cell.y(), brush->lastPickedColorIndex());
    update();
    return true;
}

QPoint ViewportWidget::onMapCellAtWidgetPos(const QPoint &widgetPos) const
{
    const QPoint cell = cellAtWidgetPos(widgetPos);
    if (!renderer_ || !renderer_->isBound()
            || cell.x() < 0 || cell.y() < 0
            || cell.x() >= renderer_->width()
            || cell.y() >= renderer_->height()) {
        return {-1, -1};
    }
    return cell;
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
    // The upper bound depends on the render style: Chunky stays at the
    // plan T2.2 range "1:1…fit" (solid cell blocks), Smooth allows
    // magnifying bilinearly like the game camera — up to kMaxSmoothZoom,
    // but never below the fit scale (a world smaller than the widget must
    // still be able to fill it). The lower bound is fit.
    const double maxZoom = renderStyle_ == RenderStyle::Smooth
                               ? std::max(fitScale_, kMaxSmoothZoom) : 1.0;
    if (zoom_ > maxZoom) {
        zoom_ = maxZoom;
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
    needs3DRedraw_ = true;
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
// Renderer style (chunky / smooth)
// ---------------------------------------------------------------------------

void ViewportWidget::setRenderStyle(RenderStyle style)
{
    if (renderStyle_ == style) {
        return;
    }
    renderStyle_ = style;
    if (style == RenderStyle::Chunky && zoom_ > 1.0) {
        // Back from a magnified Smooth view: clamping to 1:1 shrinks the map
        // under a fixed pan offset, so keep the widget-centre cell stable.
        const QPointF widgetCenter(width() / 2.0, height() / 2.0);
        const QPoint cell = screenToMap(qRound(widgetCenter.x()),
                                        qRound(widgetCenter.y()));
        clampZoom();
        if (cell.x() >= 0 && cell.y() >= 0) {
            const QPointF screenAfter = mapToScreen(cell.x(), cell.y());
            panOffset_ -= (screenAfter - widgetCenter);
        }
    } else {
        clampZoom();
    }
    needs3DRedraw_ = true;
    update();
    emit renderStyleChanged(renderStyle_);
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
    obliqueCamera_.cameraDZ = View3DRenderer::clampCameraDZ(camera.cameraDZ, gpuView_ != nullptr);
    obliqueCamera_.focusC = View3DRenderer::clampFocus(camera.focusC);
    needs3DRedraw_ = true;
    applyGpuCamera();
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
    needs3DRedraw_ = true;
    applyGpuCamera();
    emit obliqueCameraChanged(obliqueCamera_);
    update();
}

void ViewportWidget::adjustObliqueTilt(double deltaDeg)
{
    obliqueCamera_.leanDeg += deltaDeg;
    needs3DRedraw_ = true;
    applyGpuCamera();
    emit obliqueCameraChanged(obliqueCamera_);
    update();
}

void ViewportWidget::adjustObliqueLift(int deltaCells)
{
    const int next = View3DRenderer::clampCameraDZ(
        obliqueCamera_.cameraDZ + deltaCells, gpuView_ != nullptr);
    if (next == obliqueCamera_.cameraDZ) {
        return;
    }
    obliqueCamera_.cameraDZ = next;
    needs3DRedraw_ = true;
    applyGpuCamera();
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
    needs3DRedraw_ = true;
    applyGpuCamera();
    emit obliqueCameraChanged(obliqueCamera_);
    update();
}

void ViewportWidget::resetObliqueView()
{
    obliqueCamera_ = View3DCamera{};
    obliqueCenter_ = renderer_ ? QPointF(renderer_->width() / 2.0,
                                         renderer_->height() / 2.0)
                               : QPointF();
    needs3DRedraw_ = true;
    applyGpuCamera();
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
    // The 3D overlay is baked into the cached RGB565 frame — force a fresh
    // render so the toggle/step shows up in Oblique mode too (2D repaints
    // through update() alone).
    needs3DRedraw_ = true;
    update();
}

void ViewportWidget::setGridStep(int cellsX, int cellsY)
{
    gridStepX_ = std::clamp(cellsX, 1, 1024);
    gridStepY_ = std::clamp(cellsY, 1, 1024);
    needs3DRedraw_ = true; // the 3D overlay is baked into the frame
    update();
}

void ViewportWidget::setGeoDimmed(bool dimmed)
{
    if (geoDimmed_ == dimmed) {
        return;
    }
    geoDimmed_ = dimmed;
    if (renderer_) {
        renderer_->setGeoDimmed(dimmed); // marks the whole cache + mips dirty
    }
    // The GPU tilemap baked colors from the cache — full rebuild.
    scheduleGpuRebuild();
    needs3DRedraw_ = true;
    update();
}

void ViewportWidget::setDamFrozen(bool frozen)
{
    if (damFrozen_ == frozen) {
        return;
    }
    damFrozen_ = frozen;
    needs3DRedraw_ = true; // applied per-frame in draw3DRaster()
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

    if (gpuActive()) {
        // GPU mode: the view maps 1:1 onto the widget (device pixels), no
        // 640-cell window clamp — the ground scale per widget pixel is
        // (F+Z)/F, the depth (rows) stretch uses the same kv heuristic as
        // the CPU path but on the raw lean.
        const double kv = obliqueForwardScale(obliqueCamera_,
                                              obliqueCamera_.leanDeg);
        const double cellsPerPx = obliqueCellsPerWidgetPx();
        const double groundWidth = width() * cellsPerPx;
        const double groundHeight = height() * kv * cellsPerPx;
        const int x0 = qRound(obliqueCenter_.x() - groundWidth / 2.0);
        const int y0 = qRound(obliqueCenter_.y() - groundHeight / 2.0);
        return QRect(x0, y0, qRound(groundWidth), qRound(groundHeight))
            .intersected(QRect(0, 0, renderer_->width(), renderer_->height()));
    }

    // Oblique CPU: the 3D window is not a linear map projection — approximate the
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

QPolygonF ViewportWidget::currentViewPolygon() const
{
    if (!renderer_) {
        return {};
    }
    if (viewMode_ == ViewMode::TopDown) {
        const QRect rect = visibleCellRange();
        return rect.isEmpty() ? QPolygonF() : QPolygonF(QRectF(rect));
    }

    // Oblique, GPU path: one ray per screen corner, intersected with the
    // zeroplast plane. This is what makes the outline a rotated trapezoid —
    // the azimuth comes from the camera turn, and the far (top) edge covers
    // much more ground than the near one, which an axis-aligned rect hides.
    if (gpuActive()) {
        double xs[4];
        double ys[4];
        if (gpuView_->groundFootprint(gpuView_->zeroplastZ(), xs, ys)) {
            QPolygonF poly;
            poly.reserve(4);
            for (int i = 0; i < 4; ++i) {
                poly << QPointF(xs[i], ys[i]);
            }
            return poly;
        }
    }

    // Oblique, CPU fallback: the viewV2D projection is affine, so the
    // footprint is the ground rectangle of the view basis rotated by the turn
    // angle — the same screen→ground axes panObliqueBy() inverts.
    const QSize buffer = obliqueBufferSize();
    const double leanDeg = View3DRenderer::stableLeanDeg(
        obliqueCamera_.leanDeg, obliqueCamera_.focusC, buffer.height() / 2);
    const double kv = obliqueForwardScale(obliqueCamera_, leanDeg);
    const double cellsPerPx =
        (obliqueCamera_.focusC + obliqueCamera_.cameraDZ) /
        static_cast<double>(obliqueCamera_.focusC);
    const double theta = qDegreesToRadians(obliqueCamera_.turnAngleDeg);
    const double cosT = std::cos(theta);
    const double sinT = std::sin(theta);

    // Corner offsets in widget pixels (screen order: TL, TR, BR, BL).
    const double dx[4] = {-width() / 2.0, width() / 2.0,
                          width() / 2.0, -width() / 2.0};
    const double dy[4] = {-height() / 2.0, -height() / 2.0,
                          height() / 2.0, height() / 2.0};
    QPolygonF poly;
    poly.reserve(4);
    for (int i = 0; i < 4; ++i) {
        poly << obliqueCenter_ + QPointF(
            (dx[i] * cosT - dy[i] * kv * sinT) * cellsPerPx,
            (dx[i] * sinT + dy[i] * kv * cosT) * cellsPerPx);
    }
    return poly;
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
        needs3DRedraw_ = true;
        applyGpuCamera();
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
        if (!lastEmittedViewRect_.isEmpty() || !lastEmittedViewPolygon_.isEmpty()) {
            lastEmittedViewRect_ = {};
            lastEmittedViewPolygon_ = {};
            emit viewRectChanged(lastEmittedViewRect_);
            emit viewPolygonChanged(lastEmittedViewPolygon_);
        }
        return;
    }
    const QRect viewRect = currentViewRect();
    if (viewRect != lastEmittedViewRect_) {
        lastEmittedViewRect_ = viewRect;
        emit viewRectChanged(lastEmittedViewRect_);
    }
    // The polygon is what the minimap actually draws (a rotated/trapezoidal
    // 3D zone), so it is compared and emitted on its own.
    const QPolygonF viewPoly = currentViewPolygon();
    if (viewPoly != lastEmittedViewPolygon_) {
        lastEmittedViewPolygon_ = viewPoly;
        emit viewPolygonChanged(lastEmittedViewPolygon_);
    }
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void ViewportWidget::paintGL()
{
    QOpenGLFunctions *gl = context()->functions();

    // Deferred GPU scene (re)build — needs the context current, which
    // paintGL is the first place after setSession/reloadWorld to guarantee.
    if (gpuRebuildPending_) {
        gpuRebuildPending_ = false;
        editorTrace("ViewportWidget: paintGL deferred rebuild");
        rebuildGpuView();
        editorTrace("ViewportWidget: paintGL deferred rebuild done");
    }

    // Notify MainWindow about the GPU/CPU path so it can show/hide the
    // freeze-dam action (a CPU-path-only fallback). Emitted once per
    // transition, not per frame.
    const bool gpuReadyNow = gpuView_ && gpuView_->isReady();
    if (gpuReadyNow != lastGpuViewReady_) {
        lastGpuViewReady_ = gpuReadyNow;
        editorTrace("ViewportWidget: paintGL GPU path %s",
                    gpuReadyNow ? "READY" : "fallback CPU");
        emit gpuViewReadyChanged(gpuReadyNow);
    }

    // Drain dirty rects / recompute lighting before painting, so the cache is
    // current when we upload/draw it.
    std::vector<DirtyRect> dirty;
    if (renderer_) {
        dirty = renderer_->update();
    }

    // Terrain cells were actually repainted (brush stamp, undo/redo, image
    // import, palette edit) — their composite colors are final now, so the
    // minimap can re-sample its overview from the live terrain. The signal is
    // deliberately emitted here rather than from the edit itself: at stamp time
    // renderQuant() has not run yet and the edited cells would be sampled with
    // their old lighting.
    if (!dirty.empty()) {
        emit terrainRepainted();
    }

    // The GPU tilemap consumes the same changed-cell rectangles the 2D cache
    // got (brush stamps, undo/redo) — queue them for the next terrain frame.
    if (gpuView_) {
        for (const DirtyRect &r : dirty) {
            gpuView_->updateCells(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1);
        }
    }

    if (!renderer_ || !renderer_->isBound()) {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(32, 32, 32));
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No world loaded.\n"
                            "Use File → Load World to open a map."));
        return;
    }

    // Base raster on the GPU (clear + textured quad), then the QPainter
    // overlays on the same framebuffer. The blit works like the old
    // painter.drawImage(): the whole texture is mapped onto the target
    // rectangle, GL handles the linear/nearest sampling per the render style.
    bool baseDrawn = false;
    if (viewMode_ == ViewMode::Oblique) {
        if (gpuActive()) {
            // The game's sokol terrain renderer draws the oblique view
            // straight into this widget's framebuffer; no intermediate
            // texture, no CPU raster involved.
            const qreal dpr = devicePixelRatioF();
            const int fboW = qRound(size().width() * dpr);
            const int fboH = qRound(size().height() * dpr);
            static int gpuFrameLog = 0;
            if (gpuFrameLog < 5) {
                editorTrace("ViewportWidget: GPU frame %d (%dx%d)", ++gpuFrameLog,
                            fboW, fboH);
            }
            gpuView_->setNearestFiltering(renderStyle_ == RenderStyle::Chunky);
            gpuView_->renderFrame(context()->defaultFramebufferObject(),
                                  fboW, fboH);
            // sokol left its own GL state behind — restore what QPainter
            // needs (its engine sets the rest itself).
            gl->glBindFramebuffer(GL_FRAMEBUFFER,
                                  context()->defaultFramebufferObject());
            gl->glDisable(GL_DEPTH_TEST);
            gl->glDisable(GL_BLEND);
            gl->glDisable(GL_SCISSOR_TEST);
            // sokol renders with GL_CULL_FACE enabled (the game's camera
            // cull mode) and never turns it off — the texture blitter never
            // touches cull state, so its CW-wound quad triangle would be
            // culled and the map drawn as a single half-triangle.
            gl->glDisable(GL_CULL_FACE);
            // Reset viewport to the full framebuffer. The sokol render
            // pipeline may leave the viewport set to the camera's
            // sub-region (via SetDrawTransform → SetVPMatrix). If the
            // next draw (TopDown) does not explicitly set the viewport,
            // the texture blitter would draw only into the old region,
            // producing a clipped / "two triangles" image.
            gl->glViewport(0, 0, fboW, fboH);
            gl->glActiveTexture(GL_TEXTURE0);
            baseDrawn = true;
        } else {
            baseDrawn = drawObliqueGL(gl);
        }
    } else {
        baseDrawn = drawTopDownGL(gl, dirty);
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!baseDrawn) {
        painter.fillRect(rect(), QColor(32, 32, 32));
    }

    if (viewMode_ == ViewMode::Oblique) {
        // The brush works in the 3D view as well — the preview ring and the
        // pending line stroke are drawn in screen space at the cursor.
        if (gridVisible_) drawGrid3D(painter);
        if (brushPreviewVisible_) drawBrushPreview(painter);
        drawHud(painter);
        emitViewRectIfChanged();
        return;
    }

    // --- Overlays (QPainter on top of the GL-drawn base raster) ---
    if (gridVisible_) drawGrid(painter);
    if (brushPreviewVisible_) drawBrushPreview(painter);
    drawMarkers(painter);
    drawSpawnPoints(painter);
    drawHud(painter);

    // --- Announce the view rectangle for the minimap (T2.4) ---
    emitViewRectIfChanged();
}

// ---------------------------------------------------------------------------
// GL state helpers
// ---------------------------------------------------------------------------

bool ViewportWidget::ensureMapTexture(QOpenGLFunctions *gl, int mapW, int mapH)
{
    if (mapTexture_ != 0 && mapTextureSize_ == QSize(mapW, mapH)) {
        return false;
    }
    if (mapTexture_ != 0) {
        gl->glDeleteTextures(1, &mapTexture_);
        mapTexture_ = 0;
    }
    gl->glGenTextures(1, &mapTexture_);
    gl->glBindTexture(GL_TEXTURE_2D, mapTexture_);
    // Sampling params are per-texture: default to the Smooth look, every
    // frame's draw sets the active style anyway.
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    mapTextureSize_ = QSize(mapW, mapH);
    return true;
}

void ViewportWidget::uploadFrameTexture(QOpenGLFunctions *gl)
{
    if (!view3d_ || view3d_->pixels() == nullptr) {
        return;
    }
    const QSize size(view3d_->windowWidth(), view3d_->windowHeight());
    if (frameTexture_ != 0 && frameTextureSize_ != size) {
        gl->glDeleteTextures(1, &frameTexture_);
        frameTexture_ = 0;
    }
    if (frameTexture_ == 0) {
        gl->glGenTextures(1, &frameTexture_);
        gl->glBindTexture(GL_TEXTURE_2D, frameTexture_);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        frameTextureSize_ = size;
    } else {
        gl->glBindTexture(GL_TEXTURE_2D, frameTexture_);
    }
    // RGB565 rows: the window width is kept even by resizeWindow(), so every
    // row is 4-byte aligned (the default GL_UNPACK_ALIGNMENT holds).
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     size.width(), size.height(), 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                     reinterpret_cast<const GLvoid *>(view3d_->pixels()));
}

// ---------------------------------------------------------------------------
// GL base rasters
// ---------------------------------------------------------------------------

bool ViewportWidget::drawTopDownGL(QOpenGLFunctions *gl,
                                   const std::vector<DirtyRect> &dirty)
{
    const int mapW = renderer_->width();
    const int mapH = renderer_->height();
    if (!glBlitter_ || !renderer_->pixels() || mapW <= 0 || mapH <= 0) {
        return false;
    }

    const bool fresh = ensureMapTexture(gl, mapW, mapH);
    gl->glBindTexture(GL_TEXTURE_2D, mapTexture_);
    if (fresh) {
        // Memory layout of the cache is Format_ARGB32: byte order B,G,R,A —
        // GL_BGRA feeds the channels straight through.
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mapW, mapH, 0,
                         GL_BGRA, GL_UNSIGNED_BYTE,
                         reinterpret_cast<const GLvoid *>(renderer_->pixels()));
    } else {
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, mapW);
        for (const DirtyRect &r : dirty) {
            gl->glTexSubImage2D(
                GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h,
                GL_BGRA, GL_UNSIGNED_BYTE,
                reinterpret_cast<const GLvoid *>(renderer_->pixels() +
                                                 r.y * mapW + r.x));
        }
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    // Ensure viewport covers the full widget — the GPU pass may have left
    // a sub-region viewport from the sokol camera's SetDrawTransform.
    gl->glViewport(0, 0, qRound(size().width() * devicePixelRatioF()),
                   qRound(size().height() * devicePixelRatioF()));

    gl->glClearColor(32.0f / 255.0f, 32.0f / 255.0f, 32.0f / 255.0f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_BLEND);
    // The blitter quad is a triangle strip whose first triangle winds CW.
    // A sokol 3D frame leaves GL_CULL_FACE enabled (front CW, cull front),
    // which would cull exactly that triangle and leave the map cut along
    // the quad diagonal — make the blit independent of the previous pass.
    gl->glDisable(GL_CULL_FACE);

    // Chunky shows solid blocks (nearest sampling, no filter at >1:1);
    // Smooth blends neighboring cells like the in-game camera. The same rule
    // the old raster path used for QPainter::SmoothPixmapTransform.
    const GLint filter = (zoom_ < 1.0 || renderStyle_ == RenderStyle::Smooth)
                             ? GL_LINEAR : GL_NEAREST;
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    const QRectF targetRect(panOffset_, QSizeF(mapW * zoom_, mapH * zoom_));
    glBlitter_->bind();
    glBlitter_->blit(mapTexture_,
                     QOpenGLTextureBlitter::targetTransform(
                         targetRect, QRect(QPoint(0, 0), size())),
                     QOpenGLTextureBlitter::OriginTopLeft);
    glBlitter_->release();
    return true;
}

bool ViewportWidget::drawObliqueGL(QOpenGLFunctions *gl)
{
    if (!glBlitter_ || !renderer_ || !renderer_->isBound()) {
        return false;
    }

    // Resize the brain-free window: full widget size, clamped by Terra's own
    // MAX_XSIZE_VX_WINDOW (the widget then scales the frame back up). The
    // same buffer size drives currentViewRect(), so the minimap footprint
    // matches the frame about to be drawn.
    if (!view3d_) {
        view3d_ = new View3DRenderer();
    }
    if (gpuActive()) {
        view3d_->toggleGPUIsAvailable();
    }
    const QSize buffer = obliqueBufferSize();
    view3d_->resizeWindow(buffer.width(), buffer.height());

    // Camera + look-at. The centre is passed UNWRAPPED (it may sit outside
    // the map after a long scroll): viewV2D/renderSmooth wrap it themselves,
    // and the freeze-dam mode needs the raw value to tell the one real map
    // copy from its torus repeats.
    view3d_->setCamera(obliqueCamera_);
    view3d_->setDamFrozen(damFrozen_);
    // The chunky path greys Terra's 16-bit tables on the fly; the smooth
    // path bakes the dimming into the mip pyramid (same widget flag).
    view3d_->setGeoDimmed(geoDimmed_);
    // The smooth style filters the voxel view through the mip pyramid kept by
    // the 2D renderer; chunky keeps Terra's point-sampled viewV2D output.
    view3d_->setColorSource(renderer_);
    view3d_->setSmooth(renderStyle_ == RenderStyle::Smooth);

    // Only re-render the 3D frame when the camera moved, terrain was
    // stamped, or the window resized; otherwise the frame texture is
    // already current and just gets blitted again.
    if (needs3DRedraw_) {
        view3d_->render(static_cast<int>(std::floor(obliqueCenter_.x())),
                        static_cast<int>(std::floor(obliqueCenter_.y())));

        // The one grid overlays the 3D frame like the 2D raster's: a flat
        // reference-plane grid at the zeroplast height (OXY offsets,
        // independent of the terrain shape), white lines two pixels thick,
        // drawn once its lines resolve on screen.
        if (gridVisible_) {
            const QPointF pxPerCell = obliquePxPerCell();
            const double bufferScaleX =
                width() > 0 && buffer.width() > 0
                    ? buffer.width() / static_cast<double>(width()) : 1.0;
            const double cellPx = qMin(pxPerCell.x(), pxPerCell.y()) *
                                  bufferScaleX;
            if (gridStepX_ > 0 &&
                gridStepX_ * cellPx >= kMinGridSpacingPx * 0.75) {
                view3d_->drawGridOverlay(gridStepX_, gridStepY_, 0xFFFF);
            }
        }

        uploadFrameTexture(gl);
        needs3DRedraw_ = false;
    }
    if (frameTexture_ == 0) {
        return false;
    }

    // Ensure viewport covers the full widget before blitting the 3D frame.
    gl->glViewport(0, 0, qRound(size().width() * devicePixelRatioF()),
                   qRound(size().height() * devicePixelRatioF()));

    gl->glClearColor(32.0f / 255.0f, 32.0f / 255.0f, 32.0f / 255.0f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_BLEND);
    // The blitter quad is a triangle strip whose first triangle winds CW.
    // A sokol 3D frame leaves GL_CULL_FACE enabled (front CW, cull front),
    // which would cull exactly that triangle and leave the map cut along
    // the quad diagonal — make the blit independent of the previous pass.
    gl->glDisable(GL_CULL_FACE);

    // The 3D window buffer is widget-size clamped to Terra's
    // MAX_XSIZE_VX_WINDOW, so it is usually upscaled to the widget: Smooth
    // style interpolates the frame, Chunky draws the raw pixels — the voxels
    // keep their hard edges.
    const GLint filter = renderStyle_ == RenderStyle::Smooth
                             ? GL_LINEAR : GL_NEAREST;
    gl->glBindTexture(GL_TEXTURE_2D, frameTexture_);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    glBlitter_->bind();
    glBlitter_->blit(frameTexture_,
                     QOpenGLTextureBlitter::targetTransform(
                         QRectF(rect()), QRect(QPoint(0, 0), size())),
                     QOpenGLTextureBlitter::OriginTopLeft);
    glBlitter_->release();
    return true;
}

// ---------------------------------------------------------------------------
// Grid overlay
// ---------------------------------------------------------------------------

/// GPU-mode grid: a flat reference-plane grid at ground height zero (the
/// bottom of the terrain, NOT the zeroplast plane — the zeroplast sits above
/// the lowest geo, so a grid drawn on it floats over the landscape), projected
/// through the sokol camera and drawn with QPainter as white 4 px lines.
///
/// A straight ground line stays straight under the perspective projection, so
/// two end points define it — but the camera looks along the ground, so most
/// lines leave through the near plane rather than the map edge. Each line is
/// therefore clipped against the camera near plane (the segment parameter at
/// which the view-space depth crosses it), and only the visible part is drawn.
void ViewportWidget::drawGrid3D(QPainter &painter)
{
    if (!gpuActive() || gridStepX_ <= 0 || gridStepY_ <= 0) return;

    const int mapW = renderer_->width();
    const int mapH = renderer_->height();
    const double z = 0.0;   // the bottom ground plane
    const qreal dpr = devicePixelRatioF();

    // Visible ground area: the frustum footprint on the plane (exact, unlike
    // the old kv-heuristic rect), clamped to the map.
    double fx[4], fy[4];
    if (!gpuView_->groundFootprint(z, fx, fy)) return;
    double minX = fx[0], maxX = fx[0], minY = fy[0], maxY = fy[0];
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, fx[i]);
        maxX = std::max(maxX, fx[i]);
        minY = std::min(minY, fy[i]);
        maxY = std::max(maxY, fy[i]);
    }
    const int xBegin = qMax(0, static_cast<int>(std::floor(minX)));
    const int xEnd = qMin(mapW, static_cast<int>(std::ceil(maxX)));
    const int yBegin = qMax(0, static_cast<int>(std::floor(minY)));
    const int yEnd = qMin(mapH, static_cast<int>(std::ceil(maxY)));
    if (xEnd <= xBegin || yEnd <= yBegin) return;

    // Resolution guard: lines are drawn every gridStepX_/gridStepY_ world
    // units, exactly as in the 2D grid. The perspective scale varies across
    // the view, so measure the spacing where the grid is largest on screen —
    // at the near edge of the footprint. Below the minimum spacing the lines
    // would merge into a white field, so the grid stays hidden.
    const double nearY = static_cast<double>(yBegin);
    const double midX = 0.5 * (xBegin + xEnd);
    double camZ0, sx0, sy0, camZ1, sx1, sy1, camZ2, sx2, sy2;
    if (!gpuView_->projectToScreenZ(midX, nearY, z, camZ0, sx0, sy0)
        || !gpuView_->projectToScreenZ(midX + gridStepX_, nearY, z, camZ1, sx1, sy1)
        || !gpuView_->projectToScreenZ(midX, nearY + gridStepY_, z, camZ2, sx2, sy2)
        || camZ0 <= 0.0) {
        return;
    }
    const double stepPxX = std::hypot(sx1 - sx0, sy1 - sy0) / dpr;
    const double stepPxY = std::hypot(sx2 - sx0, sy2 - sy0) / dpr;
    if (stepPxX < kMinGridSpacingPx && stepPxY < kMinGridSpacingPx) {
        editorTrace("ViewportWidget: grid step=%dx%d hidden (pxX=%.2f pxY=%.2f at near edge)",
                    gridStepX_, gridStepY_, stepPxX, stepPxY);
        return;
    }

    // Near-plane clipping constant: the camera's own near plane — terrain
    // closer than this is not rasterized, and the grid must use the same
    // bound so its lines do not intrude into the sky region.
    const double nearPlane = gpuView_->nearPlaneZ();
    const auto clipToNear = [this, z, nearPlane, dpr](
                                double ax, double ay, double bx, double by) {
        double za, sa, ta, zb, sb, tb;
        if (!gpuView_->projectToScreenZ(ax, ay, z, za, sa, ta)
            || !gpuView_->projectToScreenZ(bx, by, z, zb, sb, tb)) {
            return QLineF();
        }
        if (za <= nearPlane && zb <= nearPlane) {
            return QLineF();   // the whole segment is behind the near plane
        }
        if (za <= nearPlane) {
            const double t = (nearPlane - za) / (zb - za);
            ax += (bx - ax) * t;
            ay += (by - ay) * t;
            if (!gpuView_->projectToScreenZ(ax, ay, z, za, sa, ta)) {
                return QLineF();
            }
        }
        if (zb <= nearPlane) {
            const double t = (nearPlane - zb) / (za - zb);
            bx += (ax - bx) * t;
            by += (ay - by) * t;
            if (!gpuView_->projectToScreenZ(bx, by, z, zb, sb, tb)) {
                return QLineF();
            }
        }
        return QLineF(sa / dpr, ta / dpr, sb / dpr, tb / dpr);
    };

    painter.setPen(QPen(QColor(255, 255, 255), 4.0));

    // Align to the grid origin, not to the first visible cell (as in 2D).
    const int x0f = (xBegin / gridStepX_) * gridStepX_;
    for (int x = x0f; x <= xEnd; x += gridStepX_) {
        const QLineF line = clipToNear(x, yBegin, x, yEnd);
        if (!line.isNull()) painter.drawLine(line);
    }
    const int y0f = (yBegin / gridStepY_) * gridStepY_;
    for (int y = y0f; y <= yEnd; y += gridStepY_) {
        const QLineF line = clipToNear(xBegin, y, xEnd, y);
        if (!line.isNull()) painter.drawLine(line);
    }
}

void ViewportWidget::drawGrid(QPainter &painter)
{
    if (!renderer_ || !renderer_->isBound()) return;

    const QRect vis = visibleCellRange();
    const int x1 = vis.x() + vis.width();
    const int y1 = vis.y() + vis.height();
    const int mapH = renderer_->height();
    const int mapW = renderer_->width();

    // The one grid: the editable cell size (default Terra's kmGrid = 4),
    // drawn once its lines resolve on screen. No second coarse reference
    // frame — the step spinboxes control the single grid.
    if (zoom_ * gridStepX_ >= kMinGridSpacingPx ||
        zoom_ * gridStepY_ >= kMinGridSpacingPx) {
        painter.setPen(QColor(70, 70, 70, 90));
        // Align to the grid origin, not to the first visible cell.
        const int x0f = (vis.x() / gridStepX_) * gridStepX_;
        for (int x = x0f; x <= x1; x += gridStepX_) {
            QPointF p1 = mapToScreen(x, 0);
            QPointF p2 = mapToScreen(x, mapH);
            painter.drawLine(p1, p2);
        }
        const int y0f = (vis.y() / gridStepY_) * gridStepY_;
        for (int y = y0f; y <= y1; y += gridStepY_) {
            QPointF p1 = mapToScreen(0, y);
            QPointF p2 = mapToScreen(mapW, y);
            painter.drawLine(p1, p2);
        }
    }
}

// ---------------------------------------------------------------------------
// Brush preview
// ---------------------------------------------------------------------------

void ViewportWidget::drawBrushPreview(QPainter &painter)
{
    if (!renderer_ || !renderer_->isBound() || !brushPreviewVisible_) return;
    // The stamp silhouette only follows the brush; the Eyedropper mode has no
    // stamp (a pipette point), the View mode does not edit at all.
    if (editorMode_ != EditorMode::Brush) return;

    QPoint cell = cursorCell_;
    if (cell.x() < 0 || cell.y() < 0 ||
        cell.x() >= renderer_->width() || cell.y() >= renderer_->height()) {
        return;
    }

    painter.setPen(QPen(QColor(255, 255, 0, 180), 1.5, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);

    if (viewMode_ == ViewMode::Oblique) {
        // GPU path: the perspective camera images a ground disc as a sheared,
        // offset ellipse whose size depends on how far the cursor sits from the
        // look-at point — the CPU view's (F+Z)/F ground scale is wrong here (it
        // is the nadir scale of the voxel window, not this camera's), so the
        // ring was drawn far too large. Project the real stamp footprint.
        const QPolygonF contour = projectedBrushContour();
        if (contour.size() >= 3) {
            painter.drawPolygon(contour);
            if (linePending_ && lineStartWidgetPos_.x() >= 0) {
                painter.drawLine(QPointF(lineStartWidgetPos_),
                                 QPointF(cursorWidgetPos_));
            }
            return;
        }
        // The oblique projection has no linear cell→screen map, so the ring is
        // centred on the cursor and sized by the camera's approximate
        // widget-pixels-per-ground-cell (the same basis the pick uses).
        const QPointF pxPerCell = obliquePxPerCell();
        // GPU path: use a smaller minimum (1.0) so the perspective ellipse
        // is visible even for small brush radii. The 3.0 minimum was hiding
        // the Y-stretch from the oblique camera deformation.
        const double minDim = (gpuView_ && gpuView_->isReady()) ? 1.0 : 3.0;
        const double rx = qMax(minDim, brushRadius_ * pxPerCell.x());
        const double ry = qMax(minDim, brushRadius_ * pxPerCell.y());
        const QPointF center(cursorWidgetPos_);
        if (brushShape_ == BrushPreviewShape::Circle) {
            painter.drawEllipse(center, rx, ry);
        } else {
            painter.drawRect(QRectF(center.x() - rx, center.y() - ry,
                                    rx * 2.0, ry * 2.0));
        }
        if (linePending_ && lineStartWidgetPos_.x() >= 0) {
            painter.drawLine(QPointF(lineStartWidgetPos_), center);
        }
        return;
    }

    // Center of the cell in widget coords.
    QPointF center = mapToScreen(cell.x() + 0.5, cell.y() + 0.5);
    double screenR = brushRadius_ * zoom_;

    if (brushShape_ == BrushPreviewShape::Circle) {
        painter.drawEllipse(center, screenR, screenR);
    } else {
        QRectF r(center.x() - screenR, center.y() - screenR,
                 screenR * 2, screenR * 2);
        painter.drawRect(r);
    }

    // Pending Line stroke: start marker + the line to the cursor cell.
    if (linePending_ && lineStart_.x() >= 0) {
        const QPointF start = mapToScreen(lineStart_.x() + 0.5,
                                          lineStart_.y() + 0.5);
        painter.drawLine(start, center);
        if (brushShape_ == BrushPreviewShape::Circle) {
            painter.drawEllipse(start, screenR, screenR);
        } else {
            painter.drawEllipse(start, 3.0, 3.0);
        }
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
        if (m.radius > 0) {
            // Zone: a circle outline with the radius in map cells.
            const double screenRadius = static_cast<double>(m.radius) * zoom_;
            painter.setPen(QPen(m.color, 2));
            painter.setBrush(QColor(m.color.red(), m.color.green(),
                                    m.color.blue(), 40));
            painter.drawEllipse(pos, screenRadius, screenRadius);
        }
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
    // The HUD text looks blurry when the GPU path is active (the widget
    // renders at device-pixel resolution and QPainter's text gets scaled
    // back down). Skip it when the GPU scene is ready.
    if (lastGpuViewReady_) return;

    QString info;
    // The editor interaction mode the cursor carries right now.
    const QString modeLabel = editorMode_ == EditorMode::View
        ? tr("View") : (editorMode_ == EditorMode::Eyedropper ? tr("Pipette")
                                                              : tr("Brush"));
    if (viewMode_ == ViewMode::Oblique) {
        // The 3D camera line-up; lean is the value the active renderer will
        // actually use (the CPU voxel window clamps it into the stable
        // band, the GPU camera takes it raw — see obliqueMathLeanDeg()).
        const double lean = obliqueMathLeanDeg();
        info = tr("%1  |  3D  |  Rot: %2\u00B0  Tilt: %3\u00B0  Lift: %4  Focus: %5")
                   .arg(modeLabel)
                   .arg(qRound(obliqueCamera_.turnAngleDeg))
                   .arg(qRound(lean))
                   .arg(obliqueCamera_.cameraDZ)
                   .arg(obliqueCamera_.focusC);
    } else {
        info = modeLabel;
        if (cursorCell_.x() >= 0 && cursorCell_.y() >= 0 &&
            cursorCell_.x() < renderer_->width() &&
            cursorCell_.y() < renderer_->height()) {
            info += QString(tr("  |  X: %1  Y: %2")).arg(cursorCell_.x()).arg(cursorCell_.y());
        } else {
            info += tr("  |  Outside map");
        }
        info += QString(tr("  |  Zoom: %1%")).arg(qRound(zoom_ * 100));
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
        // The pan drag works in every editor mode (navigation is always
        // available; the eyedropper of T4.2 is left-click, so right-click is
        // free for the camera here).
        panning_ = true;
        panStart_ = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    const bool altDown = event->modifiers().testFlag(Qt::AltModifier);
    if (event->button() == Qt::LeftButton
            && viewMode_ == ViewMode::Oblique
            && (editorMode_ == EditorMode::View || altDown)) {
        // Orbit drag — the View-mode record of camera control and the
        // "limited" camera change the Brush/Eyedropper modes keep (Alt held).
        // Horizontal — rotation around Z, vertical — tilt.
        orbiting_ = true;
        panStart_ = event->position();
        setCursor(Qt::SizeAllCursor);
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton
            && viewMode_ == ViewMode::TopDown
            && editorMode_ == EditorMode::View) {
        // Navigation mode in the 2D view: left drag pans the map (in Oblique
        // the left drag orbits instead — handled above).
        panning_ = true;
        panStart_ = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton
            && editorMode_ == EditorMode::Eyedropper) {
        // T4.2 — the eyedropper click: sample the Geo/Dam pixel (SurBuf index)
        // under the cursor in BOTH view modes through the PickColor brush
        // type; holding the button down keeps sampling while the cursor moves
        // over new cells (drag = stamping in Brush mode, sampling here).
        const QPoint pos = event->position().toPoint();
        const QPoint cell = onMapCellAtWidgetPos(pos);
        if (cell.x() >= 0 && cell.y() >= 0) {
            cursorWidgetPos_ = pos;
            cursorCell_ = cell;
            picking_ = true;
            lastPickCell_ = cell;
            setCursor(Qt::CrossCursor);
            pickColorAtCell(cell);
        }
        event->accept();
        return;
    }

    const bool brushLeft = event->button() == Qt::LeftButton
                           && isBrushEditingEnabled();

    if (brushLeft) {
        // Brush painting (T3.1..T3.5 tools) works in BOTH view modes when the
        // editor mode is Brush: TopDown maps the cursor linearly, Oblique
        // ray-picks the voxel surface (cellAtWidgetPos). Press applies one
        // stamp at the cursor cell; a following drag stamps along the path
        // (mouseMoveEvent).
        const QPoint pos = event->position().toPoint();
        const QPoint cell = onMapCellAtWidgetPos(pos);
        if (cell.x() >= 0) {
            cursorWidgetPos_ = pos;
            cursorCell_ = cell;
            if (brushTool_ && brushTool_->params().shape == BrushShape::Line &&
                brushTool_->params().effect != BrushEffect::ImageDecal) {
                // The Line form is a two-point stroke: press marks the start,
                // the release draws the whole line as ONE undo entry.
                linePending_ = true;
                lineStart_ = cell;
                lineStartWidgetPos_ = pos;
                setCursor(Qt::CrossCursor);
                update();
                event->accept();
                return;
            }
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
    QOpenGLWidget::mousePressEvent(event);
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

    // Compute the cell under cursor in the CURRENT view mode (TopDown is a
    // linear map; Oblique ray-picks the voxel surface) and emit only when it
    // changes.
    QPoint pos = event->position().toPoint();
    cursorWidgetPos_ = pos;
    QPoint cell = cellAtWidgetPos(pos);
    if (cell != cursorCell_) {
        cursorCell_ = cell;
        if (cell.x() >= 0 && cell.y() >= 0) {
            emit cursorMapPosition(cell.x(), cell.y());
        }
        // Continue painting / picking along the drag path, or update the
        // brush preview position.
        if (picking_ && cell != lastPickCell_ && cell.x() >= 0) {
            lastPickCell_ = cell;
            pickColorAtCell(cell);
        } else if (brushPainting_ && cell != lastPaintCell_ && cell.x() >= 0) {
            lastPaintCell_ = cell;
            applyBrushStampHere(*this, cell);
        } else if (linePending_ && brushPreviewVisible_) {
            update(); // redraw the pending line preview
        } else if (brushPreviewVisible_) {
            update();
        }
    } else if ((picking_ && cell != lastPickCell_ && cell.x() >= 0)
               || (brushPainting_ && cell != lastPaintCell_ && cell.x() >= 0)) {
        // Re-entered the previous drag cell after panning the view beneath.
        if (picking_) {
            lastPickCell_ = cell;
            pickColorAtCell(cell);
        } else {
            lastPaintCell_ = cell;
            applyBrushStampHere(*this, cell);
        }
    }
    // The pick result is announced by pickColorAtCell(); the brush preview is
    // Brush-only, so a plain move in the Eyedropper mode needs no repaint.
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (panning_) {
        panning_ = false;
        setCursor(paintCursorShape());
        event->accept();
        return;
    }
    if (picking_) {
        picking_ = false;
        lastPickCell_ = {-1, -1};
        setCursor(paintCursorShape());
        event->accept();
        return;
    }
    if (linePending_) {
        // Finish the Line-shape stroke: one applyLine() call = one undo entry.
        linePending_ = false;
        const QPoint pos = event->position().toPoint();
        const QPoint cell = onMapCellAtWidgetPos(pos);
        if (brushTool_ && cell.x() >= 0 && cell != lineStart_) {
            if (brushTool_->applyLine(lineStart_.x(), lineStart_.y(),
                                      cell.x(), cell.y())) {
                needs3DRedraw_ = true;
                emit worldEdited();
            }
        }
        lineStart_ = {-1, -1};
        setCursor(paintCursorShape());
        update();
        event->accept();
        return;
    }
    if (brushPainting_) {
        brushPainting_ = false;
        setCursor(paintCursorShape());
        event->accept();
        return;
    }
    if (orbiting_) {
        orbiting_ = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
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
    cursorWidgetPos_ = {-1, -1};
    // A stroke interrupted by the cursor leaving the widget must not leave a
    // dangling line preview / half-finished line state.
    linePending_ = false;
    lineStart_ = {-1, -1};
    lineStartWidgetPos_ = {-1, -1};
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
    QOpenGLWidget::resizeEvent(event);
    updateFitScale();
    clampZoom();
    needs3DRedraw_ = true;
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
    if (width() <= 0) {
        return;
    }

    const double focus = obliqueCamera_.focusC;
    const double lift = obliqueCamera_.cameraDZ;
    // Screen→ground scale: the drag happened on widget pixels.
    //   GPU mode — the view maps 1:1 onto the widget, so (F+Z)/F cells per px.
    //   CPU mode — the projection constants are per rendered buffer pixel.
    double cellsPerPxX;
    if (gpuActive()) {
        cellsPerPxX = obliqueCellsPerWidgetPx();
    } else {
        if (!view3d_ || view3d_->windowWidth() <= 0) {
            return;
        }
        cellsPerPxX =
            (focus + lift) / focus * view3d_->windowWidth() / width();
    }

    const double leanDeg = obliqueMathLeanDeg();
    const double forwardScale = obliqueForwardScale(obliqueCamera_, leanDeg);
    const double theta = qDegreesToRadians(obliqueCamera_.turnAngleDeg);
    const double cosT = std::cos(theta);
    const double sinT = std::sin(theta);

    // Δcenter = -dx * right + dy * forward * kv (screen y grows downwards).
    const double moveX = (-dxCursorPx * cosT + dyCursorPx * forwardScale * sinT) * cellsPerPxX;
    const double moveY = (-dxCursorPx * sinT - dyCursorPx * forwardScale * cosT) * cellsPerPxX;
    obliqueCenter_ += QPointF(moveX, moveY);
    needs3DRedraw_ = true;
    applyGpuCamera();
}

// ---------------------------------------------------------------------------
// GPU oblique path helpers
// ---------------------------------------------------------------------------

bool ViewportWidget::gpuActive() const
{
    return viewMode_ == ViewMode::Oblique && gpuView_ && gpuView_->isReady();
}

double ViewportWidget::obliqueMathLeanDeg() const
{
    if (gpuActive()) {
        return obliqueCamera_.leanDeg; // the GPU camera has no stable band
    }
    return View3DRenderer::stableLeanDeg(
        obliqueCamera_.leanDeg, obliqueCamera_.focusC,
        view3d_ ? qMax(1, view3d_->windowHeight() / 2) : height() / 2);
}

double ViewportWidget::obliqueCellsPerWidgetPx() const
{
    const double scale =
        (obliqueCamera_.focusC + obliqueCamera_.cameraDZ) /
        static_cast<double>(qMax(1, obliqueCamera_.focusC));
    if (gpuActive()) {
        return scale; // view maps 1:1 onto the widget
    }
    if (!view3d_ || view3d_->windowWidth() <= 0 || width() <= 0) {
        return scale;
    }
    return scale * view3d_->windowWidth() / width();
}

void ViewportWidget::rebuildGpuView()
{
    // The GL context must be current (paintGL / initializeGL).
    if (!renderer_ || !renderer_->isBound()) {
        editorTrace("ViewportWidget: rebuildGpuView — no renderer, deleting gpuView=%p",
                    static_cast<void*>(gpuView_));
        delete gpuView_;
        gpuView_ = nullptr;
        editorTrace("ViewportWidget: rebuildGpuView — no renderer, dropped");
        return;
    }
    if (gpuView_) {
        // Same context, new world/color source — rebuild inside the view.
        editorTrace("ViewportWidget: rebuildGpuView — reloadWorld path");
        gpuView_->setColorSource(renderer_);
        applyGpuCamera();
        return;
    }
    editorTrace("ViewportWidget: rebuildGpuView — creating new TerrainGpuView");
    auto *view = new TerrainGpuView();
    view->setColorSource(renderer_);
    const qreal dpr = devicePixelRatioF();
    const QSize fbo(qRound(size().width() * dpr),
                    qRound(size().height() * dpr));
    editorTrace("ViewportWidget: rebuildGpuView — init (%dx%d)", fbo.width(),
                fbo.height());
    if (view->init(fbo.width(), fbo.height())) {
        gpuView_ = view;
        applyGpuCamera();
        editorTrace("ViewportWidget: rebuildGpuView — init OK");
    } else {
        editorTrace("ViewportWidget: rebuildGpuView — init failed, deleting view");
        delete view;
        qWarning("ViewportWidget: GPU oblique view unavailable, "
                 "the CPU renderer stays in charge");
        editorTrace("ViewportWidget: rebuildGpuView — init FAILED");
    }
}

void ViewportWidget::applyGpuCamera()
{
    if (!gpuView_) {
        return;
    }
    // No GL calls here — setCamera only records state applied on the next
    // renderFrame, so mouse handlers may call this freely.
    TerrainGpuView::CameraState state;
    state.centerX = obliqueCenter_.x();
    state.centerY = obliqueCenter_.y();
    state.turnAngleDeg = obliqueCamera_.turnAngleDeg;
    state.leanDeg = obliqueCamera_.leanDeg;
    state.heightCells = obliqueCamera_.focusC + obliqueCamera_.cameraDZ;
    state.focalDevPx = obliqueCamera_.focusC * devicePixelRatioF();
    gpuView_->setCamera(state);
}

QPoint ViewportWidget::cellAtWidgetPos(const QPoint &widgetPos) const
{
    if (!renderer_ || !renderer_->isBound()) {
        return {-1, -1};
    }
    if (viewMode_ == ViewMode::TopDown) {
        return screenToMap(widgetPos.x(), widgetPos.y());
    }
    if (gpuActive()) {
        // GPU mode: the view maps 1:1 onto the widget — pick through the
        // sokol camera's ray, coordinates in device pixels.
        const qreal dpr = devicePixelRatioF();
        int mx = -1;
        int my = -1;
        if (gpuView_->pickCell(widgetPos.x() * dpr, widgetPos.y() * dpr,
                                mx, my)) {
            return {mx, my};
        }
        // GPU pick failed (ray above horizon / sky) — the GPU camera has a
        // wider FOV than the CPU voxel view, so edge positions can point
        // above the terrain. Fallback to the CPU path which has a narrower
        // FOV and can see more ground.
    }
    if (!view3d_ || view3d_->windowWidth() <= 0 || view3d_->windowHeight() <= 0) {
        return {-1, -1};
    }
    // The 3D buffer is the widget size clamped to Terra's window limit and
    // then scaled back up by draw3DRaster — convert widget px to buffer px.
    const int bw = view3d_->windowWidth();
    const int bh = view3d_->windowHeight();
    const int col = qFloor(widgetPos.x() * bw / static_cast<double>(qMax(1, width())));
    const int row = qFloor(widgetPos.y() * bh / static_cast<double>(qMax(1, height())));
    if (col < 0 || col >= bw || row < 0 || row >= bh) {
        return {-1, -1};
    }
    // The look-at cell draw3DRaster renders with — the pick must use the same.
    const int mapW = renderer_->width();
    const int mapH = renderer_->height();
    const int cx = ((static_cast<int>(std::floor(obliqueCenter_.x())) % mapW) + mapW) % mapW;
    const int cy = ((static_cast<int>(std::floor(obliqueCenter_.y())) % mapH) + mapH) % mapH;
    int mx = -1;
    int my = -1;
    if (!view3d_->pickGroundCell(cx, cy, col, row, mx, my)) {
        return {-1, -1}; // sky
    }
    return {mx, my};
}

QPolygonF ViewportWidget::projectedBrushContour() const
{
    if (!gpuActive() || cursorCell_.x() < 0 || brushRadius_ <= 0) {
        return {};
    }
    const double cx = cursorCell_.x() + 0.5;
    const double cy = cursorCell_.y() + 0.5;
    const double r = brushRadius_;

    // Stamp boundary in map cells. The square is subdivided along its edges so
    // the terrain height variation shows up in the projection (a straight world
    // line is only straight on screen at a constant z).
    std::vector<double> pts;
    if (brushShape_ == BrushPreviewShape::Circle) {
        const int steps = qBound(24, static_cast<int>(r) * 2, 180);
        pts.reserve(static_cast<std::size_t>(steps) * 2);
        for (int i = 0; i < steps; ++i) {
            const double a = 2.0 * M_PI * i / steps;
            pts.push_back(cx + r * std::cos(a));
            pts.push_back(cy + r * std::sin(a));
        }
    } else {
        const int seg = qBound(2, static_cast<int>(r) / 8, 16);
        static const double kCorners[4][2] = {
            {-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0},
        };
        pts.reserve(4 * static_cast<std::size_t>(seg) * 2);
        for (int e = 0; e < 4; ++e) {
            const double x0 = cx + kCorners[e][0] * r;
            const double y0 = cy + kCorners[e][1] * r;
            const double x1 = cx + kCorners[(e + 1) % 4][0] * r;
            const double y1 = cy + kCorners[(e + 1) % 4][1] * r;
            for (int i = 0; i < seg; ++i) {
                const double t = i / static_cast<double>(seg);
                pts.push_back(x0 + (x1 - x0) * t);
                pts.push_back(y0 + (y1 - y0) * t);
            }
        }
    }

    const int count = static_cast<int>(pts.size() / 2);
    std::vector<double> sx(count);
    std::vector<double> sy(count);
    const int n = gpuView_->projectGroundContour(pts.data(), count,
                                                 sx.data(), sy.data());
    if (n < 3) {
        return {}; // mostly behind the camera — keep the heuristic ring
    }
    const qreal dpr = devicePixelRatioF();
    QPolygonF poly;
    poly.reserve(n);
    for (int i = 0; i < n; ++i) {
        poly << QPointF(sx[i] / dpr, sy[i] / dpr);
    }
    return poly;
}

QPointF ViewportWidget::obliquePxPerCell() const
{
    if (!renderer_ || width() <= 0 || height() <= 0) {
        return QPointF(1.0, 1.0);
    }
    if (gpuActive()) {
        // GPU mode: cells per widget px = (F+Z)/F with no window clamp, so
        // widget px per cell is simply the reciprocal; rows keep the kv
        // depth stretch of the CPU heuristic.
        const double kv = obliqueForwardScale(obliqueCamera_,
                                              obliqueCamera_.leanDeg);
        const double pxPerCell = 1.0 / obliqueCellsPerWidgetPx();
        return QPointF(pxPerCell, pxPerCell / kv);
    }
    if (!view3d_ || view3d_->windowWidth() <= 0 || view3d_->windowHeight() <= 0) {
        return QPointF(1.0, 1.0);
    }
    const QSize buffer = obliqueBufferSize();
    const double leanDeg = View3DRenderer::stableLeanDeg(
        obliqueCamera_.leanDeg, obliqueCamera_.focusC, buffer.height() / 2);
    const double kv = obliqueForwardScale(obliqueCamera_, leanDeg);
    // Ground cells per rendered buffer pixel (see currentViewRect()).
    const double cellsPerPx =
        (obliqueCamera_.focusC + obliqueCamera_.cameraDZ) /
        static_cast<double>(qMax(1, obliqueCamera_.focusC));
    const double widgetPxPerBufferPxX =
        width() / static_cast<double>(qMax(1, buffer.width()));
    const double widgetPxPerBufferPxY =
        height() / static_cast<double>(qMax(1, buffer.height()));
    return QPointF(widgetPxPerBufferPxX / cellsPerPx,
                   widgetPxPerBufferPxY / (cellsPerPx * kv));
}

} // namespace MapEditor


#include "MainWindow.h"

#include <View3DRenderer.h>
#include <BrushTool.h>
#include <EditorSession.h>
#include <GameDirManager.h>
#include <MinimapWidget.h>
#include <ViewportWidget.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QPolygonF>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QTransform>
#include <QVBoxLayout>

namespace {

// ---------------------------------------------------------------------------
// Toolbar icons — painted once per action with QPainter so the view toolbar
// does not depend on any icon theme being installed.
// ---------------------------------------------------------------------------

constexpr qreal kDeg = 3.14159265358979323846 / 180.0;
const QColor kInk(45, 45, 45);
const QColor kAccent(0, 96, 170);

/// Small filled arrow head at `tip` pointing into `direction` (degrees,
/// screen coords: y grows downwards).
void drawArrowHead(QPainter &p, const QPointF &tip, double directionDeg,
                   double length = 6.0, double halfWidth = 3.0)
{
    const double a = directionDeg * kDeg;
    const QPointF dir(std::cos(a), std::sin(a));
    const QPointF normal(-dir.y(), dir.x());
    QPolygonF head;
    head << tip
         << tip - dir * length + normal * halfWidth
         << tip - dir * length - normal * halfWidth;
    p.drawPolygon(head);
}

void drawGroundLine(QPainter &p, double y)
{
    p.drawLine(QPointF(4.0, y), QPointF(18.0, y));
}

/// Arc-with-arrow rotation icon; `mirror` flips the arc direction by about
/// the vertical axis (drawn explicitly — no QPixmap::transformed involved).
QIcon makeRotateIcon(bool mirror)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0));

    const QRectF circle(4.0, 4.0, 14.0, 14.0);
    const double gapCenterDeg = 45.0;   // gap centred on the 45° ray…
    const double spanDeg = 270.0;       // …270° long

    // CCW (left rotation) draws from the gap start going counter-clockwise;
    // the mirrored (right rotation) draws clockwise from the same gap.
    if (mirror) {
        p.drawArc(circle,
                  static_cast<int>((gapCenterDeg - 15.0) * 16),
                  static_cast<int>(-spanDeg * 16));
    } else {
        p.drawArc(circle,
                  static_cast<int>((gapCenterDeg + 15.0) * 16),
                  static_cast<int>(spanDeg * 16));
    }

    // Arrow head at the arc end, tangent to the drawing direction.
    const QPointF center = circle.center();
    const double radius = circle.width() / 2.0;
    const double endAngle = gapCenterDeg
        + (mirror ? -15.0 - spanDeg : 15.0 + spanDeg);
    const double endAngleRad = endAngle * kDeg;
    const QPointF end(center.x() + radius * std::cos(endAngleRad),
                      center.y() - radius * std::sin(endAngleRad));
    // Tangent: for CCW arcs the position angle grows, for CW it shrinks.
    const double turn = mirror ? -1.0 : 1.0;
    drawArrowHead(p, end, std::atan2(turn * radius * std::cos(endAngleRad),
                                     turn * radius * std::sin(endAngleRad)) / kDeg);
    return QIcon(pm);
}

QIcon makeTiltIcon(bool up)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0));
    // Horizon the camera tilts about.
    drawGroundLine(p, 16.0);
    if (up) {
        p.drawLine(QPointF(11.0, 14.0), QPointF(11.0, 6.0));
        drawArrowHead(p, QPointF(11.0, 6.0), -90.0);
    } else {
        p.drawLine(QPointF(11.0, 5.0), QPointF(11.0, 13.0));
        drawArrowHead(p, QPointF(11.0, 13.0), 90.0);
    }
    return QIcon(pm);
}

QIcon makeLiftIcon(bool up)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0));
    // Ground the camera lifts from.
    drawGroundLine(p, 18.0);
    // The camera itself as a floating box + its height "rail".
    const qreal boxY = up ? 4.0 : 11.0;
    QPen rail(kAccent, 1.5, Qt::DotLine);
    p.setPen(rail);
    p.drawLine(QPointF(11.0, boxY + 4.0), QPointF(11.0, 18.0));
    p.setPen(QPen(kInk, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(7.5, boxY, 7.0, 4.5), 1.5, 1.5);
    return QIcon(pm);
}

QIcon makeZoomIcon(bool in)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kAccent, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(4.0, 4.0, 11.0, 11.0));
    p.drawLine(QPointF(14.0, 14.0), QPointF(19.0, 19.0));
    p.setPen(QPen(kInk, 2.0));
    p.drawLine(QPointF(6.0, 9.5), QPointF(13.0, 9.5));
    if (in) {
        p.drawLine(QPointF(9.5, 6.0), QPointF(9.5, 13.0));
    }
    return QIcon(pm);
}

QIcon makeResetIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0));
    // Corner brackets of a "frame"…
    const double r = 4.0, half = 7.0;
    const QPointF c(11.0, 11.0);
    for (int corner = 0; corner < 4; ++corner) {
        const double dx = (corner & 1) ? half : -half;
        const double dy = (corner & 2) ? half : -half;
        const QPointF cornerPoint = c + QPointF(dx, dy);
        p.drawLine(cornerPoint, cornerPoint - QPointF(dx * 0.5, 0.0));
        p.drawLine(cornerPoint, cornerPoint - QPointF(0.0, dy * 0.5));
    }
    // …and the frame centre it refers to.
    p.setBrush(QBrush(kAccent));
    p.setPen(Qt::NoPen);
    p.drawEllipse(c, 2.5, 2.5);
    return QIcon(pm);
}

QIcon makeModeIcon(bool oblique)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(oblique ? kAccent : kInk, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2.0, 5.0, 18.0, 12.0), 3.0, 3.0);
    QFont font = p.font();
    font.setPointSizeF(6.5);
    font.setBold(true);
    p.setFont(font);
    p.drawText(QRectF(2.0, 5.0, 18.0, 12.0), Qt::AlignCenter,
               oblique ? QStringLiteral("3D")
                       : QStringLiteral("2D"));
    return QIcon(pm);
}

/// Post-T3.3 chunky/smooth renderer toggle glyphs. Both draw the same 2x2 block of
/// map cells; Chunky shows the blocks as solid hard-edged pixels, Smooth
/// shades them with blurred/gradient edges as if neighbors were blended.
QIcon makeChunkyIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setPen(Qt::NoPen);
    p.setBrush(kInk);
    // 2x2 blocks with visible gaps — the "big pixels" of nearest-neighbor
    // sampling.
    p.drawRect(QRectF(4.0, 4.0, 6.0, 6.0));
    p.drawRect(QRectF(12.0, 4.0, 6.0, 6.0));
    p.drawRect(QRectF(4.0, 12.0, 6.0, 6.0));
    p.drawRect(QRectF(12.0, 12.0, 6.0, 6.0));
    return QIcon(pm);
}

QIcon makeSmoothIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // The same 2x2 block, but the cells bleed into each other: soft round
    // blobs on top of a fading base square.
    QLinearGradient fade(0, 0, 22, 22);
    fade.setColorAt(0.0, QColor(kInk.red(), kInk.green(), kInk.blue(), 200));
    fade.setColorAt(1.0, QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 200));
    p.setBrush(fade);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(4.0, 4.0, 14.0, 14.0), 6.0, 6.0);
    p.setBrush(QColor(255, 255, 255, 120));
    p.drawEllipse(QPointF(9.0, 9.0), 3.5, 3.5);
    p.setBrush(QColor(255, 255, 255, 60));
    p.drawEllipse(QPointF(14.0, 14.0), 3.0, 3.0);
    return QIcon(pm);
}

/// T3.1 — brush edit toggle: a paintbrush glyph (handle + ferrule + tip).
QIcon makeBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(4.5, 4.0), QPointF(12.0, 11.5));    // handle
    QPen ferrule(kAccent, 3.0, Qt::SolidLine, Qt::RoundCap);
    p.setPen(ferrule);
    p.drawLine(QPointF(13.0, 12.5), QPointF(15.0, 14.5));  // ferrule
    p.setBrush(kInk);
    p.setPen(Qt::NoPen);
    QPolygonF drop;
    drop << QPointF(14.0, 17.5)
         << QPointF(11.5, 18.5)
         << QPointF(10.5, 16.5)
         << QPointF(14.0, 15.5);
    p.drawPolygon(drop);                                   // bristle tip
    return QIcon(pm);
}

/// T3.2 — height tool glyphs: a small mound on a ground line moved/levelled
/// by an arrow (up = raise, down = dig).
QIcon makeHeightArrowIcon(bool up)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0));
    drawGroundLine(p, 17.0);
    // The mound the tool works on.
    p.setBrush(kInk);
    p.setPen(Qt::NoPen);
    QPolygonF mound;
    mound << QPointF(6.0, 16.0)
          << QPointF(9.0, up ? 12.0 : 13.5)
          << QPointF(12.0, 16.0);
    p.drawPolygon(mound);
    // Movement arrow above / above the mound.
    p.setPen(QPen(kAccent, 2.0));
    p.setBrush(Qt::NoBrush);
    const QPointF tailStart(9.0, up ? 10.0 : 6.5);
    const QPointF tailEnd(9.0, up ? 3.5 : 9.5);
    p.drawLine(tailStart, tailEnd);
    drawArrowHead(p, tailEnd, up ? -90.0 : 90.0);
    return QIcon(pm);
}

/// T3.2 — letter glyphs on a ground line: "Z" — level to the world
/// zeroplast height, "H" — level to the chosen height, "G"/"D" — Geo/Dam
/// brush mode.
QIcon makeGroundLetterIcon(const QString &letter, const QColor &color)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0));
    drawGroundLine(p, 17.0);
    QFont font = p.font();
    font.setPointSizeF(9.0);
    font.setBold(true);
    p.setFont(font);
    p.setPen(color);
    p.drawText(QRectF(2.0, 2.0, 18.0, 14.0), Qt::AlignCenter, letter);
    return QIcon(pm);
}

/// T3.3 — color palette brush: a filled circle representing the surface
/// color index.
QIcon makeColorBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // A palette swatch: concentric half-circles.
    p.setBrush(QColor(220, 80, 60));
    p.setPen(Qt::NoPen);
    p.drawChord(QRectF(3, 3, 16, 16), 0 * 16, 120 * 16);
    p.setBrush(QColor(60, 160, 60));
    p.drawChord(QRectF(3, 3, 16, 16), 120 * 16, 120 * 16);
    p.setBrush(QColor(60, 80, 200));
    p.drawChord(QRectF(3, 3, 16, 16), 240 * 16, 120 * 16);
    return QIcon(pm);
}

/// T3.3 — eyedropper glyph: a pipette with a drop.
QIcon makeEyedropperIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0, Qt::SolidLine, Qt::RoundCap));
    // Pipette body (diagonal line).
    p.drawLine(QPointF(15.0, 3.0), QPointF(6.0, 12.0));
    // Bulb at the top.
    p.setBrush(kAccent);
    p.setPen(QPen(kInk, 1.5));
    p.drawEllipse(QRectF(13.5, 1.5, 5.0, 5.0));
    // Tip at the bottom.
    p.setPen(QPen(kInk, 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(6.0, 12.0), QPointF(4.0, 18.0));
    // Drop.
    p.setBrush(QColor(80, 160, 220));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(2.5, 17.0, 4.0, 4.0));
    return QIcon(pm);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------



MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("MainWindow"));
    setWindowTitle(QStringLiteral("Perimeter Map Editor"));
    resize(1280, 800);

    session_ = new MapEditor::EditorSession();
    gameDirMgr_ = new GameDirManager();

    // T3.1 — the single brush tool of the session; its default parameters
    // (filled circle, raise) let the viewport paint right away.
    brushTool_ = new MapEditor::BrushTool(*session_);

    // T3.2 — the type/mode buttons of the Brush toolbar drive the tool.
    createBrushActions();

    buildMenuBar();
    buildViewToolbar();
    buildBrushToolbar();
    buildStatusBar();
    buildViewport();

    updateStatusBar();
}

MainWindow::~MainWindow()
{
    delete brushTool_;
    delete session_;
    delete gameDirMgr_;
    // viewport_ is a child widget; deleted by the parent.
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------

void MainWindow::buildViewport()
{
    viewport_ = new MapEditor::ViewportWidget(this);

    // T3.1 — plug the brush tool in and mirror its shape/radius in the
    // cursor preview (the viewport draws the outline over the raster).
    viewport_->setBrushTool(brushTool_);
    const MapEditor::BrushParams &brush = brushTool_->params();
    const MapEditor::BrushPreviewShape previewShape =
        (brush.shape == MapEditor::BrushShape::FilledSquare ||
         brush.shape == MapEditor::BrushShape::SquareOutline)
            ? MapEditor::BrushPreviewShape::Square
            : MapEditor::BrushPreviewShape::Circle;
    viewport_->setBrushPreview(previewShape, brush.radius);
    viewport_->setBrushPreviewVisible(true);

    // Connect cursor position signal to status bar.
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition,
            this, &MainWindow::updateCursorInfo);

    // Brush edits change the modified state — keep the status bar/undo
    // actions live.
    connect(viewport_, &MapEditor::ViewportWidget::worldEdited,
            this, &MainWindow::updateStatusBar);

    // Keep the toolbar in sync with whatever the viewport does (mouse
    // orbit/pan/zoom change the camera too, not only the actions).
    connect(viewport_, &MapEditor::ViewportWidget::viewModeChanged, this,
            [this](MapEditor::ViewportWidget::ViewMode mode) {
                onViewportViewModeChanged(static_cast<int>(mode));
            });
    connect(viewport_, &MapEditor::ViewportWidget::renderStyleChanged, this,
            [this](MapEditor::ViewportWidget::RenderStyle style) {
                // Keep the View menu / View toolbar toggle pair in sync with
                // whatever set the style (the menu buttons are the only other
                // driver today).
                if (actChunkyRender_) {
                    actChunkyRender_->setChecked(
                        style == MapEditor::ViewportWidget::RenderStyle::Chunky);
                }
                if (actSmoothRender_) {
                    actSmoothRender_->setChecked(
                        style == MapEditor::ViewportWidget::RenderStyle::Smooth);
                }
            });
    connect(viewport_, &MapEditor::ViewportWidget::obliqueCameraChanged,
            this, &MainWindow::onObliqueCameraChanged);

    // T3.3 — eyedropper signal: right-click in TopDown mode picks SurBuf.
    connect(viewport_, &MapEditor::ViewportWidget::eyedropperPicked,
            this, &MainWindow::onEyedropperPicked);

    /// Initial style button state (the viewport defaults to Smooth; post-T3.3
    /// chunky/smooth renderer toggle).
    actChunkyRender_->setChecked(viewport_->renderStyle() !=
        MapEditor::ViewportWidget::RenderStyle::Smooth);
    actSmoothRender_->setChecked(viewport_->renderStyle() ==
        MapEditor::ViewportWidget::RenderStyle::Smooth);

    setCentralWidget(viewport_);

    // T2.4 — the minimap goes into its own dock next to the central widget.
    buildMinimapDock();
}

void MainWindow::buildMinimapDock()
{
    minimap_ = new MapEditor::MinimapWidget(this);

    minimapDock_ = new QDockWidget(tr("Minimap"), this);
    minimapDock_->setObjectName(QStringLiteral("MinimapDock"));
    minimapDock_->setWidget(minimap_);
    minimapDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                  Qt::RightDockWidgetArea |
                                  Qt::TopDockWidgetArea |
                                  Qt::BottomDockWidgetArea);
    minimapDock_->setFeatures(QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, minimapDock_);

    // The View menu owns the show/hide toggle (default: visible).
    if (viewMenu_) {
        viewMenu_->addSeparator();
        QAction *toggleAction = minimapDock_->toggleViewAction();
        toggleAction->setText(tr("&Minimap"));
        toggleAction->setToolTip(
            tr("Show or hide the minimap panel (M)"));
        toggleAction->setShortcut(QKeySequence(Qt::Key_M));
        viewMenu_->addAction(toggleAction);
    }

    // The rectangle outline follows the viewport — whatever moves it
    // (pan, zoom, orbit, mode switch, resize, world load/unload) ends in
    // ViewportWidget::viewRectChanged after the next repaint.
    connect(viewport_, &MapEditor::ViewportWidget::viewRectChanged,
            minimap_, &MapEditor::MinimapWidget::setViewRect);

    // Clicking the minimap navigates there instead.
    connect(minimap_, &MapEditor::MinimapWidget::cellClicked,
            this, &MainWindow::onMinimapCellClicked);

    // Load the image for the world that may already be bound and push the
    // current view rectangle once (subsequent updates come from the signal).
    minimap_->setSession(session_);
    minimap_->setViewRect(viewport_->currentViewRect());
}

void MainWindow::onWorldLoaded()
{
    // Rebind the viewport to pick up the newly loaded world.
    if (viewport_ && session_) {
        viewport_->setSession(session_);
    }
    // Rebind the minimap image (map.tga of the new world) and reset the
    // outline until the viewport's next repaint pushes a rectangle.
    if (minimap_ && session_) {
        minimap_->setSession(session_);
        minimap_->setViewRect(viewport_ ? viewport_->currentViewRect()
                                        : QRect());
    }
    // The title carries the world name (also after "Save As", which
    // rebinds the session onto the renamed world and lands here).
    updateWindowTitle();
    updateStatusBar();
}

void MainWindow::onWorldUnload()
{
    if (viewport_ && session_) {
        viewport_->setSession(session_);
    }
    if (minimap_ && session_) {
        minimap_->setSession(session_);
        minimap_->setViewRect(QRect());
    }
    updateWindowTitle();
    updateStatusBar();
}

void MainWindow::updateCursorInfo(int mapX, int mapY)
{
    if (viewport_ && viewport_->viewMode() == MapEditor::ViewportWidget::ViewMode::Oblique) {
        // The oblique view has no linear screen→cell mapping; the viewport
        // does not emit positions there.
        return;
    }
    QString info = QString(tr("X: %1  Y: %2")).arg(mapX).arg(mapY);
    statusBar()->showMessage(info, 3000);
}

void MainWindow::onZoomToFit()
{
    if (viewport_) {
        viewport_->zoomToFit();
    }
}

void MainWindow::onActionToggle3D()
{
    if (viewport_) {
        viewport_->toggleViewMode();
    }
}

void MainWindow::onViewportViewModeChanged(int mode)
{
    if (actView3D_) {
        actView3D_->setChecked(mode ==
            static_cast<int>(MapEditor::ViewportWidget::ViewMode::Oblique));
    }
    updateViewReadout();
}

void MainWindow::onObliqueCameraChanged(const MapEditor::View3DCamera &)
{
    updateViewReadout();
}

void MainWindow::onMinimapCellClicked(int mapX, int mapY)
{
    if (viewport_) {
        viewport_->centerOnCell(QPoint(mapX, mapY));
    }
}

void MainWindow::onEyedropperPicked(int mapX, int mapY, unsigned char surIndex)
{
    // T3.3 — set the color brush to the picked SurBuf index.
    MapEditor::BrushParams params = brushTool_->params();
    params.effect = MapEditor::BrushEffect::SetColor;
    params.colorIndex = surIndex;
    brushTool_->setParams(params);

    if (brushColorIndexSpin_) {
        brushColorIndexSpin_->blockSignals(true);
        brushColorIndexSpin_->setValue(surIndex);
        brushColorIndexSpin_->blockSignals(false);
    }
    syncColorControls();
    syncBrushControls();

    statusBar()->showMessage(
        tr("Eyedropper: SurBuf(%1,%2) = %3")
            .arg(mapX)
            .arg(mapY)
            .arg(static_cast<int>(surIndex)),
        5000);
}

void MainWindow::updateViewReadout()
{
    if (!viewReadout_ || !viewport_) {
        return;
    }
    if (viewport_->viewMode() == MapEditor::ViewportWidget::ViewMode::Oblique) {
        const MapEditor::View3DCamera &cam = viewport_->obliqueCamera();
        viewReadout_->setText(tr("3D    rot %1\u00B0  tilt %2\u00B0  lift %3  focus %4")
                                  .arg(qRound(cam.turnAngleDeg))
                                  .arg(qRound(cam.leanDeg))
                                  .arg(cam.cameraDZ)
                                  .arg(cam.focusC));
    } else {
        viewReadout_->setText(tr("2D    zoom %1%")
                                  .arg(qRound(viewport_->zoom() * 100)));
    }
}

// ---------------------------------------------------------------------------
// Menu / status bar
// ---------------------------------------------------------------------------

void MainWindow::buildMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));

    QAction *actSelDir = fileMenu->addAction(tr("Select &Game Directory..."));
    actSelDir->setToolTip(tr("Open the folder that contains RESOURCE/Worlds/"));
    connect(actSelDir, &QAction::triggered, this, &MainWindow::onActionSelectGameDir);

    fileMenu->addSeparator();

    QAction *actScan = fileMenu->addAction(tr("&Scan Worlds"));
    actScan->setShortcut(QKeySequence(Qt::Key_F5));
    connect(actScan, &QAction::triggered, this, &MainWindow::onActionScanWorlds);

    QAction *actLoad = fileMenu->addAction(tr("&Load World..."));
    actLoad->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    connect(actLoad, &QAction::triggered, this, &MainWindow::onActionLoadWorld);

    QAction *actUnload = fileMenu->addAction(tr("&Unload World"));
    actUnload->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    connect(actUnload, &QAction::triggered, this, &MainWindow::onActionUnloadWorld);

    fileMenu->addSeparator();

    actSave_ = fileMenu->addAction(tr("&Save"));
    actSave_->setShortcut(QKeySequence(QKeySequence::Save));
    actSave_->setToolTip(tr("Save the loaded world (Ctrl+S)"));
    actSave_->setEnabled(false);
    connect(actSave_, &QAction::triggered, this, &MainWindow::onActionSave);

    actSaveAs_ = fileMenu->addAction(tr("Save &As..."));
    actSaveAs_->setShortcut(QKeySequence(QKeySequence::SaveAs));
    actSaveAs_->setToolTip(tr("Save a copy of the loaded world as a new world folder (Ctrl+Shift+S)"));
    actSaveAs_->setEnabled(false);
    connect(actSaveAs_, &QAction::triggered, this, &MainWindow::onActionSaveAs);

    fileMenu->addSeparator();

    QAction *actQuit = fileMenu->addAction(tr("E&xit"));
    actQuit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    connect(actQuit, &QAction::triggered, this, &QApplication::quit);

    buildEditMenu(menuBar());
    buildViewMenu(menuBar());
}

void MainWindow::buildEditMenu(QMenuBar *menuBar)
{
    QMenu *editMenu = menuBar->addMenu(tr("&Edit"));

    actUndo_ = editMenu->addAction(tr("&Undo"));
    actUndo_->setToolTip(tr("Undo the last terrain edit (Ctrl+Z)"));
    actUndo_->setEnabled(false);
    connect(actUndo_, &QAction::triggered, this, &MainWindow::onActionUndo);

    actRedo_ = editMenu->addAction(tr("&Redo"));
    actRedo_->setToolTip(tr("Redo the last undone terrain edit (Ctrl+Y)"));
    actRedo_->setEnabled(false);
    connect(actRedo_, &QAction::triggered, this, &MainWindow::onActionRedo);

    editMenu->addSeparator();

    actClearUndoHistory_ = editMenu->addAction(tr("C&lear Undo History"));
    actClearUndoHistory_->setToolTip(tr("Discard the whole undo/redo history"));
    actClearUndoHistory_->setEnabled(false);
    connect(actClearUndoHistory_, &QAction::triggered,
            this, &MainWindow::onActionClearUndoHistory);

    editMenu->addSeparator();

    // T3.1 — brush painting toggle: when checked, a left-button drag in the
    // 2D view applies the brush stamps (default raise, filled circle).
    actBrushEdit_ = editMenu->addAction(tr("&Brush Editing"));
    actBrushEdit_->setIcon(makeBrushIcon());
    actBrushEdit_->setToolTip(
        tr("Apply the brush with a left drag in the 2D view (T).\n"
           "The brush type (raise/lower/levellers) and layer mode "
           "(Geo/Dam) are picked on the Brush toolbar."));
    actBrushEdit_->setCheckable(true);
    actBrushEdit_->setChecked(true);
    actBrushEdit_->setShortcut(QKeySequence(Qt::Key_T));
    connect(actBrushEdit_, &QAction::toggled, viewport_, [this](bool checked) {
        viewport_->setBrushEditingEnabled(checked);
    });

    // T3.2 — the same brush type/mode actions as the Brush toolbar,
    // packed into an Edit-menu submenu for discoverability.
    QMenu *brushMenu = editMenu->addMenu(tr("Br&ush Type / Mode"));
    brushMenu->addAction(actBrushRaise_);
    brushMenu->addAction(actBrushLower_);
    brushMenu->addAction(actBrushLevelZP_);
    brushMenu->addAction(actBrushLevelHeight_);
    brushMenu->addSeparator();
    brushMenu->addAction(actBrushGeo_);
    brushMenu->addAction(actBrushDam_);
    brushMenu->addSeparator();
    brushMenu->addAction(actBrushColor_);
    brushMenu->addAction(actEyedropper_);
}

void MainWindow::buildViewMenu(QMenuBar *menuBar)
{
    QMenu *viewMenu = menuBar->addMenu(tr("&View"));
    viewMenu_ = viewMenu;   // T2.4 appends the minimap toggle later.
    const auto create = [&viewMenu]() { return viewMenu->addAction(QString()); };

    // ------------------------------------------------------------------
    // TopDown / common
    // ------------------------------------------------------------------
    actGrid_ = create();
    actGrid_->setText(tr("&Grid"));
    actGrid_->setCheckable(true);
    actGrid_->setChecked(true);
    actGrid_->setShortcut(QKeySequence(Qt::Key_G));
    connect(actGrid_, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setGridVisible(checked);
        }
    });

    actZoomToFit_ = create();
    actZoomToFit_->setText(tr("Zoom to &Fit"));
    actZoomToFit_->setIcon(makeResetIcon());
    actZoomToFit_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(actZoomToFit_, &QAction::triggered, this, &MainWindow::onZoomToFit);

    viewMenu->addSeparator();

    // ------------------------------------------------------------------
    // Oblique (3D) camera (T2.3)
    // ------------------------------------------------------------------
    actView3D_ = create();
    actView3D_->setText(tr("&3D View"));
    actView3D_->setIcon(makeModeIcon(true));
    actView3D_->setToolTip(
        tr("Switch the viewport between the flat 2D palette and the tilted "
           "3D voxel view (F9)"));
    actView3D_->setCheckable(true);
    actView3D_->setShortcut(QKeySequence(Qt::Key_F9));
    connect(actView3D_, &QAction::triggered, this, &MainWindow::onActionToggle3D);

    // ------------------------------------------------------------------
    // Renderer style: chunky (pixel-exact) vs smooth (bilinear, the
    // in-game camera look). Both actions share one exclusive group.
    // ------------------------------------------------------------------
    QActionGroup *styleGroup = new QActionGroup(this);
    actChunkyRender_ = new QAction(makeChunkyIcon(), tr("&Chunky Renderer"), this);
    actChunkyRender_->setToolTip(
        tr("Pixel-exact rendering: one map cell = one solid block, hard cell "
           "edges (the classic look; the 2D view stays at or below 1:1)."));
    actChunkyRender_->setCheckable(true);
    actSmoothRender_ = new QAction(makeSmoothIcon(), tr("&Smooth Renderer"), this);
    actSmoothRender_->setToolTip(
        tr("Bilinear rendering like the in-game camera: neighboring cells "
           "blend into smooth gradients; the 2D view may zoom above 1:1."));
    actSmoothRender_->setCheckable(true);
    styleGroup->addAction(actChunkyRender_);
    styleGroup->addAction(actSmoothRender_);
    connect(actChunkyRender_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->setRenderStyle(
            MapEditor::ViewportWidget::RenderStyle::Chunky);
    });
    connect(actSmoothRender_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->setRenderStyle(
            MapEditor::ViewportWidget::RenderStyle::Smooth);
    });

    viewMenu->addSeparator();

    actRotateLeft_ = create();
    actRotateLeft_->setText(tr("Rotate &Left"));
    actRotateLeft_->setIcon(makeRotateIcon(false));
    actRotateLeft_->setToolTip(tr("Rotate the 3D camera 15\u00B0 left (Ctrl+Left)"));
    actRotateLeft_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    connect(actRotateLeft_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueRotation(-15.0);
    });

    actRotateRight_ = create();
    actRotateRight_->setText(tr("Rotate &Right"));
    actRotateRight_->setIcon(makeRotateIcon(true));
    actRotateRight_->setToolTip(tr("Rotate the 3D camera 15\u00B0 right (Ctrl+Right)"));
    actRotateRight_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right));
    connect(actRotateRight_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueRotation(15.0);
    });

    actTiltUp_ = create();
    actTiltUp_->setText(tr("Tilt &Up"));
    actTiltUp_->setIcon(makeTiltIcon(true));
    actTiltUp_->setToolTip(
        tr("Look further along the horizon (Ctrl+Up); "
           "Ctrl+Up/Ctrl+Down change the camera pitch"));
    actTiltUp_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    connect(actTiltUp_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueTilt(5.0);
    });

    actTiltDown_ = create();
    actTiltDown_->setText(tr("Tilt &Down"));
    actTiltDown_->setIcon(makeTiltIcon(false));
    actTiltDown_->setToolTip(tr("Look more down onto the surface (Ctrl+Down)"));
    actTiltDown_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    connect(actTiltDown_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueTilt(-5.0);
    });

    actLiftUp_ = create();
    actLiftUp_->setText(tr("Camera &Higher"));
    actLiftUp_->setIcon(makeLiftIcon(true));
    actLiftUp_->setToolTip(tr("Lift the 3D camera one step (Page Up)"));
    actLiftUp_->setShortcut(QKeySequence(Qt::Key_PageUp));
    connect(actLiftUp_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueLift(100);
    });

    actLiftDown_ = create();
    actLiftDown_->setText(tr("Camera &Lower"));
    actLiftDown_->setIcon(makeLiftIcon(false));
    actLiftDown_->setToolTip(tr("Lower the 3D camera one step (Page Down)"));
    actLiftDown_->setShortcut(QKeySequence(Qt::Key_PageDown));
    connect(actLiftDown_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueLift(-100);
    });

    viewMenu->addSeparator();

    actZoomIn_ = create();
    actZoomIn_->setText(tr("Zoom &In"));
    actZoomIn_->setIcon(makeZoomIcon(true));
    actZoomIn_->setToolTip(tr("Zoom in (Ctrl+=; the 3D view moves the camera closer)"));
    actZoomIn_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal));
    connect(actZoomIn_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->zoomStep(1.25);
    });

    actZoomOut_ = create();
    actZoomOut_->setText(tr("Zoom &Out"));
    actZoomOut_->setIcon(makeZoomIcon(false));
    actZoomOut_->setToolTip(tr("Zoom out (Ctrl+-; the 3D view moves the camera away)"));
    actZoomOut_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    connect(actZoomOut_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->zoomStep(0.8);
    });

    actResetView_ = create();
    actResetView_->setText(tr("&Reset View"));
    actResetView_->setToolTip(tr("2D — fit the map; 3D — default camera"));
    actResetView_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Home));
    connect(actResetView_, &QAction::triggered, this, [this] {
        if (!viewport_) return;
        if (viewport_->viewMode() == MapEditor::ViewportWidget::ViewMode::Oblique) {
            viewport_->resetObliqueView();
        } else {
            viewport_->zoomToFit();
        }
    });
}

void MainWindow::buildViewToolbar()
{
    QToolBar *bar = addToolBar(tr("View"));
    bar->setObjectName(QStringLiteral("ViewToolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(22, 22));
    bar->setStyleSheet(
        QStringLiteral("QToolButton:enabled:hover, QToolButton:enabled:checked "
                       "{ background: rgba(0, 96, 170, 35); border-radius: 4px; }"));

    bar->addSeparator();
    bar->addAction(actGrid_);
    bar->addAction(actZoomToFit_);
    bar->addSeparator();
    bar->addAction(actView3D_);
    bar->addAction(actChunkyRender_);
    bar->addAction(actSmoothRender_);
    bar->addSeparator();
    bar->addAction(actRotateLeft_);
    bar->addAction(actRotateRight_);
    bar->addAction(actTiltUp_);
    bar->addAction(actTiltDown_);
    bar->addSeparator();
    bar->addAction(actLiftUp_);
    bar->addAction(actLiftDown_);
    bar->addSeparator();
    bar->addAction(actZoomIn_);
    bar->addAction(actZoomOut_);
    bar->addAction(actResetView_);

    // The T3.1 brush editing toggle moved to the dedicated Brush toolbar
    // (T3.2), where the brush type/mode buttons live next to it.

    // A compact live readout of the active camera.
    viewReadout_ = new QLabel(bar);
    viewReadout_->setObjectName(QStringLiteral("viewReadout"));
    viewReadout_->setStyleSheet(
        QStringLiteral("QLabel { color: #303030; padding: 0 10px; }"));
    // Spacing for the monospace-ish look without a fixed width.
    QFont font = viewReadout_->font();
    font.setPointSizeF(8.5);
    viewReadout_->setFont(font);
    bar->addWidget(viewReadout_);
}

void MainWindow::createBrushActions()
{
    const MapEditor::BrushParams &brush = brushTool_->params();

    // --- Type: what the brush does -----------------------------------
    QActionGroup *typeGroup = new QActionGroup(this);
    const auto addType = [this, typeGroup](QIcon icon, const QString &text,
                                           const QString &toolTip,
                                           MapEditor::BrushEffect effect,
                                           int direction) {
        QAction *action = new QAction(icon, text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);

        const MapEditor::BrushParams &current = brushTool_->params();
        action->setChecked(current.effect == effect &&
                           (effect != MapEditor::BrushEffect::ChangeHeight ||
                            current.direction == direction));

        connect(action, &QAction::triggered, this, [this, effect, direction] {
            MapEditor::BrushParams params = brushTool_->params();
            params.effect = effect;
            params.direction = direction;
            brushTool_->setParams(params);
            syncBrushControls();
        });
        typeGroup->addAction(action);
        return action;
    };

    actBrushRaise_ = addType(
        makeHeightArrowIcon(true), tr("Raise Terrain"),
        tr("The brush raises the terrain under it (left drag in the 2D view)."),
        MapEditor::BrushEffect::ChangeHeight, 1);
    actBrushLower_ = addType(
        makeHeightArrowIcon(false), tr("Lower Terrain"),
        tr("The brush digs the terrain under it. Hard terrain slows the dig "
           "down just like in the game."),
        MapEditor::BrushEffect::ChangeHeight, -1);
    actBrushLevelZP_ = addType(
        makeGroundLetterIcon(QStringLiteral("Z"), kInk),
        tr("Level to Zeroplast"),
        tr("The brush levels the terrain to the world zeroplast height. "
           "Zeroplast-marked cells are not touched; cells reaching the "
           "level are marked as leveled."),
        MapEditor::BrushEffect::Align4ZP, 1);
    actBrushLevelHeight_ = addType(
        makeGroundLetterIcon(QStringLiteral("H"), kInk),
        tr("Level to Height"),
        tr("The brush levels the terrain to the height chosen next to it "
           "(whole voxels, 1 voxel = 1/32 m)."),
        MapEditor::BrushEffect::AlignVariableH, 1);

    // T3.3 — color brush: paints SurBuf with the selected palette index.
    actBrushColor_ = addType(
        makeColorBrushIcon(), tr("Paint Color"),
        tr("The brush paints the surface color (SurBuf) with the selected "
           "palette index. Height is not changed.\n"
           "Geo palette: indices 0..127; Dam palette: 128..255."),
        MapEditor::BrushEffect::SetColor, 0);

    // --- Mode: which terrain layer the height tools drive -------------
    QActionGroup *modeGroup = new QActionGroup(this);
    const auto addMode = [this, modeGroup](QIcon icon, const QString &text,
                                           const QString &toolTip,
                                           MapEditor::BrushMode mode) {
        QAction *action = new QAction(icon, text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);
        action->setChecked(brushTool_->params().mode == mode);
        connect(action, &QAction::triggered, this, [this, mode] {
            MapEditor::BrushParams params = brushTool_->params();
            params.mode = mode;
            brushTool_->setParams(params);
            syncBrushControls();
        });
        modeGroup->addAction(action);
        return action;
    };

    actBrushGeo_ = addMode(
        makeGroundLetterIcon(QStringLiteral("G"), kInk), tr("Geo Layer"),
        tr("The height tools drive the geo layer (VxGBuf) only — a dam above "
           "the surface is never created or removed."),
        MapEditor::BrushMode::Geo);
    actBrushDam_ = addMode(
        makeGroundLetterIcon(QStringLiteral("D"), kAccent), tr("Dam Layer"),
        tr("The height tools drive the dam layer: raising seeds the dam, "
           "digging it down to the geo height removes it. The dam is always "
           "at or above the geo."),
        MapEditor::BrushMode::Dam);

    // T3.3 — eyedropper toggle: when checked, right-click reads the SurBuf
    // index at the cursor cell and sets it as the color brush index.
    actEyedropper_ = new QAction(makeEyedropperIcon(), tr("Eyedropper"), this);
    actEyedropper_->setToolTip(
        tr("Right-click in the 2D view to pick the surface color index "
           "under the cursor into the color brush (E)."));
    actEyedropper_->setCheckable(true);
    actEyedropper_->setChecked(false);
    actEyedropper_->setShortcut(QKeySequence(Qt::Key_E));
    connect(actEyedropper_, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setEyedropperEnabled(checked);
        }
    });
}

void MainWindow::buildBrushToolbar()
{
    QToolBar *bar = addToolBar(tr("Brush"));
    bar->setObjectName(QStringLiteral("BrushToolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(22, 22));
    bar->setStyleSheet(
        QStringLiteral("QToolButton:enabled:hover, QToolButton:enabled:checked "
                       "{ background: rgba(0, 96, 170, 35); border-radius: 4px; }"));

    // T3.1 painting toggle (moved from the View toolbar).
    bar->addAction(actBrushEdit_);
    bar->addSeparator();

    // T3.2 type buttons: raise / lower / the two levellers.
    bar->addAction(actBrushRaise_);
    bar->addAction(actBrushLower_);
    bar->addAction(actBrushLevelZP_);
    bar->addAction(actBrushLevelHeight_);

    // Target height of "Level to Height" — whole voxels (0..255, the domain
    // BrushTool::clampHeightLevel enforces too).
    brushLevelSpin_ = new QSpinBox(bar);
    brushLevelSpin_->setRange(0, 255);
    brushLevelSpin_->setSuffix(tr(" vox"));
    brushLevelSpin_->setToolTip(
        tr("Target height of the \"Level to Height\" brush (whole voxels; "
           "1 voxel = 1/32 m)."));
    connect(brushLevelSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.heightLevel = value;
        brushTool_->setParams(params);
    });
    bar->addWidget(brushLevelSpin_);

    bar->addSeparator();

    // T3.2 mode buttons: Geo / Dam.
    bar->addAction(actBrushGeo_);
    bar->addAction(actBrushDam_);

    bar->addSeparator();

    // T3.3 — color palette index spinbox (0..255): 0..127 for geo, 128..255
    // for dam surface.
    brushColorIndexSpin_ = new QSpinBox(bar);
    brushColorIndexSpin_->setRange(0, 255);
    brushColorIndexSpin_->setToolTip(
        tr("Palette index for the \"Paint Color\" brush (0..127 = geo, "
           "128..255 = dam)."));
    connect(brushColorIndexSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.colorIndex = value;
        brushTool_->setParams(params);
        syncColorControls();
    });
    bar->addWidget(brushColorIndexSpin_);

    // Color swatch: a label with a colored rectangle showing the current
    // palette index color (read from the geo/dam palette).
    brushColorSwatch_ = new QLabel(bar);
    brushColorSwatch_->setFixedSize(24, 24);
    brushColorSwatch_->setStyleSheet(
        QStringLiteral("QLabel { border: 1px solid #808080; }"));
    brushColorSwatch_->setAlignment(Qt::AlignCenter);
    brushColorSwatch_->setToolTip(
        tr("Current color index preview."));
    bar->addWidget(brushColorSwatch_);

    syncBrushControls();
    syncColorControls();
}

void MainWindow::syncBrushControls()
{
    const MapEditor::BrushParams params = brushTool_->params();

    if (brushLevelSpin_) {
        // "Level to Height" is the only consumer of the spinbox value.
        brushLevelSpin_->setEnabled(
            params.effect == MapEditor::BrushEffect::AlignVariableH);
        if (brushLevelSpin_->value() != params.heightLevel) {
            brushLevelSpin_->blockSignals(true);
            brushLevelSpin_->setValue(params.heightLevel);
            brushLevelSpin_->blockSignals(false);
        }
    }

    // T3.3 — color index spinbox is enabled only for the SetColor effect.
    if (brushColorIndexSpin_) {
        brushColorIndexSpin_->setEnabled(
            params.effect == MapEditor::BrushEffect::SetColor);
        if (brushColorIndexSpin_->value() != params.colorIndex) {
            brushColorIndexSpin_->blockSignals(true);
            brushColorIndexSpin_->setValue(params.colorIndex);
            brushColorIndexSpin_->blockSignals(false);
        }
    }
    syncColorControls();
}

void MainWindow::syncColorControls()
{
    if (!brushTool_ || !brushColorIndexSpin_) {
        return;
    }

    // Build the color swatch from the palette via the session helper.
    if (session_ && session_->isWorldLoaded()) {
        const int idx = brushColorIndexSpin_->value();
        const unsigned int rgb = session_->paletteRgb(idx);
        QColor color(static_cast<int>((rgb >> 16) & 0xFF),
                     static_cast<int>((rgb >> 8) & 0xFF),
                     static_cast<int>(rgb & 0xFF));
        if (rgb != 0 || idx == 0) {
            brushColorSwatch_->setStyleSheet(
                QStringLiteral("QLabel { border: 1px solid #808080; "
                               "background-color: %1; }")
                    .arg(color.name()));
        }
    }
}

void MainWindow::buildStatusBar()
{
    statusBar()->showMessage(tr("Map editor"));
}

// ---------------------------------------------------------------------------
// Window title
// ---------------------------------------------------------------------------

void MainWindow::updateWindowTitle()
{
    if (session_ && session_->isWorldLoaded()) {
        setWindowTitle(tr("Perimeter Map Editor - %1")
                           .arg(QString::fromStdString(session_->currentWorldName())));
    } else {
        setWindowTitle(tr("Perimeter Map Editor"));
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onActionSelectGameDir()
{
    // Switching the game directory unloads the current world — offer to save.
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    const QString autoDetected = gameDirMgr_->autoDetect();

    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Game Directory"), autoDetected,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (dir.isEmpty()) {
        return;
    }

    if (!GameDirManager::isValidGameDir(dir)) {
        QMessageBox::warning(this, tr("Invalid Directory"),
            tr("The selected directory does not contain RESOURCE/Worlds/.\n"
               "Please select the Perimeter game root directory."));
        return;
    }

    if (!session_->setGameDir(dir.toStdString())) {
        QMessageBox::critical(this, tr("Error"),
            tr("Failed to set game directory: %1").arg(dir));
        return;
    }

    gameDirMgr_->saveGameDir(dir);
    // The session unloaded the world when the directory switched — drop the
    // viewport raster binding and the minimap image with it.
    onWorldUnload();

    // Scan the worlds right away so the status bar reflects the selected
    // directory immediately instead of only after a world is loaded
    // (File → Scan Worlds stays available as an explicit re-scan, e.g.
    // after new worlds appear in the folder).
    if (!session_->scanWorlds()) {
        QMessageBox::warning(this, tr("Scan Failed"),
            tr("The game directory was selected, but no worlds were found "
               "in RESOURCE/Worlds/."));
    }
    updateStatusBar();
}

void MainWindow::onActionScanWorlds()
{
    if (!session_->hasGameDir()) {
        QMessageBox::information(this, tr("No Game Directory"),
            tr("Please select a game directory first via File \u2192 Select Game Directory."));
        return;
    }

    if (!session_->scanWorlds()) {
        QMessageBox::critical(this, tr("Scan Failed"),
            tr("Failed to scan worlds in the game directory."));
        return;
    }

    updateStatusBar();
}

void MainWindow::showWorldListDialog()
{
    if (!session_->hasGameDir()) {
        return;
    }

    if (session_->worldCount() == 0) {
        if (!session_->scanWorlds()) {
            QMessageBox::critical(this, tr("Scan Failed"),
                tr("Failed to scan worlds. Try File \u2192 Select Game Directory first."));
            return;
        }
    }

    const std::vector<MapEditor::WorldInfo> &worlds = session_->getWorldList();
    if (worlds.empty()) {
        QMessageBox::information(this, tr("No Worlds"),
            tr("No worlds found. Make sure the game directory contains RESOURCE/Worlds/."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Select World"));
    dialog.resize(400, 300);

    auto *listWidget = new QListWidget(&dialog);
    for (const auto &w : worlds) {
        listWidget->addItem(QString::fromStdString(w.name));
    }

    const int curIdx = session_->currentWorldIndex();
    if (curIdx >= 0 && curIdx < listWidget->count()) {
        listWidget->setCurrentRow(curIdx);
    }

    auto *btnLoad   = new QPushButton(tr("Load"),   &dialog);
    auto *btnCancel = new QPushButton(tr("Cancel"), &dialog);

    connect(btnLoad,   &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(btnCancel, &QPushButton::clicked, &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(listWidget);

    auto *btnBox = new QHBoxLayout();
    btnBox->addWidget(btnLoad);
    btnBox->addWidget(btnCancel);
    layout->addLayout(btnBox);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const int row = listWidget->currentRow();
    if (row < 0 || row >= static_cast<int>(worlds.size())) {
        return;
    }

    if (!session_->loadWorld(static_cast<size_t>(row))) {
        QMessageBox::critical(this, tr("Load Failed"),
            tr("Failed to load world '%1'.")
                .arg(QString::fromStdString(worlds[static_cast<size_t>(row)].name)));
    }

    updateStatusBar();
}

void MainWindow::onActionLoadWorld()
{
    // Loading another world discards the current one — offer to save first.
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    showWorldListDialog();
    if (session_->isWorldLoaded()) {
        onWorldLoaded();
    }
}

void MainWindow::onActionUnloadWorld()
{
    // Unloading discards every change made since the last save — offer to
    // save first.
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    session_->unloadWorld();
    onWorldUnload();
}

void MainWindow::onActionSave()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    if (!session_->saveWorld()) {
        QMessageBox::critical(this, tr("Save Failed"),
            tr("Failed to save world '%1'. See the log for details.")
                .arg(QString::fromStdString(session_->currentWorldName())));
        return;
    }
    // map.tga was regenerated by the save — keep the minimap image in sync.
    if (minimap_) {
        minimap_->reload();
    }
    updateStatusBar();
}

void MainWindow::onActionSaveAs()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString name = askSaveAsName();
    if (name.isEmpty()) {
        return; // cancelled by the user
    }
    if (!session_->saveWorldAs(name.toStdString())) {
        QMessageBox::critical(this, tr("Save As Failed"),
            tr("Failed to save world '%1' as '%2'.\n"
               "A world with that name may already exist in RESOURCE/Worlds/.")
                .arg(QString::fromStdString(session_->currentWorldName()), name));
        return;
    }
    // saveWorldAs() reloaded the session onto the new world — rebind the
    // viewport and refresh the world list-driven UI state.
    onWorldLoaded();
}

void MainWindow::onActionUndo()
{
    if (session_->undo()) {
        // regRender() marked the affected area; a repaint drains the dirty
        // rectangles and refreshes the raster.
        if (viewport_) {
            viewport_->update();
        }
        updateStatusBar();
    }
}

void MainWindow::onActionRedo()
{
    if (session_->redo()) {
        if (viewport_) {
            viewport_->update();
        }
        updateStatusBar();
    }
}

void MainWindow::onActionClearUndoHistory()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    session_->clearUndoHistory();
    updateStatusBar();
}

// ---------------------------------------------------------------------------
// Unsaved changes / Save As helpers (T1.4)
// ---------------------------------------------------------------------------

bool MainWindow::confirmDiscardUnsavedChanges()
{
    if (session_ == nullptr || !session_->hasUnsavedChanges()) {
        return true;
    }

    const QString message = QString::fromStdString(
        session_->getUnsavedChangesWarning());
    const QMessageBox::StandardButton chosen = QMessageBox::question(
        this, tr("Unsaved Changes"), message,
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (chosen == QMessageBox::Cancel) {
        return false;
    }
    if (chosen == QMessageBox::Save) {
        if (!session_->saveWorld()) {
            QMessageBox::critical(this, tr("Save Failed"),
                tr("Failed to save world '%1'. See the log for details.")
                    .arg(QString::fromStdString(session_->currentWorldName())));
            return false;
        }
    }
    return true;
}

QString MainWindow::askSaveAsName()
{
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Save World As"));
    dialog.setLabelText(
        tr("Name of the new world folder inside RESOURCE/Worlds/:"));
    QString defaultName = QString::fromStdString(session_->currentWorldName());
    defaultName += QStringLiteral("_copy");
    dialog.setTextValue(defaultName);

    for (;;) {
        if (dialog.exec() != QDialog::Accepted) {
            return QString();
        }
        const QString name = dialog.textValue().trimmed();

        if (name.isEmpty()) {
            QMessageBox::warning(this, tr("Invalid Name"),
                tr("The world name must not be empty."));
            continue;
        }
        if (name.contains('/') || name.contains('\\') ||
            name == QStringLiteral(".") || name == QStringLiteral("..")) {
            QMessageBox::warning(this, tr("Invalid Name"),
                tr("The world name must not be a path — only the folder name "
                   "of the new world."));
            continue;
        }
        if (dialog.textValue() != name) {
            // Whitespace was trimmed; show the cleaned value so the user sees
            // what will actually be created before it is written.
            dialog.setTextValue(name);
        }
        return name;
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscardUnsavedChanges()) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

// ---------------------------------------------------------------------------
// Actions state
// ---------------------------------------------------------------------------

void MainWindow::updateActionStates()
{
    const bool worldLoaded = session_ != nullptr && session_->isWorldLoaded();
    if (actSave_) {
        actSave_->setEnabled(worldLoaded);
    }
    if (actSaveAs_) {
        actSaveAs_->setEnabled(worldLoaded);
    }
    if (actUndo_) {
        actUndo_->setEnabled(worldLoaded && session_->canUndo());
    }
    if (actRedo_) {
        actRedo_->setEnabled(worldLoaded && session_->canRedo());
    }
    if (actClearUndoHistory_) {
        actClearUndoHistory_->setEnabled(worldLoaded);
    }
    // View actions make sense only when there is something to look at.
    for (QAction *action : {actZoomToFit_, actView3D_, actRotateLeft_,
                            actRotateRight_, actTiltUp_, actTiltDown_,
                            actLiftUp_, actLiftDown_, actZoomIn_,
                            actZoomOut_, actResetView_}) {
        if (action) {
            action->setEnabled(worldLoaded);
        }
    }
    // T3.1 — painting without a world cannot change anything.
    if (actBrushEdit_) {
        actBrushEdit_->setEnabled(worldLoaded);
    }
    // T3.2 — the brush type/mode buttons follow the same rule.
    for (QAction *action : {actBrushRaise_, actBrushLower_, actBrushLevelZP_,
                            actBrushLevelHeight_, actBrushColor_,
                            actBrushGeo_, actBrushDam_}) {
        if (action) {
            action->setEnabled(worldLoaded);
        }
    }
    // T3.3 — eyedropper and color spinbox.
    if (actEyedropper_) {
        actEyedropper_->setEnabled(worldLoaded);
    }
    if (brushColorIndexSpin_) {
        brushColorIndexSpin_->setEnabled(
            worldLoaded &&
            brushTool_ &&
            brushTool_->params().effect ==
                MapEditor::BrushEffect::SetColor);
    }
    if (brushLevelSpin_) {
        brushLevelSpin_->setEnabled(
            worldLoaded &&
            brushTool_ &&
            brushTool_->params().effect ==
                MapEditor::BrushEffect::AlignVariableH);
    }
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void MainWindow::updateStatusBar()
{
    QString text;

    switch (session_->state()) {
    case MapEditor::SessionState::NoGame:
        // The session stays in NoGame both before any directory is picked
        // and when the picked directory has no worlds — phrase accordingly.
        text = session_->hasGameDir()
            ? tr("Game directory selected, but no worlds found "
                 "(File \u2192 Scan Worlds).")
            : tr("No game directory selected.");
        break;
    case MapEditor::SessionState::GameReady:
        text = tr("Game ready \u2014 %1 world(s) found.").arg(
            static_cast<int>(session_->worldCount()));
        break;
    case MapEditor::SessionState::WorldLoaded:
        text = tr("World: %1%2").arg(
            QString::fromStdString(session_->currentWorldName()),
            session_->isModified() ? QStringLiteral(" [modified]") : QString());
        break;
    }

    statusBar()->showMessage(text);

    // Keep the Edit/File actions in sync with the session state.
    updateActionStates();
}

#include "MainWindow.h"

#include <View3DRenderer.h>
#include <BrushTool.h>
#include <EditorSession.h>
#include <GameCameraPreset.h>
#include <GameDirManager.h>
#include <BattleMapPanel.h>
#include <HelpWindow.h>
#include <ImageDecal.h>
#include <MinimapWidget.h>
#include <NewWorldWizard.h>
#include <MarkerPanel.h>
#include <PalettePanel.h>
#include <ViewportRenderer.h>
#include <ViewportWidget.h>
#include <WorldFiles.h>
#include <WorldImages.h>
#include <WorldSettingsDialog.h>
#include <ValidationDialog.h>

#include <filesystem>
#include <algorithm>
#include <cmath>

#include "EditorTrace.h"
using MapEditor::editorTrace;

namespace fs = std::filesystem;

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
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

/// T4.2 — View editor mode: the mouse-arrow the navigation gives back after
/// the brush/pipette are put down.
QIcon makeViewModeIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 1.6));
    p.setBrush(kAccent);
    QPolygonF arrow;
    arrow << QPointF(6.0, 3.5)
          << QPointF(6.0, 17.0)
          << QPointF(9.8, 13.6)
          << QPointF(12.4, 18.5)
          << QPointF(14.6, 17.3)
          << QPointF(12.0, 12.6)
          << QPointF(16.5, 12.0);
    p.drawPolygon(arrow);
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

/// T3.5 — load-image action icon: folder with import arrow.
QIcon makeLoadImageIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 1.5));
    p.setBrush(QColor(220, 200, 140));
    QPolygonF folder;
    folder << QPointF(3.0, 8.0) << QPointF(3.0, 18.0) << QPointF(19.0, 18.0)
           << QPointF(19.0, 8.0) << QPointF(11.0, 8.0) << QPointF(9.0, 5.0)
           << QPointF(3.0, 5.0);
    p.drawPolygon(folder);
    p.setPen(QPen(kAccent, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(11.0, 10.0), QPointF(11.0, 15.0));
    p.setBrush(kAccent);
    p.setPen(Qt::NoPen);
    QPolygonF arrow;
    arrow << QPointF(11.0, 16.5) << QPointF(8.5, 13.5) << QPointF(13.5, 13.5);
    p.drawPolygon(arrow);
    return QIcon(pm);
}

/// ImageDecal — a framed picture glyph: the stamp is a palette-limited image,
/// so the button shows a small landscape in a frame.
QIcon makeDecalIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 1.5));
    p.setBrush(QColor(235, 235, 235));
    p.drawRect(QRectF(3.5, 4.5, 15.0, 13.0));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(120, 170, 220));
    p.drawRect(QRectF(5.0, 6.0, 12.0, 6.0));
    p.setBrush(QColor(90, 150, 90));
    p.drawRect(QRectF(5.0, 12.0, 12.0, 4.0));
    p.setBrush(QColor(245, 220, 120));
    p.drawEllipse(QPointF(8.0, 8.0), 1.6, 1.6);
    return QIcon(pm);
}

/// T3.1 — brush form (stamp shape) glyphs: the silhouette the brush paints
/// with, drawn as it looks on the map (filled/outline circle and square, the
/// two-point line).
QIcon makeShapeIcon(MapEditor::BrushShape shape)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor fill(120, 170, 220, 200);
    switch (shape) {
    case MapEditor::BrushShape::FilledCircle:
        p.setPen(QPen(kInk, 1.5));
        p.setBrush(fill);
        p.drawEllipse(QRectF(4.0, 4.0, 14.0, 14.0));
        break;
    case MapEditor::BrushShape::FilledSquare:
        p.setPen(QPen(kInk, 1.5));
        p.setBrush(fill);
        p.drawRect(QRectF(4.5, 4.5, 13.0, 13.0));
        break;
    case MapEditor::BrushShape::CircleOutline:
        p.setPen(QPen(kAccent, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(4.0, 4.0, 14.0, 14.0));
        break;
    case MapEditor::BrushShape::SquareOutline:
        p.setPen(QPen(kAccent, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(4.5, 4.5, 13.0, 13.0));
        break;
    case MapEditor::BrushShape::Line:
        p.setPen(QPen(kInk, 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(5.0, 16.0), QPointF(17.0, 6.0));
        p.setBrush(kAccent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(5.0, 16.0), 2.5, 2.5);
        p.drawEllipse(QPointF(17.0, 6.0), 2.5, 2.5);
        break;
    }
    return QIcon(pm);
}

/// T3.5: Convert a QImage to a BrushMask (pixel alpha → influence).
/// Transparent pixels (alpha = 0) carry no effect; non-transparent ones
/// influence the brush proportionally to their alpha (anti-aliased edges
/// get a partial effect) — the color/luminance is deliberately ignored,
/// the mask is about the shape, not the picture's brightness.
MapEditor::BrushMask imageToBrushMask(const QImage &img)
{
    MapEditor::BrushMask mask;
    if (img.isNull() || img.width() <= 0 || img.height() <= 0) {
        return mask;
    }
    const QImage fmt = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = fmt.width();
    const int h = fmt.height();
    mask.sx = w;
    mask.sy = h;
    mask.x = -(w / 2);
    mask.y = -(h / 2);
    mask.influence.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(fmt.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            mask.influence[static_cast<std::size_t>(y) * w + x] =
                static_cast<unsigned char>(qAlpha(line[x]));
        }
    }
    return mask;
}

/// T3.5 — scale a brush mask by `factor` (bilinear over the influence
/// values). The result is re-centred on the stamp origin like a freshly
/// converted image mask. Used by the mask scale control on the Brush
/// toolbar; the source is always the un-scaled original mask, so repeated
/// scale changes do not accumulate interpolation blur.
MapEditor::BrushMask scaleBrushMask(const MapEditor::BrushMask &src, double factor)
{
    MapEditor::BrushMask dst;
    if (src.empty() || factor <= 0.0) {
        return dst;
    }
    const int w = std::max(1, static_cast<int>(std::lround(src.sx * factor)));
    const int h = std::max(1, static_cast<int>(std::lround(src.sy * factor)));
    dst.sx = w;
    dst.sy = h;
    dst.x = -(w / 2);
    dst.y = -(h / 2);
    dst.influence.assign(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        // Source coordinate of the destination pixel centre (bilinear).
        const double sy = (y + 0.5) / factor - 0.5;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src.sy - 1);
        const int y1 = std::min(y0 + 1, src.sy - 1);
        const double fy = std::clamp(sy - y0, 0.0, 1.0);
        for (int x = 0; x < w; ++x) {
            const double sx = (x + 0.5) / factor - 0.5;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src.sx - 1);
            const int x1 = std::min(x0 + 1, src.sx - 1);
            const double fx = std::clamp(sx - x0, 0.0, 1.0);
            const double v =
                src.at(x0, y0) * (1.0 - fx) * (1.0 - fy) +
                src.at(x1, y0) * fx * (1.0 - fy) +
                src.at(x0, y1) * (1.0 - fx) * fy +
                src.at(x1, y1) * fx * fy;
            dst.influence[static_cast<std::size_t>(y) * w + x] =
                static_cast<unsigned char>(std::lround(v));
        }
    }
    return dst;
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
    addToolBarBreak();
    buildBrushToolbar();
    buildStatusBar();
    buildViewport();

    // The toggle/spinbox states were set before the viewport existed — push
    // them now (they reach the renderer when a world is bound).
    if (viewport_) {
        if (actDimGeo_) {
            viewport_->setGeoDimmed(actDimGeo_->isChecked());
        }
        if (gridStepWSpin_ && gridStepHSpin_) {
            viewport_->setGridStep(gridStepWSpin_->value(),
                                   gridStepHSpin_->value());
        }
    }

    // The viewport was built silently — no camera signal has fired yet, so
    // fill the readout with the actual 2D zoom state instead of leaving it
    // blank until the first pan/zoom.
    updateViewReadout();
    updateStatusBar();
    editorTrace("MainWindow: ctor done");
}

MainWindow::~MainWindow()
{
    editorTrace("MainWindow: dtor start");
    delete brushTool_;
    delete session_;
    delete gameDirMgr_;
    // viewport_ is a child widget; deleted by the parent.
    editorTrace("MainWindow: dtor members deleted (viewport dies with parent)");
}

void MainWindow::debugOpenGameDir(const std::string& dir)
{
    editorTrace("autotest: setGameDir('%s')", dir.c_str());
    if (!session_->setGameDir(dir)) {
        editorTrace("autotest: setGameDir FAILED");
        return;
    }
    if (!session_->scanWorlds()) {
        editorTrace("autotest: scanWorlds FAILED");
        return;
    }
    updateStatusBar();
    editorTrace("autotest: game dir opened (%zu worlds)",
                session_->worldCount());
}

void MainWindow::debugLoadWorld(const std::string& worldName)
{
    editorTrace("autotest: loadWorldByName('%s')", worldName.c_str());
    if (!session_->loadWorldByName(worldName)) {
        editorTrace("autotest: loadWorldByName FAILED");
        return;
    }
    editorTrace("autotest: calling onWorldLoaded");
    onWorldLoaded();
    editorTrace("autotest: after onWorldLoaded, updating status bar");
    updateStatusBar();
    editorTrace("autotest: world loaded");
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
    // The editor mode actions were created before this widget existed — push
    // the checked state into the viewport now that it is here (the Brush
    // mode is the viewport default, so it needs no explicit set).
    if (viewport_ && actModeEyedropper_ && actModeView_) {
        if (actModeEyedropper_->isChecked()) {
            viewport_->setEditorMode(
                MapEditor::ViewportWidget::EditorMode::Eyedropper);
        } else if (actModeView_->isChecked()) {
            viewport_->setEditorMode(MapEditor::ViewportWidget::EditorMode::View);
        }
    }
    syncBrushControls();
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

    // T4.2 — editor mode follows the viewport too (mode actions keep their
    // check state when the interaction switches the mode side-effects).
    connect(viewport_, &MapEditor::ViewportWidget::editorModeChanged, this,
            [this](MapEditor::ViewportWidget::EditorMode mode) {
                onEditorModeChanged(static_cast<int>(mode));
            });

    // T3.3 — eyedropper signal: right-click in TopDown mode picks SurBuf.
    connect(viewport_, &MapEditor::ViewportWidget::eyedropperPicked,
            this, &MainWindow::onEyedropperPicked);

    // Hide the freeze-dam action when the GPU terrain path is available —
    // the feature is a CPU-path-only fallback (it depends on the torus
    // repetition logic in View3DRenderer, which sokol does not implement).
    connect(viewport_, &MapEditor::ViewportWidget::gpuViewReadyChanged, this,
            [this](bool gpuReady) {
                if (actFreezeDam_) {
                    actFreezeDam_->setVisible(!gpuReady);
                }
            });

    // Keep the camera preset pair (like in game / like in editor) in sync
    // when the viewport's game camera mode changes. The switch also changes
    // WHICH rotation the spinbox mirrors (the game preset psi=0 vs the
    // editor turnAngleDeg), so refresh it here.
    connect(viewport_, &MapEditor::ViewportWidget::gameCameraModeChanged,
            this, [this](bool gameMode) {
                if (actGameCamera_) {
                    actGameCamera_->setChecked(gameMode);
                }
                if (actEditorCamera_) {
                    actEditorCamera_->setChecked(!gameMode);
                }
                syncCameraRotationSpin();
            });

    /// Initial style button state (the viewport defaults to Smooth; post-T3.3
    /// chunky/smooth renderer toggle).
    actChunkyRender_->setChecked(viewport_->renderStyle() !=
        MapEditor::ViewportWidget::RenderStyle::Smooth);
    actSmoothRender_->setChecked(viewport_->renderStyle() ==
        MapEditor::ViewportWidget::RenderStyle::Smooth);

    setCentralWidget(viewport_);

    // T2.4 — the minimap goes into its own dock next to the central widget.
    buildMinimapDock();
    // T4.1 — the palette panel takes its place below the minimap dock.
    buildPaletteDock();
    // T7.2 — the marker panel dock.
    buildMarkerDock();
    // T8 — the battle map panel dock.
    buildBattleMapDock();
}

void MainWindow::buildMarkerDock()
{
    markerPanel_ = new MapEditor::MarkerPanel(markerStore_, this);

    markerDock_ = new QDockWidget(tr("Markers"), this);
    markerDock_->setObjectName(QStringLiteral("MarkerDock"));
    markerDock_->setWidget(markerPanel_);
    markerDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                 Qt::RightDockWidgetArea |
                                 Qt::TopDockWidgetArea |
                                 Qt::BottomDockWidgetArea);
    markerDock_->setFeatures(QDockWidget::DockWidgetClosable |
                             QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, markerDock_);

    if (viewMenu_) {
        viewMenu_->addSeparator();
        QAction *toggleAction = markerDock_->toggleViewAction();
        toggleAction->setText(tr("&Markers"));
        toggleAction->setToolTip(tr("Show or hide the marker panel (K)"));
        toggleAction->setShortcut(QKeySequence(Qt::Key_K));
        viewMenu_->addAction(toggleAction);
    }

    // Any panel mutation: autosave the sidecar and refresh the overlay.
    connect(markerPanel_, &MapEditor::MarkerPanel::markersChanged, this, [this] {
        saveMarkerSidecar();
        syncMarkerOverlay();
    });
    connect(markerPanel_, &MapEditor::MarkerPanel::goToMarker, this,
            [this](int mapX, int mapY) {
                if (viewport_ && session_ && session_->isWorldLoaded()) {
                    viewport_->centerOnCell(QPoint(mapX, mapY));
                    viewport_->update();
                }
            });
}

void MainWindow::loadMarkerSidecar()
{
    markerStore_.clear();
    const auto files = session_ ? session_->currentWorldFiles() : std::nullopt;
    if (files && session_->isWorldLoaded()) {
        markerStore_.load(MapEditor::MarkerStore::sidecarPathFor(
            files->worldDir()));
    }
    if (markerPanel_) {
        markerPanel_->refresh();
    }
    syncMarkerOverlay();
}

void MainWindow::saveMarkerSidecar()
{
    const auto files = session_ ? session_->currentWorldFiles() : std::nullopt;
    if (!files || !session_->isWorldLoaded()) {
        return;
    }
    if (!markerStore_.save(
            MapEditor::MarkerStore::sidecarPathFor(files->worldDir()),
            session_->currentWorldName())) {
        statusBar()->showMessage(tr("Failed to save the marker sidecar."), 5000);
    }
}

void MainWindow::syncMarkerOverlay()
{
    if (!viewport_) {
        return;
    }
    std::vector<MapEditor::MapMarker> overlay;
    overlay.reserve(markerStore_.markers().size());
    for (const MapEditor::MapMarkerData &marker : markerStore_.markers()) {
        MapEditor::MapMarker ui;
        ui.label = QString::fromStdString(marker.label);
        ui.type = QString::fromStdString(marker.type);
        ui.mapX = marker.x;
        ui.mapY = marker.y;
        ui.radius = marker.radius;
        ui.color = QColor(static_cast<int>((marker.colorRgb >> 16) & 0xFF),
                          static_cast<int>((marker.colorRgb >> 8) & 0xFF),
                          static_cast<int>(marker.colorRgb & 0xFF));
        overlay.push_back(std::move(ui));
    }
    viewport_->setMarkers(overlay);
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

    // The view outline follows the viewport — whatever moves it
    // (pan, zoom, orbit, mode switch, resize, world load/unload) ends in
    // ViewportWidget::viewPolygonChanged after the next repaint. The polygon
    // carries the 3D camera azimuth/perspective that an axis-aligned rect
    // would flatten.
    connect(viewport_, &MapEditor::ViewportWidget::viewPolygonChanged,
            minimap_, &MapEditor::MinimapWidget::setViewPolygon);

    // The top view follows the terrain: every viewport repaint that repainted
    // cells (brush stamp, undo/redo, image import, palette edit) re-samples the
    // panel's raster from the live world — map.tga on disk only catches up on
    // save, so reading the file back after an edit would show the old surface.
    connect(viewport_, &MapEditor::ViewportWidget::terrainRepainted,
            minimap_, &MapEditor::MinimapWidget::refreshFromLiveTerrain);

    // T7 — the "Add" marker default position follows the view centre.
    connect(viewport_, &MapEditor::ViewportWidget::viewRectChanged, this,
            [this](const QRect &rect) {
                if (markerPanel_) {
                    markerPanel_->setDefaultPosition(rect.center().x(),
                                                     rect.center().y());
                }
            });

    // Clicking the minimap navigates there instead.
    connect(minimap_, &MapEditor::MinimapWidget::cellClicked,
            this, &MainWindow::onMinimapCellClicked);

    // Load the image for the world that may already be bound and push the
    // current view rectangle once (subsequent updates come from the signal).
    minimap_->setSession(session_);
    minimap_->setViewPolygon(viewport_->currentViewPolygon());
}

void MainWindow::buildPaletteDock()
{
    palettePanel_ = new MapEditor::PalettePanel(this);

    paletteDock_ = new QDockWidget(tr("Palette"), this);
    paletteDock_->setObjectName(QStringLiteral("PaletteDock"));
    paletteDock_->setWidget(palettePanel_);
    paletteDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                  Qt::RightDockWidgetArea |
                                  Qt::TopDockWidgetArea |
                                  Qt::BottomDockWidgetArea);
    paletteDock_->setFeatures(QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable);
    // Stacks under the minimap dock on the same side.
    addDockWidget(Qt::RightDockWidgetArea, paletteDock_);

    // The View menu owns the show/hide toggle (default: visible).
    if (viewMenu_) {
        viewMenu_->addSeparator();
        QAction *toggleAction = paletteDock_->toggleViewAction();
        toggleAction->setText(tr("&Palette"));
        toggleAction->setToolTip(
            tr("Show or hide the palette panel (P)"));
        toggleAction->setShortcut(QKeySequence(Qt::Key_P));
        viewMenu_->addAction(toggleAction);
    }

    // A selected swatch becomes the color the brush paints: the palette the
    // swatch lives in follows the brush (Geo / Dam layer, T4.1 model — a
    // SurBuf value indexes the palette of the cell's layer directly).
    connect(palettePanel_, &MapEditor::PalettePanel::colorSelected,
            this, [this](int paletteIndex, bool dam) {
        if (brushTool_) {
            MapEditor::BrushParams params = brushTool_->params();
            params.mode = dam ? MapEditor::BrushMode::Dam
                              : MapEditor::BrushMode::Geo;
            params.colorIndex = paletteIndex;
            brushTool_->setParams(params);
        }
        if (brushColorIndexSpin_) {
            brushColorIndexSpin_->blockSignals(true);
            brushColorIndexSpin_->setValue(paletteIndex);
            brushColorIndexSpin_->blockSignals(false);
        }
        syncBrushControls();
        syncColorControls();
    });

    // An edited palette re-tints the whole world: rebuild both render paths.
    connect(palettePanel_, &MapEditor::PalettePanel::paletteEdited,
            this, [this](int, bool) {
        refreshPaletteRenderers();
        // The current brush color swatch may be the entry that was edited.
        syncColorControls();
    });

    // Push the session that may already be bound.
    palettePanel_->setSession(session_);
}

void MainWindow::onWorldLoaded()
{
    editorTrace("onWorldLoaded: start (viewport=%p, session=%p)",
                static_cast<void*>(viewport_), static_cast<void*>(session_));
    // Rebind the viewport to pick up the newly loaded world.
    if (viewport_ && session_) {
        editorTrace("onWorldLoaded: calling viewport->setSession");
        viewport_->setSession(session_);
        editorTrace("onWorldLoaded: viewport setSession done");
    }
    // Rebind the minimap image (map.tga of the new world) and reset the
    // outline until the viewport's next repaint pushes a rectangle.
    editorTrace("onWorldLoaded: setting minimap session");
    if (minimap_ && session_) {
        minimap_->setSession(session_);
        minimap_->setViewPolygon(viewport_ ? viewport_->currentViewPolygon()
                                           : QPolygonF());
    }
    editorTrace("onWorldLoaded: setting palette session");
    // Rebuild the palette panel grids (T4.1) — the palette belongs to the
    // newly loaded world. setSession() resets the swatch selection, so push
    // the brush state back in: the highlighted entry must keep showing the
    // color the brush will paint.
    if (palettePanel_ && session_) {
        palettePanel_->setSession(session_);
        const MapEditor::BrushParams &params = brushTool_->params();
        palettePanel_->selectIndex(params.colorIndex,
                                   params.mode == MapEditor::BrushMode::Dam);
    }
    editorTrace("onWorldLoaded: loading markers");
    // T7 — markers of the new world: sidecar + overlay.
    loadMarkerSidecar();
    editorTrace("onWorldLoaded: sync brush controls");
    // The brush-dependent controls (color swatch, spinboxes, cursor preview)
    // were only partially initialized before the world existed — re-read the
    // tool state now that the palettes are available.
    syncBrushControls();
    editorTrace("onWorldLoaded: updating view readout");
    // The camera readout has never fired its signal yet on a fresh session.
    updateViewReadout();
    editorTrace("onWorldLoaded: updating window title");
    // The title carries the world name (also after "Save As", which
    // rebinds the session onto the renamed world and lands here).
    updateWindowTitle();
    editorTrace("onWorldLoaded: updating status bar");
    updateStatusBar();
    editorTrace("onWorldLoaded: done");
}

void MainWindow::onWorldUnload()
{
    if (viewport_ && session_) {
        viewport_->setSession(session_);
    }
    if (minimap_ && session_) {
        minimap_->setSession(session_);
        minimap_->setViewPolygon(QPolygonF());
    }
    if (palettePanel_ && session_) {
        palettePanel_->setSession(session_);
    }
    // T7 — no world, no markers (the sidecar belongs to the world folder).
    markerStore_.clear();
    if (markerPanel_) {
        markerPanel_->refresh();
    }
    syncMarkerOverlay();
    // The brush controls lose their world-bound state (color swatch RGB,
    // palette selection) — re-read the tool so nothing stale stays on screen.
    syncBrushControls();
    updateWindowTitle();
    updateStatusBar();
}

void MainWindow::updateCursorInfo(int mapX, int mapY)
{
    // Both view modes report the cell under the cursor: TopDown maps the
    // screen linearly, Oblique ray-picks the rendered voxel surface.
    QString info = QString(tr("X: %1  Y: %2")).arg(mapX).arg(mapY);
    // T4.2 — also show the palette entry and RGB of the pixel: a SurBuf
    // value indexes the palette of the cell's own layer (T4.1 model), so the
    // readout resolves it the same way the renderer does (geo/dam).
    if (session_ && session_->isWorldLoaded() && mapX >= 0 && mapY >= 0) {
        const int surIndex = session_->surBufAt(mapX, mapY);
        const bool dam = session_->cellIsDam(mapX, mapY);
        const unsigned int rgb =
            session_->paletteRgb(dam ? MapEditor::EditorSession::PaletteLayer::Dam
                                     : MapEditor::EditorSession::PaletteLayer::Geo,
                                 surIndex);
        info += QString(tr("  Sur: %1 (%2)  %3"))
                    .arg(surIndex)
                    .arg(dam ? tr("Dam") : tr("Geo"))
                    .arg(rgb, 6, 16, QLatin1Char('0'));
    }
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

void MainWindow::onActionGameCamera()
{
    if (!viewport_) {
        return;
    }

    // Aim at the first player of the loaded world — the camera state the
    // game itself starts a battle with (psi=0, theta=0, min zoom distance;
    // FORTOP and the other shipped worlds carry no camera splines, so the
    // frame anchor is the player view centre).
    QPointF presetCenter;
    if (session_ && session_->isWorldLoaded()) {
        
        // Used preset from FORTOP, since current can be also not present in world
        static constexpr MapEditor::GameCameraPreset default_preset{ 354.791000,306.425000,89.509186,17.999985,1000.000000,true };
        /*const MapEditor::GameCameraPreset preset =
            MapEditor::resolveGameCameraPreset(
                session_->getGameDir(), session_->currentWorldName(),
                viewport_->renderer() ? viewport_->renderer()->width() / 2.0
                                      : 0.0,
                viewport_->renderer() ? viewport_->renderer()->height() / 2.0
                                      : 0.0);
        */
        const MapEditor::GameCameraPreset preset = default_preset;
        presetCenter = QPointF(preset.centerX, preset.centerY);
    }

    // The game camera model (FOV frustum, terCameraType matrix) lives in
    // the GPU terrain path; without it there is nothing to switch to.
    if (!viewport_->setGameCameraMode(true, presetCenter)) {
        QMessageBox::warning(
            this, tr("Camera Like in Game"),
            tr("The game camera is only available in the GPU 3D view. "
               "The editor camera stays active."));
        if (actGameCamera_) {
            actGameCamera_->setChecked(false);
        }
        if (actEditorCamera_) {
            actEditorCamera_->setChecked(true);
        }
        return;
    }

    // Make sure the 3D view is visible — the game camera replaces the
    // editor 3D camera, switching from the 2D view otherwise shows nothing.
    if (viewport_->viewMode() != MapEditor::ViewportWidget::ViewMode::Oblique) {
        viewport_->setViewMode(MapEditor::ViewportWidget::ViewMode::Oblique);
    }
}

void MainWindow::onActionEditorCamera()
{
    if (viewport_) {
        viewport_->setGameCameraMode(false, QPointF());
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
    syncCameraRotationSpin();
    updateViewReadout();
}

void MainWindow::syncCameraRotationSpin()
{
    if (!camRotationSpin_ || !viewport_) {
        return;
    }
    // The spinbox shows a single decimal; compare at that precision so a
    // camera change below the display step does not re-write the control.
    const double shown =
        std::round(viewport_->rotationDeg() * 10.0) / 10.0;
    if (std::abs(camRotationSpin_->value() - shown) < 1e-9) {
        return;
    }
    camRotationSpin_->blockSignals(true);
    camRotationSpin_->setValue(shown);
    camRotationSpin_->blockSignals(false);
}

void MainWindow::onEditorModeChanged(int mode)
{
    using Viewport = MapEditor::ViewportWidget;
    // Only mirrors the viewport state into the exclusive mode actions —
    // the View mode stays reachable no matter how the mode was switched.
    if (actModeView_) {
        actModeView_->setChecked(mode == static_cast<int>(Viewport::EditorMode::View));
    }
    if (actModeBrush_) {
        actModeBrush_->setChecked(mode == static_cast<int>(Viewport::EditorMode::Brush));
    }
    if (actModeEyedropper_) {
        actModeEyedropper_->setChecked(
            mode == static_cast<int>(Viewport::EditorMode::Eyedropper));
    }
}

void MainWindow::onMinimapCellClicked(int mapX, int mapY)
{
    if (viewport_) {
        viewport_->centerOnCell(QPoint(mapX, mapY));
    }
}

void MainWindow::onEyedropperPicked(int mapX, int mapY, unsigned char surIndex)
{
    // T4.2 — BrushTool::pickColorAt already stored the sampled pixel and put
    // it into params().colorIndex; the eyedropper readout slots the color
    // brush in for painting the sampled color afterwards. The SurBuf value
    // indexes the palette of the sampled cell's layer — point the brush at
    // the same layer so the color readout and the paint resolve it exactly
    // like the renderer does (T4.1 model).
    const bool dam = session_ && session_->cellIsDam(mapX, mapY);
    MapEditor::BrushParams params = brushTool_->params();
    params.effect = MapEditor::BrushEffect::SetColor;
    params.colorIndex = surIndex;
    params.mode = dam ? MapEditor::BrushMode::Dam : MapEditor::BrushMode::Geo;
    brushTool_->setParams(params);

    if (brushColorIndexSpin_) {
        brushColorIndexSpin_->blockSignals(true);
        brushColorIndexSpin_->setValue(surIndex);
        brushColorIndexSpin_->blockSignals(false);
    }
    syncColorControls();
    syncBrushControls();

    // T4.1 — mirror the pick into the palette panel selection.
    if (palettePanel_) {
        palettePanel_->selectIndex(static_cast<int>(surIndex), dam);
    }

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
        // rot follows the ACTIVE camera model (game psi in the game camera
        // mode) so the readout agrees with the rotation spinbox; the other
        // fields are editor-camera knobs either way.
        viewReadout_->setText(tr("3D    rot %1\u00B0  tilt %2\u00B0  lift %3  focus %4")
                                  .arg(qRound(viewport_->rotationDeg()))
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

    QAction *actNewWorld = fileMenu->addAction(tr("New &World..."));
    actNewWorld->setToolTip(tr("Create a new world folder (flat or generated terrain)"));
    connect(actNewWorld, &QAction::triggered, this, &MainWindow::onActionNewWorld);

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

    actWorldParams_ = fileMenu->addAction(tr("World &Parameters..."));
    actWorldParams_->setToolTip(tr("Zero-layer height, generation powers, storage compression, geo noise and the zeroplast color"));
    actWorldParams_->setEnabled(false);
    connect(actWorldParams_, &QAction::triggered, this, &MainWindow::onActionWorldParams);

    QMenu *imageMenu = fileMenu->addMenu(tr("&Images"));
    QAction *actImportMapImage = imageMenu->addAction(tr("Import Image onto &Map..."));
    actImportMapImage->setToolTip(tr("Paint the surface colors from an image (RGB -> nearest palette index, indexed TGA -> indices as-is)"));
    actImportMapImage->setEnabled(false);
    connect(actImportMapImage, &QAction::triggered, this, &MainWindow::onActionImportMapImage);
    importMapImageAction_ = actImportMapImage;

    QAction *actImportUpTga = imageMenu->addAction(tr("Import &up.tga (cover image)..."));
    actImportUpTga->setToolTip(tr("Scale any image to 512x512 and store it as the world's up.tga"));
    actImportUpTga->setEnabled(false);
    connect(actImportUpTga, &QAction::triggered, this, &MainWindow::onActionImportUpTga);
    importUpTgaAction_ = actImportUpTga;

    QAction *actImportLeveled = imageMenu->addAction(tr("Import &leveledSurfaceTexture.tga..."));
    actImportLeveled->setToolTip(tr("Replace the leveled-zones texture (validated: indexed TGA, power of two, <= map size)"));
    actImportLeveled->setEnabled(false);
    connect(actImportLeveled, &QAction::triggered, this, &MainWindow::onActionImportLeveledTexture);
    importLeveledAction_ = actImportLeveled;

    imageMenu->addSeparator();
    QAction *actExportHeightmap = imageMenu->addAction(tr("&Export Heightmap (TGA)..."));
    actExportHeightmap->setToolTip(tr("8-bit grayscale heightmap (SGetAlt >> 5, dams included)"));
    actExportHeightmap->setEnabled(false);
    connect(actExportHeightmap, &QAction::triggered, this, &MainWindow::onActionExportHeightmap);
    exportHeightmapAction_ = actExportHeightmap;

    QAction *actExportTopView = imageMenu->addAction(tr("Export &Top View (TGA)..."));
    actExportTopView->setToolTip(tr("The surface raster exactly as the viewport and cache.tga show it"));
    actExportTopView->setEnabled(false);
    connect(actExportTopView, &QAction::triggered, this, &MainWindow::onActionExportTopView);
    exportTopViewAction_ = actExportTopView;

    fileMenu->addSeparator();

    QAction *actOpenBattleMap = fileMenu->addAction(tr("Open Battle &Map (.spg)..."));
    actOpenBattleMap->setToolTip(tr("Load a battle map (RESOURCE/Battle, RESOURCE/Worlds, SCENARIO, SURVIVAL) and its world"));
    actOpenBattleMap->setEnabled(false);
    connect(actOpenBattleMap, &QAction::triggered, this, &MainWindow::onActionOpenBattleMap);
    actOpenBattleMap_ = actOpenBattleMap;

    QAction *actSaveBattleMap = fileMenu->addAction(tr("Save Battle &Map"));
    actSaveBattleMap->setToolTip(tr("Write the loaded battle map back to its .spg"));
    actSaveBattleMap->setEnabled(false);
    connect(actSaveBattleMap, &QAction::triggered, this, &MainWindow::onActionSaveBattleMap);
    actSaveBattleMap_ = actSaveBattleMap;

    QAction *actSaveBattleMapAs = fileMenu->addAction(tr("Save Battle Map &As..."));
    actSaveBattleMapAs->setToolTip(tr("Write the battle map under a new name (RESOURCE/Battle or RESOURCE/Worlds)"));
    actSaveBattleMapAs->setEnabled(false);
    connect(actSaveBattleMapAs, &QAction::triggered, this, &MainWindow::onActionSaveBattleMapAs);
    actSaveBattleMapAs_ = actSaveBattleMapAs;

    QAction *actNewBattleMap = fileMenu->addAction(tr("New Battle &Map..."));
    actNewBattleMap->setToolTip(tr("Create a battle map (.spg) for the loaded world: player spawns, cameras and player settings"));
    actNewBattleMap->setEnabled(false);
    connect(actNewBattleMap, &QAction::triggered, this, &MainWindow::onActionNewBattleMap);
    actNewBattleMap_ = actNewBattleMap;

    fileMenu->addSeparator();

    QAction *actQuit = fileMenu->addAction(tr("E&xit"));
    actQuit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    connect(actQuit, &QAction::triggered, this, &QApplication::quit);

    buildEditMenu(menuBar());
    buildViewMenu(menuBar());
    buildToolsMenu();
    buildHelpMenu();
}

void MainWindow::buildEditMenu(QMenuBar *menuBar)
{
    QMenu *editMenu = menuBar->addMenu(tr("&Edit"));

    actUndo_ = editMenu->addAction(tr("&Undo"));
    actUndo_->setToolTip(tr("Undo the last terrain edit (Ctrl+Z)"));
    actUndo_->setShortcut(QKeySequence::Undo);
    actUndo_->setEnabled(false);
    connect(actUndo_, &QAction::triggered, this, &MainWindow::onActionUndo);

    actRedo_ = editMenu->addAction(tr("&Redo"));
    actRedo_->setToolTip(tr("Redo the last undone terrain edit (Ctrl+R or Ctrl+Y)"));
    actRedo_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_R),
                            QKeySequence::Redo});
    actRedo_->setEnabled(false);
    connect(actRedo_, &QAction::triggered, this, &MainWindow::onActionRedo);

    editMenu->addSeparator();

    actClearUndoHistory_ = editMenu->addAction(tr("C&lear Undo History"));
    actClearUndoHistory_->setToolTip(tr("Discard the whole undo/redo history"));
    actClearUndoHistory_->setEnabled(false);
    connect(actClearUndoHistory_, &QAction::triggered,
            this, &MainWindow::onActionClearUndoHistory);

    editMenu->addSeparator();

    // T4.2 — the editor interaction modes (View / Brush / Eyedropper) as an
    // exclusive switch, exposed at the top of the Edit menu next to the
    // brush function they drive.
    QMenu *modeMenu = editMenu->addMenu(tr("Edito&r Mode"));
    modeMenu->addAction(actModeView_);
    modeMenu->addAction(actModeBrush_);
    modeMenu->addAction(actModeEyedropper_);

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
    brushMenu->addAction(actBrushPickColor_);
    brushMenu->addSeparator();
    brushMenu->addAction(actBrushImageDecal_);
    brushMenu->addSeparator();
    brushMenu->addAction(actBrushSoot_);
    brushMenu->addAction(actBrushVeryLightDam_);
    brushMenu->addSeparator();
    brushMenu->addAction(actLoadBrushImage_);
    brushMenu->addSeparator();
    // T3.1 — the stamp form, same actions as on the Brush toolbar.
    brushMenu->addAction(actShapeCircle_);
    brushMenu->addAction(actShapeSquare_);
    brushMenu->addAction(actShapeCircleOutline_);
    brushMenu->addAction(actShapeSquareOutline_);
    brushMenu->addAction(actShapeLine_);
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
    actGrid_->setChecked(false);
    actGrid_->setShortcut(QKeySequence(Qt::Key_G));
    connect(actGrid_, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setGridVisible(checked);
        }
    });

    // In-game look: the game dims the un-terraformed geo surface to shades
    // of grey; the editor renders full palette colors by default. This
    // toggle switches the composite view (2D raster + smooth 3D) to the
    // dimmed game look and back.
    actDimGeo_ = create();
    actDimGeo_->setText(tr("Dim &Geo Layer (Game Look)"));
    actDimGeo_->setToolTip(
        tr("Render cells without a dam as darkened shades of grey, like the "
           "game shows the un-terraformed surface (2D view and smooth 3D; "
           "the dam areas keep their colors)."));
    actDimGeo_->setCheckable(true);
    actDimGeo_->setChecked(true);
    connect(actDimGeo_, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setGeoDimmed(checked);
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

    // Freeze-dam 3D scroll: the world repeats infinitely, but with the dam
    // frozen only the geo layer scrolls — the repeats render geo-only, so
    // the dam stays in its single original place.
    actFreezeDam_ = create();
    actFreezeDam_->setText(tr("&Freeze Dam Layer (3D Scroll)"));
    actFreezeDam_->setToolTip(
        tr("The 3D world repeats infinitely when scrolling. With the dam "
           "frozen, only the geo layer repeats — the dam stays in its one "
           "original place, which makes the real map edge easy to spot. "
           "Smooth renderer only."));
    actFreezeDam_->setCheckable(true);
    actFreezeDam_->setChecked(false);
    connect(actFreezeDam_, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setDamFrozen(checked);
        }
    });

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
        if (viewport_) viewport_->adjustObliqueLift(25);
    });

    actLiftDown_ = create();
    actLiftDown_->setText(tr("Camera &Lower"));
    actLiftDown_->setIcon(makeLiftIcon(false));
    actLiftDown_->setToolTip(tr("Lower the 3D camera one step (Page Down)"));
    actLiftDown_->setShortcut(QKeySequence(Qt::Key_PageDown));
    connect(actLiftDown_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueLift(-25);
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

    viewMenu->addSeparator();

    // ------------------------------------------------------------------
    // Camera preset pair (exclusive): the main game's camera model vs the
    // editor camera model. The game variant needs the GPU terrain path —
    // the game frustum/FOV lives in TerrainGpuView.
    // ------------------------------------------------------------------
    QActionGroup *cameraGroup = new QActionGroup(this);
    actGameCamera_ = create();
    actGameCamera_->setText(tr("Camera Like in &Game"));
    actGameCamera_->setToolTip(
        tr("Switch the 3D view to the main game's camera: psi/theta/distance "
           "model with the game FOV (HardwareCameraFocus), aimed at the first "
           "player of the loaded world — the view a player has at battle "
           "start. The minimap outline follows the game frustum."));
    actGameCamera_->setCheckable(true);
    actEditorCamera_ = create();
    actEditorCamera_->setText(tr("Camera Like in &Editor"));
    actEditorCamera_->setToolTip(
        tr("Switch the 3D view back to the editor camera (rotation/tilt/lift/"
           "focus with its own zoom model)."));
    actEditorCamera_->setCheckable(true);
    actEditorCamera_->setChecked(true);
    cameraGroup->addAction(actGameCamera_);
    cameraGroup->addAction(actEditorCamera_);
    connect(actGameCamera_, &QAction::triggered, this,
            &MainWindow::onActionGameCamera);
    connect(actEditorCamera_, &QAction::triggered, this,
            &MainWindow::onActionEditorCamera);
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
    bar->addAction(actDimGeo_);
    // Editable grid cell size (map cells): the steps of the grid overlay in
    // the 2D lines and the 3D surface dots. Defaults to Terra's kmGrid (4).
    gridStepWSpin_ = new QSpinBox(bar);
    gridStepWSpin_->setRange(1, 1024);
    gridStepWSpin_->setValue(128);
    gridStepWSpin_->setSuffix(tr(" c"));
    gridStepWSpin_->setToolTip(
        tr("Grid cell width in map cells (the horizontal line spacing of "
           "the grid overlay, 2D and 3D)."));
    gridStepHSpin_ = new QSpinBox(bar);
    gridStepHSpin_->setRange(1, 1024);
    gridStepHSpin_->setValue(128);
    gridStepHSpin_->setSuffix(tr(" c"));
    gridStepHSpin_->setToolTip(
        tr("Grid cell height in map cells (the vertical line spacing of "
           "the grid overlay, 2D and 3D)."));
    const auto pushGridStep = [this](int) {
        if (viewport_) {
            viewport_->setGridStep(gridStepWSpin_->value(),
                                   gridStepHSpin_->value());
        }
    };
    connect(gridStepWSpin_, &QSpinBox::valueChanged, this, pushGridStep);
    connect(gridStepHSpin_, &QSpinBox::valueChanged, this, pushGridStep);
    bar->addWidget(gridStepWSpin_);
    bar->addWidget(gridStepHSpin_);
    bar->addAction(actZoomToFit_);
    bar->addSeparator();
    bar->addAction(actView3D_);
    bar->addAction(actFreezeDam_);
    bar->addAction(actChunkyRender_);
    bar->addAction(actSmoothRender_);
    bar->addSeparator();
    bar->addAction(actRotateLeft_);
    bar->addAction(actRotateRight_);
    // Absolute camera rotation (azimuth) of the 3D view, degrees — the same
    // knob the rotate buttons and the orbit drag move. Wraps across the
    // 360/0 edge like the buttons do; follows the active camera model (the
    // editor turnAngleDeg or the game camera psi, see ViewportWidget).
    camRotationSpin_ = new QDoubleSpinBox(bar);
    camRotationSpin_->setObjectName(QStringLiteral("camRotationSpin"));
    camRotationSpin_->setRange(0.0, 360.0);
    camRotationSpin_->setDecimals(1);
    camRotationSpin_->setSingleStep(5.0);
    camRotationSpin_->setWrapping(true);
    camRotationSpin_->setSuffix(tr("\u00B0"));
    camRotationSpin_->setToolTip(
        tr("3D camera rotation (azimuth) in degrees. Edits the same camera "
           "rotation as the rotate buttons and the Alt+left orbit drag; "
           "wraps 360\u00B0 \u2192 0\u00B0."));
    connect(camRotationSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) {
                if (viewport_) {
                    viewport_->setRotationDeg(value);
                }
            });
    bar->addWidget(camRotationSpin_);
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
    using Viewport = MapEditor::ViewportWidget;
    using EditorMode = Viewport::EditorMode;

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
            // T4.2 — keep the interaction mode in step with the brush type:
            // the picking type (eyedropper) drives the Eyedropper mode, every
            // painting type drives the Brush mode. The View mode stays as the
            // user left it (the type buttons themselves are Edit actions).
            if (viewport_) {
                viewport_->setEditorMode(
                    effect == MapEditor::BrushEffect::PickColor
                        ? MapEditor::ViewportWidget::EditorMode::Eyedropper
                        : MapEditor::ViewportWidget::EditorMode::Brush);
            }
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
        tr("The brush levels the terrain to the world zeroplast height in "
           "one stamp (strength has no effect on it) and marks the reached "
           "cells as zeroplast (Dam mode only — the zeroplast belongs to "
           "the dam domain)."),
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

    // T4.2 — the eyedropper as a brush type: the PickColor effect samples the
    // Geo/Dam pixel (SurBuf index) at the click cell into the color brush —
    // no buffer is written, the pick is a BrushTool point operation like any
    // other type (the interaction runs in the editor mode Eyedropper).
    actBrushPickColor_ = addType(
        makeEyedropperIcon(), tr("Pick Color"),
        tr("The brush samples the surface color under the cursor (the Geo/Dam "
           "palette pixel) into the color brush instead of painting. A click "
           "or a drag over the map in the 2D and the 3D view; use the editor "
           "mode \"Eyedropper\" (E) as the switch."),
        MapEditor::BrushEffect::PickColor, 0);

    // T3.4 — plot soot (burn marks on the terrain).
    actBrushSoot_ = addType(
        makeGroundLetterIcon(QStringLiteral("S"), QColor(80, 50, 20)),
        tr("Plot Soot"),
        tr("The brush marks cells as soot (burn residue). Zeroplast cells "
           "are never marked as soot — the game's rule."),
        MapEditor::BrushEffect::PlotSoot, 0);

    // T3.4 — very light dam: seed a thin dam layer at geo height.
    actBrushVeryLightDam_ = addType(
        makeGroundLetterIcon(QStringLiteral("V"), QColor(100, 140, 180)),
        tr("Very Light Dam"),
        tr("The brush seeds a very light dam layer at the geo height and "
           "paints the surface with the very light dam texture index. "
           "Equivalent to the game's initial dam deployment."),
         MapEditor::BrushEffect::VeryLightDam, 0);

    // T3.5 — load brush image from file. The loaded image becomes the stamp
    // mask of the CURRENT brush type (raise/lower/level/... — the image
    // pixel value is the per-cell influence) until a stamp shape is picked
    // explicitly.
    actLoadBrushImage_ = new QAction(makeLoadImageIcon(), tr("&Load Image..."),
                                      this);
    actLoadBrushImage_->setToolTip(
        tr("Load a TGA/PNG/BMP image as the brush stamp mask: the pixel "
           "brightness (0..255) controls the per-cell influence of the "
           "current brush type. Picking a stamp shape switches back to it."));
    connect(actLoadBrushImage_, &QAction::triggered,
            this, &MainWindow::onLoadBrushImage);

    // ImageDecal — a palette-limited image stamp. The button is both the brush
    // type and the image picker: selecting it switches to the decal brush,
    // forces the Dam layer (the decal colours come from the Dam palette) and
    // opens the file dialog. Cancelling keeps the previously loaded decal.
    actBrushImageDecal_ = addType(
        makeDecalIcon(), tr("Image Decal"),
        tr("Stamps a picture into the dam surface colours: the image is "
           "converted to a limited set of Dam palette colours (k-means), the "
           "new colours are added to the Dam palette and every pixel paints "
           "its palette index. Dam layer only; transparent pixels are never "
           "painted. Click to choose a PNG/TGA/BMP image (max 512x512)."),
        MapEditor::BrushEffect::ImageDecal, 0);
    connect(actBrushImageDecal_, &QAction::triggered, this, [this] {
        MapEditor::BrushParams params = brushTool_->params();
        params.effect = MapEditor::BrushEffect::ImageDecal;
        params.mode = MapEditor::BrushMode::Dam; // the decal owns the Dam layer
        params.useImageMask = false;
        brushTool_->setParams(params);
        if (viewport_) {
            viewport_->setEditorMode(
                MapEditor::ViewportWidget::EditorMode::Brush);
        }
        onLoadDecalImage();
        // Cancelled with no decal loaded: do not leave a brush that paints
        // nothing selected.
        if (!brushTool_->hasDecalImage()) {
            params.effect = MapEditor::BrushEffect::ChangeHeight;
            params.direction = 1;
            brushTool_->setParams(params);
        }
        syncBrushControls();
    });

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
            // The zeroplast brushes are a Dam-mode tool (the zeroplast
            // belongs to the dam domain) — the UI keeps them disabled in the
            // Geo mode, so a type left active by the switch falls back to a
            // plain raise.
            if (mode == MapEditor::BrushMode::Geo &&
                (params.effect == MapEditor::BrushEffect::Align4ZP ||
                 params.effect == MapEditor::BrushEffect::ImageDecal)) {
                params.effect = MapEditor::BrushEffect::ChangeHeight;
                params.direction = 1;
            }
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

    // --- Form: the stamp silhouette (T3.1 BrushShape) -----------------
    // T3.5 — picking a shape switches the stamp geometry back from a loaded
    // image mask to the generated shape mask (useImageMask = false).
    QActionGroup *shapeGroup = new QActionGroup(this);
    const auto addShape = [this, shapeGroup](MapEditor::BrushShape shape,
                                             const QString &text,
                                             const QString &toolTip) {
        QAction *action = new QAction(makeShapeIcon(shape), text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);
        action->setChecked(brushTool_->params().shape == shape);
        connect(action, &QAction::triggered, this, [this, shape] {
            MapEditor::BrushParams params = brushTool_->params();
            params.shape = shape;
            params.useImageMask = false;
            brushTool_->setParams(params);
            syncBrushControls();
        });
        shapeGroup->addAction(action);
        return action;
    };

    actShapeCircle_ = addShape(
        MapEditor::BrushShape::FilledCircle, tr("Circle"),
        tr("Filled circle stamp — the shape the game's terraformers influence. "
           "Radius is the disc half-size in map cells."));
    actShapeSquare_ = addShape(
        MapEditor::BrushShape::FilledSquare, tr("Square"),
        tr("Filled square stamp covering |dx| <= radius and |dy| <= radius."));
    actShapeCircleOutline_ = addShape(
        MapEditor::BrushShape::CircleOutline, tr("Circle Outline"),
        tr("One-cell ring exactly at the radius — draws circular borders."));
    actShapeSquareOutline_ = addShape(
        MapEditor::BrushShape::SquareOutline, tr("Square Outline"),
        tr("One-cell square border at the radius — draws rectangular borders."));
    actShapeLine_ = addShape(
        MapEditor::BrushShape::Line, tr("Line"),
        tr("Two-point stroke: press marks the start, release draws a "
           "Bresenham line stamped with the filled circle of the current "
           "radius — one undo entry per line. Works in both view modes."));

    // --- Editor interaction mode: View / Brush / Eyedropper -------------
    // What a click in the world does (T4.2), in both the 2D and the 3D view:
    //   View  — full camera control (left drag pans/orbits);
    //   Brush — the click applies the brush (camera: pan/zoom, Alt+left orbit
    //           in the 3D view);
    //   Eyedropper — the click samples the Geo/Dam pixel through the PickColor
    //           brush type (same limited camera as Brush).
    QActionGroup *interactionGroup = new QActionGroup(this);
    const auto addEditorMode = [this, interactionGroup](
            QIcon icon, const QString &text, const QString &toolTip,
            const QKeySequence &shortcut, EditorMode mode) {
        QAction *action = new QAction(icon, text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, [this, mode] {
            if (!viewport_) {
                return;
            }
            // The brush type carries which tool the interaction works with:
            // entering the Eyedropper mode selects the PickColor type, and
            // returning to the Brush mode turns it back into an actually
            // painting color brush (SetBrush params note in BrushTool.h).
            MapEditor::BrushParams params = brushTool_->params();
            if (mode == EditorMode::Eyedropper) {
                params.effect = MapEditor::BrushEffect::PickColor;
                brushTool_->setParams(params);
            } else if (mode == EditorMode::Brush
                       && params.effect == MapEditor::BrushEffect::PickColor) {
                params.effect = MapEditor::BrushEffect::SetColor;
                brushTool_->setParams(params);
            }
            viewport_->setEditorMode(mode);
            syncBrushControls();
        });
        interactionGroup->addAction(action);
        return action;
    };

    actModeView_ = addEditorMode(
        makeViewModeIcon(), tr("&View Mode"),
        tr("Navigate only: left drag pans the 2D view / orbits the 3D view, "
           "middle/right drag pans, wheel zooms (V). The brush does nothing."),
        QKeySequence(Qt::Key_V), EditorMode::View);
    actModeBrush_ = addEditorMode(
        makeBrushIcon(), tr("&Brush"),
        tr("Apply the brush with a left click/drag in the 2D and the 3D view "
           "(T). Camera changes stay limited: pan and zoom are always "
           "available, Alt+left still orbits in the 3D view."),
        QKeySequence(Qt::Key_T), EditorMode::Brush);
    actModeEyedropper_ = addEditorMode(
        makeEyedropperIcon(), tr("&Eyedropper"),
        tr("Sample the surface color instead of painting: a click/drag picks "
           "the Geo/Dam palette pixel under the cursor through the PickColor "
           "brush type (E). Same limited camera as the Brush mode."),
        QKeySequence(Qt::Key_E), EditorMode::Eyedropper);
    actModeBrush_->setChecked(true);
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

    // T4.2 — the editor interaction mode switch (View / Brush / Eyedropper);
    // picking the Eyedropper mode selects the PickColor brush type, picking
    // the Brush mode swaps it back to the painting color brush.
    bar->addAction(actModeView_);
    bar->addAction(actModeBrush_);
    bar->addAction(actModeEyedropper_);
    bar->addSeparator();

    // T3.1 form buttons: the stamp silhouette + the stamp half-size. The
    // silhouette is what the cursor preview shows and what a stamp paints.
    bar->addAction(actShapeCircle_);
    bar->addAction(actShapeSquare_);
    bar->addAction(actShapeCircleOutline_);
    bar->addAction(actShapeSquareOutline_);
    bar->addAction(actShapeLine_);

    // Stamp half-size in map cells (BrushParams::radius, clamped by the tool
    // to BrushTool::maxRadius() = 512).
    brushRadiusSpin_ = new QSpinBox(bar);
    brushRadiusSpin_->setRange(0, MapEditor::BrushTool::maxRadius());
    brushRadiusSpin_->setSuffix(tr(" c"));
    brushRadiusSpin_->setToolTip(
        tr("Brush radius in map cells (the stamp half-size)."));
    connect(brushRadiusSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.radius = value;
        brushTool_->setParams(params);
        syncBrushControls();
    });
    bar->addWidget(brushRadiusSpin_);

    // Stroke width in cells for the unfilled shapes (CircleOutline,
    // SquareOutline, Line). The band is inset from the outer edge, so it never
    // exceeds the stamp footprint. 0 = full stamp (the pre-thickness behaviour);
    // UI shows 1..maxThickness where 1 = original one-cell ring/border.
    brushThicknessSpin_ = new QSpinBox(bar);
    brushThicknessSpin_->setSuffix(tr(" c"));
    brushThicknessSpin_->setToolTip(
        tr("Stroke width in cells of the unfilled shapes (CircleOutline, "
           "SquareOutline, Line). The band grows inward from the outer edge, "
           "so a thicker stroke never edits outside the brush footprint. "
           "Ignored by the filled shapes. 0 = full stamp."));
    connect(brushThicknessSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.thickness = value;
        brushTool_->setParams(params);
        syncBrushControls();
    });
    bar->addWidget(brushThicknessSpin_);
    bar->addSeparator();

    // T3.2 type buttons: raise / lower / the two levellers.
    bar->addAction(actBrushRaise_);
    bar->addAction(actBrushLower_);
    bar->addAction(actBrushLevelZP_);
    bar->addAction(actBrushLevelHeight_);

    // The height value of the height tools — whole voxels (0..255). One
    // control, meaning per type: the raise/lower step (per stamp at full
    // influence) for Raise/Lower Terrain, the target height for "Level to
    // Height" (the domain BrushTool::clampHeightLevel enforces too).
    brushLevelSpin_ = new QSpinBox(bar);
    brushLevelSpin_->setRange(0, 255);
    brushLevelSpin_->setSuffix(tr(" vox"));
    connect(brushLevelSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        if (params.effect == MapEditor::BrushEffect::AlignVariableH) {
            params.heightLevel = value;
        } else {
            params.strength = MapEditor::BrushTool::clampStrength(value);
        }
        brushTool_->setParams(params);
    });
    bar->addWidget(brushLevelSpin_);

    bar->addSeparator();

    // T3.2 mode buttons: Geo / Dam.
    bar->addAction(actBrushGeo_);
    bar->addAction(actBrushDam_);

    bar->addSeparator();

    // T3.3/T4.3 — color palette index spinbox (0..255): the index into the
    // palette of the brush's current layer (T4.1 model).
    brushColorIndexSpin_ = new QSpinBox(bar);
    brushColorIndexSpin_->setRange(0, 255);
    brushColorIndexSpin_->setToolTip(
        tr("Palette index for the \"Paint Color\" brush — an entry of the "
           "palette of the brush's current layer (Geo or Dam)."));
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

    // T3.3/T4.2 — the paint/pick color types next to their color controls
    // (the same actions the Edit menu carries).
    bar->addAction(actBrushColor_);
    bar->addAction(actBrushPickColor_);

    // ImageDecal — the palette-limited image stamp (Dam layer only).
    bar->addAction(actBrushImageDecal_);

    bar->addSeparator();

    // T3.4 — soot/very-light-dam buttons.
    bar->addAction(actBrushSoot_);
    bar->addAction(actBrushVeryLightDam_);

    bar->addSeparator();

    // T3.5 — load-image button: the image becomes the stamp mask of the
    // current brush type.
    bar->addAction(actLoadBrushImage_);

    // T3.5 — the image mask scale (percent of the loaded image size). The
    // scaled mask is recomputed from the stored original on every change.
    brushMaskScaleSpin_ = new QSpinBox(bar);
    brushMaskScaleSpin_->setRange(10, 800);
    brushMaskScaleSpin_->setValue(100);
    brushMaskScaleSpin_->setSuffix(tr(" %"));
    brushMaskScaleSpin_->setToolTip(
        tr("Scale of the loaded brush image relative to its original pixel "
           "size (the mask stays centred under the cursor)."));
    connect(brushMaskScaleSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!brushTool_) {
            return;
        }
        const double factor = value / 100.0;
        // The same control scales whichever image stamp is active: the
        // influence mask of the terrain tools (T3.5) or the palette-index
        // decal image (ImageDecal). Both are recomputed from the un-scaled
        // original, so repeated changes never accumulate blur.
        if (brushTool_->params().effect == MapEditor::BrushEffect::ImageDecal) {
            if (!decalOriginal_.empty()) {
                brushTool_->setDecalImage(
                    MapEditor::ImageDecal::scaleNearest(decalOriginal_, factor));
            }
        } else if (!brushMaskOriginal_.empty()) {
            brushTool_->setImageMask(scaleBrushMask(brushMaskOriginal_, factor));
        }
        syncBrushControls();
    });
    bar->addWidget(brushMaskScaleSpin_);

    syncBrushControls();
    syncColorControls();
}

void MainWindow::syncBrushControls()
{
    const MapEditor::BrushParams params = brushTool_->params();

    // T4.2 — brush type buttons: the exclusive action group only auto-checks
    // user-triggered actions, so every programmatic effect change (eyedropper
    // pick, Eyedropper/Brush mode switch, ...) mirrors into the buttons here.
    const auto syncType = [params](QAction *action, bool active) {
        if (action) {
            action->setChecked(active);
        }
    };
    syncType(actBrushRaise_, params.effect == MapEditor::BrushEffect::ChangeHeight
                                 && params.direction > 0);
    syncType(actBrushLower_, params.effect == MapEditor::BrushEffect::ChangeHeight
                                 && params.direction < 0);
    syncType(actBrushLevelZP_, params.effect == MapEditor::BrushEffect::Align4ZP);
    syncType(actBrushLevelHeight_,
             params.effect == MapEditor::BrushEffect::AlignVariableH);
    syncType(actBrushColor_, params.effect == MapEditor::BrushEffect::SetColor);
    syncType(actBrushImageDecal_,
             params.effect == MapEditor::BrushEffect::ImageDecal);
    syncType(actBrushPickColor_,
             params.effect == MapEditor::BrushEffect::PickColor);
    syncType(actBrushSoot_, params.effect == MapEditor::BrushEffect::PlotSoot);
    syncType(actBrushVeryLightDam_,
             params.effect == MapEditor::BrushEffect::VeryLightDam);

    if (brushLevelSpin_) {
        // One height control for the height tools: the raise/lower step for
        // ChangeHeight, the target height for AlignVariableH (every other
        // type does not consume it — the value display keeps the last
        // choice of the height tools).
        const int shown = params.effect == MapEditor::BrushEffect::AlignVariableH
            ? params.heightLevel
            : params.strength;
        brushLevelSpin_->setToolTip(
            params.effect == MapEditor::BrushEffect::AlignVariableH
                ? tr("Target height of the \"Level to Height\" brush "
                     "(whole voxels; 1 voxel = 1/32 m).")
                : tr("Height change per stamp of the Raise/Lower Terrain "
                     "brush, at full mask influence (whole voxels; "
                     "1 voxel = 1/32 m)."));
        if (brushLevelSpin_->value() != shown) {
            brushLevelSpin_->blockSignals(true);
            brushLevelSpin_->setValue(shown);
            brushLevelSpin_->blockSignals(false);
        }
    }

    // The brush mode buttons are mirrored too: some brush types force a layer
    // (the zeroplast brushes and the ImageDecal decal are Dam-only), so a
    // programmatic mode change has to reach the exclusive group.
    if (actBrushGeo_) {
        actBrushGeo_->setChecked(params.mode == MapEditor::BrushMode::Geo);
    }
    if (actBrushDam_) {
        actBrushDam_->setChecked(params.mode == MapEditor::BrushMode::Dam);
    }

    // T3.1 — the stamp form drives the cursor preview silhouette; T3.5 — a
    // loaded image mask replaces the shapes (no shape button is checked
    // then); ImageDecal has its own rectangular geometry; T4.2 — the
    // eyedropper is a point operation, the radius then does not matter.
    if (brushRadiusSpin_) {
        if (brushRadiusSpin_->value() != params.radius) {
            brushRadiusSpin_->blockSignals(true);
            brushRadiusSpin_->setValue(params.radius);
            brushRadiusSpin_->blockSignals(false);
        }
    }

    // Thickness: only enabled for the unfilled shapes; the range is the stamp's
    // own extent (2*radius+1 cells) so a thicker band never leaves the stamp.
    // Internal 0 = "auto" (full stamp); UI maps 0 to 1 for outlines and to
    // maxThickness for the Line shape.
    const bool thicknessEnabled =
        MapEditor::BrushMasks::isUnfilledShape(params.shape) &&
        !params.useImageMask &&
        params.effect != MapEditor::BrushEffect::ImageDecal &&
        params.effect != MapEditor::BrushEffect::PickColor;
    if (brushThicknessSpin_) {
        const int maxThick = MapEditor::BrushTool::maxThickness(params.radius);
        brushThicknessSpin_->setRange(1, maxThick);
        brushThicknessSpin_->setEnabled(thicknessEnabled);
        // Map the internal 0 ("auto") to the shape's natural default.
        int shownThick = params.thickness;
        if (shownThick <= 0) {
            shownThick = params.shape == MapEditor::BrushShape::Line ? maxThick : 1;
        }
        shownThick = std::clamp(shownThick, 1, maxThick);
        if (brushThicknessSpin_->value() != shownThick) {
            brushThicknessSpin_->blockSignals(true);
            brushThicknessSpin_->setValue(shownThick);
            brushThicknessSpin_->blockSignals(false);
        }
    }
    const bool imageMaskActive = params.useImageMask && brushTool_->hasImageMask();
    const bool decalActive = params.effect == MapEditor::BrushEffect::ImageDecal &&
                             brushTool_->hasDecalImage();
    if (actShapeCircle_) actShapeCircle_->setChecked(!imageMaskActive && !decalActive &&
        params.shape == MapEditor::BrushShape::FilledCircle);
    if (actShapeSquare_) actShapeSquare_->setChecked(!imageMaskActive && !decalActive &&
        params.shape == MapEditor::BrushShape::FilledSquare);
    if (actShapeCircleOutline_) actShapeCircleOutline_->setChecked(!imageMaskActive && !decalActive &&
        params.shape == MapEditor::BrushShape::CircleOutline);
    if (actShapeSquareOutline_) actShapeSquareOutline_->setChecked(!imageMaskActive && !decalActive &&
        params.shape == MapEditor::BrushShape::SquareOutline);
    if (actShapeLine_) actShapeLine_->setChecked(!imageMaskActive && !decalActive &&
        params.shape == MapEditor::BrushShape::Line);

    // T3.3/T4.2 — the color index spinbox paints (SetColor) and displays the
    // last sampled pixel (PickColor).
    if (brushColorIndexSpin_) {
        if (brushColorIndexSpin_->value() != params.colorIndex) {
            brushColorIndexSpin_->blockSignals(true);
            brushColorIndexSpin_->setValue(params.colorIndex);
            brushColorIndexSpin_->blockSignals(false);
        }
    }

    // Mirror the tool into the viewport cursor preview: the square silhouette
    // for the square forms and for the image mask (a rectangle), the circle
    // otherwise; the radius is the mask half-size for the image mask. The
    // preview only follows the brush — the View/Eyedropper modes hide it in
    // the viewport (drawBrushPreview) anyway.
    if (viewport_) {
        const bool squarePreview = imageMaskActive || decalActive ||
            params.shape == MapEditor::BrushShape::FilledSquare ||
            params.shape == MapEditor::BrushShape::SquareOutline;
        int previewRadius = params.radius;
        if (imageMaskActive) {
            previewRadius = std::max(brushTool_->imageMask().sx,
                                     brushTool_->imageMask().sy) / 2;
        } else if (decalActive) {
            previewRadius = std::max(brushTool_->decalImage().width,
                                     brushTool_->decalImage().height) / 2;
        }
        // Inner ring of the band for the outline shapes; 0 = no inner ring.
        // thickness 0 ("auto") → the original one-cell ring, so no inner ring;
        // the Line shape's stroke is centred on the path, not a concentric
        // band, so its preview keeps the outer footprint only.
        int previewInner = 0;
        if (!imageMaskActive && !decalActive && params.thickness > 0 &&
            (params.shape == MapEditor::BrushShape::CircleOutline ||
             params.shape == MapEditor::BrushShape::SquareOutline)) {
            previewInner = std::max(0, params.radius - params.thickness + 1);
        }
        viewport_->setBrushPreview(
            squarePreview ? MapEditor::BrushPreviewShape::Square
                          : MapEditor::BrushPreviewShape::Circle,
            previewRadius, previewInner);
    }

    // Enabled/disabled states depend on the effect AND on the session state
    // (a world must be loaded) — updateActionStates() is the single source
    // of truth for both.
    updateActionStates();

    // The palette panel highlight follows the brush: the entry the brush
    // paints with, in the grid of the brush's current layer. (selectIndex
    // only restyles swatches — no feedback loop into the brush.)
    if (palettePanel_) {
        palettePanel_->selectIndex(
            params.colorIndex, params.mode == MapEditor::BrushMode::Dam);
    }

    syncColorControls();
}

void MainWindow::syncColorControls()
{
    if (!brushTool_ || !brushColorIndexSpin_) {
        return;
    }

    // Build the color swatch from the palette via the session helper.
    // The colorIndex is a SurBuf value: it indexes the palette of the
    // brush's current layer (T4.1 model, docs/architecture.md §5.13).
    if (!session_ || !session_->isWorldLoaded()) {
        // No palette to resolve the index through — drop any color left
        // over from a previously loaded world instead of keeping it stale.
        brushColorSwatch_->setStyleSheet(
            QStringLiteral("QLabel { border: 1px solid #808080; }"));
        return;
    }
    {
        const int idx = brushColorIndexSpin_->value();
        const bool dam = brushTool_->params().mode == MapEditor::BrushMode::Dam;
        const unsigned int rgb =
            session_->paletteRgb(dam ? MapEditor::EditorSession::PaletteLayer::Dam
                                     : MapEditor::EditorSession::PaletteLayer::Geo,
                                 idx);
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

void MainWindow::refreshPaletteRenderers()
{
    if (!viewport_) {
        return;
    }
    // Every pixel's color may have changed: the 2D cache is repainted as a
    // whole, and the 3D view re-seeds its mip pyramid from the same cache
    // (which invalidateAll() starts by marking the map fully dirty).
    if (MapEditor::ViewportRenderer *renderer = viewport_->renderer()) {
        renderer->invalidateAll();
    }
    viewport_->setNeeds3DRedraw();
    viewport_->update();
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

void MainWindow::onActionNewWorld()
{
    if (!session_->hasGameDir() || session_->worldCount() == 0) {
        QMessageBox::warning(this, tr("New World"),
                             tr("Select a game directory with at least one "
                                "world first (it donates the palettes and "
                                "textures for the new world)."));
        return;
    }
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    NewWorldWizard wizard(this, session_->getWorldList(),
                          session_->getGameDir());
    if (wizard.exec() != QDialog::Accepted) {
        return;
    }
    const MapEditor::WorldCreateResult result = MapEditor::createNewWorld(
        *session_, wizard.wizardParams(),
        wizard.templateWorldName().toStdString());
    if (!result.ok) {
        QMessageBox::critical(this, tr("New World"),
                              tr("Failed to create the world:\n%1")
                                  .arg(QString::fromStdString(result.error)));
        return;
    }
    // createNewWorld loaded and saved the new world — rebind the viewport.
    onWorldLoaded();
}

void MainWindow::onActionWorldParams()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const std::optional<MapEditor::WorldParams> current =
        session_->currentWorldParams();
    if (!current) {
        QMessageBox::warning(this, tr("World Parameters"),
                             tr("Cannot read the parameters of the loaded world."));
        return;
    }
    WorldSettingsDialog dialog(this, WorldSettingsPanel::Mode::ExistingWorld,
                               *current);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (!session_->applyWorldSettings(dialog.params())) {
        QMessageBox::warning(this, tr("World Parameters"),
                             tr("Some parameters could not be written "
                                "(see the log for details)."));
    }
    // hZeroPlast drives the 3D grid reference plane; the 2D raster is
    // unaffected (palette colors did not change).
    if (viewport_) {
        viewport_->setNeeds3DRedraw();
        viewport_->update();
    }
    updateStatusBar();
}

// ---------------------------------------------------------------------------
// T6 — image import/export.
// ---------------------------------------------------------------------------

void MainWindow::onActionImportMapImage()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Image onto Map"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tga);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    // Options: which layers take the paint.
    QDialog optionsDialog(this);
    optionsDialog.setWindowTitle(tr("Import Options"));
    auto *form = new QFormLayout(&optionsDialog);
    auto *geoBox = new QCheckBox(tr("Paint geo cells (no dam)"), &optionsDialog);
    auto *damBox = new QCheckBox(tr("Paint dam cells"), &optionsDialog);
    geoBox->setChecked(true);
    damBox->setChecked(true);
    form->addRow(geoBox);
    form->addRow(damBox);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel,
                                         &optionsDialog);
    connect(buttons, &QDialogButtonBox::accepted, &optionsDialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &optionsDialog, &QDialog::reject);
    form->addRow(buttons);
    if (optionsDialog.exec() != QDialog::Accepted) {
        return;
    }
    MapEditor::EditorSession::SurfaceImportOptions options;
    options.geoCells = geoBox->isChecked();
    options.damCells = damBox->isChecked();
    if (!options.geoCells && !options.damCells) {
        return;
    }

    bool ok = false;
    // Indexed TGA: the indices go into SurBuf as-is (any size — the core
    // samples nearest). Everything else: RGB -> nearest palette entry.
    if (path.endsWith(QStringLiteral(".tga"), Qt::CaseInsensitive)) {
        unsigned width = 0;
        unsigned height = 0;
        std::vector<unsigned char> indices;
        std::vector<unsigned char> palette;
        if (MapEditor::WorldImages::readIndexedTga(
                path.toStdWString(), width, height, indices, palette)) {
            ok = session_->importSurfaceImageIndexed(width, height, indices,
                                                     options);
        }
    }
    if (!ok) {
        QImage image(path);
        if (image.isNull()) {
            QMessageBox::warning(this, tr("Import Image onto Map"),
                                 tr("Cannot read the image file."));
            return;
        }
        image = image.convertToFormat(QImage::Format_RGB888)
                    .scaled(static_cast<int>(session_->mapWidth()),
                            static_cast<int>(session_->mapHeight()),
                            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const int w = image.width();
        const int h = image.height();
        std::vector<unsigned char> bgr(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            const uchar *line = image.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                // RGB888 -> BGR (the game/TGA byte order).
                bgr[(static_cast<size_t>(y) * w + x) * 3 + 0] = line[x * 3 + 2];
                bgr[(static_cast<size_t>(y) * w + x) * 3 + 1] = line[x * 3 + 1];
                bgr[(static_cast<size_t>(y) * w + x) * 3 + 2] = line[x * 3 + 0];
            }
        }
        ok = session_->importSurfaceImageRgb(static_cast<unsigned>(w),
                                             static_cast<unsigned>(h), bgr,
                                             options);
    }
    if (!ok) {
        QMessageBox::warning(this, tr("Import Image onto Map"),
                             tr("The image could not be applied to the map."));
        return;
    }
    if (viewport_) {
        viewport_->setNeeds3DRedraw();
        viewport_->update();
    }
    updateStatusBar();
}

void MainWindow::onActionImportUpTga()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import up.tga (Cover Image)"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tga);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QImage image(path);
    if (image.isNull()) {
        QMessageBox::warning(this, tr("Import up.tga"),
                             tr("Cannot read the image file."));
        return;
    }
    image = image.convertToFormat(QImage::Format_RGB888)
                .scaled(static_cast<int>(MapEditor::WorldImages::kUpTgaSize),
                        static_cast<int>(MapEditor::WorldImages::kUpTgaSize),
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<unsigned char> bgr(static_cast<size_t>(
        MapEditor::WorldImages::kUpTgaSize *
        MapEditor::WorldImages::kUpTgaSize * 3));
    for (int y = 0; y < image.height(); ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            bgr[(static_cast<size_t>(y) * image.width() + x) * 3 + 0] =
                line[x * 3 + 2];
            bgr[(static_cast<size_t>(y) * image.width() + x) * 3 + 1] =
                line[x * 3 + 1];
            bgr[(static_cast<size_t>(y) * image.width() + x) * 3 + 2] =
                line[x * 3 + 0];
        }
    }
    const auto files = session_->currentWorldFiles();
    if (!files || !MapEditor::WorldImages::writeUpTga(files->worldDir(), bgr)) {
        QMessageBox::warning(this, tr("Import up.tga"),
                             tr("Failed to write up.tga."));
        return;
    }
    statusBar()->showMessage(tr("up.tga imported (512x512, 24-bit)."), 5000);
}

void MainWindow::onActionImportLeveledTexture()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import leveledSurfaceTexture.tga"), QString(),
        tr("TGA images (*.tga);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import leveledSurfaceTexture"),
                             tr("Cannot read the file."));
        return;
    }
    const QByteArray bytes = file.readAll();
    const auto files = session_->currentWorldFiles();
    if (!files) {
        return;
    }
    const std::vector<std::string> errors =
        MapEditor::WorldFiles::validateLeveledTexture(
            std::vector<unsigned char>(bytes.constData(),
                                       bytes.constData() + bytes.size()),
            static_cast<int>(session_->mapWidth()),
            static_cast<int>(session_->mapHeight()));
    if (!errors.empty()) {
        QString message = tr("The texture is not valid:\n");
        for (const std::string &error : errors) {
            message += QStringLiteral("\n- ") + QString::fromStdString(error);
        }
        QMessageBox::warning(this, tr("Import leveledSurfaceTexture"), message);
        return;
    }
    if (!files->writeLeveledTexture(
            std::vector<unsigned char>(bytes.constData(),
                                       bytes.constData() + bytes.size()),
            static_cast<int>(session_->mapWidth()),
            static_cast<int>(session_->mapHeight()))) {
        QMessageBox::warning(this, tr("Import leveledSurfaceTexture"),
                             tr("Failed to write the texture."));
        return;
    }
    // Reload the texture into the running world so the viewport shows it.
    if (session_->reloadLeveledTextureLive()) {
        statusBar()->showMessage(
            tr("leveledSurfaceTexture.tga imported and reloaded."), 5000);
    } else {
        statusBar()->showMessage(
            tr("leveledSurfaceTexture.tga imported (reload the world to see "
               "it)."), 5000);
    }
    if (viewport_) {
        viewport_->setNeeds3DRedraw();
        viewport_->update();
    }
}

void MainWindow::onActionExportHeightmap()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Heightmap"), QStringLiteral("heightmap.tga"),
        tr("TGA images (*.tga)"));
    if (path.isEmpty()) {
        return;
    }
    if (MapEditor::WorldImages::exportHeightmapTga(path.toStdWString())) {
        statusBar()->showMessage(tr("Heightmap exported to %1.").arg(path), 5000);
    } else {
        QMessageBox::warning(this, tr("Export Heightmap"),
                             tr("Failed to write the heightmap."));
    }
}

void MainWindow::onActionExportTopView()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Top View"), QStringLiteral("topview.tga"),
        tr("TGA images (*.tga)"));
    if (path.isEmpty()) {
        return;
    }
    if (MapEditor::WorldImages::exportTopViewTga(path.toStdWString())) {
        statusBar()->showMessage(tr("Top view exported to %1.").arg(path), 5000);
    } else {
        QMessageBox::warning(this, tr("Export Top View"),
                             tr("Failed to write the top view."));
    }
}

// ---------------------------------------------------------------------------
// T9 — validation + autofixes.
// ---------------------------------------------------------------------------

void MainWindow::buildToolsMenu()
{
    QMenu *toolsMenu = menuBar()->addMenu(tr("&Tools"));

    QAction *actValidateWorld = toolsMenu->addAction(tr("&Validate World..."));
    actValidateWorld->setToolTip(tr("Files, sizes, palettes, textures and the volume balance of the loaded world"));
    actValidateWorld->setEnabled(false);
    connect(actValidateWorld, &QAction::triggered, this, &MainWindow::onActionValidateWorld);
    actValidateWorld_ = actValidateWorld;

    QAction *actValidateMap = toolsMenu->addAction(tr("Validate &Battle Map..."));
    actValidateMap->setToolTip(tr("World reference, spawn bounds, cameras, shuffling indices of the loaded battle map"));
    actValidateMap->setEnabled(false);
    connect(actValidateMap, &QAction::triggered, this, &MainWindow::onActionValidateBattleMap);
    actValidateBattleMap_ = actValidateMap;

    toolsMenu->addSeparator();

    QAction *actFixShuffling = toolsMenu->addAction(tr("Autofix: &Normalize Shuffling Indices"));
    actFixShuffling->setEnabled(false);
    connect(actFixShuffling, &QAction::triggered, this, &MainWindow::onActionAutofixShuffling);
    actFixShuffling_ = actFixShuffling;

    QAction *actFixZP = toolsMenu->addAction(tr("Autofix: Set zeroLayerHeight from &World"));
    actFixZP->setEnabled(false);
    connect(actFixZP, &QAction::triggered, this, &MainWindow::onActionAutofixZeroLayerHeight);
    actFixZeroLayer_ = actFixZP;

    QAction *actRegenImages = toolsMenu->addAction(tr("Autofix: &Regenerate cache.tga / map.tga"));
    actRegenImages->setEnabled(false);
    connect(actRegenImages, &QAction::triggered, this, &MainWindow::onActionRegenerateImages);
    actRegenerateImages_ = actRegenImages;
}

void MainWindow::showValidationReport(const QString &title,
                                      const MapEditor::ValidationReport &report)
{
    MapEditor::ValidationDialog dialog(this, title, report);
    if (dialog.exec() == QDialog::Accepted) {
        int x = -1;
        int y = -1;
        dialog.currentIssueCell(x, y);
        if (x >= 0 && y >= 0 && viewport_) {
            viewport_->centerOnCell(QPoint(x, y));
            viewport_->update();
        }
    }
}

void MainWindow::onActionValidateWorld()
{
    const auto files = session_ ? session_->currentWorldFiles() : std::nullopt;
    if (!files || !session_->isWorldLoaded()) {
        return;
    }
    MapEditor::ValidationReport report =
        MapEditor::validateWorldFiles(*files);
    long long fill = 0;
    long long dig = 0;
    report.issues.push_back(
        MapEditor::validateVolumeBalance(MapEditor::VolumeBalanceOptions{},
                                         fill, dig));
    showValidationReport(
        tr("World Validation: %1")
            .arg(QString::fromStdString(session_->currentWorldName())),
        report);
}

void MainWindow::onActionValidateBattleMap()
{
    if (!battleMap_.loaded() || !session_ || !session_->hasGameDir()) {
        return;
    }
    const fs::path worldsRoot =
        fs::u8path(session_->getGameDir()) / "RESOURCE" / "Worlds";
    showValidationReport(
        tr("Battle Map Validation"),
        MapEditor::validateBattleMap(battleMap_, worldsRoot));
}

void MainWindow::onActionAutofixShuffling()
{
    if (!battleMap_.loaded()) {
        return;
    }
    if (MapEditor::autofixShufflingIndices(battleMap_)) {
        refreshBattleMapUI();
        statusBar()->showMessage(
            tr("playersShufflingIndices normalized."), 5000);
    }
}

void MainWindow::onActionAutofixZeroLayerHeight()
{
    if (!battleMap_.loaded() || !session_->isWorldLoaded()) {
        return;
    }
    if (MapEditor::autofixZeroLayerHeight(battleMap_,
                                          session_->zeroLayerHeight())) {
        refreshBattleMapUI();
        statusBar()->showMessage(
            tr("zeroLayerHeight set to the world hZeroPlast (%1).")
                .arg(session_->zeroLayerHeight()),
            5000);
    }
}

void MainWindow::onActionRegenerateImages()
{
    const auto files = session_ ? session_->currentWorldFiles() : std::nullopt;
    if (!files || !session_->isWorldLoaded()) {
        return;
    }
    const bool ok = MapEditor::WorldImages::generateCacheTga(
                        files->worldDir()) &&
                    MapEditor::WorldImages::generateMapTga(
                        files->worldDir());
    statusBar()->showMessage(
        ok ? tr("cache.tga / map.tga regenerated.")
           : tr("Image regeneration failed."),
        5000);
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

void MainWindow::buildHelpMenu()
{
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *actHelp = helpMenu->addAction(tr("Map Editor &Help..."));
    actHelp->setToolTip(tr("The editor manual (create a skirmish map, brushes, view, images...)"));
    actHelp->setShortcut(QKeySequence(Qt::Key_F1));
    connect(actHelp, &QAction::triggered, this, &MainWindow::onActionHelp);
}

void MainWindow::onActionHelp()
{
    // Non-modal: the manual stays open while the editor is being used.
    MapEditor::HelpWindow::openHelp(this);
}

void MainWindow::onActionUndo()
{
    if (session_->undo()) {
        // regRender() marked the affected area; a repaint drains the dirty
        // rectangles and refreshes the raster.
        if (viewport_) {
            viewport_->setNeeds3DRedraw();
            viewport_->update();
        }
        updateStatusBar();
    }
}

void MainWindow::onActionRedo()
{
    if (session_->redo()) {
        if (viewport_) {
            viewport_->setNeeds3DRedraw();
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

void MainWindow::onLoadBrushImage()
{
    const QString filter = tr("Images (*.png *.tga *.bmp *.jpg *.jpeg);;All Files (*)");
    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("Load Brush Image"), QString(), filter);
    if (filePath.isEmpty()) {
        return;
    }

    // Load via QImageReader for explicit format control.
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Image Load Failed"),
            tr("Cannot load image '%1':\n%2")
                .arg(filePath, reader.errorString()));
        return;
    }

    // Convert to brush mask.
    const MapEditor::BrushMask mask = imageToBrushMask(img);
    if (mask.empty()) {
        QMessageBox::warning(this, tr("Empty Image"),
            tr("The loaded image produced an empty brush mask."));
        return;
    }

    // Keep the un-scaled original: the scale control recomputes the mask
    // from it, so repeated scale changes never blur the source.
    brushMaskOriginal_ = mask;
    brushTool_->setImageMask(mask);
    if (brushMaskScaleSpin_) {
        brushMaskScaleSpin_->blockSignals(true);
        brushMaskScaleSpin_->setValue(100);
        brushMaskScaleSpin_->blockSignals(false);
    }

    // The image becomes the stamp mask of the current brush type (T3.5
    // rework): raise/lower/level/... keep working, only the stamp geometry
    // switches from the shape to the image. The eyedropper is a point
    // operation — fall back to a raise so the mask has an immediate effect,
    // and make sure the editor mode actually paints (a Load Image from the
    // View mode would otherwise do nothing on click).
    MapEditor::BrushParams params = brushTool_->params();
    params.useImageMask = true;
    if (params.effect == MapEditor::BrushEffect::PickColor ||
        params.effect == MapEditor::BrushEffect::None) {
        params.effect = MapEditor::BrushEffect::ChangeHeight;
        params.direction = 1;
    }
    brushTool_->setParams(params);
    if (viewport_) {
        viewport_->setEditorMode(MapEditor::ViewportWidget::EditorMode::Brush);
    }
    syncBrushControls();

    statusBar()->showMessage(
        tr("Brush image loaded: %1 (%2x%3)")
            .arg(QFileInfo(filePath).fileName())
            .arg(mask.sx)
            .arg(mask.sy),
        5000);
}

void MainWindow::onLoadDecalImage()
{
    if (!session_ || !session_->isWorldLoaded() || !brushTool_) {
        return;
    }

    const QString filter =
        tr("Images (*.png *.tga *.bmp);;All Files (*)");
    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("Load Image Decal"), QString(), filter);
    if (filePath.isEmpty()) {
        return;
    }

    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Image Load Failed"),
            tr("Cannot load image '%1':\n%2")
                .arg(filePath, reader.errorString()));
        return;
    }

    // The decal is a brush stamp, not a map import: a huge image would make
    // one click repaint a large part of the map, so the size is capped.
    if (img.width() > MapEditor::ImageDecal::kMaxImageSize ||
        img.height() > MapEditor::ImageDecal::kMaxImageSize) {
        QMessageBox::warning(this, tr("Image Too Large"),
            tr("The image is too large (%1x%2 px).\n"
               "The maximum size of an image is %3 px on each side.")
                .arg(img.width())
                .arg(img.height())
                .arg(MapEditor::ImageDecal::kMaxImageSize));
        return;
    }

    // Exact, un-premultiplied RGBA: the conversion matches the colours
    // against the palette and skips anything that is not fully opaque.
    const QImage rgbaImage = img.convertToFormat(QImage::Format_ARGB32);
    const int width = rgbaImage.width();
    const int height = rgbaImage.height();
    std::vector<unsigned char> rgba(
        static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        const QRgb *line =
            reinterpret_cast<const QRgb *>(rgbaImage.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const std::size_t base =
                (static_cast<std::size_t>(y) * width + x) * 4;
            rgba[base + 0] = static_cast<unsigned char>(qRed(line[x]));
            rgba[base + 1] = static_cast<unsigned char>(qGreen(line[x]));
            rgba[base + 2] = static_cast<unsigned char>(qBlue(line[x]));
            rgba[base + 3] = static_cast<unsigned char>(qAlpha(line[x]));
        }
    }

    const std::vector<unsigned char> palette =
        session_->paletteBytes(MapEditor::EditorSession::PaletteLayer::Dam);
    const std::vector<unsigned char> usage = session_->damPaletteUsage();
    const MapEditor::ImageDecal::PrepareResult prepared =
        MapEditor::ImageDecal::prepare(rgba.data(), width, height,
                                       palette.data(), usage.data());
    if (!prepared.ok) {
        QMessageBox::warning(this, tr("Image Decal"),
                             QString::fromStdString(prepared.error));
        return;
    }

    // Step 5 — the new colours go into the Dam palette and every palette view
    // (panel, renderer) is refreshed with them.
    for (const MapEditor::ImageDecal::PaletteWrite &write :
         prepared.paletteWrites) {
        session_->setPaletteRgb(MapEditor::EditorSession::PaletteLayer::Dam,
                                write.index, write.rgb);
    }
    session_->applyPalettes();
    if (palettePanel_) {
        palettePanel_->refresh();
    }
    refreshPaletteRenderers();

    // Step 8 — the decal becomes the stamp of the ImageDecal brush.
    decalOriginal_ = prepared.image;
    brushTool_->setDecalImage(prepared.image);
    if (brushMaskScaleSpin_) {
        brushMaskScaleSpin_->blockSignals(true);
        brushMaskScaleSpin_->setValue(100);
        brushMaskScaleSpin_->blockSignals(false);
    }

    MapEditor::BrushParams params = brushTool_->params();
    params.effect = MapEditor::BrushEffect::ImageDecal;
    params.mode = MapEditor::BrushMode::Dam;
    params.useImageMask = false;
    brushTool_->setParams(params);
    if (viewport_) {
        viewport_->setEditorMode(MapEditor::ViewportWidget::EditorMode::Brush);
    }
    syncBrushControls();

    statusBar()->showMessage(
        tr("Image decal ready: %1x%2 px, %3 palette colour(s), %4 new, "
           "%5 free Dam entr(y/ies).")
            .arg(width)
            .arg(height)
            .arg(prepared.resultColors)
            .arg(prepared.paletteWrites.size())
            .arg(prepared.unusedDamColors -
                 static_cast<int>(prepared.paletteWrites.size())),
        8000);
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
    if (actWorldParams_) {
        actWorldParams_->setEnabled(worldLoaded);
    }
    // T8/T9 — battle map and validation actions: opening a battle map needs
    // a game directory only (its world may be loaded later or not at all);
    // the battle-map tools need a loaded .spg, the world-dependent ones both.
    const bool gameDirSet = session_ != nullptr && session_->hasGameDir();
    const bool battleMapLoaded = battleMap_.loaded();
    if (actOpenBattleMap_) {
        actOpenBattleMap_->setEnabled(gameDirSet);
    }
    if (actSaveBattleMap_) {
        actSaveBattleMap_->setEnabled(battleMapLoaded &&
                                      !battleMapPath_.isEmpty());
    }
    if (actSaveBattleMapAs_) {
        actSaveBattleMapAs_->setEnabled(battleMapLoaded);
    }
    if (actNewBattleMap_) {
        // Creating a battle map builds it for the currently loaded world.
        actNewBattleMap_->setEnabled(gameDirSet && worldLoaded);
    }
    if (actValidateBattleMap_) {
        actValidateBattleMap_->setEnabled(battleMapLoaded);
    }
    if (actFixShuffling_) {
        actFixShuffling_->setEnabled(battleMapLoaded);
    }
    if (actValidateWorld_) {
        actValidateWorld_->setEnabled(worldLoaded);
    }
    if (actRegenerateImages_) {
        actRegenerateImages_->setEnabled(worldLoaded);
    }
    if (actFixZeroLayer_) {
        actFixZeroLayer_->setEnabled(battleMapLoaded && worldLoaded);
    }
    for (QAction *action : {importMapImageAction_, importUpTgaAction_,
                            importLeveledAction_, exportHeightmapAction_,
                            exportTopViewAction_}) {
        if (action) {
            action->setEnabled(worldLoaded);
        }
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
    for (QAction *action : {actZoomToFit_, actView3D_, actDimGeo_,
                            actFreezeDam_, actRotateLeft_,
                            actRotateRight_, actTiltUp_, actTiltDown_,
                            actLiftUp_, actLiftDown_, actZoomIn_,
                            actZoomOut_, actResetView_}) {
        if (action) {
            action->setEnabled(worldLoaded);
        }
    }
    // The grid cell size drives the grid overlay — needs a world too.
    if (gridStepWSpin_) {
        gridStepWSpin_->setEnabled(worldLoaded);
    }
    if (gridStepHSpin_) {
        gridStepHSpin_->setEnabled(worldLoaded);
    }
    // T3.1/T4.2 — the editor interaction mode: navigation works everywhere,
    // painting and picking need a world.
    if (actModeView_) {
        actModeView_->setEnabled(true);
    }
    if (actModeBrush_) {
        actModeBrush_->setEnabled(worldLoaded);
    }
    if (actModeEyedropper_) {
        actModeEyedropper_->setEnabled(worldLoaded);
    }
    // T3.2 — the brush type/mode buttons follow the same rule. The
    // zeroplast brushes are a Dam-mode tool (the zeroplast belongs to the
    // dam domain) — not accessible in the Geo mode.
    const bool damMode = brushTool_ &&
        brushTool_->params().mode == MapEditor::BrushMode::Dam;
    for (QAction *action : {actBrushRaise_, actBrushLower_, actBrushLevelZP_,
                            actBrushLevelHeight_, actBrushColor_,
                            actBrushImageDecal_,
                            actBrushPickColor_,
                            actBrushGeo_, actBrushDam_,
                            actBrushSoot_, actBrushVeryLightDam_,
                            actLoadBrushImage_,
                            actShapeCircle_, actShapeSquare_,
                            actShapeCircleOutline_, actShapeSquareOutline_,
                            actShapeLine_}) {
        if (action) {
            action->setEnabled(worldLoaded);
        }
    }
    for (QAction *action : {actBrushLevelZP_}) {
        if (action) {
            action->setEnabled(worldLoaded && damMode);
        }
    }
    // ImageDecal paints Dam palette indices only: the Geo layer is not
    // available while the decal brush is selected (switching to Geo falls back
    // to a plain raise, see the mode action).
    const bool decalEffect = brushTool_ &&
        brushTool_->params().effect == MapEditor::BrushEffect::ImageDecal;
    if (actBrushGeo_) {
        actBrushGeo_->setEnabled(worldLoaded && !decalEffect);
    }
    // T3.1 — the stamp radius applies to every form except the image stamps
    // (the brush image and the decal carry their own size) and the
    // point-operated pick, and only with a loaded world.
    if (brushRadiusSpin_) {
        brushRadiusSpin_->setEnabled(
            worldLoaded && brushTool_ &&
            !brushTool_->params().useImageMask && !decalEffect &&
            brushTool_->params().effect != MapEditor::BrushEffect::PickColor);
    }
    // T3.5 — the image scale applies to whichever image stamp is active: the
    // influence mask or the ImageDecal decal.
    if (brushMaskScaleSpin_) {
        const bool imageMaskActive = brushTool_ &&
            brushTool_->params().useImageMask && brushTool_->hasImageMask();
        const bool decalActive = decalEffect && brushTool_->hasDecalImage();
        brushMaskScaleSpin_->setEnabled(worldLoaded &&
                                        (imageMaskActive || decalActive));
        brushMaskScaleSpin_->setToolTip(
            decalActive
                ? tr("Scale of the loaded image decal relative to its original "
                     "pixel size (the stamp stays centred under the cursor).")
                : tr("Scale of the loaded brush image relative to its original "
                     "pixel size (the mask stays centred under the cursor)."));
    }
    // T3.3/T4.2 — color spinbox: painting and pick readout.
    if (brushColorIndexSpin_) {
        const MapEditor::BrushEffect effect =
            brushTool_ ? brushTool_->params().effect
                       : MapEditor::BrushEffect::None;
        brushColorIndexSpin_->setEnabled(
            worldLoaded &&
            (effect == MapEditor::BrushEffect::SetColor ||
             effect == MapEditor::BrushEffect::PickColor));
    }
    // T3.2 — the single height control serves the raise/lower step and the
    // "Level to Height" target.
    if (brushLevelSpin_) {
        const MapEditor::BrushEffect effect =
            brushTool_ ? brushTool_->params().effect
                       : MapEditor::BrushEffect::None;
        brushLevelSpin_->setEnabled(
            worldLoaded &&
            (effect == MapEditor::BrushEffect::ChangeHeight ||
             effect == MapEditor::BrushEffect::AlignVariableH));
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

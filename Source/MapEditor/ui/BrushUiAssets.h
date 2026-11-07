#pragma once

// Toolbar icon helpers + the brush-mask scaler, shared between MainWindow
// (view/editor-mode toolbar icons) and BrushUiController (the brush
// toolbars). Extracted from MainWindow.cpp during the BrushUiController
// decomposition (docs/architecture.md §5.36): header-only inline functions
// in MapEditor::brushui so neither side duplicates them.

#include <algorithm>
#include <cmath>

#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QTransform>

#include "BrushTool.h"  // BrushMask, BrushShape
#include "EditorMath.h" // kDegToRad
#include "Shape3D.h"    // Shape3DKind

namespace MapEditor {
namespace brushui {

// Toolbar icons — painted once per action with QPainter so the view toolbar
// does not depend on any icon theme being installed.
// ---------------------------------------------------------------------------

// kDegToRad lives in EditorMath.h (shared math constants).
const QColor kInk(45, 45, 45);
const QColor kAccent(0, 96, 170);

/// Small filled arrow head at `tip` pointing into `direction` (degrees,
/// screen coords: y grows downwards).
inline void drawArrowHead(QPainter &p, const QPointF &tip, double directionDeg,
                   double length = 6.0, double halfWidth = 3.0)
{
    const double a = directionDeg * MapEditor::kDegToRad;
    const QPointF dir(std::cos(a), std::sin(a));
    const QPointF normal(-dir.y(), dir.x());
    QPolygonF head;
    head << tip
         << tip - dir * length + normal * halfWidth
         << tip - dir * length - normal * halfWidth;
    p.drawPolygon(head);
}

inline void drawGroundLine(QPainter &p, double y)
{
    p.drawLine(QPointF(4.0, y), QPointF(18.0, y));
}

/// Arc-with-arrow rotation icon; `mirror` flips the arc direction by about
/// the vertical axis (drawn explicitly — no QPixmap::transformed involved).
inline QIcon makeRotateIcon(bool mirror)
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
    const double endAngleRad = endAngle * MapEditor::kDegToRad;
    const QPointF end(center.x() + radius * std::cos(endAngleRad),
                      center.y() - radius * std::sin(endAngleRad));
    // Tangent: for CCW arcs the position angle grows, for CW it shrinks.
    const double turn = mirror ? -1.0 : 1.0;
    drawArrowHead(p, end, std::atan2(turn * radius * std::cos(endAngleRad),
                                     turn * radius * std::sin(endAngleRad)) / MapEditor::kDegToRad);
    return QIcon(pm);
}

inline QIcon makeTiltIcon(bool up)
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

inline QIcon makeLiftIcon(bool up)
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

inline QIcon makeZoomIcon(bool in)
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

inline QIcon makeResetIcon()
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

inline QIcon makeModeIcon(bool oblique)
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
inline QIcon makeChunkyIcon()
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

inline QIcon makeSmoothIcon()
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


/// T4.2 — View editor mode: the mouse-arrow the navigation gives back after
/// the brush/pipette are put down.
inline QIcon makeViewModeIcon()
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
inline QIcon makeHeightArrowIcon(bool up)
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
inline QIcon makeGroundLetterIcon(const QString &letter, const QColor &color)
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
inline QIcon makeColorBrushIcon()
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
inline QIcon makeEyedropperIcon()
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

/// 3D shape brush glyphs: side-view silhouettes on a ground line.
inline QIcon makeShape3DIcon(MapEditor::Shape3DKind kind)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0));
    drawGroundLine(p, 18.5);
    p.setPen(QPen(kInk, 1.6));
    p.setBrush(kAccent);
    switch (kind) {
    case MapEditor::Shape3DKind::Cone:
        // Triangle with an elliptic base.
        p.drawLine(QPointF(11.0, 3.0), QPointF(4.0, 17.0));
        p.drawLine(QPointF(11.0, 3.0), QPointF(18.0, 17.0));
        p.drawLine(QPointF(4.0, 17.0), QPointF(18.0, 17.0));
        break;
    case MapEditor::Shape3DKind::Pyramid:
        // A triangle too — the polygon plan shows in the preview.
        p.drawLine(QPointF(11.0, 3.0), QPointF(4.0, 17.0));
        p.drawLine(QPointF(11.0, 3.0), QPointF(18.0, 17.0));
        p.drawLine(QPointF(4.0, 17.0), QPointF(18.0, 17.0));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(9.2, 1.2, 3.6, 3.6)); // apex dot
        break;
    case MapEditor::Shape3DKind::TruncatedCone:
        p.drawPolygon(QPolygonF()
                      << QPointF(8.0, 4.0) << QPointF(14.0, 4.0)
                      << QPointF(19.0, 17.0) << QPointF(3.0, 17.0));
        break;
    case MapEditor::Shape3DKind::TruncatedPyramid:
        p.drawPolygon(QPolygonF()
                      << QPointF(8.0, 4.0) << QPointF(14.0, 4.0)
                      << QPointF(17.0, 17.0) << QPointF(5.0, 17.0));
        break;
    case MapEditor::Shape3DKind::Torus:
        // A half ring: outer arc + inner arc.
        p.drawArc(QRectF(2.0, 6.0, 18.0, 18.0), 0, 180 * 16);
        p.drawArc(QRectF(7.0, 11.0, 8.0, 8.0), 0, 180 * 16);
        break;
    case MapEditor::Shape3DKind::HalfEllipsoid:
        p.drawChord(QRectF(3.0, 5.0, 16.0, 16.0), 0, 180 * 16);
        break;
    case MapEditor::Shape3DKind::None:
        break;
    }
    return QIcon(pm);
}

/// Place Spawn mode glyph: a map pin over a ground line — the spawn the
/// click drops at the target cell.
inline QIcon makePlaceSpawnIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kInk, 2.0));
    drawGroundLine(p, 18.0);
    // Pin: accent head with an ink tip onto the ground line.
    p.setBrush(kAccent);
    p.setPen(QPen(kInk, 1.5));
    p.drawEllipse(QRectF(7.0, 2.5, 8.0, 8.0));
    QPolygonF tip;
    tip << QPointF(8.6, 9.0) << QPointF(13.4, 9.0) << QPointF(11.0, 15.5);
    p.setPen(Qt::NoPen);
    p.setBrush(kAccent);
    p.drawPolygon(tip);
    p.setPen(QPen(kInk, 1.5));
    p.drawLine(QPointF(11.0, 15.5), QPointF(11.0, 18.0));
    // The dot inside the head.
    p.setBrush(kInk);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(9.7, 5.2, 2.6, 2.6));
    return QIcon(pm);
}

/// T3.5 — load-image action icon: folder with import arrow.
inline QIcon makeLoadImageIcon()
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
inline QIcon makeDecalIcon()
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
inline QIcon makeShapeIcon(MapEditor::BrushShape shape)
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
    case MapEditor::BrushShape::FilledPolygon:
    case MapEditor::BrushShape::PolygonOutline:
        // A hexagon in the icon regardless of the configured side count —
        // the glyph stands for the regular-polygon family.
        p.setPen(QPen(shape == MapEditor::BrushShape::FilledPolygon ? kInk : kAccent,
                      1.8));
        if (shape == MapEditor::BrushShape::FilledPolygon) {
            p.setBrush(fill);
        } else {
            p.setBrush(Qt::NoBrush);
        }
        {
            QPolygonF hex;
            for (int i = 0; i < 6; ++i) {
                const double a = MapEditor::kDegToRad * (60.0 * i - 90.0);
                hex << QPointF(11.0 + 7.5 * std::cos(a), 11.0 + 7.5 * std::sin(a));
            }
            p.drawPolygon(hex);
        }
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

/// Scripted brush type glyph: a ground tile with a "JS" scroll running over
/// it — the script replaces the plain stamp with scripted actions.
inline QIcon makeScriptBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // Ground tile.
    p.setPen(QPen(kInk, 1.4));
    p.setBrush(QColor(235, 235, 235));
    p.drawRect(QRectF(3.5, 10.5, 15.0, 8.0));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(120, 170, 220, 210));
    p.drawRect(QRectF(5.0, 12.0, 12.0, 3.4));
    p.setBrush(QColor(90, 150, 90, 210));
    p.drawRect(QRectF(5.0, 15.4, 12.0, 2.4));
    // Script scroll with "JS".
    p.setPen(QPen(kInk, 1.2));
    p.setBrush(QColor(255, 250, 230));
    p.drawRoundedRect(QRectF(4.0, 2.5, 14.0, 10.0), 2.0, 2.0);
    // Rolled edges.
    p.setBrush(kAccent);
    p.drawEllipse(QPointF(4.0, 7.5), 1.6, 1.9);
    p.drawEllipse(QPointF(18.0, 7.5), 1.6, 1.9);
    QFont font = p.font();
    font.setBold(true);
    font.setPixelSize(7);
    p.setFont(font);
    p.setPen(QColor(0x8a, 0x4f, 0x0f));
    p.drawText(QRectF(4.0, 2.5, 14.0, 10.0), Qt::AlignCenter,
               QStringLiteral("JS"));
    return QIcon(pm);
}

/// Script-editor action glyph: a pencil over a script page.
inline QIcon makeScriptEditIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // Page with text lines.
    p.setPen(QPen(kInk, 1.3));
    p.setBrush(QColor(252, 252, 248));
    p.drawRect(QRectF(4.0, 2.5, 12.5, 15.5));
    p.setPen(QPen(QColor(150, 160, 175), 1.1));
    for (int i = 0; i < 4; ++i) {
        p.drawLine(QPointF(6.0, 6.2 + i * 3.0), QPointF(14.5, 6.2 + i * 3.0));
    }
    // Pencil across the corner.
    p.save();
    p.translate(13.5, 13.0);
    p.rotate(45.0);
    p.setPen(QPen(kInk, 0.9));
    p.setBrush(QColor(255, 200, 60));
    p.drawRect(QRectF(-1.6, -7.5, 3.2, 11.0));
    p.setBrush(QColor(240, 230, 210));
    p.drawPolygon(QPolygonF({QPointF(-1.6, 3.5), QPointF(1.6, 3.5),
                             QPointF(0.0, 6.5)}));
    p.setBrush(QColor(60, 60, 60));
    p.drawPolygon(QPolygonF({QPointF(-0.7, 5.2), QPointF(0.7, 5.2),
                             QPointF(0.0, 6.5)}));
    p.restore();
    return QIcon(pm);
}

/// Smooth-brush glyph: bumpy ground profile settling into a flat line.
inline QIcon makeSmoothBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // Ground base.
    p.setPen(QPen(kInk, 1.3));
    p.setBrush(QColor(235, 235, 235));
    p.drawRect(QRectF(3.0, 12.0, 16.0, 6.5));
    // Bumpy surface (before).
    p.setPen(QPen(QColor(100, 140, 180), 1.8, Qt::SolidLine, Qt::RoundCap));
    QPolygonF bump;
    bump << QPointF(3.0, 12.0) << QPointF(5.5, 8.5) << QPointF(8.0, 12.5)
         << QPointF(10.5, 7.5) << QPointF(13.0, 12.5) << QPointF(15.5, 9.0)
         << QPointF(19.0, 12.0);
    p.drawPolyline(bump);
    // Flattening arrow onto the average line.
    p.setPen(QPen(kAccent, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(10.5, 8.2), QPointF(10.5, 3.2));
    p.setBrush(kAccent);
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygonF({QPointF(10.5, 1.6), QPointF(8.9, 4.6),
                             QPointF(12.1, 4.6)}));
    return QIcon(pm);
}

/// Flatten-brush glyph: scattered bumps being pulled onto an inclined
/// plane (the slope survives, the noise dies).
inline QIcon makeFlattenBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // The target plane.
    p.setPen(QPen(kAccent, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(3.0, 15.0), QPointF(19.0, 8.0));
    // Bumps above/below the plane.
    p.setPen(QPen(QColor(100, 140, 180), 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(7.0, 11.5), QPointF(7.0, 8.0));    // above
    p.drawLine(QPointF(13.0, 13.8), QPointF(13.0, 17.0));  // below
    p.setBrush(QColor(100, 140, 180));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(7.0, 7.2), 1.5, 1.5);
    p.drawEllipse(QPointF(13.0, 17.8), 1.5, 1.5);
    // Ground corner.
    p.setPen(QPen(kInk, 1.2));
    p.setBrush(QColor(235, 235, 235));
    p.drawRect(QRectF(3.0, 16.5, 16.0, 2.5));
    return QIcon(pm);
}

/// Tricubic-brush glyph: coarse control points with a smooth cubic curve
/// threading through them (fine wobble removed, the curve survives).
inline QIcon makeTricubicBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // Control lattice nodes (coarse, blocky).
    p.setPen(QPen(kInk, 1.1));
    p.setBrush(QColor(200, 210, 220));
    for (const QPointF& node : {QPointF(4.0, 12.0), QPointF(11.0, 8.0),
                                QPointF(18.0, 11.0)}) {
        p.drawRect(QRectF(node.x() - 1.6, node.y() - 1.6, 3.2, 3.2));
    }
    // Fine noise above the curve (dies).
    p.setPen(QPen(QColor(100, 140, 180), 1.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(7.0, 12.5), QPointF(7.0, 9.5));
    p.drawLine(QPointF(14.5, 12.0), QPointF(14.5, 14.5));
    // The smooth interpolated curve through the nodes (survives).
    QPainterPath curve(QPointF(2.5, 14.5));
    curve.cubicTo(QPointF(6.0, 6.0), QPointF(9.0, 5.0), QPointF(11.0, 8.0));
    curve.cubicTo(QPointF(13.0, 11.0), QPointF(16.0, 10.0),
                  QPointF(19.5, 12.5));
    p.setPen(QPen(kAccent, 1.9, Qt::SolidLine, Qt::RoundCap));
    p.drawPath(curve);
    return QIcon(pm);
}

/// Inflate-brush glyph: a mound with arrows growing out along its normals.
inline QIcon makeInflateBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // The mound profile.
    p.setPen(QPen(kInk, 1.4));
    p.setBrush(QColor(235, 235, 235));
    QPainterPath mound(QPointF(3.0, 18.0));
    mound.cubicTo(QPointF(6.0, 17.0), QPointF(8.0, 9.0), QPointF(11.0, 8.0));
    mound.cubicTo(QPointF(14.0, 9.0), QPointF(16.0, 17.0),
                  QPointF(19.0, 18.0));
    mound.closeSubpath();
    p.drawPath(mound);
    // Normal arrows (vertical on top, tilted on the flanks).
    p.setPen(QPen(kAccent, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(kAccent);
    const struct { QPointF a, b; } arrows[] = {
        {QPointF(11.0, 7.2), QPointF(11.0, 2.4)},
        {QPointF(7.6, 11.0), QPointF(5.2, 7.2)},
        {QPointF(14.4, 11.0), QPointF(16.8, 7.2)},
    };
    for (const auto& ar : arrows) {
        p.drawLine(ar.a, ar.b);
        const QPointF d = ar.b - ar.a;
        const double l = std::hypot(d.x(), d.y());
        const QPointF n(d.x() / l, d.y() / l);
        const QPointF perp(-n.y(), n.x());
        p.drawPolygon(QPolygonF({ar.b, ar.b - n * 2.6 + perp * 1.3,
                                 ar.b - n * 2.6 - perp * 1.3}));
    }
    return QIcon(pm);
}

/// Deflate-brush glyph: the same mound caving in (arrows pointing down
/// along the normals).
inline QIcon makeDeflateBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // The pressed-down profile.
    p.setPen(QPen(kInk, 1.4));
    p.setBrush(QColor(235, 235, 235));
    QPainterPath mound(QPointF(3.0, 18.0));
    mound.cubicTo(QPointF(6.0, 17.0), QPointF(8.0, 14.0),
                  QPointF(11.0, 13.2));
    mound.cubicTo(QPointF(14.0, 14.0), QPointF(16.0, 17.0),
                  QPointF(19.0, 18.0));
    mound.closeSubpath();
    p.drawPath(mound);
    // Inward-pointing normal arrows.
    p.setPen(QPen(kAccent, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(kAccent);
    const struct { QPointF a, b; } arrows[] = {
        {QPointF(11.0, 1.8), QPointF(11.0, 6.6)},
        {QPointF(4.6, 6.0), QPointF(7.4, 10.2)},
        {QPointF(17.4, 6.0), QPointF(14.6, 10.2)},
    };
    for (const auto& ar : arrows) {
        p.drawLine(ar.a, ar.b);
        const QPointF d = ar.b - ar.a;
        const double l = std::hypot(d.x(), d.y());
        const QPointF n(d.x() / l, d.y() / l);
        const QPointF perp(-n.y(), n.x());
        p.drawPolygon(QPolygonF({ar.b, ar.b - n * 2.6 + perp * 1.3,
                                 ar.b - n * 2.6 - perp * 1.3}));
    }
    return QIcon(pm);
}

/// Gaussian-blur glyph: a soft bell kernel over a noisy profile.
inline QIcon makeGaussianBlurBrushIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // Noisy profile (before).
    p.setPen(QPen(QColor(100, 140, 180), 1.4, Qt::SolidLine, Qt::RoundCap));
    QPolygonF noise;
    noise << QPointF(3.0, 12.0) << QPointF(5.0, 9.0) << QPointF(7.0, 12.5)
          << QPointF(9.0, 8.5) << QPointF(11.0, 12.0) << QPointF(13.0, 9.0)
          << QPointF(15.0, 12.5) << QPointF(17.0, 9.5) << QPointF(19.0, 12.0);
    p.drawPolyline(noise);
    // The smooth blurred curve (after).
    p.setPen(QPen(kAccent, 1.9, Qt::SolidLine, Qt::RoundCap));
    QPainterPath smooth(QPointF(3.0, 12.0));
    smooth.cubicTo(QPointF(7.0, 10.5), QPointF(9.0, 10.5), QPointF(11.0, 10.8));
    smooth.cubicTo(QPointF(13.0, 11.0), QPointF(15.0, 10.6),
                   QPointF(19.0, 12.0));
    p.drawPath(smooth);
    // The kernel bell.
    p.setPen(QPen(kInk, 1.1));
    p.setBrush(QColor(120, 170, 220, 90));
    QPainterPath bell(QPointF(4.0, 19.0));
    bell.cubicTo(QPointF(7.0, 18.6), QPointF(8.0, 15.5), QPointF(11.0, 15.2));
    bell.cubicTo(QPointF(14.0, 15.5), QPointF(15.0, 18.6), QPointF(18.0, 19.0));
    bell.closeSubpath();
    p.drawPath(bell);
    return QIcon(pm);
}

/// Height-gate modifier glyphs: the ground profile, a dashed gate level and
/// an arrow showing which side of the gate the brush works on (up = apply
/// where the dam is higher than the gate, down = where it is lower).
inline QIcon makeGateIcon(bool above)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // The gate level — a dashed horizontal line.
    QPen gatePen(kAccent);
    gatePen.setStyle(Qt::DashLine);
    gatePen.setWidthF(1.5);
    p.setPen(gatePen);
    p.drawLine(QPointF(2.0, 8.5), QPointF(20.0, 8.5));
    // The ground.
    p.setPen(QPen(kInk, 2.0));
    drawGroundLine(p, 17.0);
    // The direction arrow under / above the gate.
    p.setPen(QPen(kInk, 2.0));
    const double ax = 11.0;
    if (above) {
        p.drawLine(QPointF(ax, 15.0), QPointF(ax, 10.5));
        drawArrowHead(p, QPointF(ax, 10.5), -90.0);
    } else {
        p.drawLine(QPointF(ax, 5.0), QPointF(ax, 14.5));
        drawArrowHead(p, QPointF(ax, 14.5), 90.0);
    }
    return QIcon(pm);
}

/// T3.5: Convert a QImage to a BrushMask (pixel alpha → influence).
/// Transparent pixels (alpha = 0) carry no effect; non-transparent ones
/// influence the brush proportionally to their alpha (anti-aliased edges
/// get a partial effect) — the color/luminance is deliberately ignored,
/// the mask is about the shape, not the picture's brightness.
inline MapEditor::BrushMask imageToBrushMask(const QImage &img)
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

/// T3.1 — brush edit toggle: a paintbrush glyph (handle + ferrule + tip).
inline QIcon makeBrushIcon()
{
    const QColor kInk(45, 45, 45);
    const QColor kAccent(0, 96, 170);
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

/// T3.5 — scale a brush mask by `factor` (bilinear over the influence
/// values). The result is re-centred on the stamp origin like a freshly
/// converted image mask. The source is always the un-scaled original mask,
/// so repeated scale changes do not accumulate interpolation blur.
inline BrushMask scaleBrushMask(const BrushMask &src, double factor)
{
    BrushMask dst;
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

} // namespace brushui
} // namespace MapEditor

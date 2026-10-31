#include "MainWindow.h"
#include "ViewportWidget.h"
#include "MinimapWidget.h"
#include "BrushTool.h"

#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QImage>
#include <QMouseEvent>
#include <QTimer>

// MapEditorSupport — shims and content root initialization.
#include "errh_wrapper.h"
#include "shims.h"

// Lifecycle tracing (no-op unless MAP_EDITOR_TRACE is set).
#include "EditorTrace.h"

// Forward-declare setup_argcv (from XUTIL/XUTIL.cpp).
void setup_argcv(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    // Names are used later by QSettings (last game folder, window geometry, ...).
    QCoreApplication::setOrganizationName(QStringLiteral("Perimeter"));
    QCoreApplication::setApplicationName(QStringLiteral("MapEditor"));

    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral(PERIMETER_VERSION));

    // Initialize the command-line argument vector before any Terra/XTool call.
    // Terra's vMap.prepare() calls check_command_line() which requires it.
    setup_argcv(argc, argv);

    // Initialize the ErrH crash wrapper so Terra calls get a safety net.
    MapEditor::setupErrHWrapper();

    MapEditor::editorTrace("main: start");

    MainWindow window;
    if (qEnvironmentVariable("MAP_EDITOR_MAXIMIZE") == QLatin1String("1")) {
        window.showMaximized();
    } else {
        window.show();
    }

    // Repro harness for teardown crashes: MAP_EDITOR_AUTOTEST=1 with
    // MAP_EDITOR_GAMEDIR=<game root> and MAP_EDITOR_WORLD=<world name> drives
    // open dir -> load world -> close window automatically, so shutdown
    // problems can be reproduced without manual interaction.
    //
    // MAP_EDITOR_SHOTS=<dir> additionally walks 2D -> 3D -> 2D and saves
    // grabFramebuffer() PNGs into <dir> for offline diagnosis of the
    // viewport-switch rendering.
    if (qEnvironmentVariable("MAP_EDITOR_AUTOTEST") == QLatin1String("1")) {
        const QString gameDir = qEnvironmentVariable("MAP_EDITOR_GAMEDIR");
        const QString world = qEnvironmentVariable("MAP_EDITOR_WORLD");
        const QString shots = qEnvironmentVariable("MAP_EDITOR_SHOTS");
        MapEditor::editorTrace("autotest: scheduled (gamedir='%s', world='%s', shots='%s')",
                               gameDir.toStdString().c_str(),
                               world.toStdString().c_str(),
                               shots.toStdString().c_str());
        QTimer::singleShot(1000, &window, [&window, gameDir] {
            window.debugOpenGameDir(gameDir.toStdString());
        });
        QTimer::singleShot(10000, &window, [&window, world] {
            window.debugLoadWorld(world.toStdString());
        });
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            using ViewMode = MapEditor::ViewportWidget::ViewMode;
            const auto capture = [&window, shots](const char *name) {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) {
                    MapEditor::editorTrace("autotest: capture '%s' — no viewport", name);
                    return;
                }
                vp->repaint(); // synchronous paintGL into the widget FBO
                const QImage img = vp->grabFramebuffer();
                const QString path = shots + QLatin1Char('/') + name + QLatin1String(".png");
                const bool ok = img.save(path);
                MapEditor::editorTrace("autotest: capture '%s' — %dx%d saved=%d",
                                       name, img.width(), img.height(), ok ? 1 : 0);
            };
            QTimer::singleShot(13000, &window, [capture] { capture("01_topdown"); });
            // Minimap shots + viewRect logging (GPU minimap repro).
            const auto captureMini = [&window, shots](const char *name) {
                MapEditor::MinimapWidget *mm = window.debugMinimap();
                if (!mm) {
                    MapEditor::editorTrace("autotest: minimap capture '%s' — no widget", name);
                    return;
                }
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (vp) {
                    const QRect r = vp->currentViewRect();
                    const QPolygonF p = vp->currentViewPolygon();
                    QString pts;
                    for (const QPointF &pt : p) {
                        pts += QString(" (%1,%2)").arg(pt.x(), 0, 'f', 1).arg(pt.y(), 0, 'f', 1);
                    }
                    MapEditor::editorTrace(
                        "autotest: minimap '%s' — vp %dx%d viewRect(%d,%d %dx%d) poly=%s",
                        name, vp->width(), vp->height(), r.x(), r.y(), r.width(), r.height(),
                        pts.isEmpty() ? "none" : qPrintable(pts));
                }
                MapEditor::editorTrace(
                    "autotest: minimap '%s' — mm widget %dx%d dpr=%.2f mapArea(%.1f,%.1f %.1fx%.1f)",
                    name, mm->width(), mm->height(), mm->devicePixelRatioF(),
                    mm->debugMapArea().x(), mm->debugMapArea().y(),
                    mm->debugMapArea().width(), mm->debugMapArea().height());
                mm->repaint();
                const QImage img = mm->grabFramebuffer();
                const QString path = shots + QLatin1Char('/') + name + QLatin1String(".png");
                const bool ok = img.save(path);
                MapEditor::editorTrace("autotest: minimap capture '%s' — %dx%d saved=%d",
                                       name, img.width(), img.height(), ok ? 1 : 0);
            };
            QTimer::singleShot(13300, &window, [captureMini] { captureMini("10_minimap_topdown"); });
            QTimer::singleShot(14000, &window, [&window] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                MapEditor::View3DCamera cam;
                cam.turnAngleDeg = 180.0;
                cam.leanDeg = 135.0;
                cam.cameraDZ = 500;
                cam.focusC = 150;
                vp->setObliqueCamera(cam);
                vp->setViewMode(ViewMode::Oblique);
                MapEditor::editorTrace("autotest: switched to 3D (rot180 lean135 dz500 f150)");
            });
            QTimer::singleShot(17000, &window, [capture] { capture("02_oblique"); });
            QTimer::singleShot(17100, &window, [captureMini] { captureMini("11_minimap_oblique"); });
            // Switch to chunky to verify GPU oblique uses nearest filtering
            QTimer::singleShot(17500, &window, [&window] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                vp->setRenderStyle(MapEditor::ViewportWidget::RenderStyle::Chunky);
                MapEditor::editorTrace("autotest: switched to chunky");
            });
            QTimer::singleShot(18000, &window, [capture] { capture("03_oblique_chunky"); });
            // Switch back to smooth so the 2D shot is unaffected
            QTimer::singleShot(18300, &window, [&window] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                vp->setViewMode(ViewMode::TopDown);
                vp->setRenderStyle(MapEditor::ViewportWidget::RenderStyle::Smooth);
                MapEditor::editorTrace("autotest: switched back to 2D + smooth");
            });
            QTimer::singleShot(20000, &window, [capture] { capture("04_topdown_after3d"); });
            // A rotated 3D camera at the default tilt/lift: the minimap zone
            // must come out as a rotated trapezoid (far edge wider than the
            // near one), not an axis-aligned rect.
            QTimer::singleShot(20500, &window, [&window] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                MapEditor::View3DCamera cam;
                cam.turnAngleDeg = 45.0;
                cam.leanDeg = 140.0;
                cam.cameraDZ = 100;
                cam.focusC = 600;
                vp->setObliqueCamera(cam);
                vp->setViewMode(ViewMode::Oblique);
                MapEditor::editorTrace("autotest: switched to 3D rot45 lean140 dz100 f600");
            });
            QTimer::singleShot(22500, &window, [captureMini] {
                captureMini("12_minimap_oblique_rot45");
            });
            // Rotation spinbox <-> 3D camera binding: after the rot45 camera
            // set above the spinbox must mirror 45; editing it must drive
            // the camera, and a camera-side change must re-sync the spinbox.
            QTimer::singleShot(22800, &window, [&window] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                QDoubleSpinBox *spin = window.findChild<QDoubleSpinBox *>(
                    QStringLiteral("camRotationSpin"));
                if (!vp || !spin) {
                    MapEditor::editorTrace(
                        "autotest: rot spin check skipped (vp=%d spin=%d)",
                        vp ? 1 : 0, spin ? 1 : 0);
                    return;
                }
                const double mirrored = spin->value();
                const MapEditor::View3DCamera savedCam = vp->obliqueCamera();
                spin->setValue(90.0); // user edit -> camera rotation
                const double driven = vp->rotationDeg();
                vp->setRotationDeg(123.4); // camera-side change (orbit-like)
                const double resynced = spin->value();
                vp->setObliqueCamera(savedCam); // leave the camera as found
                MapEditor::editorTrace(
                    "autotest: rot spin mirror=%.1f editDrivesVp=%.1f "
                    "vpChangeResyncs=%.1f restored=%.1f",
                    mirrored, driven, resynced, vp->rotationDeg());
            });
            // Brush preview vs the real stamp footprint, the user's repro
            // camera: rot 15, lean 155, lift -500, focus 1950, radius 64
            // filled circle, Dam layer.
            QTimer::singleShot(23000, &window, [&window] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                MapEditor::BrushTool *bt = window.debugBrushTool();
                if (!vp || !bt) return;
                MapEditor::View3DCamera cam;
                cam.turnAngleDeg = 15.0;
                cam.leanDeg = 155.0;
                cam.cameraDZ = -500;
                cam.focusC = 1950;
                vp->setObliqueCamera(cam);
                vp->setViewMode(MapEditor::ViewportWidget::ViewMode::Oblique);
                MapEditor::BrushParams params = bt->params();
                params.shape = MapEditor::BrushShape::FilledCircle;
                params.radius = 64;
                params.mode = MapEditor::BrushMode::Dam;
                params.effect = MapEditor::BrushEffect::ChangeHeight;
                params.direction = 1;
                params.strength = 30;
                bt->setParams(params);
                vp->setEditorMode(MapEditor::ViewportWidget::EditorMode::Brush);
                vp->setBrushPreview(MapEditor::BrushPreviewShape::Circle, 64);
                vp->setBrushPreviewVisible(true);
                // Deterministic state for the preview shot: the grid overlay is
                // exercised by the later steps and must not leak into this one.
                vp->setGridVisible(false);
                // The GPU camera matrix is pushed inside renderFrame — paint once
                // before picking, otherwise the pick (and the preview built from
                // it) still uses the previous camera.
                vp->repaint();
                // Park the cursor below the look-at centre (near field), where
                // the perspective scale differs most from the heuristic.
                const QPoint pos(vp->width() / 2, vp->height() * 3 / 4);
                QMouseEvent move(QEvent::MouseMove, QPointF(pos),
                                 QPointF(vp->mapToGlobal(pos)),
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &move);
                vp->repaint();
                MapEditor::editorTrace("autotest: brush preview repro at (%d,%d), cursor cell (%d,%d)",
                                       pos.x(), pos.y(), vp->cursorCell().x(),
                                       vp->cursorCell().y());
            });
            // The captures below run far apart on purpose: each of them paints
            // synchronously (repaint + grabFramebuffer), and that nested paint
            // processes the timer queue — steps scheduled closely would run
            // reentrantly and capture each other's state.
            QTimer::singleShot(24500, &window, [capture] {
                capture("13_brush_preview");
            });
            // NOTE: no real stamp here on purpose — applyStamp() marks the world
            // modified, and closing then raises the modal "Unsaved Changes"
            // dialog, which blocks an unattended run.
            // Grid overlay in the GPU 3D view at the repro camera: the step is
            // user-editable, so three sizes are captured to prove it is honoured.
            const auto captureGrid = [&window, capture](const char *name, int step) {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                vp->setGridStep(step, step);
                vp->setGridVisible(true);
                vp->repaint();
                MapEditor::editorTrace("autotest: grid capture '%s' step=%d", name, step);
                capture(name);
                vp->setGridVisible(false);
            };
            QTimer::singleShot(27000, &window, [captureGrid] {
                captureGrid("15_grid3d_step32", 32);
            });
            QTimer::singleShot(29500, &window, [captureGrid] {
                captureGrid("17_grid3d_step128", 128);
            });
            QTimer::singleShot(32000, &window, [captureGrid] {
                captureGrid("16_grid3d_step4", 4);
            });

            // ------------------------------------------------------------
            // Game camera mode (View → Camera Like in Game): the FORTOP
            // first-player preset (psi/theta/distance + the game FOV) and a
            // real brush stamp under it — the stamp must land on the cell
            // under the cursor and render through the game frustum; undo
            // must restore the terrain (and keep the window closing
            // unattended — no unsaved-changes dialog).
            // ------------------------------------------------------------
            const auto stampDiff = [](const QImage &a, const QImage &b) {
                if (a.size() != b.size()) {
                    return -1;
                }
                int diff = 0;
                for (int y = 0; y < a.height(); ++y) {
                    const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                    const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                    for (int x = 0; x < a.width(); ++x) {
                        if (pa[x] != pb[x]) {
                            ++diff;
                        }
                    }
                }
                return diff;
            };
            const auto brushStampAt = [&window](const QPoint &pos) {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) {
                    return;
                }
                QMouseEvent move(QEvent::MouseMove, QPointF(pos),
                                 QPointF(vp->mapToGlobal(pos)),
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &move);
                vp->repaint();
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos),
                                  QPointF(vp->mapToGlobal(pos)),
                                  Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(vp, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos),
                                    QPointF(vp->mapToGlobal(pos)),
                                    Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(vp, &release);
                vp->repaint();
            };
            auto gameBefore = std::make_shared<QImage>();
            QTimer::singleShot(34500, &window, [&window] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                // The real menu action: resolves the preset (the first
                // player's camera spline of the loaded world) and switches.
                const bool invoked = QMetaObject::invokeMethod(
                    &window, "onActionGameCamera", Qt::DirectConnection);
                vp->repaint();
                MapEditor::editorTrace(
                    "autotest: game camera invoked=%d gpuReady=%d mode=%d "
                    "viewMode=%d",
                    invoked ? 1 : 0, vp->gpuViewReady() ? 1 : 0,
                    vp->isGameCameraMode() ? 1 : 0,
                    vp->viewMode() == MapEditor::ViewportWidget::ViewMode::Oblique
                        ? 1 : 0);
            });
            QTimer::singleShot(36500, [capture] { capture("18_game_camera"); });
            QTimer::singleShot(36600, [captureMini] { captureMini("19_minimap_game"); });
            QTimer::singleShot(37500, [&window, capture, stampDiff,
                                       brushStampAt, gameBefore] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                vp->repaint();
                *gameBefore = vp->grabFramebuffer();
                const QPoint pos(vp->width() / 2, vp->height() * 3 / 4);
                brushStampAt(pos);
                const QImage after = vp->grabFramebuffer();
                capture("20_game_brush_stamp");
                MapEditor::editorTrace(
                    "autotest: game brush stamp at (%d,%d) cursor cell (%d,%d) "
                    "pixels changed=%d",
                    pos.x(), pos.y(), vp->cursorCell().x(), vp->cursorCell().y(),
                    stampDiff(after, *gameBefore));
            });
            QTimer::singleShot(40500, [&window, capture, stampDiff,
                                       gameBefore] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                const bool invoked = QMetaObject::invokeMethod(
                    &window, "onActionUndo", Qt::DirectConnection);
                vp->repaint();
                const QImage undone = vp->grabFramebuffer();
                capture("21_game_brush_undo");
                MapEditor::editorTrace(
                    "autotest: game brush undo invoked=%d pixels restored=%d",
                    invoked ? 1 : 0, stampDiff(undone, *gameBefore));
            });
        }
        QTimer::singleShot(44500, &window, [&window] {
            MapEditor::editorTrace("autotest: closing window");
            window.close();
        });
    }

    const int result = app.exec();
    MapEditor::editorTrace("main: exec returned %d", result);
    return result;
}

#include "MainWindow.h"
#include "ViewportWidget.h"

#include <QApplication>
#include <QDir>
#include <QImage>
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
    window.show();

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
            QTimer::singleShot(18000, &window, [&window] {
                MapEditor::ViewportWidget *vp = window.debugViewport();
                if (!vp) return;
                vp->setViewMode(ViewMode::TopDown);
                MapEditor::editorTrace("autotest: switched back to 2D");
            });
            QTimer::singleShot(20000, &window, [capture] { capture("03_topdown_after3d"); });
        }
        QTimer::singleShot(25000, &window, [&window] {
            MapEditor::editorTrace("autotest: closing window");
            window.close();
        });
    }

    const int result = app.exec();
    MapEditor::editorTrace("main: exec returned %d", result);
    return result;
}

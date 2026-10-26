#include "MainWindow.h"

#include <QApplication>
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
    if (qEnvironmentVariable("MAP_EDITOR_AUTOTEST") == QLatin1String("1")) {
        const QString gameDir = qEnvironmentVariable("MAP_EDITOR_GAMEDIR");
        const QString world = qEnvironmentVariable("MAP_EDITOR_WORLD");
        MapEditor::editorTrace("autotest: scheduled (gamedir='%s', world='%s')",
                               gameDir.toStdString().c_str(),
                               world.toStdString().c_str());
        QTimer::singleShot(1000, &window, [&window, gameDir] {
            window.debugOpenGameDir(gameDir.toStdString());
        });
        QTimer::singleShot(10000, &window, [&window, world] {
            window.debugLoadWorld(world.toStdString());
        });
        QTimer::singleShot(25000, &window, [&window] {
            MapEditor::editorTrace("autotest: closing window");
            window.close();
        });
    }

    const int result = app.exec();
    MapEditor::editorTrace("main: exec returned %d", result);
    return result;
}

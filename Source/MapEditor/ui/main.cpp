#include "MainWindow.h"

#include <QApplication>

// MapEditorSupport — shims and content root initialization.
#include "errh_wrapper.h"
#include "shims.h"

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

    MainWindow window;
    window.show();

    return app.exec();
}

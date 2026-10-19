#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    // Names are used later by QSettings (last game folder, window geometry, ...).
    QCoreApplication::setOrganizationName(QStringLiteral("Perimeter"));
    QCoreApplication::setApplicationName(QStringLiteral("MapEditor"));

    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral(PERIMETER_VERSION));

    MainWindow window;
    window.show();

    return app.exec();
}

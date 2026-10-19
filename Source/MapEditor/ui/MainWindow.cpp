#include "MainWindow.h"

#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("MainWindow"));
    setWindowTitle(QStringLiteral("Perimeter Map Editor"));
    resize(1280, 800);

    // Status bar — ready for coordinate / brush info in T2.
    statusBar()->showMessage(QStringLiteral("Map editor skeleton — ready for T0.2"));

    // Placeholder central widget so the window looks non-empty.
    auto *central = new QLabel(QStringLiteral("Map editor skeleton — viewports, tools and maps will appear in T1+"), this);
    central->setAlignment(Qt::AlignCenter);
    setCentralWidget(central);
}

MainWindow::~MainWindow() = default;

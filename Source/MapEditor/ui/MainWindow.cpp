#include "MainWindow.h"

#include <EditorSession.h>
#include <GameDirManager.h>

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("MainWindow"));
    setWindowTitle(QStringLiteral("Perimeter Map Editor"));
    resize(1280, 800);

    session_ = new MapEditor::EditorSession();
    gameDirMgr_ = new GameDirManager();

    buildMenuBar();
    buildStatusBar();

    auto *central = new QLabel(
        QStringLiteral("Perimeter Map Editor\nT1.1: EditorSession integrated.\n"
                       "Use File \u2192 Select Game Directory to get started."),
        this);
    central->setAlignment(Qt::AlignCenter);
    central->setWordWrap(true);
    setCentralWidget(central);

    updateStatusBar();
}

MainWindow::~MainWindow()
{
    delete session_;
    delete gameDirMgr_;
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

    QAction *actQuit = fileMenu->addAction(tr("E&xit"));
    actQuit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    connect(actQuit, &QAction::triggered, this, &QApplication::quit);
}

void MainWindow::buildStatusBar()
{
    statusBar()->showMessage(tr("Map editor \u2014 T1.1"));
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onActionSelectGameDir()
{
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
    showWorldListDialog();
}

void MainWindow::onActionUnloadWorld()
{
    session_->unloadWorld();
    updateStatusBar();
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void MainWindow::updateStatusBar()
{
    QString text;

    switch (session_->state()) {
    case MapEditor::SessionState::NoGame:
        text = tr("No game directory selected.");
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
}

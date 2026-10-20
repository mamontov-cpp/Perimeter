#include "MainWindow.h"

#include <EditorSession.h>
#include <GameDirManager.h>
#include <ViewportWidget.h>

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
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
    buildViewport();

    updateStatusBar();
}

MainWindow::~MainWindow()
{
    delete session_;
    delete gameDirMgr_;
    delete viewport_;
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------

void MainWindow::buildViewport()
{
    viewport_ = new MapEditor::ViewportWidget(this);

    // Connect cursor position signal to status bar.
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition,
            this, &MainWindow::updateCursorInfo);

    setCentralWidget(viewport_);
}

void MainWindow::onWorldLoaded()
{
    // Rebind the viewport to pick up the newly loaded world.
    if (viewport_ && session_) {
        viewport_->setSession(session_);
    }
    updateStatusBar();
}

void MainWindow::onWorldUnload()
{
    if (viewport_ && session_) {
        viewport_->setSession(session_);
    }
    updateStatusBar();
}

void MainWindow::updateCursorInfo(int mapX, int mapY)
{
    QString info = QString(tr("X: %1  Y: %2")).arg(mapX).arg(mapY);
    statusBar()->showMessage(info, 3000);
}

void MainWindow::onZoomToFit()
{
    if (viewport_) {
        viewport_->zoomToFit();
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
}

void MainWindow::buildViewMenu(QMenuBar *menuBar)
{
    QMenu *viewMenu = menuBar->addMenu(tr("&View"));

    QAction *actGrid = viewMenu->addAction(tr("&Grid"));
    actGrid->setCheckable(true);
    actGrid->setChecked(true);
    actGrid->setShortcut(QKeySequence(Qt::Key_G));
    connect(actGrid, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setGridVisible(checked);
        }
    });

    QAction *actFit = viewMenu->addAction(tr("Zoom to &Fit"));
    actFit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(actFit, &QAction::triggered, this, &MainWindow::onZoomToFit);
}

void MainWindow::buildStatusBar()
{
    statusBar()->showMessage(tr("Map editor \u2014 T2.2 Viewport"));
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

    // Keep the Edit/File actions in sync with the session state.
    updateActionStates();
}

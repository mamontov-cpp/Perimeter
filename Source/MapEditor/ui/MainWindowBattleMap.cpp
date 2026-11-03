// ============================================================================
#include "EditorMath.h"
// MainWindow — battle map (.spg) implementation (T8.2/T8.3/T8.4/T8.5).
//
// Split out of MainWindow.cpp: the battle-map flow (open/save/save-as/new,
// spawn overlay, world switching) is self-contained and MainWindow.cpp was
// already large.
// ============================================================================

#include "MainWindow.h"

#include "BattleMapPanel.h"
#include "EditorSession.h"
#include "ViewportWidget.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>

namespace fs = std::filesystem;

namespace {

// Overlay colors for the spawn rings (distinct per player).
const QColor kSpawnColors[8] = {
    QColor(255, 64, 64),   QColor(64, 96, 255),  QColor(255, 200, 0),
    QColor(80, 200, 80),   QColor(220, 100, 255), QColor(0, 220, 220),
    QColor(255, 128, 0),   QColor(180, 180, 180),
};

} // namespace

void MainWindow::buildBattleMapDock()
{
    battleMapPanel_ = new MapEditor::BattleMapPanel(battleMap_, *session_, this);
    battleMapPanel_->setViewCenterProvider([this]() {
        return viewport_ ? viewport_->currentViewRect() : QRect();
    });

    battleMapDock_ = new QDockWidget(tr("Battle Map"), this);
    battleMapDock_->setObjectName(QStringLiteral("BattleMapDock"));
    battleMapDock_->setWidget(battleMapPanel_);
    battleMapDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                    Qt::RightDockWidgetArea |
                                    Qt::TopDockWidgetArea |
                                    Qt::BottomDockWidgetArea);
    battleMapDock_->setFeatures(QDockWidget::DockWidgetClosable |
                                QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, battleMapDock_);

    if (viewMenu_) {
        viewMenu_->addSeparator();
        QAction *toggleAction = battleMapDock_->toggleViewAction();
        toggleAction->setText(tr("Battle &Map"));
        toggleAction->setToolTip(tr("Show or hide the battle map panel (B)"));
        toggleAction->setShortcut(QKeySequence(Qt::Key_B));
        viewMenu_->addAction(toggleAction);
    }

    connect(battleMapPanel_, &MapEditor::BattleMapPanel::battleMapChanged,
            this, &MainWindow::syncSpawnOverlay);
    connect(battleMapPanel_, &MapEditor::BattleMapPanel::goToCell, this,
            [this](int mapX, int mapY) {
                if (viewport_ && session_ && session_->isWorldLoaded()) {
                    viewport_->centerOnCell(QPoint(mapX, mapY));
                    viewport_->update();
                }
            });
    // Place Spawn mode: the viewport announces the clicked cell, the move
    // itself happens here (terrain z-correction + undo recording).
    connect(viewport_, &MapEditor::ViewportWidget::spawnPlacementRequested,
            this, &MainWindow::onSpawnPlacementRequested);
    connect(battleMapPanel_,
            &MapEditor::BattleMapPanel::newBattleMapRequested, this,
            &MainWindow::onNewBattleMapRequested);

    // Template combo: the battle maps of the current game directory.
    battleMapPanel_->setTemplates(battleMapTemplates());
}

QStringList MainWindow::battleMapTemplates() const
{
    QStringList templates;
    const fs::path battleDir =
        fs::u8path(session_ ? session_->getGameDir() : std::string()) /
        "RESOURCE" / "Battle";
    std::error_code ec;
    if (fs::is_directory(battleDir, ec)) {
        for (const fs::directory_entry &entry :
             fs::directory_iterator(battleDir, ec)) {
            if (!entry.is_regular_file(ec) ||
                entry.path().extension().u8string() != ".spg") {
                continue;
            }
            templates << QString::fromStdString(
                entry.path().filename().u8string());
        }
    }
    templates.sort();
    return templates;
}

bool MainWindow::ensureWorldForBattleMap()
{
    if (!battleMap_.loaded() || !session_) {
        return false;
    }
    const std::string worldName = battleMap_.worldName();
    if (worldName.empty()) {
        return session_->isWorldLoaded();
    }
    if (session_->isWorldLoaded() &&
        session_->currentWorldName() == worldName) {
        return true;
    }
    // Switch to the world the battle map refers to.
    if (!confirmDiscardUnsavedChanges()) {
        return false;
    }
    if (!session_->scanWorlds() || !session_->loadWorldByName(worldName)) {
        QMessageBox::warning(
            this, tr("Open Battle Map"),
            tr("The world \"%1\" of this battle map was not found in "
               "RESOURCE/Worlds.")
                .arg(QString::fromStdString(worldName)));
        return session_->isWorldLoaded();
    }
    onWorldLoaded();
    return true;
}

void MainWindow::syncSpawnOverlay()
{
    if (!viewport_) {
        return;
    }
    std::vector<MapEditor::SpawnPoint> spawns;
    if (battleMap_.loaded()) {
        spawns.reserve(static_cast<size_t>(battleMap_.playerCount()));
        for (int i = 0; i < battleMap_.playerCount(); ++i) {
            double x = 0;
            double y = 0;
            if (!battleMap_.playerAnchor(i, x, y)) {
                continue;
            }
            MapEditor::SpawnPoint spawn;
            spawn.label =
                QString::fromStdString(battleMap_.playerData(i).name);
            spawn.playerIndex = i;
            spawn.mapX = qRound(x);
            spawn.mapY = qRound(y);
            spawn.color = kSpawnColors[i % 8];
            spawns.push_back(std::move(spawn));
        }
    }
    viewport_->setSpawnPoints(spawns);
    if (viewport_->hasWorld()) {
        viewport_->update();
    }
}

void MainWindow::onSpawnPlacementRequested(int mapX, int mapY)
{
    if (!battleMap_.loaded()) {
        return;
    }
    const int player = battleMapPanel_->selectedPlayer();
    if (player < 0) {
        statusBar()->showMessage(
            tr("Select a player in the Battle Map panel first — the Place "
               "Spawn mode moves that player's spawn."),
            5000);
        return;
    }

    double oldX = 0;
    double oldY = 0;
    if (!battleMap_.playerAnchor(player, oldX, oldY)) {
        statusBar()->showMessage(tr("Failed to read the player's spawn."),
                                 4000);
        return;
    }
    // Clicking the cell the spawn already sits on is a no-op — do not spam
    // the undo history.
    if (qRound(oldX) == mapX && qRound(oldY) == mapY) {
        return;
    }

    // z_new = z_old + (terrain_new - terrain_old): the models keep their
    // vertical offset over the ground (same rule as "Move to View Centre").
    // Without the world loaded the heights are unknown and the offsets are
    // kept as-is (dz = 0).
    double dz = 0;
    if (session_ && session_->isWorldLoaded()) {
        const int oldTerrain =
            session_->terrainHeightAt(qRound(oldX), qRound(oldY));
        const int newTerrain = session_->terrainHeightAt(mapX, mapY);
        if (oldTerrain < 0 || newTerrain < 0) {
            statusBar()->showMessage(
                tr("Cannot read the terrain height under the old or new "
                   "spawn — the location was not changed."),
                4000);
            return;
        }
        dz = static_cast<double>(newTerrain - oldTerrain);
    }

    if (!battleMap_.setPlayerAnchorUndoable(player, mapX, mapY, dz)) {
        statusBar()->showMessage(tr("Failed to move the spawn group."), 4000);
        return;
    }
    // The .spg is modified in place — Save Battle Map persists it. The panel
    // list and the viewport overlay read the anchors: refresh both, and the
    // undo/redo actions pick up the new history depth.
    battleMapPanel_->refresh();
    syncSpawnOverlay();
    updateActionStates();
}

void MainWindow::refreshBattleMapUI()
{
    battleMapPanel_->refresh();
    syncSpawnOverlay();
    // The one place that maps session/battle-map state onto every action
    // (Save/Save As, Open, Validate, autofixes).
    updateActionStates();
}

void MainWindow::onActionOpenBattleMap()
{
    if (!session_ || !session_->hasGameDir()) {
        QMessageBox::warning(this, tr("Open Battle Map"),
                             tr("Select a game directory first."));
        return;
    }
    const QString startDir =
        QDir(QString::fromStdString(session_->getGameDir()))
            .filePath(QStringLiteral("RESOURCE/Battle"));
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Battle Map"), startDir,
        tr("Battle maps (*.spg);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (!battleMap_.load(path.toStdString())) {
        QMessageBox::warning(this, tr("Open Battle Map"),
                             tr("Cannot read the battle map file."));
        return;
    }
    battleMapPath_ = path;
    if (!ensureWorldForBattleMap()) {
        // The user cancelled the world switch or the world is missing — the
        // map stays loaded (spawn moves need its world though).
    }
    refreshBattleMapUI();
}

QString MainWindow::askBattleMapSaveName(bool &toWorldsFolder) const
{
    toWorldsFolder = false;
    if (!session_ || !session_->hasGameDir()) {
        return QString();
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        nullptr, tr("Save Battle Map As"), tr("Map name:"), QLineEdit::Normal,
        QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return QString();
    }
    const QMessageBox::StandardButton folder = QMessageBox::question(
        nullptr, tr("Save Battle Map As"),
        tr("Save into RESOURCE/Worlds (Yes) or RESOURCE/Battle (No)?"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    if (folder == QMessageBox::Cancel) {
        return QString();
    }
    toWorldsFolder = folder == QMessageBox::Yes;
    return name.trimmed();
}

void MainWindow::normalizeBattleMapSpawnHeights()
{
    if (!battleMap_.loaded() || !session_ || !session_->isWorldLoaded()) {
        return;
    }
    // The game lands frames relative to vMap.hZeroPlast (world.ini) and the
    // local terrain height, so the normalization must use the world the map
    // actually refers to.
    if (session_->currentWorldName() != battleMap_.worldName()) {
        return;
    }
    battleMap_.restPlayersOnZeroLayer(
        session_->zeroLayerHeight(),
        [this](int x, int y) { return session_->terrainHeightAt(x, y); });
    // Frames must also be stored "installed" (attached + powered + a
    // zeroplast disc counter) — the COMPASS-family template state
    // (attached = false) leaves them hovering until something installs
    // them in-game.
    battleMap_.setFramesInstalledForStart();
}

void MainWindow::onActionSaveBattleMap()
{
    if (!battleMap_.loaded() || battleMapPath_.isEmpty()) {
        return;
    }
    normalizeBattleMapSpawnHeights();
    if (!battleMap_.save(battleMapPath_.toStdString())) {
        QMessageBox::warning(this, tr("Save Battle Map"),
                             tr("Failed to write the battle map."));
        return;
    }
    statusBar()->showMessage(tr("Battle map saved: %1").arg(battleMapPath_),
                             5000);
}

void MainWindow::onActionSaveBattleMapAs()
{
    if (!battleMap_.loaded() || !session_ || !session_->hasGameDir()) {
        return;
    }
    bool toWorlds = false;
    const QString name = askBattleMapSaveName(toWorlds);
    if (name.isEmpty()) {
        return;
    }
    const fs::path dir =
        fs::u8path(session_->getGameDir()) / "RESOURCE" /
        (toWorlds ? "Worlds" : "Battle");
    const fs::path target =
        dir / fs::u8path(name.toStdString() + ".spg");
    if (fs::exists(target) &&
        QMessageBox::question(
            this, tr("Save Battle Map As"),
            tr("%1 already exists. Overwrite?")
                .arg(QString::fromStdString(target.u8string()))) !=
            QMessageBox::Yes) {
        return;
    }
    normalizeBattleMapSpawnHeights();
    if (!battleMap_.save(target.u8string())) {
        QMessageBox::warning(this, tr("Save Battle Map"),
                             tr("Failed to write the battle map."));
        return;
    }
    battleMapPath_ = QString::fromStdString(target.u8string());
    refreshBattleMapUI();
    statusBar()->showMessage(tr("Battle map saved: %1").arg(battleMapPath_),
                             5000);
}

void MainWindow::onActionNewBattleMap()
{
    if (!session_ || !session_->hasGameDir()) {
        QMessageBox::warning(this, tr("New Battle Map"),
                             tr("Select a game directory first."));
        return;
    }
    if (!session_->isWorldLoaded()) {
        QMessageBox::information(
            this, tr("New Battle Map"),
            tr("Load a world first — the battle map is built for it "
               "(create one with File / New World if needed)."));
        return;
    }
    const QStringList templates = battleMapTemplates();
    if (templates.isEmpty()) {
        QMessageBox::warning(
            this, tr("New Battle Map"),
            tr("No template battle maps found in RESOURCE/Battle — a new "
               "battle map is built from one of the shipped maps."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Battle Map for '%1'")
                              .arg(QString::fromStdString(
                                  session_->currentWorldName())));
    auto *form = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(tr("MyMap"));
    form->addRow(tr("Map name:"), nameEdit);

    auto *templateCombo = new QComboBox(&dialog);
    templateCombo->addItems(templates);
    templateCombo->setToolTip(tr("Player settings and unit layouts are taken "
                                 "from this map and adapted"));
    form->addRow(tr("Template:"), templateCombo);

    auto *playersSpin = new QSpinBox(&dialog);
    playersSpin->setRange(1, 8);
    playersSpin->setValue(2);
    form->addRow(tr("Players:"), playersSpin);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    // Ok is disabled until the name is filled in.
    auto *okButton = buttons->button(QDialogButtonBox::Ok);
    okButton->setEnabled(false);
    connect(nameEdit, &QLineEdit::textChanged, okButton,
            [okButton](const QString &text) {
                okButton->setEnabled(!text.trimmed().isEmpty());
            });

    if (dialog.exec() != QDialog::Accepted ||
        nameEdit->text().trimmed().isEmpty()) {
        return;
    }
    onNewBattleMapRequested(nameEdit->text().trimmed(),
                            templateCombo->currentText(),
                            playersSpin->value());
}

void MainWindow::onNewBattleMapRequested(const QString &name,
                                         const QString &templateMap,
                                         int playerCount)
{
    if (!session_ || !session_->hasGameDir()) {
        return;
    }
    if (!session_->isWorldLoaded()) {
        QMessageBox::information(
            this, tr("New Battle Map"),
            tr("Load a world first — the new battle map is built for it."));
        return;
    }
    const QString mapName =
        name.isEmpty()
            ? QInputDialog::getText(this, tr("New Battle Map"),
                                    tr("Map name:"), QLineEdit::Normal,
                                    QString())
            : name;
    // `name` comes pre-filled from the File-menu dialog; the input-dialog
    // fallback above already validated its own result.
    if (mapName.trimmed().isEmpty()) {
        return;
    }
    const fs::path templatePath =
        fs::u8path(session_->getGameDir()) / "RESOURCE" / "Battle" /
        fs::u8path(templateMap.toStdString());

    MapEditor::SpgFile map;
    if (!map.load(templatePath.u8string())) {
        QMessageBox::warning(this, tr("New Battle Map"),
                             tr("Cannot read the template battle map."));
        return;
    }
    map.setWorldName(session_->currentWorldName());
    map.setPlayerCount(playerCount);
    map.setZeroLayerHeight(session_->zeroLayerHeight());

    // Spread the spawn anchors on a circle around the map centre and correct
    // every z by the terrain delta (the model z offsets of the template are
    // preserved).
    const int mapW = static_cast<int>(session_->mapWidth());
    const int mapH = static_cast<int>(session_->mapHeight());
    const double radiusX = mapW * 0.35;
    const double radiusY = mapH * 0.35;
    for (int i = 0; i < playerCount; ++i) {
        double ax = 0;
        double ay = 0;
        if (!map.playerAnchor(i, ax, ay)) {
            break;
        }
        const double angle =
            2.0 * MapEditor::kPi * i / playerCount;
        const double nx = mapW / 2.0 + radiusX * std::cos(angle);
        const double ny = mapH / 2.0 + radiusY * std::sin(angle);
        const int oldTerrain =
            session_->terrainHeightAt(qRound(ax), qRound(ay));
        const int newTerrain =
            session_->terrainHeightAt(qRound(nx), qRound(ny));
        const double dz =
            (oldTerrain < 0 || newTerrain < 0)
                ? 0.0
                : static_cast<double>(newTerrain - oldTerrain);
        map.setPlayerAnchor(i, nx, ny, dz);
    }

    const fs::path target =
        fs::u8path(session_->getGameDir()) / "RESOURCE" / "Battle" /
        fs::u8path(mapName.trimmed().toStdString() + ".spg");
    if (fs::exists(target) &&
        QMessageBox::question(
            this, tr("New Battle Map"),
            tr("%1 already exists. Overwrite?")
                .arg(QString::fromStdString(target.u8string()))) !=
            QMessageBox::Yes) {
        return;
    }
    // The wizard placed the anchors terrain-referenced — rest the frames on
    // their grounded pose over the terrain and store them "installed" so
    // they start landed (same rule as Save).
    map.restPlayersOnZeroLayer(
        session_->zeroLayerHeight(),
        [this](int x, int y) { return session_->terrainHeightAt(x, y); });
    map.setFramesInstalledForStart();
    if (!map.save(target.u8string())) {
        QMessageBox::warning(this, tr("New Battle Map"),
                             tr("Failed to write the battle map."));
        return;
    }
    // Open the fresh map right away.
    battleMap_ = MapEditor::SpgFile();
    if (battleMap_.load(target.u8string())) {
        battleMapPath_ = QString::fromStdString(target.u8string());
    }
    refreshBattleMapUI();
    statusBar()->showMessage(
        tr("Battle map created: %1").arg(battleMapPath_), 5000);
}

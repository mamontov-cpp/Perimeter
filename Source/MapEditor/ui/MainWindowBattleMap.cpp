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
#include "BuildingsPanel.h"
#include "BuildingCatalog.h"
#include "ScumSpotsPanel.h"
#include "NaturePanel.h"
#include "EditorSession.h"
#include "MinimapWidget.h"
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
#include <QFileInfo>
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

    buildBuildingsDock();
    buildScumSpotsDock();
    buildNatureDock();
}

void MainWindow::buildBuildingsDock()
{
    buildingsPanel_ = new MapEditor::BuildingsPanel(this);

    buildingsDock_ = new QDockWidget(tr("Buildings"), this);
    buildingsDock_->setObjectName(QStringLiteral("BuildingsDock"));
    buildingsDock_->setWidget(buildingsPanel_);
    buildingsDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                    Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, buildingsDock_);
    // Share the space with the battle map panel instead of stacking the
    // right edge with docks.
    tabifyDockWidget(battleMapDock_, buildingsDock_);
    buildingsDock_->hide();

    // The three tools + the building choice + the rotation/HP edits.
    connect(buildingsPanel_, &MapEditor::BuildingsPanel::toolChosen, this,
            [this](int tool) {
                if (!viewport_) {
                    return;
                }
                using VT = MapEditor::ViewportWidget::BuildingTool;
                viewport_->setBuildingTool(tool == 0 ? VT::Place
                                          : tool == 1 ? VT::Select
                                                      : VT::Delete);
                if (tool != 0) {
                    viewport_->setBuildingPreview(-1, 0, true);
                }
            });
    connect(buildingsPanel_, &MapEditor::BuildingsPanel::buildingChosen, this,
            [this](int) {
                // Refresh the placement footprint for the current cursor.
                if (viewport_ && viewport_->cursorCell().x() >= 0) {
                    onBuildingCursorMoved(viewport_->cursorCell().x(),
                                          viewport_->cursorCell().y());
                }
            });
    connect(buildingsPanel_, &MapEditor::BuildingsPanel::rotationEdited, this,
            [this](double deg) {
                if (selectedBuildingRow_ < 0 ||
                    selectedBuildingRow_ >=
                        static_cast<int>(placedBuildings_.size())) {
                    return;
                }
                const MapEditor::SpgFile::PlacedBuilding& b =
                    placedBuildings_[static_cast<std::size_t>(selectedBuildingRow_)];
                if (battleMap_.setBuildingPose(
                        b.index, b.x, b.y, deg)) {
                    reloadPlacedBuildings();
                    statusBar()->showMessage(
                        tr("Building rotation set to %1°.").arg(deg), 3000);
                }
            });
    connect(buildingsPanel_, &MapEditor::BuildingsPanel::hpEdited, this,
            [this](int hp) {
                if (selectedBuildingRow_ < 0 ||
                    selectedBuildingRow_ >=
                        static_cast<int>(placedBuildings_.size())) {
                    return;
                }
                const MapEditor::SpgFile::PlacedBuilding& b =
                    placedBuildings_[static_cast<std::size_t>(selectedBuildingRow_)];
                if (battleMap_.setBuildingHp(b.index, hp)) {
                    reloadPlacedBuildings();
                    statusBar()->showMessage(
                        tr("Building HP set to %1%.").arg(hp), 3000);
                }
            });

    // Viewport tool clicks + cursor tracking for the placement preview.
    connect(viewport_, &MapEditor::ViewportWidget::buildingPlaceRequested,
            this, &MainWindow::onBuildingPlace);
    connect(viewport_, &MapEditor::ViewportWidget::buildingSelectRequested,
            this, &MainWindow::onBuildingSelect);
    connect(viewport_, &MapEditor::ViewportWidget::buildingDeleteRequested,
            this, &MainWindow::onBuildingDelete);
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition, this,
            &MainWindow::onBuildingCursorMoved);
}

void MainWindow::reloadPlacedBuildings()
{
    placedBuildings_ = battleMap_.loaded() ? battleMap_.buildings()
                                           : std::vector<MapEditor::SpgFile::PlacedBuilding>();
    if (selectedBuildingRow_ >=
        static_cast<int>(placedBuildings_.size())) {
        selectedBuildingRow_ = -1;
    }
    syncBuildingsOverlay();
}

void MainWindow::syncBuildingsOverlay()
{
    std::vector<MapEditor::BuildingPoint> points;
    QVector<QPointF> minimap;
    if (battleMap_.loaded()) {
        points.reserve(placedBuildings_.size());
        minimap.reserve(static_cast<int>(placedBuildings_.size()));
        for (int i = 0; i < static_cast<int>(placedBuildings_.size()); ++i) {
            const MapEditor::SpgFile::PlacedBuilding& b =
                placedBuildings_[static_cast<std::size_t>(i)];
            const MapEditor::BuildingInfo* info =
                MapEditor::buildingInfo(b.attributeId);
            MapEditor::BuildingPoint p;
            p.attributeId = b.attributeId;
            p.mapX = b.x;
            p.mapY = b.y;
            p.angleDeg = b.angleDeg;
            p.hp = b.hp;
            p.radiusCells = info ? info->radius : 10.0;
            p.selected = i == selectedBuildingRow_;
            points.push_back(p);
            minimap << QPointF(b.x, b.y);
        }
    }
    if (viewport_) {
        viewport_->setBuildingPoints(points);
    }
    if (minimap_) {
        minimap_->setBuildingPoints(minimap);
    }
}

bool MainWindow::buildingPlacementBlocked(double mapX, double mapY,
                                          double radius) const
{
    for (const MapEditor::SpgFile::PlacedBuilding& b : placedBuildings_) {
        const MapEditor::BuildingInfo* info =
            MapEditor::buildingInfo(b.attributeId);
        const double other = info ? info->radius : 10.0;
        if (std::hypot(b.x - mapX, b.y - mapY) < radius + other) {
            return true;
        }
    }
    return false;
}

void MainWindow::onBuildingCursorMoved(int mapX, int mapY)
{
    if (!viewport_ || !buildingsPanel_ || buildingsPanel_->tool() != 0 ||
        !battleMap_.loaded()) {
        return;
    }
    const int attr = buildingsPanel_->selectedBuilding();
    if (attr < 0) {
        viewport_->setBuildingPreview(-1, 0, true);
        return;
    }
    const MapEditor::BuildingInfo* info = MapEditor::buildingInfo(attr);
    if (!info) {
        return;
    }
    viewport_->setBuildingPreview(
        attr, info->radius,
        !buildingPlacementBlocked(mapX + 0.5, mapY + 0.5, info->radius));
}

void MainWindow::onBuildingPlace(double mapX, double mapY)
{
    if (!battleMap_.loaded()) {
        return;
    }
    const int attr = buildingsPanel_ ? buildingsPanel_->selectedBuilding() : -1;
    const MapEditor::BuildingInfo* info = MapEditor::buildingInfo(attr);
    if (!info) {
        statusBar()->showMessage(
            tr("Pick a building in the Buildings panel first."), 4000);
        return;
    }
    if (!session_ || !session_->isWorldLoaded()) {
        statusBar()->showMessage(
            tr("Load the battle map's world first."), 4000);
        return;
    }
    if (buildingPlacementBlocked(mapX, mapY, info->radius)) {
        viewport_->setBuildingPreview(attr, info->radius, false);
        statusBar()->showMessage(
            tr("The placement intersects another building."), 4000);
        return;
    }
    MapEditor::SpgFile::PlacedBuilding b;
    b.unitId = battleMap_.maxUnitId() + 1;
    b.attributeId = attr;
    b.x = mapX;
    b.y = mapY;
    // Saved buildings stand on the zero layer: z = hZeroPlast - boxMin().z
    // (terBuilding::setPose), i.e. zero layer + the catalog offset.
    b.z = session_->zeroLayerHeight() + info->zOffset;
    b.angleDeg = buildingsPanel_->rotationDeg();
    b.hp = buildingsPanel_->hp();
    if (!battleMap_.addBuilding(b)) {
        statusBar()->showMessage(tr("Failed to place the building."), 4000);
        return;
    }
    selectedBuildingRow_ = -1;
    reloadPlacedBuildings();
    if (buildingsPanel_) {
        buildingsPanel_->setSelectionInfo(
            tr("Placed %1 at (%2, %3) — saved into nobodysBuildings.")
                .arg(QString::fromLatin1(info->name))
                .arg(mapX)
                .arg(mapY));
    }
}

void MainWindow::onBuildingSelect(double mapX, double mapY)
{
    if (!viewport_ || !battleMap_.loaded()) {
        return;
    }
    const int row = viewport_->buildingIndexAtCell(mapX, mapY);
    selectedBuildingRow_ = row;
    if (row >= 0 && row < static_cast<int>(placedBuildings_.size()) &&
        buildingsPanel_) {
        const MapEditor::SpgFile::PlacedBuilding& b =
            placedBuildings_[static_cast<std::size_t>(row)];
        const MapEditor::BuildingInfo* info =
            MapEditor::buildingInfo(b.attributeId);
        buildingsPanel_->setSelectionInfo(
            tr("Selected: %1 at (%2, %3)")
                .arg(QString::fromLatin1(info ? info->name : "?"))
                .arg(b.x)
                .arg(b.y));
        buildingsPanel_->showSelectionParams(b.angleDeg, b.hp);
    } else if (buildingsPanel_) {
        buildingsPanel_->setSelectionInfo(
            tr("Click a building to select it."));
    }
    syncBuildingsOverlay();
}

void MainWindow::onBuildingDelete(double mapX, double mapY)
{
    if (!viewport_ || !battleMap_.loaded()) {
        return;
    }
    const int row = viewport_->buildingIndexAtCell(mapX, mapY);
    if (row < 0 || row >= static_cast<int>(placedBuildings_.size())) {
        return;
    }
    if (!battleMap_.removeBuilding(
            placedBuildings_[static_cast<std::size_t>(row)].index)) {
        statusBar()->showMessage(tr("Failed to remove the building."), 4000);
        return;
    }
    selectedBuildingRow_ = -1;
    reloadPlacedBuildings();
    if (buildingsPanel_) {
        buildingsPanel_->setSelectionInfo(
            tr("Building removed — save the battle map to persist."));
    }
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

void MainWindow::bakeBuildingIconsIfPossible()
{
    if (buildingIconsBaked_ || !viewport_ || !buildingsPanel_) {
        return;
    }
    bool any = false;
    for (std::size_t i = 0; i < MapEditor::kPlaceableBuildingCount; ++i) {
        const int id = MapEditor::kPlaceableBuildings[i].id;
        QIcon icon = viewport_->bakeBuildingIcon(id);
        if (!icon.isNull()) {
            buildingsPanel_->setBuildingIcon(id, icon);
            any = true;
        }
    }
    // Latch only on success — without the GPU viewport every bake fails and
    // a later refresh retries.
    buildingIconsBaked_ = any;
}

void MainWindow::refreshBattleMapUI()
{
    battleMapPanel_->refresh();
    syncSpawnOverlay();
    // The pre-placed buildings panel follows the battle map lifecycle.
    const bool active = battleMap_.loaded();
    if (active) {
        reloadPlacedBuildings();
        reloadScumSpots();
        reloadNatureObjects();
    } else {
        placedBuildings_.clear();
        selectedBuildingRow_ = -1;
        syncBuildingsOverlay();
        placedScumSpots_.clear();
        selectedScumRow_ = -1;
        syncScumOverlay();
        placedNature_.clear();
        selectedNatureRow_ = -1;
        syncNatureOverlay();
        if (viewport_) {
            viewport_->setBuildingTool(
                MapEditor::ViewportWidget::BuildingTool::None);
            viewport_->setBuildingPreview(-1, 0, true);
            viewport_->setScumTool(MapEditor::ViewportWidget::ScumTool::None);
            viewport_->setScumPreview(-1, 0, true);
            viewport_->setNatureTool(MapEditor::ViewportWidget::NatureTool::None);
            viewport_->setNaturePreview(QString(), 0, true);
        }
    }
    if (buildingsPanel_) {
        buildingsPanel_->setActive(active);
        if (active) {
            buildingsPanel_->setSelectionInfo(
                tr("%1 building(s) on the map.")
                    .arg(placedBuildings_.size()));
        }
    }
    if (buildingsDock_) {
        buildingsDock_->setVisible(active);
    }
    if (active) {
        // Bake the building-button icons from the game M3D models on first
        // use (needs the GPU viewport; retries while it stays unavailable).
        bakeBuildingIconsIfPossible();
    }
    if (scumSpotsPanel_) {
        scumSpotsPanel_->setActive(active);
        if (active) {
            scumSpotsPanel_->setSelectionInfo(
                tr("%1 scum nest(s) on the map.")
                    .arg(placedScumSpots_.size()));
        }
    }
    if (scumSpotsDock_) {
        scumSpotsDock_->setVisible(active);
    }
    if (active) {
        bakeScumIconsIfPossible();
    }
    if (naturePanel_) {
        naturePanel_->setActive(active);
        if (active) {
            if (!natureModelsScanned_ && session_ &&
                session_->hasGameDir()) {
                naturePanel_->setModels(scanNatureModels());
                natureModelsScanned_ = true;
            }
            // Zero-layer baseline for the Z spin (placement default).
            naturePanel_->setDefaultZ(battleMap_.zeroLayerHeight());
            naturePanel_->setSelectionInfo(
                tr("%1 object(s) on the map.").arg(placedNature_.size()));
        }
    }
    if (natureDock_) {
        natureDock_->setVisible(active);
    }
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
    // Warn if the file name (without extension) differs from the world name
    // inside the .spg — the game expects them to match.
    checkBattleMapNameMatchesWorld(path, battleMap_.worldName());
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
    syncScumTriggerArtifacts();
    if (!battleMap_.save(battleMapPath_.toStdString())) {
        QMessageBox::warning(this, tr("Save Battle Map"),
                             tr("Failed to write the battle map."));
        return;
    }
    flushScumTriggerFile();
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
    syncScumTriggerArtifacts();
    if (!battleMap_.save(target.u8string())) {
        QMessageBox::warning(this, tr("Save Battle Map"),
                             tr("Failed to write the battle map."));
        return;
    }
    flushScumTriggerFile();
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


// ---------------------------------------------------------------------------
// Difference mode (.gmp) — File menu save/load.
// ---------------------------------------------------------------------------

QString MainWindow::diffGmpPath(bool forSave)
{
    // Default: the loaded battle map's .gmp sibling (the game looks for
    // <map>.gmp next to <map>.spg). Without a battle map, ask for a file.
    if (!battleMapPath_.isEmpty()) {
        QString path = battleMapPath_;
        path.replace(path.size() - 3, 3, QStringLiteral("gmp"));
        if (!forSave && !QFileInfo::exists(path)) {
            QMessageBox::information(
                this, tr("Load Difference"),
                tr("No .gmp found next to the battle map:\n%1").arg(path));
            return QString();
        }
        return path;
    }
    if (forSave) {
        return QFileDialog::getSaveFileName(
            this, tr("Save Difference (.gmp)"), QString(),
            tr("GMP differences (*.gmp);;All Files (*)"));
    }
    return QFileDialog::getOpenFileName(
        this, tr("Load Difference (.gmp)"), QString(),
        tr("GMP differences (*.gmp);;All Files (*)"));
}

void MainWindow::onActionSaveDiff()
{
    if (!session_ || !session_->isWorldLoaded()) {
        return;
    }
    const QString path = diffGmpPath(true);
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo::exists(path) &&
        QMessageBox::question(this, tr("Save Difference"),
                              tr("Overwrite the existing file?\n%1").arg(path)) !=
            QMessageBox::Yes) {
        return;
    }
    if (!session_->saveDiffToGmp(path.toStdString())) {
        QMessageBox::warning(this, tr("Save Difference"),
                             tr("Failed to write '%1'.").arg(path));
        return;
    }
    statusBar()->showMessage(
        tr("Difference data saved to %1 (%2)")
            .arg(path)
            .arg(session_->hasDiffData() ? tr("with data") : tr("empty")),
        6000);
}

void MainWindow::onActionLoadDiff()
{
    if (!session_ || !session_->isWorldLoaded()) {
        return;
    }
    const QString path = diffGmpPath(false);
    if (path.isEmpty()) {
        return;
    }
    // Warn if the file name (without extension) differs from the world name.
    {
        const std::string worldName = battleMap_.loaded()
            ? battleMap_.worldName()
            : session_->currentWorldName();
        checkBattleMapNameMatchesWorld(path, worldName);
    }
    if (!session_->loadDiffFromGmp(path.toStdString())) {
        QMessageBox::warning(
            this, tr("Load Difference"),
            tr("Failed to load the difference data from '%1'.").arg(path));
        return;
    }
    // The buffers may now show different terrain: full repaint.
    refreshPaletteRenderers();
    statusBar()->showMessage(
        tr("Difference data loaded from %1%2")
            .arg(path)
            .arg(session_->isDiffMode()
                     ? QString()
                     : tr(" — switch Difference Mode on (F2) to see it")),
        6000);
}

// ---------------------------------------------------------------------------
// Helper: warn when the file name (without extension) differs from the world
// name.  The game expects battle maps and difference data to share the name
// of the world they belong to (e.g. FORTOP.spg for the "FORTOP" world).
// ---------------------------------------------------------------------------

void MainWindow::checkBattleMapNameMatchesWorld(
    const QString &filePath, const std::string &worldName)
{
    if (worldName.empty()) {
        return;
    }
    const QFileInfo fi(filePath);
    QString fileName = fi.completeBaseName();
    if (fileName.toStdString() == worldName) {
        return;
    }
    QMessageBox::warning(
        this, tr("File name mismatch"),
        tr("Battle map or difference data file name (%1) differs from "
           "the world name (%2).\n"
           "The game expects them to match; a mismatch may cause the game "
           "to break.")
            .arg(fileName, QString::fromStdString(worldName)));
}

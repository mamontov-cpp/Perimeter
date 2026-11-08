// ============================================================================
// BattleMapController — the battle-map (.spg) domain of the map editor.
//
// Absorbed from the former MainWindowBattleMap.cpp / MainWindowScumSpots.cpp /
// MainWindowNature.cpp (MainWindow was a god object, see the code review and
// docs/architecture.md §5.35). The implementations moved verbatim; the only
// changes are the host-service indirections: statusBar()->showMessage ->
// showStatus() (signal statusMessage), updateActionStates() ->
// actionStatesNeedUpdate(), refreshPaletteRenderers() ->
// palettesNeedRefresh(), confirmDiscardUnsavedChanges()/onWorldLoaded() ->
// the worldSwitchGuard_/worldLoadedHook_ callbacks, and the dialog parents
// (this -> window_).
// ============================================================================

#include "BattleMapController.h"

#include "EditorMath.h"

#include "BattleMapPanel.h"
#include "BuildingsPanel.h"
#include "BuildingCatalog.h"
#include "ScumSpotsPanel.h"
#include "ScumSpotCatalog.h"
#include "NaturePanel.h"
#include "EditorSession.h"
#include "MinimapWidget.h"
#include "ViewportWidget.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>

namespace fs = std::filesystem;

namespace MapEditor {

namespace {

// Overlay colors for the spawn rings (distinct per player).
const QColor kSpawnColors[8] = {
    QColor(255, 64, 64),   QColor(64, 96, 255),  QColor(255, 200, 0),
    QColor(80, 200, 80),   QColor(220, 100, 255), QColor(0, 220, 220),
    QColor(255, 128, 0),   QColor(180, 180, 180),
};

} // namespace

BattleMapController::BattleMapController(EditorSession *session,
                                         ViewportWidget *viewport,
                                         MinimapWidget *minimap,
                                         QMainWindow *window, QMenu *viewMenu,
                                         QObject *parent)
    : QObject(parent),
      session_(session),
      viewport_(viewport),
      minimap_(minimap),
      window_(window),
      viewMenu_(viewMenu)
{
}

void BattleMapController::setWorldSwitchGuard(std::function<bool()> guard)
{
    worldSwitchGuard_ = std::move(guard);
}

void BattleMapController::setWorldLoadedHook(std::function<void()> hook)
{
    worldLoadedHook_ = std::move(hook);
}

void BattleMapController::showStatus(const QString &message, int timeoutMs)
{
    emit statusMessage(message, timeoutMs);
}

bool BattleMapController::openPath(const QString &path)
{
    if (!battleMap_.load(path.toStdString())) {
        return false;
    }
    battleMapPath_ = path;
    checkNameMatchesWorld(path, battleMap_.worldName());
    // The world-switch result is ignored here (same as the MCP flow): the
    // map stays loaded even when its world is missing.
    ensureWorld();
    refreshUI();
    return true;
}

bool BattleMapController::saveToCurrentPath()
{
    if (!battleMap_.loaded() || battleMapPath_.isEmpty()) {
        return false;
    }
    normalizeSpawnHeights();
    syncScumTriggerArtifacts();
    if (!battleMap_.save(battleMapPath_.toStdString())) {
        return false;
    }
    flushScumTriggerFile();
    return true;
}

void BattleMapController::newBattleMapFromTemplate(const QString &name,
                                                   const QString &templateMap,
                                                   int playerCount)
{
    onNewBattleMapRequested(name, templateMap, playerCount);
}

void BattleMapController::buildDocks()
{
    buildBattleMapDock();
}

bool BattleMapController::undoPlayerMove()
{
    if (!battleMap_.undoPlayerMove()) {
        return false;
    }
    refreshPlayerPanelAndOverlay();
    return true;
}

bool BattleMapController::redoPlayerMove()
{
    if (!battleMap_.redoPlayerMove()) {
        return false;
    }
    refreshPlayerPanelAndOverlay();
    return true;
}

void BattleMapController::refreshPlayerPanelAndOverlay()
{
    if (battleMapPanel_) {
        battleMapPanel_->refresh();
    }
    syncSpawnOverlay();
}

void BattleMapController::buildBattleMapDock()
{
    battleMapPanel_ = new BattleMapPanel(battleMap_, *session_, window_);
    battleMapPanel_->setViewCenterProvider([this]() {
        return viewport_ ? viewport_->currentViewRect() : QRect();
    });

    battleMapDock_ = new QDockWidget(tr("Battle Map"), window_);
    battleMapDock_->setObjectName(QStringLiteral("BattleMapDock"));
    battleMapDock_->setWidget(battleMapPanel_);
    battleMapDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                    Qt::RightDockWidgetArea |
                                    Qt::TopDockWidgetArea |
                                    Qt::BottomDockWidgetArea);
    battleMapDock_->setFeatures(QDockWidget::DockWidgetClosable |
                                QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable);
    window_->addDockWidget(Qt::RightDockWidgetArea, battleMapDock_);

    if (viewMenu_) {
        viewMenu_->addSeparator();
        QAction *toggleAction = battleMapDock_->toggleViewAction();
        toggleAction->setText(tr("Battle &Map"));
        toggleAction->setToolTip(tr("Show or hide the battle map panel (B)"));
        toggleAction->setShortcut(QKeySequence(Qt::Key_B));
        viewMenu_->addAction(toggleAction);
    }

    connect(battleMapPanel_, &MapEditor::BattleMapPanel::battleMapChanged,
            this, &BattleMapController::syncSpawnOverlay);
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
            this, &BattleMapController::onSpawnPlacementRequested);
    connect(battleMapPanel_,
            &MapEditor::BattleMapPanel::newBattleMapRequested, this,
            &BattleMapController::onNewBattleMapRequested);
    // The panel's "Place Spawn" button asks the window to activate the
    // editor mode (the mode state lives there) — re-emitted upwards.
    connect(battleMapPanel_,
            &MapEditor::BattleMapPanel::placeSpawnRequested, this,
            [this] { emit placeSpawnRequested(); });
    // Remove Player: the panel emits with a valid selection; SpgFile keeps
    // at least one player (removePlayer refuses the last one) — the list,
    // the spawn overlay and the undo availability follow.
    connect(battleMapPanel_,
            &MapEditor::BattleMapPanel::removePlayerRequested, this, [this] {
                const int player = battleMapPanel_->selectedPlayer();
                if (!battleMap_.removePlayer(player)) {
                    showStatus(tr("Failed to remove the player."), 4000);
                    return;
                }
                battleMapPanel_->refresh();
                syncSpawnOverlay();
                emit actionStatesNeedUpdate();
                showStatus(tr("Player removed — save the battle map to "
                              "persist."),
                           5000);
            });

    // Template combo: the battle maps of the current game directory.
    battleMapPanel_->setTemplates(templates());

    buildBuildingsDock();
    buildScumSpotsDock();
    buildNatureDock();
}

void BattleMapController::buildBuildingsDock()
{
    buildingsPanel_ = new BuildingsPanel(window_);

    buildingsDock_ = new QDockWidget(tr("Buildings"), window_);
    buildingsDock_->setObjectName(QStringLiteral("BuildingsDock"));
    buildingsDock_->setWidget(buildingsPanel_);
    buildingsDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                    Qt::RightDockWidgetArea);
    window_->addDockWidget(Qt::RightDockWidgetArea, buildingsDock_);
    // Share the space with the battle map panel instead of stacking the
    // right edge with docks.
    window_->tabifyDockWidget(battleMapDock_, buildingsDock_);
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
                    showStatus(
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
                    showStatus(
                        tr("Building HP set to %1%.").arg(hp), 3000);
                }
            });

    // Viewport tool clicks + cursor tracking for the placement preview.
    connect(viewport_, &MapEditor::ViewportWidget::buildingPlaceRequested,
            this, &BattleMapController::onBuildingPlace);
    connect(viewport_, &MapEditor::ViewportWidget::buildingSelectRequested,
            this, &BattleMapController::onBuildingSelect);
    connect(viewport_, &MapEditor::ViewportWidget::buildingDeleteRequested,
            this, &BattleMapController::onBuildingDelete);
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition, this,
            &BattleMapController::onBuildingCursorMoved);
}

void BattleMapController::reloadPlacedBuildings()
{
    placedBuildings_ = battleMap_.loaded() ? battleMap_.buildings()
                                           : std::vector<MapEditor::SpgFile::PlacedBuilding>();
    if (selectedBuildingRow_ >=
        static_cast<int>(placedBuildings_.size())) {
        selectedBuildingRow_ = -1;
    }
    syncBuildingsOverlay();
}

void BattleMapController::syncBuildingsOverlay()
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

bool BattleMapController::buildingPlacementBlocked(double mapX, double mapY,
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

void BattleMapController::onBuildingCursorMoved(int mapX, int mapY)
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

void BattleMapController::onBuildingPlace(double mapX, double mapY)
{
    if (!battleMap_.loaded()) {
        return;
    }
    const int attr = buildingsPanel_ ? buildingsPanel_->selectedBuilding() : -1;
    const MapEditor::BuildingInfo* info = MapEditor::buildingInfo(attr);
    if (!info) {
        showStatus(
            tr("Pick a building in the Buildings panel first."), 4000);
        return;
    }
    if (!session_ || !session_->isWorldLoaded()) {
        showStatus(
            tr("Load the battle map's world first."), 4000);
        return;
    }
    if (buildingPlacementBlocked(mapX, mapY, info->radius)) {
        viewport_->setBuildingPreview(attr, info->radius, false);
        showStatus(
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
        showStatus(tr("Failed to place the building."), 4000);
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

void BattleMapController::onBuildingSelect(double mapX, double mapY)
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

void BattleMapController::onBuildingDelete(double mapX, double mapY)
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
        showStatus(tr("Failed to remove the building."), 4000);
        return;
    }
    selectedBuildingRow_ = -1;
    reloadPlacedBuildings();
    if (buildingsPanel_) {
        buildingsPanel_->setSelectionInfo(
            tr("Building removed — save the battle map to persist."));
    }
}

QStringList BattleMapController::templates() const
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

bool BattleMapController::ensureWorld()
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
    if (!worldSwitchGuard_()) {
        return false;
    }
    if (!session_->scanWorlds() || !session_->loadWorldByName(worldName)) {
        QMessageBox::warning(
            window_, tr("Open Battle Map"),
            tr("The world \"%1\" of this battle map was not found in "
               "RESOURCE/Worlds.")
                .arg(QString::fromStdString(worldName)));
        return session_->isWorldLoaded();
    }
    worldLoadedHook_();
    return true;
}

void BattleMapController::syncSpawnOverlay()
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

void BattleMapController::onSpawnPlacementRequested(int mapX, int mapY)
{
    if (!battleMap_.loaded()) {
        return;
    }
    const int player = battleMapPanel_->selectedPlayer();
    if (player < 0) {
        showStatus(
            tr("Select a player in the Battle Map panel first — the Place "
               "Spawn mode moves that player's spawn."),
            5000);
        return;
    }

    double oldX = 0;
    double oldY = 0;
    if (!battleMap_.playerAnchor(player, oldX, oldY)) {
        showStatus(tr("Failed to read the player's spawn."),
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
            showStatus(
                tr("Cannot read the terrain height under the old or new "
                   "spawn — the location was not changed."),
                4000);
            return;
        }
        dz = static_cast<double>(newTerrain - oldTerrain);
    }

    if (!battleMap_.setPlayerAnchorUndoable(player, mapX, mapY, dz)) {
        showStatus(tr("Failed to move the spawn group."), 4000);
        return;
    }
    // The .spg is modified in place — Save Battle Map persists it. The panel
    // list and the viewport overlay read the anchors: refresh both, and the
    // undo/redo actions pick up the new history depth.
    battleMapPanel_->refresh();
    syncSpawnOverlay();
    emit actionStatesNeedUpdate();
}

void BattleMapController::bakeBuildingIconsIfPossible()
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

void BattleMapController::refreshUI()
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
    // MainWindow::updateActionStates maps session/battle-map state onto
    // every action (Save/Save As, Open, Validate, autofixes).
    emit actionStatesNeedUpdate();
}

void BattleMapController::openBattleMap()
{
    if (!session_ || !session_->hasGameDir()) {
        QMessageBox::warning(window_, tr("Open Battle Map"),
                             tr("Select a game directory first."));
        return;
    }
    const QString startDir =
        QDir(QString::fromStdString(session_->getGameDir()))
            .filePath(QStringLiteral("RESOURCE/Battle"));
    const QString path = QFileDialog::getOpenFileName(
        window_, tr("Open Battle Map"), startDir,
        tr("Battle maps (*.spg);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (!battleMap_.load(path.toStdString())) {
        QMessageBox::warning(window_, tr("Open Battle Map"),
                             tr("Cannot read the battle map file."));
        return;
    }
    battleMapPath_ = path;
    // Warn if the file name (without extension) differs from the world name
    // inside the .spg — the game expects them to match.
    checkNameMatchesWorld(path, battleMap_.worldName());
    if (!ensureWorld()) {
        // The user cancelled the world switch or the world is missing — the
        // map stays loaded (spawn moves need its world though).
    }
    refreshUI();
}

QString BattleMapController::askSaveName(bool &toWorldsFolder) const
{
    toWorldsFolder = false;
    if (!session_ || !session_->hasGameDir()) {
        return QString();
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        window_, tr("Save Battle Map As"), tr("Map name:"), QLineEdit::Normal,
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

void BattleMapController::normalizeSpawnHeights()
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

void BattleMapController::saveBattleMap()
{
    if (!battleMap_.loaded() || battleMapPath_.isEmpty()) {
        return;
    }
    normalizeSpawnHeights();
    syncScumTriggerArtifacts();
    if (!battleMap_.save(battleMapPath_.toStdString())) {
        QMessageBox::warning(window_, tr("Save Battle Map"),
                             tr("Failed to write the battle map."));
        return;
    }
    flushScumTriggerFile();
    showStatus(tr("Battle map saved: %1").arg(battleMapPath_),
                             5000);
}

void BattleMapController::saveBattleMapAs()
{
    if (!battleMap_.loaded() || !session_ || !session_->hasGameDir()) {
        return;
    }
    bool toWorlds = false;
    const QString name = askSaveName(toWorlds);
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
            window_, tr("Save Battle Map As"),
            tr("%1 already exists. Overwrite?")
                .arg(QString::fromStdString(target.u8string()))) !=
            QMessageBox::Yes) {
        return;
    }
    normalizeSpawnHeights();
    syncScumTriggerArtifacts();
    if (!battleMap_.save(target.u8string())) {
        QMessageBox::warning(window_, tr("Save Battle Map"),
                             tr("Failed to write the battle map."));
        return;
    }
    flushScumTriggerFile();
    battleMapPath_ = QString::fromStdString(target.u8string());
    refreshUI();
    showStatus(tr("Battle map saved: %1").arg(battleMapPath_),
                             5000);
}

void BattleMapController::newBattleMap()
{
    if (!session_ || !session_->hasGameDir()) {
        QMessageBox::warning(window_, tr("New Battle Map"),
                             tr("Select a game directory first."));
        return;
    }
    if (!session_->isWorldLoaded()) {
        QMessageBox::information(
            window_, tr("New Battle Map"),
            tr("Load a world first — the battle map is built for it "
               "(create one with File / New World if needed)."));
        return;
    }
    const QStringList templateNames = templates();
    if (templateNames.isEmpty()) {
        QMessageBox::warning(
            window_, tr("New Battle Map"),
            tr("No template battle maps found in RESOURCE/Battle — a new "
               "battle map is built from one of the shipped maps."));
        return;
    }

    QDialog dialog(window_);
    dialog.setWindowTitle(tr("New Battle Map for '%1'")
                              .arg(QString::fromStdString(
                                  session_->currentWorldName())));
    auto *form = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(tr("MyMap"));
    form->addRow(tr("Map name:"), nameEdit);

    auto *templateCombo = new QComboBox(&dialog);
    templateCombo->addItems(templateNames);
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

void BattleMapController::onNewBattleMapRequested(const QString &name,
                                         const QString &templateMap,
                                         int playerCount)
{
    if (!session_ || !session_->hasGameDir()) {
        return;
    }
    if (!session_->isWorldLoaded()) {
        QMessageBox::information(
            window_, tr("New Battle Map"),
            tr("Load a world first — the new battle map is built for it."));
        return;
    }
    const QString mapName =
        name.isEmpty()
            ? QInputDialog::getText(window_, tr("New Battle Map"),
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
        QMessageBox::warning(window_, tr("New Battle Map"),
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
            window_, tr("New Battle Map"),
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
        QMessageBox::warning(window_, tr("New Battle Map"),
                             tr("Failed to write the battle map."));
        return;
    }
    // Open the fresh map right away.
    battleMap_ = MapEditor::SpgFile();
    if (battleMap_.load(target.u8string())) {
        battleMapPath_ = QString::fromStdString(target.u8string());
    }
    refreshUI();
    showStatus(
        tr("Battle map created: %1").arg(battleMapPath_), 5000);
}


// ---------------------------------------------------------------------------
// Difference mode (.gmp) — File menu save/load.
// ---------------------------------------------------------------------------

QString BattleMapController::diffGmpPath(bool forSave) const
{
    // Default: the loaded battle map's .gmp sibling (the game looks for
    // <map>.gmp next to <map>.spg). Without a battle map, ask for a file.
    if (!battleMapPath_.isEmpty()) {
        QString path = battleMapPath_;
        path.replace(path.size() - 3, 3, QStringLiteral("gmp"));
        if (!forSave && !QFileInfo::exists(path)) {
            QMessageBox::information(
                window_, tr("Load Difference"),
                tr("No .gmp found next to the battle map:\n%1").arg(path));
            return QString();
        }
        return path;
    }
    if (forSave) {
        return QFileDialog::getSaveFileName(
            window_, tr("Save Difference (.gmp)"), QString(),
            tr("GMP differences (*.gmp);;All Files (*)"));
    }
    return QFileDialog::getOpenFileName(
        window_, tr("Load Difference (.gmp)"), QString(),
        tr("GMP differences (*.gmp);;All Files (*)"));
}

void BattleMapController::saveDiffData()
{
    if (!session_ || !session_->isWorldLoaded()) {
        return;
    }
    const QString path = diffGmpPath(true);
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo::exists(path) &&
        QMessageBox::question(window_, tr("Save Difference"),
                              tr("Overwrite the existing file?\n%1").arg(path)) !=
            QMessageBox::Yes) {
        return;
    }
    if (!session_->saveDiffToGmp(path.toStdString())) {
        QMessageBox::warning(window_, tr("Save Difference"),
                             tr("Failed to write '%1'.").arg(path));
        return;
    }
    showStatus(
        tr("Difference data saved to %1 (%2)")
            .arg(path)
            .arg(session_->hasDiffData() ? tr("with data") : tr("empty")),
        6000);
}

void BattleMapController::loadDiffData()
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
        checkNameMatchesWorld(path, worldName);
    }
    if (!session_->loadDiffFromGmp(path.toStdString())) {
        QMessageBox::warning(
            window_, tr("Load Difference"),
            tr("Failed to load the difference data from '%1'.").arg(path));
        return;
    }
    // The buffers may now show different terrain: full repaint.
    emit palettesNeedRefresh();
    showStatus(
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

void BattleMapController::checkNameMatchesWorld(
    const QString &filePath, const std::string &worldName) const
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
        window_, tr("File name mismatch"),
        tr("Battle map or difference data file name (%1) differs from "
           "the world name (%2).\n"
           "The game expects them to match; a mismatch may cause the game "
           "to break.")
            .arg(fileName, QString::fromStdString(worldName)));
}

void BattleMapController::buildScumSpotsDock()
{
    scumSpotsPanel_ = new ScumSpotsPanel(window_);

    scumSpotsDock_ = new QDockWidget(tr("Scum Nests"), window_);
    scumSpotsDock_->setObjectName(QStringLiteral("ScumSpotsDock"));
    scumSpotsDock_->setWidget(scumSpotsPanel_);
    scumSpotsDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                    Qt::RightDockWidgetArea);
    window_->addDockWidget(Qt::RightDockWidgetArea, scumSpotsDock_);
    window_->tabifyDockWidget(battleMapDock_, scumSpotsDock_);
    scumSpotsDock_->hide();

    // The three tools + the mound choice + rotation/HP/filth edits.
    connect(scumSpotsPanel_, &MapEditor::ScumSpotsPanel::toolChosen, this,
            [this](int tool) {
                if (!viewport_) {
                    return;
                }
                using VT = MapEditor::ViewportWidget::ScumTool;
                viewport_->setScumTool(tool == 0 ? VT::Place
                                        : tool == 1 ? VT::Select
                                                    : VT::Delete);
                if (tool != 0) {
                    viewport_->setScumPreview(-1, 0, true);
                }
            });
    connect(scumSpotsPanel_, &MapEditor::ScumSpotsPanel::spotChosen, this,
            [this](int) {
                // Refresh the placement footprint for the current cursor.
                if (viewport_ && viewport_->cursorCell().x() >= 0) {
                    onScumCursorMoved(viewport_->cursorCell().x(),
                                      viewport_->cursorCell().y());
                }
            });
    connect(scumSpotsPanel_, &MapEditor::ScumSpotsPanel::rotationEdited, this,
            [this](double deg) {
                if (selectedScumRow_ < 0 ||
                    selectedScumRow_ >=
                        static_cast<int>(placedScumSpots_.size())) {
                    return;
                }
                const MapEditor::SpgFile::ScumSpot& s =
                    placedScumSpots_[static_cast<std::size_t>(selectedScumRow_)];
                if (battleMap_.setScumSpotPose(s.index, s.x, s.y, deg)) {
                    reloadScumSpots();
                    showStatus(
                        tr("Nest rotation set to %1°.").arg(deg), 3000);
                }
            });
    connect(scumSpotsPanel_, &MapEditor::ScumSpotsPanel::hpEdited, this,
            [this](int hp) {
                if (selectedScumRow_ < 0 ||
                    selectedScumRow_ >=
                        static_cast<int>(placedScumSpots_.size())) {
                    return;
                }
                const MapEditor::SpgFile::ScumSpot& s =
                    placedScumSpots_[static_cast<std::size_t>(selectedScumRow_)];
                if (battleMap_.setScumSpotHp(s.index, hp)) {
                    reloadScumSpots();
                    showStatus(
                        tr("Nest HP set to %1%.").arg(hp), 3000);
                }
            });
    connect(scumSpotsPanel_, &MapEditor::ScumSpotsPanel::filthEdited, this,
            [this](int filthType, int creatureNum, bool sleep,
                   double firstSleepTime, double sleepPeriod,
                   double attackPeriod, int activatingUnit,
                   double activatingDistance, int attackPlayer,
                   bool initialGeoprocess, bool createFirst) {
                pendingFilth_.filthType = filthType;
                pendingFilth_.creatureNum = creatureNum;
                pendingFilth_.sleep = sleep;
                pendingFilth_.firstSleepTime =
                    static_cast<float>(firstSleepTime);
                pendingFilth_.sleepPeriod = static_cast<float>(sleepPeriod);
                pendingFilth_.attackPeriond = static_cast<float>(attackPeriod);
                pendingFilth_.activatingUnit = activatingUnit;
                pendingFilth_.activatingDistance =
                    static_cast<float>(activatingDistance);
                pendingFilth_.attackPlayer = attackPlayer;
                pendingFilth_.initialGeoprocess = initialGeoprocess;
                pendingFilth_.createFirst = createFirst;
                // Apply live to the selected nest.
                if (selectedScumRow_ >= 0 &&
                    selectedScumRow_ <
                        static_cast<int>(placedScumSpots_.size()) &&
                    battleMap_.setScumSpotFilth(
                        placedScumSpots_[static_cast<std::size_t>(selectedScumRow_)]
                            .index,
                        pendingFilth_)) {
                    reloadScumSpots();
                }
            });

    connect(viewport_, &MapEditor::ViewportWidget::scumPlaceRequested, this,
            &BattleMapController::onScumPlace);
    connect(viewport_, &MapEditor::ViewportWidget::scumSelectRequested, this,
            &BattleMapController::onScumSelect);
    connect(viewport_, &MapEditor::ViewportWidget::scumDeleteRequested, this,
            &BattleMapController::onScumDelete);
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition, this,
            &BattleMapController::onScumCursorMoved);
}

void BattleMapController::bakeScumIconsIfPossible()
{
    if (scumIconsBaked_ || !viewport_ || !scumSpotsPanel_) {
        return;
    }
    bool any = false;
    for (const MapEditor::ScumSpotInfo& info : MapEditor::kScumSpots) {
        QIcon icon = viewport_->bakeBuildingIcon(info.id);
        if (!icon.isNull()) {
            scumSpotsPanel_->setSpotIcon(info.id, icon);
            any = true;
        }
    }
    scumIconsBaked_ = any; // retry later while the GPU view is unavailable
}

void BattleMapController::reloadScumSpots()
{
    placedScumSpots_ = battleMap_.loaded() ? battleMap_.scumSpots()
                                           : std::vector<MapEditor::SpgFile::ScumSpot>();
    if (selectedScumRow_ >= static_cast<int>(placedScumSpots_.size())) {
        selectedScumRow_ = -1;
    }
    syncScumOverlay();
}

void BattleMapController::syncScumOverlay()
{
    std::vector<MapEditor::BuildingPoint> points;
    if (battleMap_.loaded()) {
        points.reserve(placedScumSpots_.size());
        for (int i = 0; i < static_cast<int>(placedScumSpots_.size()); ++i) {
            const MapEditor::SpgFile::ScumSpot& s =
                placedScumSpots_[static_cast<std::size_t>(i)];
            MapEditor::BuildingPoint p;
            p.attributeId = s.attributeId;
            p.mapX = s.x;
            p.mapY = s.y;
            p.angleDeg = s.angleDeg;
            p.hp = s.hp;
            // The hit reach doubles as the exclusion radius in the
            // placement check.
            p.radiusCells = MapEditor::kMinScumSpotDistance;
            p.selected = i == selectedScumRow_;
            points.push_back(p);
        }
    }
    if (viewport_) {
        viewport_->setScumPoints(points);
    }
}

void BattleMapController::onScumCursorMoved(int mapX, int mapY)
{
    if (!viewport_ || !scumSpotsPanel_ || scumSpotsPanel_->tool() != 0 ||
        !battleMap_.loaded()) {
        return;
    }
    const int attr = scumSpotsPanel_->selectedSpot();
    if (attr < 0 || !MapEditor::scumSpotInfo(attr)) {
        viewport_->setScumPreview(-1, 0, true);
        return;
    }
    // Refuse when closer than 150 to any existing nest.
    bool valid = true;
    for (const MapEditor::SpgFile::ScumSpot& s : placedScumSpots_) {
        if (std::hypot(s.x - (mapX + 0.5), s.y - (mapY + 0.5)) <
            MapEditor::kMinScumSpotDistance) {
            valid = false;
            break;
        }
    }
    viewport_->setScumPreview(attr, MapEditor::kMinScumSpotDistance, valid);
}

void BattleMapController::onScumPlace(double mapX, double mapY)
{
    if (!battleMap_.loaded()) {
        return;
    }
    const int attr = scumSpotsPanel_ ? scumSpotsPanel_->selectedSpot() : -1;
    const MapEditor::ScumSpotInfo* info = MapEditor::scumSpotInfo(attr);
    if (!info) {
        showStatus(
            tr("Pick a scum spot variant in the Scum Nests panel first."),
            4000);
        return;
    }
    if (!session_ || !session_->isWorldLoaded()) {
        showStatus(tr("Load the battle map's world first."),
                                 4000);
        return;
    }
    for (const MapEditor::SpgFile::ScumSpot& s : placedScumSpots_) {
        if (std::hypot(s.x - mapX, s.y - mapY) <
            MapEditor::kMinScumSpotDistance) {
            viewport_->setScumPreview(attr, MapEditor::kMinScumSpotDistance,
                                      false);
            showStatus(
                tr("Scum nests must be at least %1 units apart.")
                    .arg(MapEditor::kMinScumSpotDistance),
                4000);
            return;
        }
    }
    MapEditor::SpgFile::ScumSpot spot;
    spot.attributeId = attr;
    spot.x = mapX;
    spot.y = mapY;
    // Mound: zero layer + the catalog offset (the game re-snaps z on load).
    spot.z = session_->zeroLayerHeight() + info->zOffset;
    spot.angleDeg = scumSpotsPanel_->rotationDeg();
    spot.hp = scumSpotsPanel_->hp();
    spot.filth = pendingFilth_;
    if (!battleMap_.addScumSpot(spot)) {
        showStatus(tr("Failed to place the scum nest."), 4000);
        return;
    }
    selectedScumRow_ = -1;
    reloadScumSpots();
    // Select the newcomer (it is the last row).
    selectedScumRow_ = static_cast<int>(placedScumSpots_.size()) - 1;
    syncScumOverlay();
    const MapEditor::SpgFile::ScumSpot& placed =
        placedScumSpots_[static_cast<std::size_t>(selectedScumRow_)];
    if (scumSpotsPanel_) {
        scumSpotsPanel_->setSelectionInfo(
            tr("Placed %1 at (%2, %3) with spawner '%4'.")
                .arg(QString::fromLatin1(info->name))
                .arg(mapX)
                .arg(mapY)
                .arg(QString::fromStdString(placed.label)));
        scumSpotsPanel_->showSelectionParams(placed.angleDeg, placed.hp);
        scumSpotsPanel_->setFilthEnabled(placed.paired);
        if (placed.paired) {
            scumSpotsPanel_->showFilthParams(
                battleMap_, placed.filth.filthType,
                placed.filth.creatureNum, placed.filth.sleep,
                placed.filth.firstSleepTime, placed.filth.sleepPeriod,
                placed.filth.attackPeriond, placed.filth.activatingUnit,
                placed.filth.activatingDistance, placed.filth.attackPlayer,
                placed.filth.initialGeoprocess, placed.filth.createFirst);
        }
    }
}

void BattleMapController::onScumSelect(double mapX, double mapY)
{
    if (!viewport_ || !battleMap_.loaded()) {
        return;
    }
    const int row = viewport_->scumIndexAtCell(mapX, mapY);
    selectedScumRow_ = row;
    if (row >= 0 && row < static_cast<int>(placedScumSpots_.size()) &&
        scumSpotsPanel_) {
        const MapEditor::SpgFile::ScumSpot& s =
            placedScumSpots_[static_cast<std::size_t>(row)];
        const MapEditor::ScumSpotInfo* info =
            MapEditor::scumSpotInfo(s.attributeId);
        scumSpotsPanel_->setSelectionInfo(
            tr("Selected: %1 at (%2, %3)%4")
                .arg(QString::fromLatin1(info ? info->name : "?"))
                .arg(s.x)
                .arg(s.y)
                .arg(s.paired ? tr(" (spawner '%1')")
                                  .arg(QString::fromStdString(s.label))
                              : tr(" (no paired spawner)")));
        scumSpotsPanel_->showSelectionParams(s.angleDeg, s.hp);
        scumSpotsPanel_->setFilthEnabled(s.paired);
        if (s.paired) {
            scumSpotsPanel_->showFilthParams(
                battleMap_, s.filth.filthType, s.filth.creatureNum,
                s.filth.sleep, s.filth.firstSleepTime, s.filth.sleepPeriod,
                s.filth.attackPeriond, s.filth.activatingUnit,
                s.filth.activatingDistance, s.filth.attackPlayer,
                s.filth.initialGeoprocess, s.filth.createFirst);
        }
    } else if (scumSpotsPanel_) {
        scumSpotsPanel_->setSelectionInfo(tr("Click a scum nest to select it."));
        scumSpotsPanel_->setFilthEnabled(false);
    }
    syncScumOverlay();
}

void BattleMapController::onScumDelete(double mapX, double mapY)
{
    if (!viewport_ || !battleMap_.loaded()) {
        return;
    }
    const int row = viewport_->scumIndexAtCell(mapX, mapY);
    if (row < 0 || row >= static_cast<int>(placedScumSpots_.size())) {
        return;
    }
    if (!battleMap_.removeScumSpot(
            placedScumSpots_[static_cast<std::size_t>(row)].index)) {
        showStatus(tr("Failed to remove the scum nest."), 4000);
        return;
    }
    selectedScumRow_ = -1;
    reloadScumSpots();
    if (scumSpotsPanel_) {
        scumSpotsPanel_->setSelectionInfo(
            tr("Scum nest removed — save the battle map to persist."));
    }
}

void BattleMapController::syncScumTriggerArtifacts()
{
    if (!battleMap_.loaded()) {
        return;
    }
    // The chain file is named after the WORLD (Scripts/Triggers/<world>.scr).
    std::string world = battleMap_.worldName();
    const std::size_t slash = world.find_last_of("/\\");
    if (slash != std::string::npos) {
        world = world.substr(slash + 1);
    }
    pendingScumChainFile_.clear();
    if (world.empty()) {
        return;
    }
    const std::string chainFile = world + ".scr";
    // Migrate the pre-fix label scheme (label on the spawner) so the
    // trigger wiring can watch the mounds.
    battleMap_.normalizeScumNestLabels();
    const bool hasSpots = !battleMap_.scumSpots().empty();
    battleMap_.syncScumTriggerChain(chainFile, hasSpots);
    pendingScumChainFile_ = QString::fromStdString(chainFile);
    pendingScumChainWrite_ = hasSpots;
}

void BattleMapController::flushScumTriggerFile()
{
    if (pendingScumChainFile_.isEmpty()) {
        return;
    }
    const QString chainFile = pendingScumChainFile_;
    const bool write = pendingScumChainWrite_;
    pendingScumChainFile_.clear();
    if (!session_ || !session_->hasGameDir()) {
        return;
    }
    const fs::path dir =
        fs::u8path(session_->getGameDir()) / "Scripts" / "Triggers";
    const fs::path target = dir / fs::u8path(chainFile.toStdString());
    std::error_code ec;
    if (write) {
        fs::create_directories(dir, ec);
        const std::string text =
            battleMap_.buildScumTriggerScript(chainFile.toStdString());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            QMessageBox::warning(
                window_, tr("Scum Nests"),
                tr("Failed to write the trigger chain file:\n%1")
                    .arg(QString::fromStdString(target.u8string())));
            return;
        }
        out << text;
        out.close();
        showStatus(
            tr("Scum trigger chain written: %1")
                .arg(QString::fromStdString(target.u8string())),
            5000);
    } else if (fs::exists(target, ec)) {
        fs::remove(target, ec);
    }
}

void BattleMapController::buildNatureDock()
{
    naturePanel_ = new NaturePanel(window_);

    natureDock_ = new QDockWidget(tr("Nature"), window_);
    natureDock_->setObjectName(QStringLiteral("NatureDock"));
    natureDock_->setWidget(naturePanel_);
    natureDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                 Qt::RightDockWidgetArea);
    window_->addDockWidget(Qt::RightDockWidgetArea, natureDock_);
    window_->tabifyDockWidget(battleMapDock_, natureDock_);
    natureDock_->hide();

    // The three tools + the model choice + the parameter edits.
    connect(naturePanel_, &MapEditor::NaturePanel::toolChosen, this,
            [this](int tool) {
                if (!viewport_) {
                    return;
                }
                using VT = MapEditor::ViewportWidget::NatureTool;
                viewport_->setNatureTool(tool == 0 ? VT::Place
                                          : tool == 1 ? VT::Select
                                                      : VT::Delete);
                if (tool != 0) {
                    viewport_->setNaturePreview(QString(), 0, true);
                }
            });
    connect(naturePanel_, &MapEditor::NaturePanel::modelChosen, this,
            [this](const QString&) {
                // Refresh the placement footprint for the current cursor.
                if (viewport_ && viewport_->cursorCell().x() >= 0) {
                    onNatureCursorMoved(viewport_->cursorCell().x(),
                                        viewport_->cursorCell().y());
                }
            });

    // Parameter edits apply to the SELECTED object (Place stamps the same
    // controls). One shared lambda keeps all five in sync.
    const auto applyToSelection = [this](const QString& what) {
        if (selectedNatureRow_ < 0 ||
            selectedNatureRow_ >= static_cast<int>(placedNature_.size())) {
            return;
        }
        const MapEditor::SpgFile::NatureObject& n =
            placedNature_[static_cast<std::size_t>(selectedNatureRow_)];
        bool ok = false;
        if (what == QLatin1String("pose")) {
            ok = battleMap_.setNatureObjectPose(
                n.index, n.x, n.y, naturePanel_->z(),
                naturePanel_->rotationDeg());
        } else {
            ok = battleMap_.setNatureObjectShape(
                n.index, naturePanel_->radius(), naturePanel_->flagDestroy(),
                naturePanel_->flagCollisions(), naturePanel_->flagLighting());
        }
        if (ok) {
            reloadNatureObjects();
        }
    };
    connect(naturePanel_, &MapEditor::NaturePanel::rotationEdited, this,
            [applyToSelection](double) { applyToSelection(QStringLiteral("pose")); });
    connect(naturePanel_, &MapEditor::NaturePanel::zEdited, this,
            [applyToSelection](double) { applyToSelection(QStringLiteral("pose")); });
    connect(naturePanel_, &MapEditor::NaturePanel::radiusEdited, this,
            [applyToSelection](double) { applyToSelection(QStringLiteral("shape")); });
    connect(naturePanel_, &MapEditor::NaturePanel::flagsEdited, this,
            [applyToSelection](bool, bool, bool) {
                applyToSelection(QStringLiteral("shape"));
            });

    connect(viewport_, &MapEditor::ViewportWidget::naturePlaceRequested, this,
            &BattleMapController::onNaturePlace);
    connect(viewport_, &MapEditor::ViewportWidget::natureSelectRequested, this,
            &BattleMapController::onNatureSelect);
    connect(viewport_, &MapEditor::ViewportWidget::natureDeleteRequested, this,
            &BattleMapController::onNatureDelete);
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition, this,
            &BattleMapController::onNatureCursorMoved);
}

QStringList BattleMapController::scanNatureModels() const
{
    QStringList models;
    if (!session_ || !session_->hasGameDir()) {
        return models;
    }
    const fs::path root =
        fs::u8path(session_->getGameDir()) / "RESOURCE" / "Models" /
        "Environment";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return models;
    }
    // Same coverage as the game content scan: the root plus ONE subfolder
    // level (GameContent.cpp: resource/models/environment/*.m3d and
    // resource/models/environment/*/*.m3d).
    const auto scanDir = [&models](const fs::path& dir,
                                   const std::string& prefix) {
        std::error_code err;
        for (const auto& entry : fs::directory_iterator(dir, err)) {
            if (!entry.is_regular_file(err)) {
                continue;
            }
            const std::string ext = entry.path().extension().string();
            std::string lower = ext;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower != ".m3d") {
                continue;
            }
            models << QString::fromStdString(
                prefix + entry.path().filename().string());
        }
    };
    scanDir(root, "RESOURCE\\Models\\Environment\\");
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        std::error_code err;
        if (entry.is_directory(err)) {
            scanDir(entry.path(),
                    "RESOURCE\\Models\\Environment\\" +
                        entry.path().filename().string() + "\\");
        }
    }
    models.sort();
    return models;
}

void BattleMapController::reloadNatureObjects()
{
    placedNature_ = battleMap_.loaded() ? battleMap_.natureObjects()
                                        : std::vector<MapEditor::SpgFile::NatureObject>();
    if (selectedNatureRow_ >= static_cast<int>(placedNature_.size())) {
        selectedNatureRow_ = -1;
    }
    syncNatureOverlay();
}

void BattleMapController::syncNatureOverlay()
{
    std::vector<MapEditor::ViewportWidget::NaturePoint> points;
    if (battleMap_.loaded()) {
        points.reserve(placedNature_.size());
        for (int i = 0; i < static_cast<int>(placedNature_.size()); ++i) {
            const MapEditor::SpgFile::NatureObject& n =
                placedNature_[static_cast<std::size_t>(i)];
            MapEditor::ViewportWidget::NaturePoint p;
            p.modelName = QString::fromStdString(n.modelName);
            p.mapX = n.x;
            p.mapY = n.y;
            p.z = n.z;
            p.angleDeg = n.angleDeg;
            p.radius = n.radius;
            p.selected = i == selectedNatureRow_;
            points.push_back(p);
        }
    }
    if (viewport_) {
        viewport_->setNaturePoints(points);
    }
}

void BattleMapController::onNatureCursorMoved(int mapX, int mapY)
{
    if (!viewport_ || !naturePanel_ || naturePanel_->tool() != 0 ||
        !battleMap_.loaded()) {
        return;
    }
    const QString model = naturePanel_->selectedModel();
    if (model.isEmpty()) {
        viewport_->setNaturePreview(QString(), 0, true);
        return;
    }
    viewport_->setNaturePreview(model, naturePanel_->radius(), true);
}

void BattleMapController::onNaturePlace(double mapX, double mapY)
{
    if (!battleMap_.loaded()) {
        return;
    }
    const QString model = naturePanel_ ? naturePanel_->selectedModel()
                                       : QString();
    if (model.isEmpty()) {
        showStatus(
            tr("Pick a model in the Nature panel first."), 4000);
        return;
    }
    if (!session_ || !session_->isWorldLoaded()) {
        showStatus(tr("Load the battle map's world first."),
                                 4000);
        return;
    }
    MapEditor::SpgFile::NatureObject object;
    object.modelName = model.toStdString();
    object.x = mapX;
    object.y = mapY;
    // Zero layer baseline (analogous to the building placement), editable
    // per object via the panel Z spin.
    object.z = naturePanel_->z();
    object.angleDeg = naturePanel_->rotationDeg();
    object.radius = naturePanel_->radius();
    object.destroyable = naturePanel_->flagDestroy();
    object.collisions = naturePanel_->flagCollisions();
    object.lighting = naturePanel_->flagLighting();
    if (!battleMap_.addNatureObject(object)) {
        showStatus(tr("Failed to place the object."), 4000);
        return;
    }
    selectedNatureRow_ = -1;
    reloadNatureObjects();
    selectedNatureRow_ = static_cast<int>(placedNature_.size()) - 1;
    syncNatureOverlay();
    if (naturePanel_) {
        const MapEditor::SpgFile::NatureObject& placed =
            placedNature_[static_cast<std::size_t>(selectedNatureRow_)];
        naturePanel_->setSelectionInfo(
            tr("Placed %1 at (%2, %3).")
                .arg(QString::fromStdString(placed.modelName))
                .arg(mapX)
                .arg(mapY));
    }
}

void BattleMapController::onNatureSelect(double mapX, double mapY)
{
    if (!viewport_ || !battleMap_.loaded()) {
        return;
    }
    const int row = viewport_->natureIndexAtCell(mapX, mapY);
    selectedNatureRow_ = row;
    if (row >= 0 && row < static_cast<int>(placedNature_.size()) &&
        naturePanel_) {
        const MapEditor::SpgFile::NatureObject& n =
            placedNature_[static_cast<std::size_t>(row)];
        naturePanel_->setSelectionInfo(
            tr("Selected: %1 at (%2, %3)")
                .arg(QString::fromStdString(n.modelName))
                .arg(n.x)
                .arg(n.y));
        naturePanel_->showSelectionParams(n.angleDeg, n.z, n.radius);
        naturePanel_->showFlags(n.destroyable, n.collisions, n.lighting);
    } else if (naturePanel_) {
        naturePanel_->setSelectionInfo(
            tr("Click a nature object to select it."));
    }
    syncNatureOverlay();
}

void BattleMapController::onNatureDelete(double mapX, double mapY)
{
    if (!viewport_ || !battleMap_.loaded()) {
        return;
    }
    const int row = viewport_->natureIndexAtCell(mapX, mapY);
    if (row < 0 || row >= static_cast<int>(placedNature_.size())) {
        return;
    }
    if (!battleMap_.removeNatureObject(
            placedNature_[static_cast<std::size_t>(row)].index)) {
        showStatus(tr("Failed to remove the object."), 4000);
        return;
    }
    selectedNatureRow_ = -1;
    reloadNatureObjects();
    if (naturePanel_) {
        naturePanel_->setSelectionInfo(
            tr("Object removed — save the battle map to persist."));
    }
}

} // namespace MapEditor

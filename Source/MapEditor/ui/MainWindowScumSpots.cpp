// ============================================================================
// MainWindow — scum nests (MainWindowScumSpots.cpp).
//
// Split out of MainWindowBattleMap.cpp: the scum nest flow (panel, paired
// placement, trigger artifacts) is self-contained. A nest = a SCUM_SPOT
// mound (SavePrm.nobodysBuildings) + its FILTH_SPOT spawner
// (SavePrm.filth.objects, labeled) + a generated Scripts/Triggers/<world>.scr
// chain registered in every player's TriggerChainNames (see SpgFile and
// docs/architecture.md).
// ============================================================================

#include "MainWindow.h"

#include "EditorSession.h"
#include "ScumSpotsPanel.h"
#include "ScumSpotCatalog.h"
#include "ViewportWidget.h"

#include <QDockWidget>
#include <QMessageBox>
#include <QStatusBar>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

void MainWindow::buildScumSpotsDock()
{
    scumSpotsPanel_ = new MapEditor::ScumSpotsPanel(this);

    scumSpotsDock_ = new QDockWidget(tr("Scum Nests"), this);
    scumSpotsDock_->setObjectName(QStringLiteral("ScumSpotsDock"));
    scumSpotsDock_->setWidget(scumSpotsPanel_);
    scumSpotsDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                    Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, scumSpotsDock_);
    tabifyDockWidget(battleMapDock_, scumSpotsDock_);
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
                    statusBar()->showMessage(
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
                    statusBar()->showMessage(
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
            &MainWindow::onScumPlace);
    connect(viewport_, &MapEditor::ViewportWidget::scumSelectRequested, this,
            &MainWindow::onScumSelect);
    connect(viewport_, &MapEditor::ViewportWidget::scumDeleteRequested, this,
            &MainWindow::onScumDelete);
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition, this,
            &MainWindow::onScumCursorMoved);
}

void MainWindow::bakeScumIconsIfPossible()
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

void MainWindow::reloadScumSpots()
{
    placedScumSpots_ = battleMap_.loaded() ? battleMap_.scumSpots()
                                           : std::vector<MapEditor::SpgFile::ScumSpot>();
    if (selectedScumRow_ >= static_cast<int>(placedScumSpots_.size())) {
        selectedScumRow_ = -1;
    }
    syncScumOverlay();
}

void MainWindow::syncScumOverlay()
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

void MainWindow::onScumCursorMoved(int mapX, int mapY)
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

void MainWindow::onScumPlace(double mapX, double mapY)
{
    if (!battleMap_.loaded()) {
        return;
    }
    const int attr = scumSpotsPanel_ ? scumSpotsPanel_->selectedSpot() : -1;
    const MapEditor::ScumSpotInfo* info = MapEditor::scumSpotInfo(attr);
    if (!info) {
        statusBar()->showMessage(
            tr("Pick a scum spot variant in the Scum Nests panel first."),
            4000);
        return;
    }
    if (!session_ || !session_->isWorldLoaded()) {
        statusBar()->showMessage(tr("Load the battle map's world first."),
                                 4000);
        return;
    }
    for (const MapEditor::SpgFile::ScumSpot& s : placedScumSpots_) {
        if (std::hypot(s.x - mapX, s.y - mapY) <
            MapEditor::kMinScumSpotDistance) {
            viewport_->setScumPreview(attr, MapEditor::kMinScumSpotDistance,
                                      false);
            statusBar()->showMessage(
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
        statusBar()->showMessage(tr("Failed to place the scum nest."), 4000);
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

void MainWindow::onScumSelect(double mapX, double mapY)
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

void MainWindow::onScumDelete(double mapX, double mapY)
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
        statusBar()->showMessage(tr("Failed to remove the scum nest."), 4000);
        return;
    }
    selectedScumRow_ = -1;
    reloadScumSpots();
    if (scumSpotsPanel_) {
        scumSpotsPanel_->setSelectionInfo(
            tr("Scum nest removed — save the battle map to persist."));
    }
}

void MainWindow::syncScumTriggerArtifacts()
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

void MainWindow::flushScumTriggerFile()
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
                this, tr("Scum Nests"),
                tr("Failed to write the trigger chain file:\n%1")
                    .arg(QString::fromStdString(target.u8string())));
            return;
        }
        out << text;
        out.close();
        statusBar()->showMessage(
            tr("Scum trigger chain written: %1")
                .arg(QString::fromStdString(target.u8string())),
            5000);
    } else if (fs::exists(target, ec)) {
        fs::remove(target, ec);
    }
}

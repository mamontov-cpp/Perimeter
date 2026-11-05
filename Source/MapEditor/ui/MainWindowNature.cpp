// ============================================================================
// MainWindow — static nature objects (MainWindowNature.cpp).
//
// Split out of MainWindowBattleMap.cpp: the nature flow (panel, model list
// from the game content, placement/editing) is self-contained. Objects are
// SaveUnitNatureData entries in SavePrm.environment — per-instance M3D
// model, radius-as-footprint, verbatim z, natureFlag bits, always-full HP
// (see SpgFile and docs/architecture.md).
// ============================================================================

#include "MainWindow.h"

#include "EditorSession.h"
#include "NaturePanel.h"
#include "ViewportWidget.h"

#include <QDockWidget>
#include <QStatusBar>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

void MainWindow::buildNatureDock()
{
    naturePanel_ = new MapEditor::NaturePanel(this);

    natureDock_ = new QDockWidget(tr("Nature"), this);
    natureDock_->setObjectName(QStringLiteral("NatureDock"));
    natureDock_->setWidget(naturePanel_);
    natureDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                 Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, natureDock_);
    tabifyDockWidget(battleMapDock_, natureDock_);
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
            &MainWindow::onNaturePlace);
    connect(viewport_, &MapEditor::ViewportWidget::natureSelectRequested, this,
            &MainWindow::onNatureSelect);
    connect(viewport_, &MapEditor::ViewportWidget::natureDeleteRequested, this,
            &MainWindow::onNatureDelete);
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition, this,
            &MainWindow::onNatureCursorMoved);
}

QStringList MainWindow::scanNatureModels() const
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

void MainWindow::reloadNatureObjects()
{
    placedNature_ = battleMap_.loaded() ? battleMap_.natureObjects()
                                        : std::vector<MapEditor::SpgFile::NatureObject>();
    if (selectedNatureRow_ >= static_cast<int>(placedNature_.size())) {
        selectedNatureRow_ = -1;
    }
    syncNatureOverlay();
}

void MainWindow::syncNatureOverlay()
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

void MainWindow::onNatureCursorMoved(int mapX, int mapY)
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

void MainWindow::onNaturePlace(double mapX, double mapY)
{
    if (!battleMap_.loaded()) {
        return;
    }
    const QString model = naturePanel_ ? naturePanel_->selectedModel()
                                       : QString();
    if (model.isEmpty()) {
        statusBar()->showMessage(
            tr("Pick a model in the Nature panel first."), 4000);
        return;
    }
    if (!session_ || !session_->isWorldLoaded()) {
        statusBar()->showMessage(tr("Load the battle map's world first."),
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
        statusBar()->showMessage(tr("Failed to place the object."), 4000);
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

void MainWindow::onNatureSelect(double mapX, double mapY)
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

void MainWindow::onNatureDelete(double mapX, double mapY)
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
        statusBar()->showMessage(tr("Failed to remove the object."), 4000);
        return;
    }
    selectedNatureRow_ = -1;
    reloadNatureObjects();
    if (naturePanel_) {
        naturePanel_->setSelectionInfo(
            tr("Object removed — save the battle map to persist."));
    }
}

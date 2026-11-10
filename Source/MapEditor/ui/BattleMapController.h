#pragma once

#include <QObject>

#include <QString>
#include <QStringList>

#include <functional>
#include <string>
#include <vector>

#include "spg/SpgFile.h"

class QMainWindow;
class QMenu;
class QDockWidget;

namespace MapEditor {
class EditorSession;
class ViewportWidget;
class MinimapWidget;
class BattleMapPanel;
class BuildingsPanel;
class ScumSpotsPanel;
class NaturePanel;

/// Owns the whole battle-map (.spg) domain of the editor: the loaded
/// SpgFile, its four dock panels (Battle Map / Buildings / Scum Nests /
/// Nature), the spawn overlay, the File-menu open/save/save-as/new flow and
/// the difference-mode (.gmp) save/load. Extracted from MainWindow (which was
/// a god object) — see docs/architecture.md §5.35.
///
/// The controller borrows the session/viewport/minimap/window (no ownership
/// transfer) and reports back through signals: statusMessage replaces the
/// statusBar() calls, actionStatesNeedUpdate replaces updateActionStates(),
/// palettesNeedRefresh replaces refreshPaletteRenderers(). The two synchronous
/// host services (the unsaved-changes guard and the post-world-load hook of
/// the world-switch path) are std::function callbacks set by the host.
class BattleMapController : public QObject
{
    Q_OBJECT

public:
    BattleMapController(EditorSession *session, ViewportWidget *viewport,
                        MinimapWidget *minimap, QMainWindow *window,
                        QMenu *viewMenu, QObject *parent = nullptr);

    /// Builds the Battle Map dock (with the View-menu toggle) and the
    /// Buildings / Scum Nests / Nature docks tabified with it, and wires all
    /// panel + viewport signals. Call once, after the session/viewport exist.
    void buildDocks();

    // Host services (set before buildDocks / first use).
    void setWorldSwitchGuard(std::function<bool()> guard);
    void setWorldLoadedHook(std::function<void()> hook);

    // The loaded battle map and its bookkeeping.
    SpgFile &spgFile() { return battleMap_; }
    const SpgFile &spgFile() const { return battleMap_; }
    bool isLoaded() const { return battleMap_.loaded(); }
    QString path() const { return battleMapPath_; }
    BattleMapPanel *panel() const { return battleMapPanel_; }
    /// *.spg file names in RESOURCE/Battle of the current game dir.
    QStringList templates() const;
    /// Mirror of SpgFile::buildings() kept for the overlay/MCP (read-only).
    const std::vector<SpgFile::PlacedBuilding> &placedBuildings() const
    {
        return placedBuildings_;
    }
    /// Re-read the placed objects from the SpgFile and refresh the overlays
    /// (called by the MCP tools after every mutation).
    void reloadPlacedBuildings();
    void reloadScumSpots();
    void reloadNatureObjects();
    /// True when a building of `radius` at (x, y) intersects an existing one
    /// (the MCP place_building guard).
    bool buildingPlacementBlocked(double mapX, double mapY,
                                  double radius) const;
    /// The game folder changed — rescan the nature model list on next use.
    void invalidateNatureModels() { natureModelsScanned_ = false; }

    // File-menu flows (dialog + message boxes use `window` as the parent).
    void openBattleMap();
    void saveBattleMap();
    void saveBattleMapAs();
    void newBattleMap();
    /// MCP variants of the flows above: no dialogs beyond the shared name
    /// mismatch warning, boolean result instead of message boxes.
    bool openPath(const QString &path);
    bool saveToCurrentPath();
    void newBattleMapFromTemplate(const QString &name,
                                  const QString &templateMap, int playerCount);

    // Difference mode (.gmp) — File menu save/load of the difference data.
    void saveDiffData();
    void loadDiffData();

    // Spawn-move history (merged into the global Undo/Redo by the host).
    bool canUndoPlayerMove() const { return battleMap_.canUndoPlayerMove(); }
    bool undoPlayerMove();
    bool canRedoPlayerMove() const { return battleMap_.canRedoPlayerMove(); }
    bool redoPlayerMove();
    /// battleMapPanel_->refresh() + syncSpawnOverlay() after an undo/redo.
    void refreshPlayerPanelAndOverlay();

    // Panel/overlay refresh after external SpgFile mutations (autofixes, MCP).
    void refreshUI();
    void syncSpawnOverlay();
    /// Warn when the file name (without extension) differs from the world
    /// name — the game expects them to be identical.
    void checkNameMatchesWorld(const QString &filePath,
                               const std::string &worldName) const;

signals:
    void statusMessage(const QString &message, int timeoutMs);
    void actionStatesNeedUpdate();
    void palettesNeedRefresh();
    /// The panel's "Place Spawn" button — the host activates the Place Spawn
    /// editor mode (same path as the toolbar button / the S shortcut).
    void placeSpawnRequested();

private:
    void buildBattleMapDock();
    void buildBuildingsDock();
    void buildScumSpotsDock();
    void buildNatureDock();
    void bakeBuildingIconsIfPossible();
    void syncBuildingsOverlay();
    /// Mirror the viewport's exclusive click routing into the three panels'
    /// tool buttons (ViewportWidget::clickRoutingChanged).
    void syncPanelToolButtons();
    void onBuildingPlace(double mapX, double mapY);
    void onBuildingSelect(double mapX, double mapY);
    void onBuildingDelete(double mapX, double mapY);
    void onBuildingCursorMoved(int mapX, int mapY);
    void bakeScumIconsIfPossible();
    void syncScumOverlay();
    void onScumPlace(double mapX, double mapY);
    void onScumSelect(double mapX, double mapY);
    void onScumDelete(double mapX, double mapY);
    void onScumCursorMoved(int mapX, int mapY);
    /// Registers/unregisters the <world>.scr chain in every player's
    /// TriggerChainNames (before a save).
    void syncScumTriggerArtifacts();
    /// Writes/deletes Scripts/Triggers/<world>.scr after a successful save.
    void flushScumTriggerFile();
    void syncNatureOverlay();
    void onNaturePlace(double mapX, double mapY);
    void onNatureSelect(double mapX, double mapY);
    void onNatureDelete(double mapX, double mapY);
    void onNatureCursorMoved(int mapX, int mapY);
    /// Nature models under RESOURCE/Models/Environment (game-content scan).
    QStringList scanNatureModels() const;
    void onNewBattleMapRequested(const QString &name, const QString &templateMap,
                                 int playerCount);
    /// Place Spawn mode click: move the selected battle-map player's spawn
    /// group to the clicked cell (terrain z-correction, undoable through
    /// SpgFile::setPlayerAnchorUndoable).
    void onSpawnPlacementRequested(int mapX, int mapY);
    /// Bring spawn groups resting above the world's zero layer down to it
    /// (SpgFile::restPlayersOnZeroLayer) so the frames start landed in the
    /// game. No-op unless the map's world is loaded.
    void normalizeSpawnHeights();
    QString askSaveName(bool &toWorldsFolder) const;
    /// Switch to the world the loaded battle map refers to (asks the host
    /// guard first). False when the user cancelled or the world is missing.
    bool ensureWorld();
    QString diffGmpPath(bool forSave) const;
    void showStatus(const QString &message, int timeoutMs);

    EditorSession *session_ = nullptr;
    ViewportWidget *viewport_ = nullptr;
    MinimapWidget *minimap_ = nullptr;
    QMainWindow *window_ = nullptr;
    QMenu *viewMenu_ = nullptr;
    std::function<bool()> worldSwitchGuard_;
    std::function<void()> worldLoadedHook_;

    SpgFile battleMap_;
    QString battleMapPath_;
    QDockWidget *battleMapDock_ = nullptr;
    BattleMapPanel *battleMapPanel_ = nullptr;
    /// Pre-placed buildings panel + state (mirror of SpgFile::buildings()).
    QDockWidget *buildingsDock_ = nullptr;
    BuildingsPanel *buildingsPanel_ = nullptr;
    bool buildingIconsBaked_ = false;
    /// Scum nests panel + state (paired SCUM_SPOT mounds / FILTH_SPOT
    /// spawners — see docs/architecture.md).
    QDockWidget *scumSpotsDock_ = nullptr;
    ScumSpotsPanel *scumSpotsPanel_ = nullptr;
    std::vector<SpgFile::ScumSpot> placedScumSpots_;
    int selectedScumRow_ = -1;
    bool scumIconsBaked_ = false;
    /// Nature panel + state (SavePrm.environment SaveUnitNatureData).
    QDockWidget *natureDock_ = nullptr;
    NaturePanel *naturePanel_ = nullptr;
    std::vector<SpgFile::NatureObject> placedNature_;
    int selectedNatureRow_ = -1;
    bool natureModelsScanned_ = false;
    /// Filth parameters the panel currently holds (stamped on placement,
    /// applied to the selection on edit).
    SpgFile::FilthParams pendingFilth_;
    /// Set by syncScumTriggerArtifacts() before a save, consumed by
    /// flushScumTriggerFile() after it: the chain file to write or delete.
    QString pendingScumChainFile_;
    bool pendingScumChainWrite_ = false;
    std::vector<SpgFile::PlacedBuilding> placedBuildings_;
    int selectedBuildingRow_ = -1;
};

} // namespace MapEditor

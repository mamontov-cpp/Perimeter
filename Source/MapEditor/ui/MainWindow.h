#pragma once

#include <QMainWindow>

#include <string>

#include "BattleMapController.h"
#include "BrushTool.h"
#include "ImageDecal.h"
#include "ScriptEditorDialog.h"
#include "Validation.h"
#include "spg/SpgFile.h"

class GameDirManager;
class QCloseEvent;
class QAction;
class QActionGroup;
class QDockWidget;
class QDialog;
class QLabel;
class QMenu;
class QSpinBox;
class QDoubleSpinBox;
class QToolButton;
class QToolBar;

namespace MapEditor {
class EditorSession;
class ViewportWidget;
class MinimapWidget;
class PalettePanel;
class BrushTool;
class BrushUiController;
class ImageIoController;
class McpBridge;
struct View3DCamera;
}

// Top level window of the map editor.
// T0.1 — empty window skeleton.
// T1.1 — integrates EditorSession: game-dir selection, world list, load/unload.
// T1.4 — File menu Save/Save As + Edit menu Undo/Redo/Clear history calling
//        EditorSession, unsaved-changes prompts on load/unload/close.
// T2.2 — integrates ViewportWidget as the central widget.
// T2.3 — View menu + view toolbar for the oblique 3D camera: 2D/3D switch,
//        rotate, tilt, camera lift, zoom, reset; buttons carry live values in
//        their tooltips and get in sync with the viewport via signals.
// T2.4 — MinimapWidget in a dockable panel (View menu toggle action), fed by
//        the viewport's viewRectChanged signal; minimap clicks navigate.
// T3.2 — Brush toolbar: the brush type (effect) buttons, the Geo/Dam brush
//        mode buttons and the stamp form/radius controls drive BrushTool.
// T4.2 — the editor interaction modes (View / Brush / Eyedropper) with their
//        exclusive switch actions in the Edit menu and on the Brush toolbar;
//        the eyedropper is the PickColor brush type in the common brush
//        interface, supported in the 2D and the 3D view alike.
// T4.1 — PalettePanel in a dockable panel (View menu toggle): the Geo/Dam
//        palettes with RGB editing (double click), apply through
//        convertPal2TableSurCol and geoPal.xml sync on save; swatch/dropper
//        selections drive the brush colorIndex.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    /// Gives the user a chance to save unsaved changes before closing.
    void closeEvent(QCloseEvent *event) override;

public:
    // Repro-harness hooks (MAP_EDITOR_AUTOTEST) — not part of the UI flow.
    void debugOpenGameDir(const std::string& dir);
    void debugLoadWorld(const std::string& worldName);
    /// The central viewport widget for the autotest harness (nullptr before
    /// buildViewport).
    MapEditor::ViewportWidget *debugViewport() const { return viewport_; }
    /// The minimap panel for the autotest harness (nullptr before
    /// buildViewport).
    MapEditor::MinimapWidget *debugMinimap() const { return minimap_; }
    /// The brush tool for the autotest harness (nullptr before the toolbar is
    /// built) — lets a repro drive a real stamp through the widget handlers.
    MapEditor::BrushTool *debugBrushTool() const { return brushTool_; }
    /// The battle-map controller for the autotest harness (nullptr before
    /// buildViewport).
    MapEditor::BattleMapController *debugBattleMaps() const { return battleMaps_; }
    /// The Place Spawn mode action for the autotest harness.
    QAction *debugPlaceSpawnAction() const { return actModePlaceSpawn_; }

private slots:
    void onActionSelectGameDir();
    void onActionScanWorlds();
    /// T5.2 — the new-world wizard; creates, loads and saves a new world.
    void onActionNewWorld();
    void onActionLoadWorld();
    void onActionUnloadWorld();
    void onActionSave();
    void onActionSaveAs();
    /// T5.1 — open the World Settings dialog for the loaded world and apply
    /// the changed parameters through EditorSession::applyWorldSettings.
    void onActionWorldParams();
    void onActionUndo();
    void onActionRedo();
    void onActionClearUndoHistory();
    void onZoomToFit();
    void onActionToggle3D();
    /// Switch the Oblique camera to the main game's model: the game
    /// psi/theta/distance + FOV aimed at the first player's frame anchor of
    /// the loaded world (GameCameraPreset). The editor camera model is
    /// restored by onActionEditorCamera().
    void onActionGameCamera();
    /// Switch the Oblique camera back to the editor model.
    void onActionEditorCamera();

    /// React to a world load: update the viewport and status bar.
    void onWorldLoaded();

    /// React to a world unload.
    void onWorldUnload();

    /// Update status bar text with cursor position from the viewport.
    void updateCursorInfo(int mapX, int mapY);

    /// React to the viewport switching its editor interaction mode
    /// (View / Brush / Eyedropper): keep the mode actions checked.
    void onEditorModeChanged(int mode);

    /// Refresh the view readout toolbar after the viewport changed its mode.
    void onViewportViewModeChanged(int mode);

    /// Refresh the view readout toolbar after the 3D camera moved.
    void onObliqueCameraChanged(const MapEditor::View3DCamera &camera);

    /// Centre the viewport on the map cell clicked in the minimap (T2.4).
    void onMinimapCellClicked(int mapX, int mapY);

    /// Handle eyedropper pick: update the color brush palette index (T3.3).
    void onEyedropperPicked(int mapX, int mapY, unsigned char surIndex);
    /// The scripted brush evaluation failed — show the message in a
    /// critical message box (QMessageBox::critical per spec).
    void onBrushScriptError(const QString& message);

    // T6 — image import/export: the actions stay here (updateActionStates
    // tables over them), the flows live in the ImageIoController — these
    // slots are thin wrappers (docs/architecture.md §5.37).
    void onActionImportMapImage();
    void onActionImportUpTga();
    void onActionImportLeveledTexture();
    void onActionExportHeightmap();
    void onActionExportTopView();

    // T8 — battle maps (.spg).
    void onActionOpenBattleMap();
    void onActionSaveBattleMap();
    void onActionSaveBattleMapAs();
    /// T8.4 — the File-menu entry: asks for the name/template/player count
    /// and creates the battle map for the currently loaded world. Thin
    /// wrapper over BattleMapController::newBattleMap().
    void onActionNewBattleMap();

    // T9 — validation + autofixes (Tools menu).
    void onActionValidateWorld();
    void onActionValidateBattleMap();
    void onActionAutofixShuffling();
    void onActionAutofixZeroLayerHeight();
    void onActionRegenerateImages();

private:
    void buildMenuBar();
    void buildEditMenu(QMenuBar *menuBar);
    void buildViewMenu(QMenuBar *menuBar);
    void buildViewToolbar();    void buildStatusBar();
    void buildViewport();
    void buildMinimapDock();
    // T4.1 — the palette panel dock (built right after the minimap dock).
    void buildPaletteDock();
    // T9 — the Tools menu (validation + autofixes).
    void buildToolsMenu();
    /// Shared report dialog with the "Go To" jump wired to the viewport.
    void showValidationReport(const QString &title,
                              const MapEditor::ValidationReport &report);
    // Help menu (F1 — the manual from data/map_editor_help.md).
    void buildHelpMenu();
    void onActionHelp();

    /// Tools → Run Script: the dialog + one-shot execution through the
    /// scripted-brush API (runEditorScript).
    void onActionRunScript();

    /// A script's setGeoPalette/setDamPalette ran in the Qt-free core —
    /// repaint the viewport (palette re-tint) and refresh the palette panel.
    void refreshAfterScriptPaletteEdit();

    // Difference mode (.gmp): File menu save/load of the difference data.
    // The default path is the loaded battle map's .gmp sibling; without a
    // battle map a file dialog is used.
    /// Thin wrappers over BattleMapController::saveDiffData()/loadDiffData().
    void onActionSaveDiff();
    void onActionLoadDiff();

    /// The battle-map domain (panels, .spg open/save/new, spawn overlay,
    /// .gmp difference save/load) — extracted from this class, see
    /// docs/architecture.md §5.35. The T8/T9 action slots below are thin
    /// wrappers over it.
#ifdef MAP_EDITOR_MCP
    // The MCP layer (server + tool dispatch) lives in McpBridge
    // (ui/McpBridge.cpp, docs/architecture.md §5.37).
    MapEditor::McpBridge *mcpBridge_ = nullptr;
#endif

    // T3.2 — create the brush actions + the editor interaction mode
    // (View/Brush/Eyedropper) actions. Called before the menus/toolbars so
    // both can embed them. The brush domain itself lives in the
    // BrushUiController (docs/architecture.md §5.36); only the editor-mode
    // actions stay here (the Edit menu, updateActionStates and the autotest
    // hooks keep them).
    void createBrushActions();
    void createEditorModeActions();
    /// The editor-mode switch body shared by the mode actions (toolbar /
    /// Edit menu), the Place Spawn panel button (via the BattleMapController
    /// signal) and the shortcut path: guards the Place Spawn precondition,
    /// swaps the PickColor brush type for the Eyedropper and pushes the mode
    /// into the viewport. `mode` is a ViewportWidget::EditorMode (int — the
    /// header only forward-declares ViewportWidget).
    void activateEditorMode(int mode);

    /// Put the current world name into the window title:
    /// "Perimeter Map Editor - <world>" while a world is loaded, otherwise
    /// plain "Perimeter Map Editor". Called on load/unload and after
    /// "Save As" rebinds the session onto the new (renamed) world.
    void updateWindowTitle();

    /// Update the toolbar label that mirrors the oblique camera state.
    void updateViewReadout();

    /// Mirror the active 3D camera rotation (ViewportWidget::rotationDeg)
    /// into the View-toolbar rotation spinbox without re-firing its edit
    /// signal. A no-op when the spinbox already shows the value (the camera
    /// reports finer precision than the spinbox's single decimal).
    void syncCameraRotationSpin();

    /// Show the world list dialog so the user can pick a world to load.
    void showWorldListDialog();

    /// Ask what to do with unsaved changes (save / discard / cancel).
    /// Returns true when it is safe to proceed and discard the changes.
    bool confirmDiscardUnsavedChanges();

    /// Ask the user for the target world folder name for "Save As".
    /// Returns an empty string when the dialog was cancelled or the requested
    /// name is unsuitable (empty, contains path separators, exists already
    /// unless overwriting was explicitly accepted...).
    QString askSaveAsName();

    /// Update status bar text based on current session state.
    void updateStatusBar();

    /// T4.1 — a palette edit re-tints every surface pixel: invalidate the
    /// whole 2D cache and force a full 3D redraw (the mip pyramid re-seeds
    /// from the cache).
    void refreshPaletteRenderers();

    /// Enable/disable Save, Save As, Undo, Redo... according to session state.
    void updateActionStates();

    MapEditor::EditorSession *session_ = nullptr;
    GameDirManager *gameDirMgr_ = nullptr;
    MapEditor::ViewportWidget *viewport_ = nullptr;

    // T3.1 — the brush tool driving left-drag editing in the viewport;
    // T3.2+ tool panels will change its parameters.
    MapEditor::BrushTool *brushTool_ = nullptr;

    // T3.2+ — the whole brush UI domain: the type/mode/stamp actions, the
    // Brush / Sculpting / Modifiers / Model toolbars, the control sync and
    // the image/decal/model import flows (docs/architecture.md §5.36).
    MapEditor::BrushUiController *brushUI_ = nullptr;

    // T6 — image import/export flows (docs/architecture.md §5.37).
    MapEditor::ImageIoController *imageIO_ = nullptr;

    // Toggled to reflect session state; owned by the menus.
    QAction *actSave_ = nullptr;
    QAction *actSaveAs_ = nullptr;
    // T5.1 — "World Parameters..." (enabled while a world is loaded).
    QAction *actWorldParams_ = nullptr;
    // T6 — image import/export actions (enabled while a world is loaded).
    QAction *importMapImageAction_ = nullptr;
    QAction *importUpTgaAction_ = nullptr;
    QAction *importLeveledAction_ = nullptr;
    QAction *exportHeightmapAction_ = nullptr;
    QAction *exportTopViewAction_ = nullptr;
    QAction *actUndo_ = nullptr;
    QAction *actRedo_ = nullptr;
    QAction *actClearUndoHistory_ = nullptr;

    // View actions — shared between the View menu and the view toolbar.
    QAction *actGrid_ = nullptr;
    // In-game look: un-dammed cells dim to grey shades (2D + smooth 3D).
    QAction *actDimGeo_ = nullptr;
    // Freeze-dam 3D scroll: repeats render geo-only, the dam stays in place.
    QAction *actFreezeDam_ = nullptr;
    QAction *actZoomToFit_ = nullptr;
    QAction *actView3D_ = nullptr;
    // Renderer style toggle pair: pixel-exact vs bilinear (in-game look) —
    // checkable, exclusive; default is Smooth (set by the viewport widget).
    QAction *actChunkyRender_ = nullptr;
    QAction *actSmoothRender_ = nullptr;
    QAction *actRotateLeft_ = nullptr;
    QAction *actRotateRight_ = nullptr;
    QAction *actTiltUp_ = nullptr;
    QAction *actTiltDown_ = nullptr;
    QAction *actLiftUp_ = nullptr;
    QAction *actLiftDown_ = nullptr;
    QAction *actZoomIn_ = nullptr;
    QAction *actZoomOut_ = nullptr;
    QAction *actResetView_ = nullptr;
    // Camera preset pair (exclusive): the main game's camera model
    // (psi/theta/distance + game FOV) vs the editor camera model — the
    // game variant aims at the first player of the loaded world.
    QAction *actGameCamera_ = nullptr;
    QAction *actEditorCamera_ = nullptr;

    // T3.1 — the editor interaction mode the mouse follows: View navigates,
    // Brush paints, Eyedropper samples the surface color. Exclusive group,
    // shared between the Edit menu and the Brush toolbar; switching to Brush
    // turns a PickColor brush type back into SetColor, switching to
    // Eyedropper selects the PickColor type (§ docs/architecture.md).
    QAction *actModeView_ = nullptr;
    QAction *actModeBrush_ = nullptr;
    QAction *actModeEyedropper_ = nullptr;
    QAction *actModePlaceSpawn_ = nullptr;

    // T3.1 — the brush tool driving left-drag editing in the viewport; its
    // UI (actions, toolbars, imports) lives in the BrushUiController.
    // (brushTool_ itself stays — the viewport, the palette panel, the run-
    // script flow and the MCP layer drive it directly.)

    // Toolbar readout of the current 3D camera.
    QLabel *viewReadout_ = nullptr;

    // Absolute 3D camera rotation (azimuth) spinbox on the View toolbar:
    // mirrors the active camera rotation (editor turnAngleDeg / the game
    // camera psi) and drives it on edit — the same knob as the rotate
    // buttons and the orbit drag.
    QDoubleSpinBox *camRotationSpin_ = nullptr;

    // Editable grid cell size in map cells (the fine grid overlay).
    QSpinBox *gridStepWSpin_ = nullptr;
    QSpinBox *gridStepHSpin_ = nullptr;

    // T2.4 — minimap panel: dockable, shown by default, toggled from the
    // View menu.
    QMenu *viewMenu_ = nullptr;
    QDockWidget *minimapDock_ = nullptr;
    MapEditor::MinimapWidget *minimap_ = nullptr;

    // T4.1 — palette panel: Geo/Dam palettes with RGB editing and selection
    // feeding the brush colorIndex; dockable like the minimap.
    QDockWidget *paletteDock_ = nullptr;
    MapEditor::PalettePanel *palettePanel_ = nullptr;

    // T9 — validation + autofixes (Tools menu).
    QAction *actValidateWorld_ = nullptr;
    QAction *actValidateBattleMap_ = nullptr;
    QAction *actFixShuffling_ = nullptr;
    QAction *actFixZeroLayer_ = nullptr;
    QAction *actRegenerateImages_ = nullptr;
    /// Tools → Run Script (the dialog reuses the scripted-brush API).
    QAction *actRunScript_ = nullptr;
    QString runScript_;

    // T8 — the battle-map domain lives in the controller (see the class doc
    // note above); these File-menu actions stay here as thin wrappers.
    MapEditor::BattleMapController *battleMaps_ = nullptr;
    /// MCP/validation accessors to the loaded battle map (null-safe is the
    /// caller's job — every use site is behind an isLoaded() check).
    MapEditor::SpgFile &battleMap() { return battleMaps_->spgFile(); }
    const MapEditor::SpgFile &battleMap() const { return battleMaps_->spgFile(); }
    QString battleMapPath() const { return battleMaps_->path(); }
    QAction *actOpenBattleMap_ = nullptr;
    QAction *actSaveBattleMap_ = nullptr;
    QAction *actSaveBattleMapAs_ = nullptr;
    QAction *actNewBattleMap_ = nullptr;
    // Difference mode (.gmp): the View toolbar/menu toggle and the File menu
    // save/load of the difference data.
    QAction *actDiffMode_ = nullptr;
    QAction *actSaveDiff_ = nullptr;
    QAction *actLoadDiff_ = nullptr;
};

#pragma once

#include <QMainWindow>

#include <string>

#include "BrushTool.h"
#include "ImageDecal.h"
#include "MarkerStore.h"
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
class QToolButton;
class QToolBar;

namespace MapEditor {
class EditorSession;
class ViewportWidget;
class MinimapWidget;
class PalettePanel;
class MarkerPanel;
class BattleMapPanel;
class BrushTool;
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

    /// T3.5 — open a file dialog, load an image as brush mask.
    void onLoadBrushImage();

    /// ImageDecal — open a file dialog, convert the image into a Dam-palette
    /// limited decal (ImageDecal::prepare), add the new colours to the Dam
    /// palette and make the decal the stamp of the ImageDecal brush.
    void onLoadDecalImage();

    // T6 — image import/export.
    /// T6.3 — paint SurBuf from an image (RGB -> nearest palette index,
    /// indexed TGA -> indices as-is).
    void onActionImportMapImage();
    /// T6.1 — any user image becomes the world's up.tga (512x512 24-bit).
    void onActionImportUpTga();
    /// T6.4 — replace leveledSurfaceTexture.tga with validation.
    void onActionImportLeveledTexture();
    /// T6.2 — 8-bit grayscale heightmap (SGetAlt >> 5).
    void onActionExportHeightmap();
    /// T6.2 — the top view raster as 24-bit TGA.
    void onActionExportTopView();

    // T8 — battle maps (.spg).
    void onActionOpenBattleMap();
    void onActionSaveBattleMap();
    void onActionSaveBattleMapAs();
    /// T8.4 — new battle map from a template (BattleMapPanel request).
    void onNewBattleMapRequested(const QString &name, const QString &templateMap,
                                 int playerCount);
    /// T8.4 — the File-menu entry: asks for the name/template/player count
    /// and creates the battle map for the currently loaded world.
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
    void buildViewToolbar();
    void buildStatusBar();
    void buildViewport();
    void buildMinimapDock();
    // T4.1 — the palette panel dock (built right after the minimap dock).
    void buildPaletteDock();
    // T7.2 — the marker panel dock + View menu toggle.
    void buildMarkerDock();
    // T8 — the battle map panel dock.
    void buildBattleMapDock();
    // T9 — the Tools menu (validation + autofixes).
    void buildToolsMenu();
    /// Shared report dialog with the "Go To" jump wired to the viewport.
    void showValidationReport(const QString &title,
                              const MapEditor::ValidationReport &report);
    // Help menu (F1 — the manual from data/map_editor_help.md).
    void buildHelpMenu();
    void onActionHelp();

    // T8 — battle map helpers: panel/overlay/action refresh, the world the
    // .spg refers to, and the spawn overlay contents.
    void refreshBattleMapUI();
    bool ensureWorldForBattleMap();
    void syncSpawnOverlay();
    QString askBattleMapSaveName(bool &toWorldsFolder) const;
    /// *.spg file names in RESOURCE/Battle of the current game dir.
    QStringList battleMapTemplates() const;

    // T7 — marker sidecar lifecycle: load on world load, autosave on change,
    // clear on unload; feeds the viewport overlay.
    void loadMarkerSidecar();
    void saveMarkerSidecar();
    void syncMarkerOverlay();

    // T3.2 — create the brush type (effect), the brush mode (Geo/Dam) and
    // the editor interaction mode (View/Brush/Eyedropper) actions. Called
    // before the menus/toolbars so both can embed them.
    void createBrushActions();
    // T3.2 — the Brush toolbar: the View/Brush/Eyedropper mode switch, the
    // type buttons, the target-height spinbox of the level tool and the
    // Geo/Dam mode buttons.
    void buildBrushToolbar();
    // T3.2 — re-read the tool parameters into the buttons/spinbox state.
    void syncBrushControls();
    // T3.3 — update the color swatch label and spinbox to reflect the
    // current colorIndex in the brush params.
    void syncColorControls();

    /// Put the current world name into the window title:
    /// "Perimeter Map Editor - <world>" while a world is loaded, otherwise
    /// plain "Perimeter Map Editor". Called on load/unload and after
    /// "Save As" rebinds the session onto the new (renamed) world.
    void updateWindowTitle();

    /// Update the toolbar label that mirrors the oblique camera state.
    void updateViewReadout();

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

    // T3.1 — the editor interaction mode the mouse follows: View navigates,
    // Brush paints, Eyedropper samples the surface color. Exclusive group,
    // shared between the Edit menu and the Brush toolbar; switching to Brush
    // turns a PickColor brush type back into SetColor, switching to
    // Eyedropper selects the PickColor type (§ docs/architecture.md).
    QAction *actModeView_ = nullptr;
    QAction *actModeBrush_ = nullptr;
    QAction *actModeEyedropper_ = nullptr;

    // T3.2 — brush type (effect) actions: raise / lower / level to
    // zeroplast / level to an arbitrary height.
    QAction *actBrushRaise_ = nullptr;
    QAction *actBrushLower_ = nullptr;
    QAction *actBrushLevelZP_ = nullptr;
    QAction *actBrushLevelHeight_ = nullptr;
    // T3.3 — color brush (SetSurf) action.
    QAction *actBrushColor_ = nullptr;
    // T4.2 — eyedropper as a brush type: the PickColor effect. Exclusive
    // member of the brush type group; while it is selected the Brush toolbar
    // acts on the surface pixels by sampling them (editor mode Eyedropper).
    QAction *actBrushPickColor_ = nullptr;
    // T3.4/T5 — dam-marking brushes (soot, very light dam); the zeroplast
    // marking itself happens through the "Level to Zeroplast" brush.
    QAction *actBrushSoot_ = nullptr;
    QAction *actBrushVeryLightDam_ = nullptr;
    // T3.5 — load-image action: the loaded image becomes the stamp mask of
    // every terrain effect (BrushParams::useImageMask) until a stamp shape
    // is picked explicitly.
    QAction *actLoadBrushImage_ = nullptr;
    // ImageDecal — palette-limited image stamp: picks the image, converts it
    // to Dam palette indices and paints them with the SetColor semantics.
    QAction *actBrushImageDecal_ = nullptr;
    // T3.1 — brush form (stamp shape) actions: filled circle/square, the two
    // outlines and the two-point line. Exclusive group, mirrored in the
    // viewport preview and in BrushParams::shape.
    QAction *actShapeCircle_ = nullptr;
    QAction *actShapeSquare_ = nullptr;
    QAction *actShapeCircleOutline_ = nullptr;
    QAction *actShapeSquareOutline_ = nullptr;
    QAction *actShapeLine_ = nullptr;
    // T3.1 — stamp half-size in map cells (BrushParams::radius).
    QSpinBox *brushRadiusSpin_ = nullptr;
    // T3.2 — brush mode actions: which layer the height tools drive.
    QAction *actBrushGeo_ = nullptr;
    QAction *actBrushDam_ = nullptr;
    // T3.2 — AlignVariableH target height (whole voxels 0..255).
    QSpinBox *brushLevelSpin_ = nullptr;
    // T3.3 — palette index spinbox for the color brush (0..255).
    QSpinBox *brushColorIndexSpin_ = nullptr;
    // T3.3 — color swatch label showing the current color preview.
    QLabel *brushColorSwatch_ = nullptr;

    // T3.5 — the image-mask scale control (percent of the loaded image
    // size) and the un-scaled original mask: the scaled mask is recomputed
    // from the original on every change, so rescaling never accumulates
    // interpolation blur.
    QSpinBox *brushMaskScaleSpin_ = nullptr;
    MapEditor::BrushMask brushMaskOriginal_;

    // ImageDecal — the un-scaled palette-index decal: the scale spinbox
    // recomputes the stamp from it, exactly like the brush-image mask above.
    MapEditor::ImageDecal::DecalImage decalOriginal_;

    // Toolbar readout of the current 3D camera.
    QLabel *viewReadout_ = nullptr;

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

    // T7 — markers: sidecar-backed store, dock panel and View menu toggle.
    MapEditor::MarkerStore markerStore_;
    QDockWidget *markerDock_ = nullptr;
    MapEditor::MarkerPanel *markerPanel_ = nullptr;

    // T9 — validation + autofixes (Tools menu).
    QAction *actValidateWorld_ = nullptr;
    QAction *actValidateBattleMap_ = nullptr;
    QAction *actFixShuffling_ = nullptr;
    QAction *actFixZeroLayer_ = nullptr;
    QAction *actRegenerateImages_ = nullptr;

    // T8 — the loaded battle map (.spg), its dock panel and File actions.
    MapEditor::SpgFile battleMap_;
    QString battleMapPath_;
    QDockWidget *battleMapDock_ = nullptr;
    MapEditor::BattleMapPanel *battleMapPanel_ = nullptr;
    QAction *actOpenBattleMap_ = nullptr;
    QAction *actSaveBattleMap_ = nullptr;
    QAction *actSaveBattleMapAs_ = nullptr;
    QAction *actNewBattleMap_ = nullptr;
};

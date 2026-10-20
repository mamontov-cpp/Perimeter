#pragma once

#include <QMainWindow>

class GameDirManager;
class QCloseEvent;
class QAction;
class QDockWidget;
class QDialog;
class QLabel;
class QMenu;
class QToolBar;

namespace MapEditor {
class EditorSession;
class ViewportWidget;
class MinimapWidget;
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
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    /// Gives the user a chance to save unsaved changes before closing.
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onActionSelectGameDir();
    void onActionScanWorlds();
    void onActionLoadWorld();
    void onActionUnloadWorld();
    void onActionSave();
    void onActionSaveAs();
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

    /// Refresh the view readout toolbar after the viewport changed its mode.
    void onViewportViewModeChanged(int mode);

    /// Refresh the view readout toolbar after the 3D camera moved.
    void onObliqueCameraChanged(const MapEditor::View3DCamera &camera);

    /// Centre the viewport on the map cell clicked in the minimap (T2.4).
    void onMinimapCellClicked(int mapX, int mapY);

private:
    void buildMenuBar();
    void buildEditMenu(QMenuBar *menuBar);
    void buildViewMenu(QMenuBar *menuBar);
    void buildViewToolbar();
    void buildStatusBar();
    void buildViewport();
    void buildMinimapDock();

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
    QAction *actUndo_ = nullptr;
    QAction *actRedo_ = nullptr;
    QAction *actClearUndoHistory_ = nullptr;

    // View actions — shared between the View menu and the view toolbar.
    QAction *actGrid_ = nullptr;
    QAction *actZoomToFit_ = nullptr;
    QAction *actView3D_ = nullptr;
    QAction *actRotateLeft_ = nullptr;
    QAction *actRotateRight_ = nullptr;
    QAction *actTiltUp_ = nullptr;
    QAction *actTiltDown_ = nullptr;
    QAction *actLiftUp_ = nullptr;
    QAction *actLiftDown_ = nullptr;
    QAction *actZoomIn_ = nullptr;
    QAction *actZoomOut_ = nullptr;
    QAction *actResetView_ = nullptr;

    // T3.1 — Edit-menu toggle for brush painting (also mirrored in the
    // toolbar icon set).
    QAction *actBrushEdit_ = nullptr;

    // Toolbar readout of the current 3D camera.
    QLabel *viewReadout_ = nullptr;

    // T2.4 — minimap panel: dockable, shown by default, toggled from the
    // View menu.
    QMenu *viewMenu_ = nullptr;
    QDockWidget *minimapDock_ = nullptr;
    MapEditor::MinimapWidget *minimap_ = nullptr;
};

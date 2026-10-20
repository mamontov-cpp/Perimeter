#pragma once

#include <QMainWindow>

class GameDirManager;
class QCloseEvent;
class QAction;
class QDialog;

namespace MapEditor {
class EditorSession;
class ViewportWidget;
}

// Top level window of the map editor.
// T0.1 — empty window skeleton.
// T1.1 — integrates EditorSession: game-dir selection, world list, load/unload.
// T1.4 — File menu Save/Save As + Edit menu Undo/Redo/Clear history calling
//        EditorSession, unsaved-changes prompts on load/unload/close.
// T2.2 — integrates ViewportWidget as the central widget.
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

    /// React to a world load: update the viewport and status bar.
    void onWorldLoaded();

    /// React to a world unload.
    void onWorldUnload();

    /// Update status bar text with cursor position from the viewport.
    void updateCursorInfo(int mapX, int mapY);

private:
    void buildMenuBar();
    void buildEditMenu(QMenuBar *menuBar);
    void buildViewMenu(QMenuBar *menuBar);
    void buildStatusBar();
    void buildViewport();

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

    // Toggled to reflect session state; owned by the menus.
    QAction *actSave_ = nullptr;
    QAction *actSaveAs_ = nullptr;
    QAction *actUndo_ = nullptr;
    QAction *actRedo_ = nullptr;
    QAction *actClearUndoHistory_ = nullptr;
};

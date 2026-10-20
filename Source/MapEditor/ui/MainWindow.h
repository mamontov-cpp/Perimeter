#pragma once

#include <QMainWindow>

class GameDirManager;
class QDialog;

namespace MapEditor {
class EditorSession;
class ViewportWidget;
}

// Top level window of the map editor.
// T0.1 — empty window skeleton.
// T1.1 — integrates EditorSession: game-dir selection, world list, load/unload.
// T2.2 — integrates ViewportWidget as the central widget.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onActionSelectGameDir();
    void onActionScanWorlds();
    void onActionLoadWorld();
    void onActionUnloadWorld();
    void onZoomToFit();

    /// React to a world load: update the viewport and status bar.
    void onWorldLoaded();

    /// React to a world unload.
    void onWorldUnload();

    /// Update status bar text with cursor position from the viewport.
    void updateCursorInfo(int mapX, int mapY);

private:
    void buildMenuBar();
    void buildViewMenu(QMenuBar *menuBar);
    void buildStatusBar();
    void buildViewport();

    /// Show the world list dialog so the user can pick a world to load.
    void showWorldListDialog();

    /// Update status bar text based on current session state.
    void updateStatusBar();

    MapEditor::EditorSession *session_ = nullptr;
    GameDirManager *gameDirMgr_ = nullptr;
    MapEditor::ViewportWidget *viewport_ = nullptr;
};

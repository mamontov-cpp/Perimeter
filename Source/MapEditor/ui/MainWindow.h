#pragma once

#include <QMainWindow>

class GameDirManager;
class QDialog;

namespace MapEditor {
class EditorSession;
}

// Top level window of the map editor.
// T0.1 — empty window skeleton.
// T1.1 — integrates EditorSession: game-dir selection, world list, load/unload.
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

private:
    void buildMenuBar();
    void buildStatusBar();

    /// Show the world list dialog so the user can pick a world to load.
    void showWorldListDialog();

    /// Update status bar text based on current session state.
    void updateStatusBar();

    MapEditor::EditorSession *session_ = nullptr;
    GameDirManager *gameDirMgr_ = nullptr;
};

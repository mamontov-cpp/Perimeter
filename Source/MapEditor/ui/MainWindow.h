#pragma once

#include <QMainWindow>

// Top level window of the map editor.
// Viewport, tool panels and menus are wired in later tasks; T0.1 only brings up an empty window.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
};

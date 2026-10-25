#pragma once

#include <QWidget>

#include "MarkerStore.h"

class QListWidget;
class QPushButton;

// ============================================================================
// MarkerPanel (T7.2) — the dockable marker list: add (at a provided default
// position), edit (label / type / note / color / position / zone radius),
// delete and "go to". The store is the single source of truth; every change
// is written back immediately and announced through markersChanged() (the
// MainWindow persists the sidecar and feeds the viewport overlay).
// ============================================================================
namespace MapEditor {

class MarkerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MarkerPanel(MapEditor::MarkerStore &store, QWidget *parent = nullptr);

    /// Rebuild the list from the store (world load, external change).
    void refresh();

    /// Where "Add" places a new marker (viewport centre, set by MainWindow).
    void setDefaultPosition(int x, int y) { defaultX_ = x; defaultY_ = y; }

signals:
    /// Any store mutation (add/update/delete) — autosave + overlay refresh.
    void markersChanged();
    /// "Go to" requested for a marker position.
    void goToMarker(int mapX, int mapY);

private slots:
    void onAdd();
    void onEdit();
    void onDelete();
    void onGoTo();
    void onSelectionChanged();

private:
    /// Opens the add/edit dialog; returns true when the store changed.
    bool runEditorDialog(MapEditor::MapMarkerData &marker, bool isNew);

    MapEditor::MarkerStore &store_;
    QListWidget *list_ = nullptr;
    QPushButton *editButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QPushButton *goButton_ = nullptr;
    int defaultX_ = 0;
    int defaultY_ = 0;
};

} // namespace MapEditor

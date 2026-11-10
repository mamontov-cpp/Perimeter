#pragma once

#include <QWidget>

#include <functional>

#include "spg/SpgFile.h"

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace MapEditor {
class EditorSession;
}

// ============================================================================
// BattleMapPanel (T8.2/T8.3) — the dockable battle-map editor:
//
//   * header: loaded .spg name, its world and zeroLayerHeight;
//   * the player list (playersData) with the spawn anchor of each player;
//   * "Go To" centres the viewport on the selected spawn;
//   * "Move to View Centre" relocates the whole spawn group (frame + squad +
//     frame slots + camera) with the terrain z-correction of the loaded
//     world (SpgFile::setPlayerAnchor + EditorSession::terrainHeightAt);
//   * "Edit Properties..." changes playersData (name / type / belligerent /
//     color / clan / difficulty / handicap) and the spawn location (X/Y,
//     moved with the same terrain z-correction as "Move to View Centre");
//   * selectedPlayer() feeds the Place Spawn editor mode: a viewport click
//     moves that player's spawn group (undoable, SpgFile history).
//
// The SpgFile itself lives in the MainWindow; the panel edits it through the
// reference and announces every mutation via battleMapChanged() (autosave
// decision is the caller's).
// ============================================================================
namespace MapEditor {

class BattleMapPanel : public QWidget
{
    Q_OBJECT

public:
    BattleMapPanel(SpgFile &battleMap, EditorSession &session,
                   QWidget *parent = nullptr);

    /// Rebuild the list from the SpgFile (after load/save/new).
    void refresh();

    /// Index of the player selected in the list (-1 when none). The Place
    /// Spawn mode places THIS player's spawn group.
    int selectedPlayer() const;

    /// Where "Move to View Centre" places the spawn — the MainWindow wires
    /// this to the viewport's current view rectangle.
    void setViewCenterProvider(std::function<QRect()> provider)
    {
        viewCenterProvider_ = std::move(provider);
    }

    /// Fill the "New Battle Map" template combo (RESOURCE/Battle names).
    void setTemplates(const QStringList &templateNames);

    QString battleMapTitle() const;

signals:
    /// The spawn overlay should be refreshed (positions changed).
    void battleMapChanged();
    /// Navigate the viewport to a map cell.
    void goToCell(int mapX, int mapY);
    /// A new battle map was requested (template + player count; the file
    /// always takes the loaded world's name).
    void newBattleMapRequested(const QString &templateMap, int playerCount);
    /// "Remove Player" clicked with a selected list row — the caller removes
    /// the player from the SpgFile and refreshes (SpgFile keeps at least
    /// one player; the signal is not emitted for the empty selection).
    void removePlayerRequested();
    /// "Place Spawn" clicked — the caller activates the Place Spawn editor
    /// mode (the panel only requests; the editor-mode state lives there).
    void placeSpawnRequested();

private slots:
    void onGoTo();
    void onMoveToViewCenter();
    void onEditProperties();

private:
    SpgFile &battleMap_;
    EditorSession &session_;
    QListWidget *list_ = nullptr;
    QLabel *header_ = nullptr;
    QComboBox *newTemplateCombo_ = nullptr;
    QSpinBox *newPlayerCountSpin_ = nullptr;
    QPushButton *placeSpawnButton_ = nullptr;
    std::function<QRect()> viewCenterProvider_;
};

} // namespace MapEditor

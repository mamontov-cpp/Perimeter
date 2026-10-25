#pragma once

#include <QWidget>

#include <functional>

#include "spg/SpgFile.h"

class QComboBox;
class QLabel;
class QListWidget;
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
//     color / clan / difficulty / handicap).
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
    /// A new battle map wizard finished: name + player count requested.
    void newBattleMapRequested(const QString &name, const QString &templateMap,
                               int playerCount);

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
    std::function<QRect()> viewCenterProvider_;
};

} // namespace MapEditor

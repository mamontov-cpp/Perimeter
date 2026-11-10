#pragma once

#include <QHash>
#include <QList>
#include <QWidget>

#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QSpinBox;
class QToolButton;

// ============================================================================
// ScumSpotsPanel — scum nests: paired SCUM_SPOT mounds + FILTH_SPOT spawners.
//
// Shown when a battle map is loaded or created:
//   * the tool row: Place / Select / Delete (fully analogous to the
//     Buildings panel);
//   * the mound grid: one toggle button per SCUM_SPOT variant (43..46);
//   * rotation + HP spins (Place stamps with them, Select edits the
//     selected nest);
//   * the filth parameter block — editable for the SELECTED nest when it
//     has a paired spawner (filth type, spawn cadence, target, ...).
// The panel owns no data — it announces the choices and MainWindow applies
// them through SpgFile.
// ============================================================================

namespace MapEditor {

class SpgFile;

class ScumSpotsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ScumSpotsPanel(QWidget* parent = nullptr);

    void setActive(bool active);

    int selectedSpot() const;          ///< attribute id (-1 when none).
    /// The active tool: 0 place, 1 select, 2 delete, -1 none (the normal
    /// brush handles clicks). The tools are mutually exclusive — within the
    /// panel by the buttons themselves, across the panels through the
    /// viewport's click routing (see ViewportWidget::clickRoutingChanged).
    int tool() const { return tool_; }
    /// Mirror the active tool into the buttons WITHOUT emitting toolChosen
    /// (-1 unchecks all). The viewport calls this when another panel's tool
    /// or a brush/editor mode takes the click over.
    void setActiveTool(int index);
    double rotationDeg() const;
    int hp() const;

    void setSelectionInfo(const QString& text);
    void showSelectionParams(double deg, int hp);
    /// Mirror the paired spawner's parameters WITHOUT emitting.
    void showFilthParams(const SpgFile& schema, int filthType, int creatureNum,
                         bool sleep, double firstSleepTime, double sleepPeriod,
                         double attackPeriod, int activatingUnit,
                         double activatingDistance, int attackPlayer,
                         bool initialGeoprocess, bool createFirst);
    /// Enable the filth block (a paired nest is selected).
    void setFilthEnabled(bool enabled);
    /// Swap a mound button to the baked model icon.
    void setSpotIcon(int attributeId, const QIcon& icon);

signals:
    void spotChosen(int attributeId);
    void toolChosen(int tool);
    void rotationEdited(double deg);
    void hpEdited(int hp);
    void filthEdited(int filthType, int creatureNum, bool sleep,
                     double firstSleepTime, double sleepPeriod,
                     double attackPeriod, int activatingUnit,
                     double activatingDistance, int attackPlayer,
                     bool initialGeoprocess, bool createFirst);

private:
    void onSpotToggled(int id, bool checked);
    void emitFilth();

    QButtonGroup* spotGroup_ = nullptr;
    QHash<int, QToolButton*> spotButtons_;
    /// The Place/Select/Delete row, in tool-index order.
    QList<QToolButton*> toolButtons_;
    QLabel* selection_ = nullptr;
    QDoubleSpinBox* rotationSpin_ = nullptr;
    QSpinBox* hpSpin_ = nullptr;

    QComboBox* filthType_ = nullptr;
    QSpinBox* creatureNum_ = nullptr;
    QCheckBox* sleep_ = nullptr;
    QDoubleSpinBox* firstSleepTime_ = nullptr;
    QDoubleSpinBox* sleepPeriod_ = nullptr;
    QDoubleSpinBox* attackPeriod_ = nullptr;
    QComboBox* activatingUnit_ = nullptr;
    QDoubleSpinBox* activatingDistance_ = nullptr;
    QComboBox* attackPlayer_ = nullptr;
    QCheckBox* initialGeoprocess_ = nullptr;
    QCheckBox* createFirst_ = nullptr;
    QWidget* filthBox_ = nullptr;

    int tool_ = -1;
    bool updatingSpins_ = false;
};

} // namespace MapEditor

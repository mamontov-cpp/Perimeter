#pragma once

#include <QWidget>

#include <vector>

class QButtonGroup;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QSpinBox;
class QToolButton;

#include <QHash>

// ============================================================================
// BuildingsPanel — pre-placed battle-map buildings (SavePrm.nobodysBuildings).
//
// Shown when a battle map is loaded or created:
//   * the tool row: Place / Select / Delete;
//   * the building grid: one toggle button per placeable building (the 43
//     entries of BuildingCatalog). Button icons are baked from the game's
//     M3D models through the viewport's GPU scene (the same models the
//     game's 3D build menu shows); until they arrive the buttons show the
//     plain text name (no game folder / no GPU viewport);
//   * the rotation angle (degrees) and HP (percent) the Place tool stamps
//     with, and the Select tool edits on the selected building.
// The panel owns no data — it announces the choices and MainWindow applies
// them through SpgFile.
// ============================================================================

namespace MapEditor {

class BuildingsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BuildingsPanel(QWidget* parent = nullptr);

    /// Enable/disable the whole panel (needs a loaded battle map).
    void setActive(bool active);

    /// The building toggled in the grid (-1 when none).
    int selectedBuilding() const;
    /// The active tool: 0 place, 1 select, 2 delete.
    int tool() const { return tool_; }
    double rotationDeg() const;
    int hp() const;

    /// Show the selected building's info (empty clears).
    void setSelectionInfo(const QString& text);
    /// Mirror the selected building's rotation/HP into the spins WITHOUT
    /// emitting the edit signals.
    void showSelectionParams(double deg, int hp);
    /// Swap a building button to the baked model icon (name moves under
    /// the icon). Null/empty icons are ignored.
    void setBuildingIcon(int attributeId, const QIcon& icon);

signals:
    /// A building button was toggled (-1 when untoggled).
    void buildingChosen(int attributeId);
    /// The tool changed (0 place, 1 select, 2 delete).
    void toolChosen(int tool);
    /// The rotation spin was edited by the user (apply to the selection).
    void rotationEdited(double deg);
    /// The HP spin was edited by the user (apply to the selection).
    void hpEdited(int hp);

private:
    void onBuildingToggled(int id, bool checked);

    QButtonGroup* buildingGroup_ = nullptr;
    QGridLayout* grid_ = nullptr;
    QHash<int, QToolButton*> buildingButtons_;
    QLabel* selection_ = nullptr;
    QDoubleSpinBox* rotationSpin_ = nullptr;
    QSpinBox* hpSpin_ = nullptr;
    int tool_ = 0;
    bool updatingSpins_ = false;
};

} // namespace MapEditor

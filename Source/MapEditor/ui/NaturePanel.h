#pragma once

#include <QWidget>

#include <QStringList>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QToolButton;

// ============================================================================
// NaturePanel — static nature objects (SavePrm.environment).
//
// Shown when a battle map is loaded or created:
//   * the tool row: Place / Select / Delete (fully analogous to the
//     Buildings panel);
//   * the model list: the M3D files under RESOURCE/Models/Environment
//     (root + one subfolder level) — the same content scan the game uses;
//     populated when a game directory is selected;
//   * parameters the Place tool stamps and the Select tool edits:
//     rotation, Z (stored verbatim — nature objects do not re-snap on
//     load), radius (footprint — scales the model), and the natureFlag
//     bits (Destroy / Collisions / Lighting).
// HP is not editable — always a full damageMolecula.
// The panel owns no data — it announces the choices and MainWindow applies
// them through SpgFile.
// ============================================================================

namespace MapEditor {

class NaturePanel : public QWidget
{
    Q_OBJECT

public:
    explicit NaturePanel(QWidget* parent = nullptr);

    void setActive(bool active);

    int tool() const { return tool_; } ///< 0 place, 1 select, 2 delete.
    QString selectedModel() const;     ///< "" when none.
    double rotationDeg() const;
    double z() const;
    double radius() const;
    bool flagDestroy() const;
    bool flagCollisions() const;
    bool flagLighting() const;

    void setSelectionInfo(const QString& text);
    /// Mirror the selected object's params WITHOUT emitting.
    void showSelectionParams(double deg, double z, double radius);
    void showFlags(bool destroy, bool collisions, bool lighting);
    /// Seed the Z spin (the zero layer height of the loaded world).
    void setDefaultZ(double z);
    /// Populate the model list (game-content scan, sorted).
    void setModels(const QStringList& models);

signals:
    void modelChosen(QString modelName);
    void toolChosen(int tool);
    void rotationEdited(double deg);
    void zEdited(double z);
    void radiusEdited(double radius);
    void flagsEdited(bool destroy, bool collisions, bool lighting);

private:
    void emitParams();

    QLabel* selection_ = nullptr;
    QListWidget* modelList_ = nullptr;
    QDoubleSpinBox* rotationSpin_ = nullptr;
    QDoubleSpinBox* zSpin_ = nullptr;
    QDoubleSpinBox* radiusSpin_ = nullptr;
    QCheckBox* flagDestroy_ = nullptr;
    QCheckBox* flagCollisions_ = nullptr;
    QCheckBox* flagLighting_ = nullptr;
    int tool_ = 0;
    bool updatingSpins_ = false;
};

} // namespace MapEditor

// ============================================================================
// NaturePanel implementation — see the header.
// ============================================================================

#include "NaturePanel.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QAbstractItemView>
#include <QToolButton>
#include <QVBoxLayout>

namespace MapEditor {

NaturePanel::NaturePanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);

    selection_ = new QLabel(tr("No battle map loaded."), this);
    selection_->setWordWrap(true);
    layout->addWidget(selection_);

    // --- Tools: Place / Select / Delete -------------------------------
    // Mutually exclusive (like the panel tools of the other panels and the
    // editor modes in the viewport): checking one resets the siblings, and
    // unchecking the active one (tool -1) hands the click back to the normal
    // brush.
    auto* tools = new QHBoxLayout;
    const auto addTool = [this, tools](int index, const QString& text,
                                       const QString& tip) {
        auto* button = new QToolButton(this);
        button->setText(text);
        button->setCheckable(true);
        button->setToolTip(tip);
        connect(button, &QToolButton::clicked, this,
                [this, index, button](bool checked) {
                    if (checked) {
                        for (QToolButton* other : std::as_const(toolButtons_)) {
                            if (other != button) {
                                other->setChecked(false);
                            }
                        }
                        tool_ = index;
                    } else {
                        tool_ = -1;
                    }
                    emit toolChosen(tool_);
                });
        toolButtons_.append(button);
        tools->addWidget(button);
    };
    addTool(0, tr("Place"),
            tr("Place the selected model: the object is written to "
               "SavePrm.environment with the parameters below."));
    addTool(1, tr("Select"),
            tr("Click an object to select it; edit its rotation, Z, radius "
               "and flags with the controls below."));
    addTool(2, tr("Delete"),
            tr("Click an object to remove it from the map."));
    layout->addLayout(tools);

    // --- Model list -----------------------------------------------------
    modelList_ = new QListWidget(this);
    modelList_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelList_->setMaximumHeight(180);
    modelList_->setToolTip(
        tr("Nature models from RESOURCE/Models/Environment (the game's "
           "content list). The model is stored per placed object."));
    connect(modelList_, &QListWidget::currentRowChanged, this,
            [this](int row) {
                if (row >= 0) {
                    emit modelChosen(modelList_->item(row)->text());
                }
            });
    layout->addWidget(modelList_);

    // --- Parameters: rotation + Z + radius ------------------------------
    auto* params = new QHBoxLayout;
    params->addWidget(new QLabel(tr("Rotation:"), this));
    rotationSpin_ = new QDoubleSpinBox(this);
    rotationSpin_->setRange(-180.0, 180.0);
    rotationSpin_->setDecimals(1);
    rotationSpin_->setSuffix(tr("°"));
    params->addWidget(rotationSpin_);
    params->addWidget(new QLabel(tr("Z:"), this));
    zSpin_ = new QDoubleSpinBox(this);
    zSpin_->setRange(-10000.0, 10000.0);
    zSpin_->setDecimals(1);
    zSpin_->setToolTip(
        tr("Stored z (world units). Nature objects keep the stored height — "
           "they do not snap to the terrain on load. Defaults to the zero "
           "layer height."));
    params->addWidget(zSpin_);
    params->addWidget(new QLabel(tr("Radius:"), this));
    radiusSpin_ = new QDoubleSpinBox(this);
    radiusSpin_->setRange(0.1, 100000.0);
    radiusSpin_->setDecimals(1);
    radiusSpin_->setToolTip(
        tr("Footprint radius (world units): the model is scaled so its "
           "bound radius equals this."));
    params->addWidget(radiusSpin_);
    layout->addLayout(params);

    // --- natureFlag bits --------------------------------------------------
    auto* flags = new QHBoxLayout;
    flagDestroy_ = new QCheckBox(tr("Destroy"), this);
    flagDestroy_->setToolTip(tr("NATURE_FLAG_DESTROY — the object can be "
                                "destroyed (falls when collided with)."));
    flags->addWidget(flagDestroy_);
    flagCollisions_ = new QCheckBox(tr("Collisions"), this);
    flagCollisions_->setToolTip(tr("NATURE_FLAG_REAL — the object has real "
                                   "collisions."));
    flags->addWidget(flagCollisions_);
    flagLighting_ = new QCheckBox(tr("Lighting"), this);
    flagLighting_->setToolTip(tr("NATURE_FLAG_LIGHT — the object emits "
                                 "light."));
    flags->addWidget(flagLighting_);
    layout->addLayout(flags);

    const auto wire = [this](auto* widget, auto signal) {
        connect(widget, signal, this, [this] {
            if (!updatingSpins_) {
                emitParams();
            }
        });
    };
    wire(rotationSpin_, &QDoubleSpinBox::valueChanged);
    wire(zSpin_, &QDoubleSpinBox::valueChanged);
    wire(radiusSpin_, &QDoubleSpinBox::valueChanged);
    wire(flagDestroy_, &QCheckBox::toggled);
    wire(flagCollisions_, &QCheckBox::toggled);
    wire(flagLighting_, &QCheckBox::toggled);

    setActive(false);
}

void NaturePanel::setActive(bool active)
{
    setEnabled(active);
    if (!active) {
        selection_->setText(tr("No battle map loaded."));
    }
}

void NaturePanel::setActiveTool(int index)
{
    // Programmatic sync only: setChecked() does not emit clicked(), so the
    // toolChosen loop stays quiet.
    for (int i = 0; i < toolButtons_.size(); ++i) {
        toolButtons_[i]->setChecked(i == index);
    }
    tool_ = index;
}

QString NaturePanel::selectedModel() const
{
    QListWidgetItem* item = modelList_->currentItem();
    return item ? item->text() : QString();
}

double NaturePanel::rotationDeg() const
{
    return rotationSpin_->value();
}

double NaturePanel::z() const
{
    return zSpin_->value();
}

double NaturePanel::radius() const
{
    return radiusSpin_->value();
}

bool NaturePanel::flagDestroy() const
{
    return flagDestroy_->isChecked();
}

bool NaturePanel::flagCollisions() const
{
    return flagCollisions_->isChecked();
}

bool NaturePanel::flagLighting() const
{
    return flagLighting_->isChecked();
}

void NaturePanel::setSelectionInfo(const QString& text)
{
    selection_->setText(text.isEmpty() ? tr("No battle map loaded.") : text);
}

void NaturePanel::showSelectionParams(double deg, double z, double radius)
{
    updatingSpins_ = true;
    rotationSpin_->setValue(deg);
    zSpin_->setValue(z);
    radiusSpin_->setValue(radius);
    updatingSpins_ = false;
}

void NaturePanel::showFlags(bool destroy, bool collisions, bool lighting)
{
    updatingSpins_ = true;
    flagDestroy_->setChecked(destroy);
    flagCollisions_->setChecked(collisions);
    flagLighting_->setChecked(lighting);
    updatingSpins_ = false;
}

void NaturePanel::setDefaultZ(double z)
{
    updatingSpins_ = true;
    zSpin_->setValue(z);
    updatingSpins_ = false;
}

void NaturePanel::setModels(const QStringList& models)
{
    modelList_->clear();
    modelList_->addItems(models);
}

void NaturePanel::emitParams()
{
    emit rotationEdited(rotationSpin_->value());
    emit zEdited(zSpin_->value());
    emit radiusEdited(radiusSpin_->value());
    emit flagsEdited(flagDestroy_->isChecked(), flagCollisions_->isChecked(),
                     flagLighting_->isChecked());
}

} // namespace MapEditor

// ============================================================================
// BuildingsPanel implementation — see the header.
// ============================================================================

#include "BuildingsPanel.h"

#include "BuildingCatalog.h"

#include <algorithm>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace MapEditor {

BuildingsPanel::BuildingsPanel(QWidget* parent)
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
            tr("Place the selected building at the click: it is written to "
               "nobodysBuildings at the zero layer plus the building's "
               "basement offset; intersecting placements are refused."));
    addTool(1, tr("Select"),
            tr("Click a building to select it; edit its rotation and HP "
               "with the spins below."));
    addTool(2, tr("Delete"),
            tr("Click a building to remove it from the map."));
    layout->addLayout(tools);

    // --- Parameters: rotation + HP -------------------------------------
    auto* params = new QHBoxLayout;
    params->addWidget(new QLabel(tr("Rotation:"), this));
    rotationSpin_ = new QDoubleSpinBox(this);
    rotationSpin_->setRange(-180.0, 180.0);
    rotationSpin_->setDecimals(1);
    rotationSpin_->setSuffix(tr("°"));
    rotationSpin_->setToolTip(
        tr("Rotation the Place tool stamps with / the Select tool applies "
           "to the selected building."));
    connect(rotationSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) {
                if (!updatingSpins_) {
                    emit rotationEdited(value);
                }
            });
    params->addWidget(rotationSpin_);
    params->addWidget(new QLabel(tr("HP:"), this));
    hpSpin_ = new QSpinBox(this);
    hpSpin_->setRange(0, 100);
    hpSpin_->setValue(100); // full HP by default — a fresh building must
                            // not be saved dead (the spin minimum would
                            // otherwise place destroyed buildings)
    hpSpin_->setSuffix(tr(" %"));
    hpSpin_->setToolTip(
        tr("Hit points the Place tool stamps with / the Select tool applies "
           "to the selected building (damageMolecula)."));
    connect(hpSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!updatingSpins_) {
            emit hpEdited(value);
        }
    });
    params->addWidget(hpSpin_);
    // --- Charge: the initial weapon charge of the charged-shot
    // superweapons (Scum Disruptor / Missile Gun / Filth Navigator) — the
    // Place tool stamps it, the Select tool edits it. Hidden for every
    // other building type (the field exists in the save but only these
    // three weapons wait on it before the first shot).
    chargeLabel_ = new QLabel(tr("Charge:"), this);
    params->addWidget(chargeLabel_);
    chargeSpin_ = new QDoubleSpinBox(this);
    chargeSpin_->setRange(0.0, 100.0);
    chargeSpin_->setDecimals(0);
    chargeSpin_->setValue(100); // fully charged, like the game default
    chargeSpin_->setSuffix(tr(" %"));
    chargeSpin_->setToolTip(
        tr("Initial weapon charge the Place tool stamps / the Select tool "
           "applies to the selected superweapon (weaponChargeLevel "
           "0..1 — 100 % fires right away, 0 % starts uncharged)."));
    connect(chargeSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) {
                if (!updatingSpins_) {
                    emit chargeEdited(value / 100.0);
                }
            });
    params->addWidget(chargeSpin_);
    setChargeEnabled(false);
    layout->addLayout(params);

    // --- The building grid ----------------------------------------------
    auto* scrollContent = new QWidget(this);
    grid_ = new QGridLayout(scrollContent);
    grid_->setSpacing(2);
    buildingGroup_ = new QButtonGroup(this);
    buildingGroup_->setExclusive(true);
    for (std::size_t i = 0; i < kPlaceableBuildingCount; ++i) {
        const BuildingInfo* info = &kPlaceableBuildings[i];
        auto* button = new QToolButton(scrollContent);
        button->setText(QString::fromLatin1(info->name));
        button->setCheckable(true);
        button->setToolTip(QString::fromLatin1("%1 (ID %2)")
                               .arg(QString::fromLatin1(info->identifier))
                               .arg(info->id));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        buildingGroup_->addButton(button, info->id);
        buildingButtons_.insert(info->id, button);
        grid_->addWidget(button, static_cast<int>(i) / 2,
                         static_cast<int>(i) % 2);
    }
    connect(buildingGroup_, &QButtonGroup::idToggled, this,
            &BuildingsPanel::onBuildingToggled);
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(scrollContent);
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll, 1);

    setActive(false);
}

void BuildingsPanel::setActive(bool active)
{
    setEnabled(active);
    if (!active) {
        selection_->setText(tr("No battle map loaded."));
    }
}

void BuildingsPanel::setActiveTool(int index)
{
    // Programmatic sync only: setChecked() does not emit clicked(), so the
    // toolChosen loop stays quiet.
    for (int i = 0; i < toolButtons_.size(); ++i) {
        toolButtons_[i]->setChecked(i == index);
    }
    tool_ = index;
}

void BuildingsPanel::onBuildingToggled(int id, bool checked)
{
    if (checked) {
        // The Charge control belongs to the charged-shot superweapons only.
        setChargeEnabled(MapEditor::isChargeableSuperweapon(id));
        emit buildingChosen(id);
    }
}

int BuildingsPanel::selectedBuilding() const
{
    QAbstractButton* button = buildingGroup_->checkedButton();
    return button ? buildingGroup_->id(button) : -1;
}

double BuildingsPanel::rotationDeg() const
{
    return rotationSpin_->value();
}

int BuildingsPanel::hp() const
{
    return hpSpin_->value();
}

double BuildingsPanel::charge() const
{
    return chargeSpin_->value() / 100.0;
}

void BuildingsPanel::setChargeEnabled(bool enabled)
{
    chargeSpin_->setVisible(enabled);
    chargeLabel_->setVisible(enabled);
}

void BuildingsPanel::setSelectionInfo(const QString& text)
{
    selection_->setText(text.isEmpty() ? tr("No battle map loaded.") : text);
}

void BuildingsPanel::showSelectionParams(double deg, int hp, double charge)
{
    updatingSpins_ = true;
    rotationSpin_->setValue(deg);
    hpSpin_->setValue(hp);
    chargeSpin_->setValue(std::clamp(charge, 0.0, 1.0) * 100.0);
    updatingSpins_ = false;
}

void BuildingsPanel::setBuildingIcon(int attributeId, const QIcon& icon)
{
    if (icon.isNull()) {
        return;
    }
    QToolButton* button = buildingButtons_.value(attributeId, nullptr);
    if (!button) {
        return;
    }
    button->setIcon(icon);
    button->setIconSize(QSize(40, 40));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
}

} // namespace MapEditor

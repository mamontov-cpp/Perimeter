// ============================================================================
// BuildingsPanel implementation — see the header.
// ============================================================================

#include "BuildingsPanel.h"

#include "BuildingCatalog.h"

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
    auto* tools = new QHBoxLayout;
    const auto addTool = [this, tools](int index, const QString& text,
                                       const QString& tip) {
        auto* button = new QToolButton(this);
        button->setText(text);
        button->setCheckable(true);
        button->setChecked(index == 0);
        button->setToolTip(tip);
        connect(button, &QToolButton::clicked, this, [this, index] {
            tool_ = index;
            emit toolChosen(index);
        });
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

void BuildingsPanel::onBuildingToggled(int id, bool checked)
{
    if (checked) {
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

void BuildingsPanel::setSelectionInfo(const QString& text)
{
    selection_->setText(text.isEmpty() ? tr("No battle map loaded.") : text);
}

void BuildingsPanel::showSelectionParams(double deg, int hp)
{
    updatingSpins_ = true;
    rotationSpin_->setValue(deg);
    hpSpin_->setValue(hp);
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

// ============================================================================
// ScumSpotsPanel implementation — see the header.
// ============================================================================

#include "ScumSpotsPanel.h"

#include "ScumSpotCatalog.h"
#include "spg/SpgFile.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace MapEditor {

ScumSpotsPanel::ScumSpotsPanel(QWidget* parent)
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
            tr("Place the selected scum nest: a SCUM_SPOT mound on the zero "
               "layer plus its FILTH_SPOT spawner beneath; nests must be at "
               "least 150 units apart."));
    addTool(1, tr("Select"),
            tr("Click a nest to select it; edit its rotation, HP and filth "
               "parameters here."));
    addTool(2, tr("Delete"),
            tr("Click a nest to remove the mound and its spawner."));
    layout->addLayout(tools);

    // --- Parameters: rotation + HP -------------------------------------
    auto* params = new QHBoxLayout;
    params->addWidget(new QLabel(tr("Rotation:"), this));
    rotationSpin_ = new QDoubleSpinBox(this);
    rotationSpin_->setRange(-180.0, 180.0);
    rotationSpin_->setDecimals(1);
    rotationSpin_->setSuffix(tr("°"));
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
    connect(hpSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        if (!updatingSpins_) {
            emit hpEdited(value);
        }
    });
    params->addWidget(hpSpin_);
    layout->addLayout(params);

    // --- The mound grid --------------------------------------------------
    auto* scrollContent = new QWidget(this);
    auto* scrollLayout = new QVBoxLayout(scrollContent);
    scrollLayout->setContentsMargins(0, 0, 0, 0);
    spotGroup_ = new QButtonGroup(this);
    spotGroup_->setExclusive(true);
    auto* grid = new QGridLayout;
    for (int i = 0; i < kScumSpotCount; ++i) {
        const ScumSpotInfo* info = &kScumSpots[i];
        auto* button = new QToolButton(scrollContent);
        button->setText(QString::fromLatin1(info->name));
        button->setCheckable(true);
        button->setToolTip(QString::fromLatin1("%1 (ID %2)")
                               .arg(QString::fromLatin1(info->identifier))
                               .arg(info->id));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        spotGroup_->addButton(button, info->id);
        spotButtons_.insert(info->id, button);
        grid->addWidget(button, i / 2, i % 2);
    }
    connect(spotGroup_, &QButtonGroup::idToggled, this,
            &ScumSpotsPanel::onSpotToggled);
    scrollLayout->addLayout(grid);

    // --- Filth parameters -------------------------------------------------
    filthBox_ = new QGroupBox(tr("Filth spawner"), scrollContent);
    auto* form = new QFormLayout(filthBox_);

    filthType_ = new QComboBox(filthBox_);
    for (int i = 0; i < SpgFile::filthTypeCount(); ++i) {
        QString name = QString::fromLatin1(SpgFile::filthTypeIdentifier(i));
        name.remove(QStringLiteral("FILTH_SPOT_ID_"));
        filthType_->addItem(name, i);
    }
    form->addRow(tr("Filth type:"), filthType_);

    creatureNum_ = new QSpinBox(filthBox_);
    creatureNum_->setRange(1, 100);
    form->addRow(tr("Creatures per spawn:"), creatureNum_);

    sleep_ = new QCheckBox(tr("Start asleep"), filthBox_);
    form->addRow(sleep_);

    firstSleepTime_ = new QDoubleSpinBox(filthBox_);
    firstSleepTime_->setRange(0.0, 100000.0);
    form->addRow(tr("First delay (s):"), firstSleepTime_);

    sleepPeriod_ = new QDoubleSpinBox(filthBox_);
    sleepPeriod_->setRange(1.0, 100000.0);
    form->addRow(tr("Sleep period (s):"), sleepPeriod_);

    attackPeriod_ = new QDoubleSpinBox(filthBox_);
    attackPeriod_->setRange(1.0, 100000.0);
    form->addRow(tr("Active period (s):"), attackPeriod_);

    activatingUnit_ = new QComboBox(filthBox_);
    activatingUnit_->addItem(tr("None"), 0);
    activatingUnit_->addItem(QStringLiteral("Relay"), 1);
    activatingUnit_->addItem(QStringLiteral("Collector"), 2);
    activatingUnit_->addItem(QStringLiteral("Command Center"), 6);
    activatingUnit_->addItem(QStringLiteral("Frame"), 49);
    form->addRow(tr("Activated by:"), activatingUnit_);

    activatingDistance_ = new QDoubleSpinBox(filthBox_);
    activatingDistance_->setRange(0.0, 100000.0);
    form->addRow(tr("Activation radius:"), activatingDistance_);

    attackPlayer_ = new QComboBox(filthBox_);
    attackPlayer_->addItem(tr("Everyone"), 0);
    attackPlayer_->addItem(tr("Players"), 1);
    attackPlayer_->addItem(tr("AI"), 2);
    form->addRow(tr("Attacks:"), attackPlayer_);

    initialGeoprocess_ =
        new QCheckBox(tr("Initial geo process"), filthBox_);
    form->addRow(initialGeoprocess_);

    createFirst_ = new QCheckBox(tr("Spawn immediately"), filthBox_);
    form->addRow(createFirst_);

    const auto wire = [this](auto* widget, auto signal) {
        connect(widget, signal, this, [this] {
            if (!updatingSpins_) {
                emitFilth();
            }
        });
    };
    wire(filthType_, &QComboBox::currentIndexChanged);
    wire(creatureNum_, &QSpinBox::valueChanged);
    wire(sleep_, &QCheckBox::toggled);
    wire(firstSleepTime_, &QDoubleSpinBox::valueChanged);
    wire(sleepPeriod_, &QDoubleSpinBox::valueChanged);
    wire(attackPeriod_, &QDoubleSpinBox::valueChanged);
    wire(activatingUnit_, &QComboBox::currentIndexChanged);
    wire(activatingDistance_, &QDoubleSpinBox::valueChanged);
    wire(attackPlayer_, &QComboBox::currentIndexChanged);
    wire(initialGeoprocess_, &QCheckBox::toggled);
    wire(createFirst_, &QCheckBox::toggled);

    scrollLayout->addWidget(filthBox_);
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(scrollContent);
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll, 1);

    setFilthEnabled(false);
    setActive(false);
}

void ScumSpotsPanel::setActive(bool active)
{
    setEnabled(active);
    if (!active) {
        selection_->setText(tr("No battle map loaded."));
    }
}

void ScumSpotsPanel::onSpotToggled(int id, bool checked)
{
    if (checked) {
        emit spotChosen(id);
    }
}

int ScumSpotsPanel::selectedSpot() const
{
    QAbstractButton* button = spotGroup_->checkedButton();
    return button ? spotGroup_->id(button) : -1;
}

double ScumSpotsPanel::rotationDeg() const
{
    return rotationSpin_->value();
}

int ScumSpotsPanel::hp() const
{
    return hpSpin_->value();
}

void ScumSpotsPanel::setSelectionInfo(const QString& text)
{
    selection_->setText(text.isEmpty() ? tr("No battle map loaded.") : text);
}

void ScumSpotsPanel::showSelectionParams(double deg, int hp)
{
    updatingSpins_ = true;
    rotationSpin_->setValue(deg);
    hpSpin_->setValue(hp);
    updatingSpins_ = false;
}

void ScumSpotsPanel::showFilthParams(const SpgFile& schema, int filthType,
                                     int creatureNum, bool sleep,
                                     double firstSleepTime,
                                     double sleepPeriod, double attackPeriod,
                                     int activatingUnit,
                                     double activatingDistance,
                                     int attackPlayer, bool initialGeoprocess,
                                     bool createFirst)
{
    Q_UNUSED(schema);
    updatingSpins_ = true;
    const int typeIndex = filthType_->findData(filthType);
    filthType_->setCurrentIndex(typeIndex >= 0 ? typeIndex : 0);
    creatureNum_->setValue(creatureNum);
    sleep_->setChecked(sleep);
    firstSleepTime_->setValue(firstSleepTime);
    sleepPeriod_->setValue(sleepPeriod);
    attackPeriod_->setValue(attackPeriod);
    const int unitIndex = activatingUnit_->findData(activatingUnit);
    activatingUnit_->setCurrentIndex(unitIndex >= 0 ? unitIndex : 0);
    activatingDistance_->setValue(activatingDistance);
    const int attackIndex = attackPlayer_->findData(attackPlayer);
    attackPlayer_->setCurrentIndex(attackIndex >= 0 ? attackIndex : 0);
    initialGeoprocess_->setChecked(initialGeoprocess);
    createFirst_->setChecked(createFirst);
    updatingSpins_ = false;
}

void ScumSpotsPanel::setFilthEnabled(bool enabled)
{
    filthBox_->setEnabled(enabled);
}

void ScumSpotsPanel::setSpotIcon(int attributeId, const QIcon& icon)
{
    if (icon.isNull()) {
        return;
    }
    QToolButton* button = spotButtons_.value(attributeId, nullptr);
    if (!button) {
        return;
    }
    button->setIcon(icon);
    button->setIconSize(QSize(40, 40));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
}

void ScumSpotsPanel::emitFilth()
{
    emit filthEdited(
        filthType_->currentData().toInt(), creatureNum_->value(),
        sleep_->isChecked(), firstSleepTime_->value(), sleepPeriod_->value(),
        attackPeriod_->value(), activatingUnit_->currentData().toInt(),
        activatingDistance_->value(), attackPlayer_->currentData().toInt(),
        initialGeoprocess_->isChecked(), createFirst_->isChecked());
}

} // namespace MapEditor

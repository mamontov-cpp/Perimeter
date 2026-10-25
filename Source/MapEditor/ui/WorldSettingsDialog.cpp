// ============================================================================
// WorldSettingsPanel / WorldSettingsDialog implementation (T5.1/T5.2).
// ============================================================================

#include "WorldSettingsDialog.h"

#include <algorithm>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QColorDialog>

namespace {

// The two legacy geo-noise functions (s_f3d::tableF in Terra/f3d.cpp).
const char *const kF3dFunctionNames[] = {
    "Marble Texture",
    "Tree Texture",
};

// Terra's limits: MAX_H_SIZE_POWER / MAX_V_SIZE_POWER (terra.h:13,16).
constexpr int kMaxSizePower = 14;

QSpinBox *makeSpin(int min, int max, int step = 1)
{
    auto *spin = new QSpinBox;
    spin->setRange(min, max);
    spin->setSingleStep(step);
    return spin;
}

} // namespace

// ---------------------------------------------------------------------------
// WorldSettingsPanel
// ---------------------------------------------------------------------------

WorldSettingsPanel::WorldSettingsPanel(Mode mode,
                                       const MapEditor::WorldParams &current,
                                       QWidget *parent)
    : QWidget(parent), params_(current)
{
    buildUi(mode);
    syncWidgets();
}

void WorldSettingsPanel::buildUi(Mode mode)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // ------------------------------------------------------------------
    // Size + zero-layer height
    // ------------------------------------------------------------------
    auto *sizeGroup = new QGroupBox(tr("Map Size and Zero Layer"), this);
    auto *sizeForm = new QFormLayout(sizeGroup);

    sizeLabel_ = new QLabel(sizeGroup);
    sizeForm->addRow(tr("Map size:"), sizeLabel_);

    const bool isNew = mode == Mode::NewWorld;
    if (isNew) {
        auto *powerRow = new QHBoxLayout;
        powerXSpin_ = makeSpin(6, kMaxSizePower);
        powerYSpin_ = makeSpin(6, kMaxSizePower);
        powerRow->addWidget(new QLabel(tr("Power X:"), sizeGroup));
        powerRow->addWidget(powerXSpin_);
        powerRow->addWidget(new QLabel(tr("Power Y:"), sizeGroup));
        powerRow->addWidget(powerYSpin_);
        powerRow->addStretch(1);
        sizeForm->addRow(tr("Size powers:"), powerRow);
    }

    zeroPlastSpin_ = makeSpin(0, 255);
    sizeForm->addRow(tr("Zero-layer height (hZeroPlast):"), zeroPlastSpin_);

    if (isNew) {
        geoHeightLabel_ = new QLabel(sizeGroup);
        geoHeightSpin_ = makeSpin(0, 254);
        sizeForm->addRow(geoHeightLabel_, geoHeightSpin_);
        connect(zeroPlastSpin_, &QSpinBox::valueChanged, this,
                &WorldSettingsPanel::onZeroPlastChanged);
    }

    mainLayout->addWidget(sizeGroup);

    // ------------------------------------------------------------------
    // Generation powers
    // ------------------------------------------------------------------
    auto *genGroup = new QGroupBox(tr("Generation Parameters"), this);
    auto *genForm = new QFormLayout(genGroup);
    geoNetSpin_ = makeSpin(0, 12);
    sectionSpin_ = makeSpin(0, 12);
    minSquareSpin_ = makeSpin(0, 12);
    genForm->addRow(tr("GeoNet Power:"), geoNetSpin_);
    genForm->addRow(tr("Section Size Power:"), sectionSpin_);
    genForm->addRow(tr("Minimal Square Power:"), minSquareSpin_);
    mainLayout->addWidget(genGroup);

    if (!isNew) {
        // The loaded buffers were sized by the values in effect at load
        // time; changed powers only take effect on reload / regeneration.
        QLabel *note = new QLabel(
            tr("Generation powers are stored in world.ini and take effect "
               "when the world is reloaded or its terrain is regenerated."),
            genGroup);
        note->setWordWrap(true);
        genForm->addRow(note);
    }

    // ------------------------------------------------------------------
    // Storage
    // ------------------------------------------------------------------
    auto *storageGroup = new QGroupBox(tr("Storage"), this);
    auto *storageForm = new QFormLayout(storageGroup);
    storageCombo_ = new QComboBox(storageGroup);
    storageCombo_->addItem(tr("S2T0 — uncompressed"));
    storageCombo_->addItem(tr("S2T1 — zlib compressed"));
    storageForm->addRow(tr("output.vmp format:"), storageCombo_);
    mainLayout->addWidget(storageGroup);

    // ------------------------------------------------------------------
    // 3DF Parameters
    // ------------------------------------------------------------------
    auto *f3dGroup = new QGroupBox(tr("3DF Parameters (geo noise)"), this);
    auto *f3dForm = new QFormLayout(f3dGroup);
    kmxSpin_ = makeSpin(1, 1000000, 10);
    kmySpin_ = makeSpin(1, 1000000, 10);
    kmzSpin_ = makeSpin(1, 1000000, 10);
    f3dForm->addRow(tr("Scaling X (kmx):"), kmxSpin_);
    f3dForm->addRow(tr("Scaling Y (kmy):"), kmySpin_);
    f3dForm->addRow(tr("Scaling Z (kmz):"), kmzSpin_);
    functionCombo_ = new QComboBox(f3dGroup);
    for (const char *name : kF3dFunctionNames) {
        functionCombo_->addItem(tr(name));
    }
    f3dForm->addRow(tr("Function:"), functionCombo_);
    mainLayout->addWidget(f3dGroup);

    // ------------------------------------------------------------------
    // Zeroplast color (game-wide, Perimeter.ini)
    // ------------------------------------------------------------------
    auto *zpGroup = new QGroupBox(tr("Zeroplast Color (game-wide)"), this);
    auto *zpForm = new QFormLayout(zpGroup);
    zeroLayerColorButton_ = new QPushButton(zpGroup);
    zeroLayerColorButton_->setFixedWidth(64);
    connect(zeroLayerColorButton_, &QPushButton::clicked, this,
            &WorldSettingsPanel::pickZeroLayerColor);
    zeroLayerColorLabel_ = new QLabel(zpGroup);
    auto *colorRow = new QHBoxLayout;
    colorRow->addWidget(zeroLayerColorButton_);
    colorRow->addWidget(zeroLayerColorLabel_);
    colorRow->addStretch(1);
    zpForm->addRow(tr("Color:"), colorRow);
    zeroLayerAlphaSpin_ = makeSpin(0, 255);
    zpForm->addRow(tr("Alpha:"), zeroLayerAlphaSpin_);
    QLabel *zpNote = new QLabel(
        tr("Stored in Perimeter.ini [TD] Color ZeroLayer and shared by every "
           "map in this game installation."), zpGroup);
    zpNote->setWordWrap(true);
    zpForm->addRow(zpNote);
    mainLayout->addWidget(zpGroup);

    mainLayout->addStretch(1);
}

void WorldSettingsPanel::syncWidgets()
{
    const QString sizeText = tr("%1 × %2 (power %3 × %4)")
                                 .arg(params_.mapWidth())
                                 .arg(params_.mapHeight())
                                 .arg(params_.mapPowerX)
                                 .arg(params_.mapPowerY);
    sizeLabel_->setText(sizeText);

    if (powerXSpin_) {
        powerXSpin_->setValue(params_.mapPowerX);
    }
    if (powerYSpin_) {
        powerYSpin_->setValue(params_.mapPowerY);
    }
    zeroPlastSpin_->setValue(params_.hZeroPlast);
    if (geoHeightSpin_) {
        geoHeightSpin_->setMaximum(std::max(0, params_.hZeroPlast - 1));
        geoHeightSpin_->setValue(geoLayerHeight_);
        geoHeightLabel_->setText(
            tr("Geo layer height (filled at creation, < zero layer):"));
    }

    geoNetSpin_->setValue(params_.geoNetPower);
    sectionSpin_->setValue(params_.sectionSizePower);
    minSquareSpin_->setValue(params_.minimalSquarePower);

    storageCombo_->setCurrentIndex(params_.compressedFormat == 1 ? 1 : 0);

    kmxSpin_->setValue(params_.f3dKmx);
    kmySpin_->setValue(params_.f3dKmy);
    kmzSpin_->setValue(params_.f3dKmz);
    functionCombo_->setCurrentIndex(params_.f3dFunction != 0 ? 1 : 0);

    updateZeroLayerColorWidgets();
}

void WorldSettingsPanel::updateZeroLayerColorWidgets()
{
    const QColor color(params_.zeroLayerColor.r, params_.zeroLayerColor.g,
                       params_.zeroLayerColor.b);
    zeroLayerColorButton_->setStyleSheet(
        QStringLiteral("background-color: %1").arg(color.name()));
    zeroLayerColorLabel_->setText(tr("R %1  G %2  B %3")
                                      .arg(params_.zeroLayerColor.r)
                                      .arg(params_.zeroLayerColor.g)
                                      .arg(params_.zeroLayerColor.b));
}

void WorldSettingsPanel::onZeroPlastChanged(int value)
{
    if (geoHeightSpin_) {
        geoHeightSpin_->setMaximum(std::max(0, value - 1));
        if (geoHeightSpin_->value() > geoHeightSpin_->maximum()) {
            geoHeightSpin_->setValue(geoHeightSpin_->maximum());
        }
    }
}

void WorldSettingsPanel::pickZeroLayerColor()
{
    const QColor initial(params_.zeroLayerColor.r, params_.zeroLayerColor.g,
                         params_.zeroLayerColor.b);
    const QColor picked = QColorDialog::getColor(initial, this);
    if (!picked.isValid()) {
        return;
    }
    params_.zeroLayerColor.r = picked.red();
    params_.zeroLayerColor.g = picked.green();
    params_.zeroLayerColor.b = picked.blue();
    updateZeroLayerColorWidgets();
}

void WorldSettingsPanel::setGeoLayerHeight(int height)
{
    geoLayerHeight_ = height;
    if (geoHeightSpin_) {
        geoHeightSpin_->setValue(height);
    }
}

MapEditor::WorldParams WorldSettingsPanel::params() const
{
    // Widget state wins over the cached struct; the zeroplast RGB lives in
    // params_ already (pickZeroLayerColor keeps it in sync).
    MapEditor::WorldParams result = params_;
    if (powerXSpin_) {
        result.mapPowerX = powerXSpin_->value();
    }
    if (powerYSpin_) {
        result.mapPowerY = powerYSpin_->value();
    }
    result.hZeroPlast = zeroPlastSpin_->value();
    result.geoNetPower = geoNetSpin_->value();
    result.sectionSizePower = sectionSpin_->value();
    result.minimalSquarePower = minSquareSpin_->value();
    result.compressedFormat = storageCombo_->currentIndex() == 1 ? 1 : 0;
    result.f3dKmx = kmxSpin_->value();
    result.f3dKmy = kmySpin_->value();
    result.f3dKmz = kmzSpin_->value();
    result.f3dFunction = functionCombo_->currentIndex();
    result.zeroLayerColor.a = zeroLayerAlphaSpin_->value();
    return result;
}

void WorldSettingsPanel::setParams(const MapEditor::WorldParams &params)
{
    params_ = params;
    syncWidgets();
}

// ---------------------------------------------------------------------------
// WorldSettingsDialog
// ---------------------------------------------------------------------------

WorldSettingsDialog::WorldSettingsDialog(QWidget *parent,
                                         WorldSettingsPanel::Mode mode,
                                         const MapEditor::WorldParams &current)
    : QDialog(parent)
{
    setModal(true);
    panel_ = new WorldSettingsPanel(mode, current, this);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(panel_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

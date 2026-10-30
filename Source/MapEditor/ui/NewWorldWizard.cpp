// ============================================================================
// NewWorldWizard implementation (T5.2).
// ============================================================================

#include "NewWorldWizard.h"

#include "WorldFiles.h"
#include "WorldSettingsDialog.h"

#include <algorithm>
#include <filesystem>

#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>

namespace {

/// Pick sensible defaults from the template world's files (sizes, heights,
/// 3DF scaling); anything unreadable falls back to the struct defaults.
MapEditor::WorldParams paramsFromTemplate(const MapEditor::WorldInfo &world,
                                          const std::string &gameDir)
{
    MapEditor::WorldParams params; // defaults (createWorld-like)
    const MapEditor::WorldFiles files(
        (std::filesystem::u8path(gameDir) /
         std::filesystem::u8path(world.dirKey))
            .u8string());
    if (const auto read =
            MapEditor::WorldParams::readFromWorld(files)) {
        params = *read;
    }
    return params;
}

} // namespace

/// Name page: registerField is protected in QWizardPage, so the mandatory
/// "worldName" field is registered from a subclass. The widgets live on the
/// page; the wizard reads them through namePage_. Declared (forward) in the
/// header, defined here at global scope.
class NewWorldNamePage : public QWizardPage
{
public:
    NewWorldNamePage()
    {
        setTitle(tr("World Name and Template"));

        auto *layout = new QFormLayout(this);
        nameEdit = new QLineEdit(this);
        nameEdit->setPlaceholderText(tr("MyWorld"));
        layout->addRow(tr("World name:"), nameEdit);

        templateCombo = new QComboBox(this);
        templateCombo->setToolTip(tr(
            "Palettes, the geo layer textures and leveledSurfaceTexture.tga "
            "are copied from this world."));
        layout->addRow(tr("Template world:"), templateCombo);

        auto *note = new QLabel(
            tr("The new world folder is created in RESOURCE/Worlds of the "
               "current game directory."), this);
        note->setWordWrap(true);
        layout->addRow(note);

        registerField("worldName*", nameEdit);
    }

    QLineEdit *nameEdit = nullptr;
    QComboBox *templateCombo = nullptr;
};

NewWorldWizard::NewWorldWizard(QWidget *parent,
                               const std::vector<MapEditor::WorldInfo> &worlds,
                               const std::string &gameDir)
    : QWizard(parent), gameDir_(gameDir)
{
    setWindowTitle(tr("New World"));
    setOption(QWizard::NoBackButtonOnStartPage, true);

    addPage(buildNamePage());
    addPage(buildTerrainPage());
    addPage(buildParamsPage());

    for (const MapEditor::WorldInfo &world : worlds) {
        namePage_->templateCombo->addItem(QString::fromStdString(world.name));
    }

    // Defaults for the parameter page come from the first template world.
    if (!worlds.empty()) {
        paramsPanel_->setParams(
            paramsFromTemplate(worlds.front(), gameDir_));
        if (paramsPanel_->geoLayerHeight() >=
            paramsPanel_->params().hZeroPlast) {
            paramsPanel_->setGeoLayerHeight(
                std::max(0, paramsPanel_->params().hZeroPlast - 10));
        }
    }
}

QWizardPage *NewWorldWizard::buildNamePage()
{
    namePage_ = new NewWorldNamePage;
    return namePage_;
}

QWizardPage *NewWorldWizard::buildTerrainPage()
{
    auto *page = new QWizardPage;
    page->setTitle(tr("Terrain"));

    auto *layout = new QVBoxLayout(page);
    auto *group = new QGroupBox(tr("Initial surface"), page);
    auto *groupLayout = new QVBoxLayout(group);

    flatRadio_ = new QRadioButton(
        tr("Flat — the geo layer filled with voxels at the geo layer height"),
        group);
    generatedRadio_ = new QRadioButton(
        tr("Generated — procedural relief (the game's buildWorld)"), group);
    flatRadio_->setChecked(true);
    groupLayout->addWidget(flatRadio_);
    groupLayout->addWidget(generatedRadio_);

    QLabel *note = new QLabel(
        tr("The geo layer height must stay below the zero-layer height so "
           "that levelling to the zeroplast has soil to work with."), page);
    note->setWordWrap(true);

    layout->addWidget(group);
    layout->addWidget(note);
    layout->addStretch(1);
    return page;
}

QWizardPage *NewWorldWizard::buildParamsPage()
{
    auto *page = new QWizardPage;
    page->setTitle(tr("World Parameters"));

    paramsPanel_ = new WorldSettingsPanel(WorldSettingsPanel::Mode::NewWorld,
                                          MapEditor::WorldParams{}, page);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(paramsPanel_);
    return page;
}

MapEditor::NewWorldParams NewWorldWizard::wizardParams() const
{
    MapEditor::NewWorldParams params;
    params.name = namePage_->nameEdit->text().trimmed().toStdString();
    const MapEditor::WorldParams panel = paramsPanel_->params();
    params.mapPowerX = panel.mapPowerX;
    params.mapPowerY = panel.mapPowerY;
    params.hZeroPlast = panel.hZeroPlast;
    params.geoLayerHeight = paramsPanel_->geoLayerHeight();
    params.geoNetPower = panel.geoNetPower;
    params.sectionSizePower = panel.sectionSizePower;
    params.minimalSquarePower = panel.minimalSquarePower;
    params.f3dKmx = panel.f3dKmx;
    params.f3dKmy = panel.f3dKmy;
    params.f3dKmz = panel.f3dKmz;
    params.f3dFunction = panel.f3dFunction;
    params.flatSurface = flatRadio_->isChecked();
    params.storageCompressed = panel.compressedFormat;
    return params;
}

QString NewWorldWizard::templateWorldName() const
{
    return namePage_->templateCombo->currentText();
}

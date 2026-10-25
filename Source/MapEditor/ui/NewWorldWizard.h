#pragma once

#include <QWizard>

#include "EditorSession.h"
#include "WorldCreation.h"
#include "WorldParams.h"

#include <string>
#include <vector>

class QComboBox;
class QLineEdit;
class QRadioButton;
class WorldSettingsPanel;
class NewWorldNamePage;

// ============================================================================
// NewWorldWizard (T5.2) — creates a new world folder:
//
//   page 1: world name + the template world that donates palettes, the geo
//           layer XMLs and the leveled-surface texture;
//   page 2: terrain kind — flat surface at the geo layer height, or Terra's
//           procedural generation (buildWorld);
//   page 3: world parameters (the same WorldSettingsPanel the "World
//           Parameters..." dialog uses, in NewWorld mode with editable size
//           powers and the geo layer height).
//
// The caller applies wizardParams() through MapEditor::createNewWorld().
// ============================================================================
class NewWorldWizard : public QWizard
{
    Q_OBJECT

public:
    /// `worlds` — the session's scanned world list (template candidates);
    /// `gameDir` — the game root, for reading the template defaults.
    NewWorldWizard(QWidget *parent,
                   const std::vector<MapEditor::WorldInfo> &worlds,
                   const std::string &gameDir);

    MapEditor::NewWorldParams wizardParams() const;
    QString templateWorldName() const;

private:
    QWizardPage *buildNamePage();
    QWizardPage *buildTerrainPage();
    QWizardPage *buildParamsPage();

    // The name page (defined in the .cpp) owns the world-name and template
    // widgets.
    NewWorldNamePage *namePage_ = nullptr;
    QRadioButton *flatRadio_ = nullptr;
    QRadioButton *generatedRadio_ = nullptr;
    WorldSettingsPanel *paramsPanel_ = nullptr;
    std::string gameDir_;
};

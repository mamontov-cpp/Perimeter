#pragma once

#include <QDialog>
#include <QWidget>

#include "WorldParams.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QPushButton;
class QSpinBox;

// ============================================================================
// WorldSettingsPanel (T5.1/T5.2) — the world parameter fields of
// WorldParams:
//
//   [Global Parameters] hZeroPlast + the generation powers (+ map size),
//   [Storage]           output.vmp compression (S2T0 / S2T1),
//   [3DF Parameters]    geo-noise scaling + function index,
//   Perimeter.ini       the game-wide zeroplast tint.
//
// Mode::ExistingWorld is used by the "World Parameters..." dialog for a
// loaded world: the map size is shown read-only (the buffers are already
// allocated). Mode::NewWorld lets the new-world wizard (T5.2) reuse the same
// panel with editable size powers and a geo-layer height that must stay
// below the zero-layer height.
// ============================================================================
class WorldSettingsPanel : public QWidget
{
    Q_OBJECT

public:
    enum class Mode
    {
        ExistingWorld,
        NewWorld,
    };

    WorldSettingsPanel(Mode mode, const MapEditor::WorldParams &current,
                       QWidget *parent = nullptr);

    /// The edited parameters, collected from the widgets (valid at any
    /// time — the dialog/wizard reads it on accept/finish).
    MapEditor::WorldParams params() const;

    /// Replace all shown values (used by the new-world wizard to seed the
    /// panel from the template world).
    void setParams(const MapEditor::WorldParams &params);

    // NewWorld mode only — the geo layer is filled with voxels of this
    // height at creation; guaranteed < hZeroPlast by the panel.
    int geoLayerHeight() const { return geoLayerHeight_; }
    void setGeoLayerHeight(int height);

private slots:
    void pickZeroLayerColor();
    void onZeroPlastChanged(int value);

private:
    void buildUi(Mode mode);
    void syncWidgets();
    void updateZeroLayerColorWidgets();

    MapEditor::WorldParams params_;
    int geoLayerHeight_ = 0;

    QLabel *sizeLabel_ = nullptr;
    QSpinBox *powerXSpin_ = nullptr;
    QSpinBox *powerYSpin_ = nullptr;
    QLabel *geoHeightLabel_ = nullptr;
    QSpinBox *geoHeightSpin_ = nullptr;
    QSpinBox *zeroPlastSpin_ = nullptr;
    QSpinBox *geoNetSpin_ = nullptr;
    QSpinBox *sectionSpin_ = nullptr;
    QSpinBox *minSquareSpin_ = nullptr;
    QComboBox *storageCombo_ = nullptr;
    QSpinBox *kmxSpin_ = nullptr;
    QSpinBox *kmySpin_ = nullptr;
    QSpinBox *kmzSpin_ = nullptr;
    QComboBox *functionCombo_ = nullptr;
    QPushButton *zeroLayerColorButton_ = nullptr;
    QLabel *zeroLayerColorLabel_ = nullptr;
    QSpinBox *zeroLayerAlphaSpin_ = nullptr;
};

// Thin QDialog wrapper: panel + Ok/Cancel (T5.1 "World Parameters...").
class WorldSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    WorldSettingsDialog(QWidget *parent, WorldSettingsPanel::Mode mode,
                        const MapEditor::WorldParams &current);

    MapEditor::WorldParams params() const { return panel_->params(); }
    int geoLayerHeight() const { return panel_->geoLayerHeight(); }
    void setGeoLayerHeight(int height) { panel_->setGeoLayerHeight(height); }

private:
    WorldSettingsPanel *panel_ = nullptr;
};

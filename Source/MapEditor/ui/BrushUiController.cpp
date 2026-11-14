// ============================================================================
// BrushUiController — the brush UI domain of the map editor.
//
// Absorbed from MainWindow.cpp (god-object decomposition, see the code
// review and docs/architecture.md §5.36). The implementations moved nearly
// verbatim; the host-service indirections are the same pattern as
// BattleMapController: statusBar()->showMessage -> showStatus() (signal
// statusMessage), updateActionStates() -> actionStatesNeedUpdate(),
// refreshPaletteRenderers()+palettePanel_->refresh() -> palettesNeedRefresh(),
// palettePanel_->selectIndex -> syncPaletteSelection, addToolBar* ->
// window_->addToolBar*, and the dialog parents (this -> window_).
// ============================================================================

#include "BrushUiController.h"

#include "BrushUiAssets.h"
#include "EditorMath.h"
#include "EditorSession.h"
#include "Shape3D.h"
#include "ViewportWidget.h"

// Terra limits (MAX_VX_HEIGHT_WHOLE) for the height spinboxes.
#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif
#include "terra.h"

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QActionGroup>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QSpinBox>
#include <QToolButton>
#include <QToolBar>
#include <QWidgetAction>

namespace MapEditor {

using namespace brushui; // the toolbar icon helpers (BrushUiAssets.h)

BrushUiController::BrushUiController(MapEditor::BrushTool *brushTool,
                                     MapEditor::EditorSession *session,
                                     QMainWindow *window, QObject *parent)
    : QObject(parent),
      brushTool_(brushTool),
      session_(session),
      window_(window)
{
}

void BrushUiController::setEditorModeActions(QAction *view, QAction *brush,
                                             QAction *eyedropper,
                                             QAction *placeSpawn)
{
    modeActionView_ = view;
    modeActionBrush_ = brush;
    modeActionEyedropper_ = eyedropper;
    modeActionPlaceSpawn_ = placeSpawn;
}

void BrushUiController::showStatus(const QString &message, int timeoutMs)
{
    emit statusMessage(message, timeoutMs);
}

void BrushUiController::createActions()
{
    createBrushTypeActions();
    createSculptTypeActions();
    createShape3DActions();
    createBrushModeActions();
    createBrushGateActions();
    createStampShapeActions();
}

void BrushUiController::buildToolbars()
{
    buildBrushToolbar();
    buildSculptToolbar();
    buildModifierToolbar();
    buildModelToolbar();
}

void BrushUiController::addEditMenuSubMenus(QMenu *editMenu)
{
    // T3.2 — the same brush type/mode actions as the Brush toolbar,
    // packed into an Edit-menu submenu for discoverability.
    QMenu *brushMenu = editMenu->addMenu(tr("Br&ush Type / Mode"));
    brushMenu->addAction(actBrushRaise_);
    brushMenu->addAction(actBrushLower_);
    brushMenu->addAction(actBrushLevelZP_);
    brushMenu->addAction(actBrushLevelHeight_);
    brushMenu->addSeparator();
    brushMenu->addAction(actBrushGeo_);
    brushMenu->addAction(actBrushDam_);
    brushMenu->addSeparator();
    brushMenu->addAction(actBrushColor_);
    brushMenu->addAction(actBrushPickColor_);
    brushMenu->addSeparator();
    brushMenu->addAction(actBrushImageDecal_);
    brushMenu->addSeparator();
    brushMenu->addAction(actBrushSoot_);
    brushMenu->addAction(actBrushVeryLightDam_);
    brushMenu->addSeparator();
    brushMenu->addAction(actLoadBrushImage_);
    brushMenu->addSeparator();
    // T3.1 — the stamp form, same actions as on the Brush toolbar.
    brushMenu->addAction(actShapeCircle_);
    brushMenu->addAction(actShapeSquare_);
    brushMenu->addAction(actShapeCircleOutline_);
    brushMenu->addAction(actShapeSquareOutline_);
    brushMenu->addAction(actShapeLine_);

    // The height-gate modifiers: the same checkable actions as the Brush
    // Modifiers toolbar, plus the gate height as a menu-hosted spinbox (a
    // mirror of the toolbar one — both write BrushParams::gateHeight and
    // syncBrushControls keeps them equal).
    QMenu *modifiersMenu = editMenu->addMenu(tr("Brush Mod&ifiers"));
    modifiersMenu->addAction(actGateAbove_);
    modifiersMenu->addAction(actGateBelow_);
    modifiersMenu->addSeparator();
    gateHeightMenuSpin_ = new QSpinBox(window_);
    gateHeightMenuSpin_->setRange(0, MAX_VX_HEIGHT_WHOLE);
    gateHeightMenuSpin_->setSuffix(tr(" vox"));
    gateHeightMenuSpin_->setToolTip(
        tr("Gate height (whole voxels; 1 voxel = 1/32 m): the active "
           "above/below modifier applies the brush effect only to the voxels "
           "whose dam height is on its side of this height."));
    connect(gateHeightMenuSpin_, &QSpinBox::valueChanged, this,
            [this](int value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.gateHeight = MapEditor::BrushTool::clampGateHeight(value);
                brushTool_->setParams(params);
                if (gateHeightSpin_ &&
                    gateHeightSpin_->value() != params.gateHeight) {
                    gateHeightSpin_->blockSignals(true);
                    gateHeightSpin_->setValue(params.gateHeight);
                    gateHeightSpin_->blockSignals(false);
                }
            });
    QWidgetAction *gateHeightAction = new QWidgetAction(this);
    gateHeightAction->setDefaultWidget(gateHeightMenuSpin_);
    gateHeightAction->setText(tr("Gate Height"));
    modifiersMenu->addAction(gateHeightAction);
}

void BrushUiController::updateEnabledStates(bool worldLoaded)
{
    // T3.2 — the brush type/mode buttons follow the same rule. The
    // zeroplast brushes are a Dam-mode tool (the zeroplast belongs to the
    // dam domain) — not accessible in the Geo mode.
    const bool damMode = brushTool_ &&
        brushTool_->params().mode == MapEditor::BrushMode::Dam;
    for (QAction *action : {actBrushRaise_, actBrushLower_, actBrushLevelZP_,
                            actBrushLevelHeight_, actBrushColor_,
                            actBrushImageDecal_,
                            actBrushPickColor_,
                            actBrushGeo_, actBrushDam_,
                            actBrushSoot_, actBrushVeryLightDam_,
                            actBrushScript_, actBrushScriptEdit_,
                            actBrushSmooth_, actBrushFlatten_,
                            actBrushTricubic_, actBrushInflate_,
                            actBrushDeflate_, actBrushGaussianBlur_,
                            actLoadBrushImage_,
                            actShapeCircle_, actShapeSquare_,
                            actShapeCircleOutline_, actShapeSquareOutline_,
                            actShapePolygon_, actShapePolygonOutline_,
                            actShapeLine_}) {
        if (action) {
            action->setEnabled(worldLoaded);
        }
    }
    for (QAction *action : {actBrushLevelZP_}) {
        if (action) {
            action->setEnabled(worldLoaded && damMode);
        }
    }
    // ImageDecal paints Dam palette indices only; the scripted and the
    // sculpting brushes operate on the Dam layer: the Geo layer is not
    // available while any of them is selected (switching to Geo falls back
    // to a plain raise, see the mode action).
    const bool decalEffect = brushTool_ &&
        brushTool_->params().effect == MapEditor::BrushEffect::ImageDecal;
    const bool scriptEffect = brushTool_ &&
        brushTool_->params().effect == MapEditor::BrushEffect::Script;
    const bool sculptEffect = brushTool_ &&
        (brushTool_->params().effect == MapEditor::BrushEffect::Smooth ||
         brushTool_->params().effect == MapEditor::BrushEffect::Flatten ||
         brushTool_->params().effect == MapEditor::BrushEffect::Tricubic ||
         brushTool_->params().effect == MapEditor::BrushEffect::Inflate ||
         brushTool_->params().effect == MapEditor::BrushEffect::Deflate ||
         brushTool_->params().effect == MapEditor::BrushEffect::GaussianBlur);
    // The 3D shape brushes own the Dam layer like the sculpting types — the
    // Geo mode button is disabled while a shape is selected.
    const MapEditor::Shape3DKind activeShape3d =
        brushTool_ ? brushTool_->params().shape3d
                   : MapEditor::Shape3DKind::None;
    const bool shape3dActive = activeShape3d != MapEditor::Shape3DKind::None;
    const bool modelBrushActive =
        brushTool_ && brushTool_->params().modelBrush;
    if (actBrushGeo_) {
        actBrushGeo_->setEnabled(worldLoaded && !decalEffect && !scriptEffect &&
                                 !sculptEffect && !shape3dActive &&
                                 !modelBrushActive);
    }
    // The sculpting fraction applies only with a sculpting type selected
    // (same image/decal/pick exclusions the radius has).
    if (sculptEffectSpin_) {
        sculptEffectSpin_->setEnabled(worldLoaded && brushTool_ && sculptEffect &&
                                      !brushTool_->params().useImageMask &&
                                      !decalEffect &&
                                      brushTool_->params().effect !=
                                          MapEditor::BrushEffect::PickColor);
    }
    // The blur radius applies only with the Gaussian-blur brush selected.
    if (blurRadiusSpin_) {
        blurRadiusSpin_->setEnabled(worldLoaded && brushTool_ &&
                                    brushTool_->params().effect ==
                                        MapEditor::BrushEffect::GaussianBlur);
    }
    // The height-gate modifiers are clickable in any mode — enabling one
    // switches to the Dam layer itself (the gate reads dam heights) — but
    // the gate height spinboxes apply only while a gate is active.
    if (actGateAbove_) {
        actGateAbove_->setEnabled(worldLoaded);
    }
    if (actGateBelow_) {
        actGateBelow_->setEnabled(worldLoaded);
    }
    const bool gateActive = brushTool_ &&
        brushTool_->params().heightGate != MapEditor::BrushHeightGate::None;
    for (QSpinBox *spin : {gateHeightSpin_, gateHeightMenuSpin_}) {
        if (spin) {
            spin->setEnabled(worldLoaded && gateActive);
        }
    }
    // T3.1 — the stamp radius applies to every form except the image stamps
    // (the brush image and the decal carry their own size) and the
    // point-operated pick, and only with a loaded world.
    if (brushRadiusSpin_) {
        brushRadiusSpin_->setEnabled(
            worldLoaded && brushTool_ &&
            !brushTool_->params().useImageMask && !decalEffect &&
            brushTool_->params().effect != MapEditor::BrushEffect::PickColor);
    }
    // The polygon params apply with a polygon shape selected OR with a 3D
    // polygonal solid (pyramid / truncated pyramid base) — and the same
    // image/decal/pick exclusions the radius has.
    if (polygonSidesSpin_ || polygonAngleSpin_) {
        const bool polygonShape =
            brushTool_ &&
            (brushTool_->params().shape == MapEditor::BrushShape::FilledPolygon ||
             brushTool_->params().shape == MapEditor::BrushShape::PolygonOutline);
        const bool polygon3d = shape3dActive &&
            MapEditor::shape3DIsPolygonal(activeShape3d);
        const bool polygonEnabled =
            worldLoaded && brushTool_ && (polygonShape || polygon3d) &&
            !brushTool_->params().useImageMask && !decalEffect &&
            brushTool_->params().effect != MapEditor::BrushEffect::PickColor;
        if (polygonSidesSpin_) {
            polygonSidesSpin_->setEnabled(polygonEnabled);
        }
        if (polygonAngleSpin_) {
            polygonAngleSpin_->setEnabled(polygonEnabled);
        }
    }
    // 3D shape settings apply only while a 3D shape is selected; each field
    // only with the shapes that consume it (Shape3D.h predicates).
    if (shapeHeightSpin_) {
        shapeHeightSpin_->setEnabled(worldLoaded && shape3dActive);
    }
    if (shapeTopOffsetXSpin_) {
        shapeTopOffsetXSpin_->setEnabled(
            worldLoaded && MapEditor::shape3DHasTopOffset(activeShape3d));
    }
    if (shapeTopOffsetYSpin_) {
        shapeTopOffsetYSpin_->setEnabled(
            worldLoaded && MapEditor::shape3DHasTopOffset(activeShape3d));
    }
    if (shapeTopRadiusSpin_) {
        shapeTopRadiusSpin_->setEnabled(
            worldLoaded && MapEditor::shape3DHasTopRadius(activeShape3d));
    }
    if (shapeTopSideSpin_) {
        shapeTopSideSpin_->setEnabled(
            worldLoaded && MapEditor::shape3DHasTopSide(activeShape3d));
    }
    // Model brush: the use-toggle is enabled once a model is imported; the
    // mode buttons and the scales apply while the brush is active.
    if (actModelBrush_) {
        actModelBrush_->setEnabled(brushTool_ &&
                                   brushTool_->hasModelBrushData());
    }
    if (actModelPlace_) {
        actModelPlace_->setEnabled(worldLoaded && modelBrushActive);
    }
    if (actModelAdd_) {
        actModelAdd_->setEnabled(worldLoaded && modelBrushActive);
    }
    if (actModelSubtract_) {
        actModelSubtract_->setEnabled(worldLoaded && modelBrushActive);
    }
    if (modelHeightSpin_) {
        modelHeightSpin_->setEnabled(worldLoaded && modelBrushActive);
    }
    if (modelScaleXSpin_) {
        modelScaleXSpin_->setEnabled(worldLoaded && modelBrushActive);
    }
    if (modelScaleYSpin_) {
        modelScaleYSpin_->setEnabled(worldLoaded && modelBrushActive);
    }
    // T3.5 — the image scale applies to whichever image stamp is active: the
    // influence mask or the ImageDecal decal.
    if (brushMaskScaleSpin_) {
        const bool imageMaskActive = brushTool_ &&
            brushTool_->params().useImageMask && brushTool_->hasImageMask();
        const bool decalActive = decalEffect && brushTool_->hasDecalImage();
        brushMaskScaleSpin_->setEnabled(worldLoaded &&
                                        (imageMaskActive || decalActive));
        brushMaskScaleSpin_->setToolTip(
            decalActive
                ? tr("Scale of the loaded image decal relative to its original "
                     "pixel size (the stamp stays centred under the cursor).")
                : tr("Scale of the loaded brush image relative to its original "
                     "pixel size (the mask stays centred under the cursor)."));
    }
    // The color button: painting and pick readout.
    if (brushColorButton_) {
        const MapEditor::BrushEffect effect =
            brushTool_ ? brushTool_->params().effect
                       : MapEditor::BrushEffect::None;
        brushColorButton_->setEnabled(
            worldLoaded &&
            (effect == MapEditor::BrushEffect::SetColor ||
             effect == MapEditor::BrushEffect::PickColor));
        // The paint height/depth spins serve the color brush AND the image
        // decal (an explicit height below the surface paints the pixel
        // colors into the internal voxel layers instead of the top).
        const bool colorActive =
            worldLoaded &&
            (effect == MapEditor::BrushEffect::SetColor ||
             effect == MapEditor::BrushEffect::ImageDecal);
        // Null-guarded: these live on the brush / Sculpting toolbars and
        // updateEnabledStates re-enters during the toolbar build (a spinbox
        // change inside syncControls emits actionStatesNeedUpdate again).
        if (brushPaintHeightSpin_) {
            brushPaintHeightSpin_->setEnabled(colorActive);
        }
        if (brushPaintDepthSpin_) {
            brushPaintDepthSpin_->setEnabled(colorActive);
        }
        if (brushPaintTransparentCheck_) {
            brushPaintTransparentCheck_->setEnabled(colorActive);
        }
        if (brushPaintInteriorCheck_) {
            brushPaintInteriorCheck_->setEnabled(
                worldLoaded &&
                (brushTool_->params().shape3d != MapEditor::Shape3DKind::None ||
                 brushTool_->params().modelBrush));
        }
    }
    // T3.2 — the single height control serves the raise/lower step and the
    // "Level to Height" target; with a 3D shape or the model brush + Paint
    // Color it is the volume base height (the paint starts there, see
    // BrushTool::applyShape3DPaint / applyModelBrushPaint).
    if (brushLevelSpin_) {
        const MapEditor::BrushEffect effect =
            brushTool_ ? brushTool_->params().effect
                       : MapEditor::BrushEffect::None;
        const bool shapePaintBase =
            effect == MapEditor::BrushEffect::SetColor &&
            (brushTool_->params().shape3d != MapEditor::Shape3DKind::None ||
             brushTool_->params().modelBrush);
        brushLevelSpin_->setEnabled(
            worldLoaded &&
            (effect == MapEditor::BrushEffect::ChangeHeight ||
             effect == MapEditor::BrushEffect::AlignVariableH ||
             shapePaintBase));
    }
}
void BrushUiController::setColorRgb(unsigned int rgb)
{
    syncColorControls();
}

void BrushUiController::buildSculptToolbar()
{
    // The toolbar break must come BEFORE the toolbar is inserted: it forces
    // this toolbar onto its own row below the View/Brush toolbars.
    window_->addToolBarBreak();

    QToolBar *bar = window_->addToolBar(tr("Sculpting"));
    bar->setObjectName(QStringLiteral("SculptToolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(22, 22));
    bar->setStyleSheet(
        QStringLiteral("QToolButton:enabled:hover, QToolButton:enabled:checked "
                       "{ background: rgba(0, 96, 170, 35); border-radius: 4px; }"));

    bar->addAction(actBrushSmooth_);
    bar->addAction(actBrushFlatten_);
    bar->addAction(actBrushTricubic_);
    bar->addAction(actBrushInflate_);
    bar->addAction(actBrushDeflate_);
    bar->addAction(actBrushGaussianBlur_);

    // The fraction spinbox (0..1) shared by both sculpting brushes: the part
    // of the distance towards the reference (average / fitted plane) that
    // ONE stamp moves a covered cell.
    sculptEffectSpin_ = new QDoubleSpinBox(bar);
    sculptEffectSpin_->setObjectName(QStringLiteral("sculptEffectSpin"));
    sculptEffectSpin_->setRange(0.0, 1.0);
    sculptEffectSpin_->setDecimals(2);
    sculptEffectSpin_->setSingleStep(0.05);
    sculptEffectSpin_->setValue(0.5);
    sculptEffectSpin_->setToolTip(
        tr("Sculpting fraction (0..1) shared by Smooth and Flatten: the "
           "part of the distance towards the reference (average height / "
           "fitted plane) that ONE stamp moves each covered cell. 1.0 "
           "reaches the reference in a single stamp."));
    connect(sculptEffectSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.sculptEffect = value;
                brushTool_->setParams(params);
            });
    bar->addWidget(new QLabel(tr("Fraction:"), bar));
    bar->addWidget(sculptEffectSpin_);

    // The Gaussian-blur convolution radius: strictly less than the stamp
    // radius — the maximum follows the brush-radius spinbox (synced in
    // syncBrushControls, like the stroke width does).
    blurRadiusSpin_ = new QSpinBox(bar);
    blurRadiusSpin_->setObjectName(QStringLiteral("blurRadiusSpin"));
    blurRadiusSpin_->setRange(1, MapEditor::BrushTool::maxRadius() - 1);
    blurRadiusSpin_->setValue(4);
    blurRadiusSpin_->setSuffix(tr(" c"));
    blurRadiusSpin_->setToolTip(
        tr("Gaussian-blur kernel radius in cells (strictly less than the "
           "stamp radius; sigma = radius/2). Only active with the Gaussian "
           "Blur brush."));
    connect(blurRadiusSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.blurRadius = value;
        brushTool_->setParams(params);
        syncControls();
    });
    bar->addWidget(new QLabel(tr("Blur:"), bar));
    bar->addWidget(blurRadiusSpin_);

    // --- 3D shape brushes --------------------------------------------------
    bar->addSeparator();
    bar->addAction(actBrush3DCone_);
    bar->addAction(actBrush3DPyramid_);
    bar->addAction(actBrush3DTruncCone_);
    bar->addAction(actBrush3DTruncPyramid_);
    bar->addAction(actBrush3DTorus_);
    bar->addAction(actBrush3DEllipsoid_);

    // Shape Height: the amplitude of the 3D shapes — the per-cell target is
    // Height + profile * Shape Height. 0 flattens the footprint down to the
    // Height base in Lower / Set-to-Height modes (the dam is removed there).
    shapeHeightSpin_ = new QSpinBox(bar);
    shapeHeightSpin_->setObjectName(QStringLiteral("shapeHeightSpin"));
    shapeHeightSpin_->setRange(0, MAX_VX_HEIGHT_WHOLE);
    shapeHeightSpin_->setValue(32);
    shapeHeightSpin_->setSuffix(tr(" v"));
    shapeHeightSpin_->setToolTip(
        tr("Shape Height of the 3D brushes: the amplitude of the solid — "
           "every covered cell moves towards Height + the shape profile * "
           "Shape Height (whole voxels). 0 flattens the footprint to the "
           "Height base (Lower / Set-to-Height remove the dam)."));
    connect(shapeHeightSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.shapeHeight = value;
        brushTool_->setParams(params);
        syncControls();
    });
    bar->addWidget(new QLabel(tr("Height:"), bar));
    bar->addWidget(shapeHeightSpin_);

    // Upper part offset: the top circle/polygon centre shift (cells) — tilts
    // the cone/pyramid/frusta. The clamp keeps the top strictly inside the
    // base (applied in Shape3D.h).
    shapeTopOffsetXSpin_ = new QSpinBox(bar);
    shapeTopOffsetXSpin_->setObjectName(QStringLiteral("shapeTopOffsetXSpin"));
    shapeTopOffsetXSpin_->setRange(-MapEditor::BrushTool::maxRadius(),
                                   MapEditor::BrushTool::maxRadius());
    shapeTopOffsetXSpin_->setSuffix(tr(" c"));
    shapeTopOffsetXSpin_->setToolTip(
        tr("Upper part X offset (cells): the top circle/polygon centre shift "
           "— tilts the cone, pyramid, truncated cone and truncated pyramid. "
           "The offset is clamped to keep the top inside the base."));
    connect(shapeTopOffsetXSpin_, &QSpinBox::valueChanged, this,
            [this](int value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.shapeTopOffsetX = value;
                brushTool_->setParams(params);
                syncControls();
            });
    bar->addWidget(new QLabel(tr("Top X:"), bar));
    bar->addWidget(shapeTopOffsetXSpin_);

    shapeTopOffsetYSpin_ = new QSpinBox(bar);
    shapeTopOffsetYSpin_->setObjectName(QStringLiteral("shapeTopOffsetYSpin"));
    shapeTopOffsetYSpin_->setRange(-MapEditor::BrushTool::maxRadius(),
                                   MapEditor::BrushTool::maxRadius());
    shapeTopOffsetYSpin_->setSuffix(tr(" c"));
    shapeTopOffsetYSpin_->setToolTip(
        tr("Upper part Y offset (cells): the top circle/polygon centre shift "
           "— tilts the cone, pyramid, truncated cone and truncated pyramid. "
           "The offset is clamped to keep the top inside the base."));
    connect(shapeTopOffsetYSpin_, &QSpinBox::valueChanged, this,
            [this](int value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.shapeTopOffsetY = value;
                brushTool_->setParams(params);
                syncControls();
            });
    bar->addWidget(new QLabel(tr("Top Y:"), bar));
    bar->addWidget(shapeTopOffsetYSpin_);

    // Upper part radius: the truncated cone's top radius, or the torus'
    // inner radius. Clamped below the base radius (Shape3D.h).
    shapeTopRadiusSpin_ = new QSpinBox(bar);
    shapeTopRadiusSpin_->setObjectName(QStringLiteral("shapeTopRadiusSpin"));
    shapeTopRadiusSpin_->setRange(0, MapEditor::BrushTool::maxRadius());
    shapeTopRadiusSpin_->setSuffix(tr(" c"));
    shapeTopRadiusSpin_->setToolTip(
        tr("Upper part radius (cells): the truncated cone's top radius, or "
           "the inner radius of the half torus. Must stay below the base "
           "radius (clamped)."));
    connect(shapeTopRadiusSpin_, &QSpinBox::valueChanged, this,
            [this](int value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.shapeTopRadius = value;
                brushTool_->setParams(params);
                syncControls();
            });
    bar->addWidget(new QLabel(tr("Top R:"), bar));
    bar->addWidget(shapeTopRadiusSpin_);

    // Upper part side length: the truncated pyramid's top edge length (the
    // top polygon stays similar to the base). Clamped below the base.
    shapeTopSideSpin_ = new QSpinBox(bar);
    shapeTopSideSpin_->setObjectName(QStringLiteral("shapeTopSideSpin"));
    shapeTopSideSpin_->setRange(0, 2 * MapEditor::BrushTool::maxRadius());
    shapeTopSideSpin_->setSuffix(tr(" c"));
    shapeTopSideSpin_->setToolTip(
        tr("Upper part side length (cells): the top edge length of the "
           "truncated pyramid (the top polygon is similar to the base). "
           "Must stay below the base (clamped)."));
    connect(shapeTopSideSpin_, &QSpinBox::valueChanged, this,
            [this](int value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.shapeTopSide = value;
                brushTool_->setParams(params);
                syncControls();
            });
    bar->addWidget(new QLabel(tr("Top side:"), bar));
    bar->addWidget(shapeTopSideSpin_);

    // 3D shapes / the model brush: paint the whole interior of the solid
    // (every voxel level from the base up to its surface at the column)
    // instead of only the top slab. Lives here on the 3D brushes toolbar —
    // the option only means something with a solid selected.
    brushPaintInteriorCheck_ = new QCheckBox(tr("Paint interior"), bar);
    brushPaintInteriorCheck_->setToolTip(
        tr("3D shape brushes and the model brush: paint every voxel inside "
           "the solid with the brush color (the full volume from the base up "
           "to the surface at each column); unchecked paints only the top "
           "slab of the Depth layers at the surface."));
    connect(brushPaintInteriorCheck_, &QCheckBox::toggled, this,
            [this](bool on) {
                MapEditor::BrushParams params = brushTool_->params();
                params.paintInterior3d = on;
                brushTool_->setParams(params);
            });
    bar->addWidget(brushPaintInteriorCheck_);

    // Type buttons are created in createBrushActions() — mirror the rest of
    // the initial control state into the new toolbar.
    emit actionStatesNeedUpdate();
}

void BrushUiController::buildModifierToolbar()
{
    // The toolbar break must come BEFORE the toolbar is inserted: it forces
    // this toolbar onto its own row below the Sculpting toolbar (same rule
    // the Sculpting toolbar uses against the View/Brush toolbars).
    window_->addToolBarBreak();

    QToolBar *bar = window_->addToolBar(tr("Brush Modifiers"));
    bar->setObjectName(QStringLiteral("ModifierToolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(22, 22));
    bar->setStyleSheet(
        QStringLiteral("QToolButton:enabled:hover, QToolButton:enabled:checked "
                       "{ background: rgba(0, 96, 170, 35); border-radius: 4px; }"));

    // The two mutually exclusive height-gate conditions (created in
    // createBrushActions() — the same actions the Edit menu carries).
    bar->addAction(actGateAbove_);
    bar->addAction(actGateBelow_);

    // The gate comparison height (whole voxels). Enabled only while a gate
    // is active; its mirror in the Edit → Brush Modifiers menu stays in
    // sync (see buildEditMenu / syncBrushControls).
    gateHeightSpin_ = new QSpinBox(bar);
    gateHeightSpin_->setObjectName(QStringLiteral("gateHeightSpin"));
    gateHeightSpin_->setRange(0, MAX_VX_HEIGHT_WHOLE);
    gateHeightSpin_->setSuffix(tr(" vox"));
    gateHeightSpin_->setToolTip(
        tr("Gate height (whole voxels; 1 voxel = 1/32 m): the active "
           "above/below modifier applies the brush effect only to the voxels "
           "whose dam height is on its side of this height. Voxels exactly at "
           "the gate height are never edited. Dam mode only."));
    connect(gateHeightSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.gateHeight = MapEditor::BrushTool::clampGateHeight(value);
        brushTool_->setParams(params);
        if (gateHeightMenuSpin_ &&
            gateHeightMenuSpin_->value() != params.gateHeight) {
            gateHeightMenuSpin_->blockSignals(true);
            gateHeightMenuSpin_->setValue(params.gateHeight);
            gateHeightMenuSpin_->blockSignals(false);
        }
    });
    bar->addWidget(new QLabel(tr("Gate:"), bar));
    bar->addWidget(gateHeightSpin_);

    emit actionStatesNeedUpdate();
}

void BrushUiController::buildModelToolbar()
{
    // The toolbar break must come BEFORE the toolbar is inserted: it forces
    // this toolbar onto its own row below the Sculpting/Modifier toolbars
    // (same rule the others use).
    window_->addToolBarBreak();

    QToolBar *bar = window_->addToolBar(tr("Model"));
    bar->setObjectName(QStringLiteral("ModelToolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(22, 22));
    bar->setStyleSheet(
        QStringLiteral("QToolButton:enabled:hover, QToolButton:enabled:checked "
                       "{ background: rgba(0, 96, 170, 35); border-radius: 4px; }"));

    actImportModel_ = new QAction(makeLoadImageIcon(), tr("Import &Model..."),
                                  this);
    actImportModel_->setToolTip(
        tr("Import a PLY model (ASCII or binary) as the heightmap brush: the "
           "model's XY rectangle becomes the stamp, each cell takes the max "
           "vertex height (scaled by the Height spinbox) and the top vertex "
           "colour as an exact 24-bit RGB (no palette quantization, like the "
           "Image Decal). A texture named by a PLY header comment is sampled "
           "through the UVs."));
    connect(actImportModel_, &QAction::triggered, this,
            &BrushUiController::onImportModel);
    bar->addAction(actImportModel_);

    actModelBrush_ = new QAction(makeBrushIcon(), tr("Use &Model Brush"),
                                 this);
    actModelBrush_->setToolTip(
        tr("Stamp the imported model as a heightmap (Dam layer only). "
           "Place sets the dam to the scaled model height and paints the "
           "palette colours; Add raises the covered average by the local "
           "model height; Subtract lowers it (removing the dam where the "
           "result drops to the terrain)."));
    actModelBrush_->setCheckable(true);
    actModelBrush_->setEnabled(false); // enabled by a successful import
    connect(actModelBrush_, &QAction::triggered, this, [this](bool checked) {
        if (checked && !brushTool_->hasModelBrushData()) {
            actModelBrush_->setChecked(false);
            return;
        }
        if (checked && (!session_ || !session_->isWorldLoaded())) {
            showStatus(
                tr("Load a world first — the model brush needs the Dam "
                   "palette of the loaded world."),
                5000);
            actModelBrush_->setChecked(false);
            return;
        }
        MapEditor::BrushParams params = brushTool_->params();
        params.modelBrush = checked;
        if (checked) {
            params.mode = MapEditor::BrushMode::Dam;
            params.shape3d = MapEditor::Shape3DKind::None;
            params.useImageMask = false;
            // Paint Color combines with the model stamp (volume paint
            // without height changes) — keep it; the other non-height
            // types fall back to a raise so the model has an effect.
            if (params.effect != MapEditor::BrushEffect::ChangeHeight &&
                params.effect != MapEditor::BrushEffect::AlignVariableH &&
                params.effect != MapEditor::BrushEffect::SetColor) {
                params.effect = MapEditor::BrushEffect::ChangeHeight;
                params.direction = 1;
            }
            quantizeModelBrush();
        }
        brushTool_->setParams(params);
        if (viewport_) {
            viewport_->setEditorMode(
                checked ? MapEditor::ViewportWidget::EditorMode::Brush
                        : MapEditor::ViewportWidget::EditorMode::Brush);
        }
        syncControls();
    });
    bar->addAction(actModelBrush_);

    bar->addSeparator();

    // Place / Add / Subtract — the exclusive mode group of the model brush.
    QActionGroup *modelModeGroup = new QActionGroup(this);
    const auto addModelMode = [this, modelModeGroup, bar](
            QAction *&slot, int mode, const QString &text,
            const QString &toolTip) {
        QAction *action = new QAction(text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);
        action->setChecked(brushTool_->params().modelMode == mode);
        connect(action, &QAction::triggered, this, [this, mode] {
            MapEditor::BrushParams params = brushTool_->params();
            params.modelMode = mode;
            brushTool_->setParams(params);
            syncControls();
        });
        modelModeGroup->addAction(action);
        slot = action;
        bar->addAction(action);
        return action;
    };
    addModelMode(actModelPlace_, 0, tr("Place"),
                 tr("Place mode: the stamp SETS the dam to the scaled model "
                    "height and paints the model's exact 24-bit colours "
                    "(a Set-to-Height style stamp)."));
    addModelMode(actModelAdd_, 1, tr("Add"),
                 tr("Add mode: the covered area's average height is RAISED "
                    "by the local model height (cells below the target move "
                    "up only)."));
    addModelMode(actModelSubtract_, 2, tr("Subtract"),
                 tr("Subtract mode: the covered area's average height is "
                    "LOWERED by the local model height; where the result "
                    "drops to the terrain the dam is removed."));
    actModelPlace_->setChecked(true);

    // Height: the model's z range is scaled proportionally onto 0..Height
    // (whole voxels).
    modelHeightSpin_ = new QSpinBox(bar);
    modelHeightSpin_->setObjectName(QStringLiteral("modelHeightSpin"));
    modelHeightSpin_->setRange(0, MAX_VX_HEIGHT_WHOLE);
    modelHeightSpin_->setValue(32);
    modelHeightSpin_->setSuffix(tr(" v"));
    modelHeightSpin_->setToolTip(
        tr("Model brush height: the model's z range maps onto 0..Height "
           "(whole voxels), proportionally scaled."));
    connect(modelHeightSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.modelHeight = value;
        brushTool_->setParams(params);
        syncControls();
    });
    bar->addWidget(new QLabel(tr("Height:"), bar));
    bar->addWidget(modelHeightSpin_);

    // X/Y scales: the placement rectangle as a fraction of the model size.
    modelScaleXSpin_ = new QDoubleSpinBox(bar);
    modelScaleXSpin_->setObjectName(QStringLiteral("modelScaleXSpin"));
    modelScaleXSpin_->setRange(0.0, 1.0);
    modelScaleXSpin_->setDecimals(2);
    modelScaleXSpin_->setSingleStep(0.05);
    modelScaleXSpin_->setValue(1.0);
    modelScaleXSpin_->setToolTip(
        tr("X scale of the placement rectangle (0..1, fraction of the "
           "imported model width)."));
    connect(modelScaleXSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.modelScaleX = value;
                brushTool_->setParams(params);
                syncControls();
            });
    bar->addWidget(new QLabel(tr("Scale X:"), bar));
    bar->addWidget(modelScaleXSpin_);

    modelScaleYSpin_ = new QDoubleSpinBox(bar);
    modelScaleYSpin_->setObjectName(QStringLiteral("modelScaleYSpin"));
    modelScaleYSpin_->setRange(0.0, 1.0);
    modelScaleYSpin_->setDecimals(2);
    modelScaleYSpin_->setSingleStep(0.05);
    modelScaleYSpin_->setValue(1.0);
    modelScaleYSpin_->setToolTip(
        tr("Y scale of the placement rectangle (0..1, fraction of the "
           "imported model height/length)."));
    connect(modelScaleYSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.modelScaleY = value;
                brushTool_->setParams(params);
                syncControls();
            });
    bar->addWidget(new QLabel(tr("Scale Y:"), bar));
    bar->addWidget(modelScaleYSpin_);

    emit actionStatesNeedUpdate();
}

void BrushUiController::onImportModel()
{
    const QString filePath = QFileDialog::getOpenFileName(
        window_, tr("Import PLY Model"), QString(),
        tr("PLY models (*.ply);;All Files (*)"));
    if (filePath.isEmpty()) {
        return;
    }
    const MapEditor::ModelBrush::ImportResult imported =
        MapEditor::ModelBrush::importPly(
            std::filesystem::u8path(filePath.toStdString()));
    const QString name = QFileInfo(filePath).fileName();
    if (!imported.ok) {
        QMessageBox::warning(
            window_, tr("Import Model"),
            tr("Unable to import from PLY the model %1\n%2")
                .arg(name, QString::fromStdString(imported.error)));
        return;
    }

    modelImportPath_ = filePath;
    // Store the raster first; the palette quantization (it needs the loaded
    // world's Dam palette) fills the indices in right after when possible.
    brushTool_->setModelBrushData(imported.model, MapEditor::ImageDecal::DecalImage());
    bool quantized = false;
    if (session_ && session_->isWorldLoaded()) {
        quantized = quantizeModelBrush();
    }
    if (actModelBrush_) {
        actModelBrush_->setEnabled(true);
    }
    showStatus(
        tr("Model imported: %1x%2 cells (%3). %4")
            .arg(imported.model.width)
            .arg(imported.model.height)
            .arg(name)
            .arg(quantized ? tr("Ready — toggle \"Use Model Brush\".")
                           : tr("Load a world, then toggle \"Use Model "
                                "Brush\".")),
        8000);
}

bool BrushUiController::quantizeModelBrush()
{
    if (!session_ || !session_->isWorldLoaded() ||
        !brushTool_->hasModelBrushData()) {
        return false;
    }
    const MapEditor::ModelBrush::ModelRaster& raster =
        brushTool_->modelRaster();
    // The raster colours as an RGBA image (all cells opaque — the holes were
    // filled with the model's average colour at import).
    std::vector<unsigned char> rgba(
        static_cast<std::size_t>(raster.width) * raster.height * 4, 255);
    for (std::size_t i = 0; i < raster.rgb.size(); ++i) {
        rgba[i * 4 + 0] = static_cast<unsigned char>((raster.rgb[i] >> 16) & 0xFF);
        rgba[i * 4 + 1] = static_cast<unsigned char>((raster.rgb[i] >> 8) & 0xFF);
        rgba[i * 4 + 2] = static_cast<unsigned char>(raster.rgb[i] & 0xFF);
    }
    const std::vector<unsigned char> palette =
        session_->paletteBytes(MapEditor::EditorSession::PaletteLayer::Dam);
    // True color: the exact 24-bit raster colours are kept; the palette is
    // consulted only for the nearest-entry side band (SurBuf surface type).
    const MapEditor::ImageDecal::PrepareResult prepared =
        MapEditor::ImageDecal::prepare(rgba.data(), raster.width,
                                       raster.height, palette.data());
    if (!prepared.ok) {
        showStatus(
            tr("Model brush colours cannot be prepared: %1")
                .arg(QString::fromStdString(prepared.error)),
            8000);
        return false;
    }
    brushTool_->setModelBrushData(raster, prepared.image);
    return true;
}

void BrushUiController::createBrushTypeActions()
{
    // --- Type: what the brush does -----------------------------------
    brushTypeGroup_ = new QActionGroup(this);

    actBrushRaise_ = addBrushTypeAction(brushTypeGroup_,
        makeHeightArrowIcon(true), tr("Raise Terrain"),
        tr("The brush raises the terrain under it (left drag in the 2D view)."),
        MapEditor::BrushEffect::ChangeHeight, 1);
    actBrushLower_ = addBrushTypeAction(brushTypeGroup_,
        makeHeightArrowIcon(false), tr("Lower Terrain"),
        tr("The brush digs the terrain under it. Hard terrain slows the dig "
           "down just like in the game."),
        MapEditor::BrushEffect::ChangeHeight, -1);
    actBrushLevelZP_ = addBrushTypeAction(brushTypeGroup_,
        makeGroundLetterIcon(QStringLiteral("Z"), kInk),
        tr("Level to Zeroplast"),
        tr("The brush levels the terrain to the world zeroplast height in "
           "one stamp (strength has no effect on it) and marks the reached "
           "cells as zeroplast (Dam mode only — the zeroplast belongs to "
           "the dam domain)."),
        MapEditor::BrushEffect::Align4ZP, 1);
    actBrushLevelHeight_ = addBrushTypeAction(brushTypeGroup_,
        makeGroundLetterIcon(QStringLiteral("H"), kInk),
        tr("Level to Height"),
        tr("The brush levels the terrain to the height chosen next to it "
           "(whole voxels, 1 voxel = 1/32 m)."),
        MapEditor::BrushEffect::AlignVariableH, 1);

    // T3.3 — color brush: paints SurBuf with the selected palette index.
    actBrushColor_ = addBrushTypeAction(brushTypeGroup_,
        makeColorBrushIcon(), tr("Paint Color"),
        tr("The brush paints the surface color (SurBuf) with the selected "
           "palette index. Height is not changed.\n"
           "Geo palette: indices 0..127; Dam palette: 128..255."),
        MapEditor::BrushEffect::SetColor, 0);

    // T4.2 — the eyedropper as a brush type: the PickColor effect samples the
    // Geo/Dam pixel (SurBuf index) at the click cell into the color brush —
    // no buffer is written, the pick is a BrushTool point operation like any
    // other type (the interaction runs in the editor mode Eyedropper).
    actBrushPickColor_ = addBrushTypeAction(brushTypeGroup_,
        makeEyedropperIcon(), tr("Pick Color"),
        tr("The brush samples the surface color under the cursor (the Geo/Dam "
           "palette pixel) into the color brush instead of painting. A click "
           "or a drag over the map in the 2D and the 3D view; use the editor "
           "mode \"Eyedropper\" (E) as the switch."),
        MapEditor::BrushEffect::PickColor, 0);

    // T3.4 — plot soot (burn marks on the terrain).
    actBrushSoot_ = addBrushTypeAction(brushTypeGroup_,
        makeGroundLetterIcon(QStringLiteral("S"), QColor(80, 50, 20)),
        tr("Plot Soot"),
        tr("The brush marks cells as soot (burn residue). Zeroplast cells "
           "are never marked as soot — the game's rule."),
        MapEditor::BrushEffect::PlotSoot, 0);

    // T3.4 — very light dam: seed a thin dam layer at geo height.
    actBrushVeryLightDam_ = addBrushTypeAction(brushTypeGroup_,
        makeGroundLetterIcon(QStringLiteral("V"), QColor(100, 140, 180)),
        tr("Very Light Dam"),
        tr("The brush seeds a very light dam layer at the geo height and "
           "paints the surface with the very light dam texture index. "
           "Equivalent to the game's initial dam deployment."),
         MapEditor::BrushEffect::VeryLightDam, 0);

    // Scripted brush (JavaScript via duktape, Dam mode only). Selecting the
    // type forces the Dam layer; the pencil button next to it opens the
    // script editor window.
    actBrushScript_ = addBrushTypeAction(brushTypeGroup_,
        makeScriptBrushIcon(), tr("Scripted Brush"),
        tr("Runs a JavaScript (duktape) script at every stamp: the script "
           "drives the editor brush API — raiseHeight/lowerHeight/setHeight/"
           "setToZP/paintSoot/paintVeryLightDam at explicit positions and "
           "radii, paintColor with an RGB colour (nearest Dam palette "
           "entry), and the sculpting brushes applyBlur/applySmooth. Dam "
           "layer only. The whole stamp evaluation is ONE undo "
           "step; a 60 s time limit applies. Edit the script with the "
           "pencil button next to this one."),
        MapEditor::BrushEffect::Script, 0);
    // The scripted brush operates on the Dam layer only: selecting it
    // switches the mode, mirroring the zeroplast brushes.
    connect(actBrushScript_, &QAction::triggered, this, [this] {
        MapEditor::BrushParams params = brushTool_->params();
        params.mode = MapEditor::BrushMode::Dam;
        brushTool_->setParams(params);
        syncControls();
    });

    actBrushScriptEdit_ = new QAction(makeScriptEditIcon(),
                                      tr("Edit Scri&pt..."), this);
    actBrushScriptEdit_->setToolTip(
        tr("Open the script editor: write the JavaScript the scripted brush "
           "runs per stamp, pick the stamp preview shape. OK compiles the "
           "script (syntax check) and keeps the dialog open on errors."));
    connect(actBrushScriptEdit_, &QAction::triggered, this, [this] {
        // Seed a starter example the first time the window opens.
        QString seed = brushScript_;
        if (seed.isEmpty()) {
            seed = QStringLiteral(
                "// Scripted brush: runs once per stamp at the cursor.\n"
                "// Available API (Dam layer only):\n"
                "//   getCurrentPos() -> {x, y}\n"
                "//   getCurrentRadius() / getCurrentThickness() / getCurrentHeight()\n"
                "//   raiseHeight(pos, height, radius, square)\n"
                "//   lowerHeight(pos, height, radius, square)\n"
                "//   setHeight(pos, height, radius, square)\n"
                "//   setToZP(pos, radius, square)\n"
                "//   paintSoot(pos, radius, square)\n"
                "//   paintVeryLightDam(pos, radius, square)\n"
                "//   paintColor(pos, red, green, blue, radius, square)\n"
                "//   applyBlur(pos, radius, blurRadius, effect, square)\n"
                "//   applySmooth(pos, radius, effect, square)\n"
                "\n"
                "var p = getCurrentPos();\n"
                "var r = getCurrentRadius();\n"
                "// Example: a zeroplast pad with a coloured rim.\n"
                "setToZP({x: p.x, y: p.y}, r, false);\n"
                "paintColor({x: p.x, y: p.y - r - 2}, 255, 200, 0, 2, false);\n");
        }
        MapEditor::ScriptEditorDialog dialog(window_, seed, scriptStampShape_);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        brushScript_ = dialog.script();
        scriptStampShape_ = dialog.stampShape();
        // Keep the tool in step: the script the stamp will run.
        MapEditor::BrushParams params = brushTool_->params();
        params.script = brushScript_.toStdString();
        brushTool_->setParams(params);
        syncControls();
        showStatus(tr("Scripted brush script updated "
                                    "(%1 characters).")
                                     .arg(brushScript_.size()),
                                 5000);
    });

    // ImageDecal — a palette-limited image stamp. The button is both the brush
    // type and the image picker: selecting it switches to the decal brush,
    // forces the Dam layer (the decal colours come from the Dam palette) and
    // opens the file dialog. Cancelling keeps the previously loaded decal.
    actBrushImageDecal_ = addBrushTypeAction(brushTypeGroup_,
        makeDecalIcon(), tr("Image Decal"),
        tr("Stamps a picture onto the dam surface in its exact 24-bit "
           "colors (no palette quantization; the nearest Dam palette entry "
           "is kept as the surface type). Dam layer only; transparent "
           "pixels are never painted. Click to choose a PNG/TGA/BMP image "
           "(max 2048x2048)."),
        MapEditor::BrushEffect::ImageDecal, 0);
    connect(actBrushImageDecal_, &QAction::triggered, this, [this] {
        MapEditor::BrushParams params = brushTool_->params();
        params.effect = MapEditor::BrushEffect::ImageDecal;
        params.mode = MapEditor::BrushMode::Dam; // the decal owns the Dam layer
        params.useImageMask = false;
        params.shape3d = MapEditor::Shape3DKind::None;
        params.modelBrush = false;
        brushTool_->setParams(params);
        if (viewport_) {
            viewport_->setEditorMode(
                MapEditor::ViewportWidget::EditorMode::Brush);
        }
        onLoadDecalImage();
        // Cancelled with no decal loaded: do not leave a brush that paints
        // nothing selected.
        if (!brushTool_->hasDecalImage()) {
            params.effect = MapEditor::BrushEffect::ChangeHeight;
            params.direction = 1;
            brushTool_->setParams(params);
        }
        syncControls();
    });
}

void BrushUiController::createSculptTypeActions()
{
    // --- Sculpting (Smooth / Flatten) ------------------------------------
    // Dam-mode only; selecting either forces the Dam layer like the other
    // Dam-only types. The fraction spinbox lives on the toolbar between the
    // two buttons (sculpting section, after the delimiter).
    const auto addSculptType = [this](
            QIcon icon, const QString& text, const QString& toolTip,
            MapEditor::BrushEffect effect) {
        QAction* action = addBrushTypeAction(brushTypeGroup_, icon, text,
                                             toolTip, effect, 0);
        // Dam layer only — mirror the scripted brush mode forcing.
        connect(action, &QAction::triggered, this, [this] {
            MapEditor::BrushParams params = brushTool_->params();
            params.mode = MapEditor::BrushMode::Dam;
            brushTool_->setParams(params);
            syncControls();
        });
        return action;
    };
    actBrushSmooth_ = addSculptType(
        makeSmoothBrushIcon(), tr("Smooth"),
        tr("Sculpting (Dam layer only): averages the surface height under "
           "the stamp and moves every covered cell a fraction towards that "
           "average — bumps and dips blend away while the overall level "
           "stays. The fraction spinbox next to it controls the blend "
           "(1.0 = full average in one stamp)."),
        MapEditor::BrushEffect::Smooth);
    actBrushFlatten_ = addSculptType(
        makeFlattenBrushIcon(), tr("Flatten"),
        tr("Sculpting (Dam layer only): fits a least-squares plane through "
           "the surface under the stamp and moves every covered cell a "
           "fraction towards that plane — large-scale slopes survive, local "
           "bumps die. Uses the same fraction spinbox as Smooth."),
        MapEditor::BrushEffect::Flatten);
    actBrushTricubic_ = addSculptType(
        makeTricubicBrushIcon(), tr("Tricubic"),
        tr("Sculpting (Dam layer only): block-averages the surface under "
           "the stamp into a coarse lattice and moves every covered cell a "
           "fraction towards the tricubic (bicubic Catmull-Rom) "
           "interpolation of that lattice — detail smaller than the lattice "
           "stride is smoothed away while curved large-scale forms survive "
           "smoothly. Uses the same fraction spinbox as Smooth/Flatten."),
        MapEditor::BrushEffect::Tricubic);
    actBrushInflate_ = addSculptType(
        makeInflateBrushIcon(), tr("Inflate"),
        tr("Sculpting (Dam layer only): offsets the surface along its "
           "voxel normals — every covered cell moves a fraction along the "
           "outward normal, so bumps grow outward-and-up. Where displaced "
           "parts intersect, the HIGHER height wins. Uses the same "
           "fraction spinbox as Smooth/Flatten."),
        MapEditor::BrushEffect::Inflate);
    actBrushDeflate_ = addSculptType(
        makeDeflateBrushIcon(), tr("Deflate"),
        tr("Sculpting (Dam layer only): the inverse of Inflate — offsets "
           "the surface along the inward voxel normals, so forms shrink "
           "inward-and-down. Where displaced parts intersect, the LOWER "
           "height wins. Uses the same fraction spinbox as Smooth/Flatten."),
        MapEditor::BrushEffect::Deflate);
    actBrushGaussianBlur_ = addSculptType(
        makeGaussianBlurBrushIcon(), tr("Gaussian Blur"),
        tr("Sculpting (Dam layer only): convolves the surface under the "
           "stamp with a Gaussian kernel (radius = the blur-radius "
           "spinbox, sigma = radius/2) and moves every covered cell a "
           "fraction towards its blurred height — planes reproduce "
           "exactly, noise smaller than the radius dies. Uses the same "
           "fraction spinbox as the other sculpting brushes."),
        MapEditor::BrushEffect::GaussianBlur);
}

void BrushUiController::createShape3DActions()
{
    // --- 3D shape brushes --------------------------------------------------
    // Dam-mode solids stamped instead of a flat mask (Shape3D.h). They layer
    // on the COMMON brush types: with Raise/Lower Terrain the cells close on
    // the shape from below/above only, with Set-to-Height the Height spin is
    // the shape's base and the move goes both ways, with Paint Color the
    // shape's volume is painted without touching heights. Selecting a 3D
    // shape forces the Dam layer and drops any other non-height brush type
    // back to a raise; selecting any other brush type or stamp shape clears
    // the 3D shape, except Paint Color (mirrors the image-mask rule).
    QActionGroup *shape3dGroup = new QActionGroup(this);
    const auto addShape3D = [this, shape3dGroup](
            MapEditor::Shape3DKind kind, const QString &text,
            const QString &toolTip) {
        QAction *action = new QAction(makeShape3DIcon(kind), text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);
        action->setChecked(brushTool_->params().shape3d == kind);
        connect(action, &QAction::triggered, this, [this, kind] {
            MapEditor::BrushParams params = brushTool_->params();
            params.shape3d = kind;
            params.mode = MapEditor::BrushMode::Dam;
            params.useImageMask = false;
            params.modelBrush = false;
            // Paint Color combines with the shapes (volume paint without
            // height changes) — keep it; the other non-height types fall
            // back to a raise so the shape has an immediate effect.
            if (params.effect != MapEditor::BrushEffect::ChangeHeight &&
                params.effect != MapEditor::BrushEffect::AlignVariableH &&
                params.effect != MapEditor::BrushEffect::SetColor) {
                params.effect = MapEditor::BrushEffect::ChangeHeight;
                params.direction = 1;
            }
            brushTool_->setParams(params);
            if (viewport_) {
                viewport_->setEditorMode(
                    MapEditor::ViewportWidget::EditorMode::Brush);
            }
            syncControls();
        });
        shape3dGroup->addAction(action);
        return action;
    };
    actBrush3DCone_ = addShape3D(
        MapEditor::Shape3DKind::Cone, tr("Cone"),
        tr("3D shape brush (Dam layer only): stamps a cone — the base is the "
           "stamp footprint, every covered cell moves towards Height + the "
           "cone profile * Shape Height. Combine with Raise / Lower / "
           "Set-to-Height; Paint Color paints the cone's volume without "
           "touching heights (Paint interior = the whole volume). The upper "
           "part offset tilts the apex."));
    actBrush3DPyramid_ = addShape3D(
        MapEditor::Shape3DKind::Pyramid, tr("Pyramid"),
        tr("3D shape brush (Dam layer only): stamps a pyramid on a regular "
           "polygon base (the polygon side count and angle spins shape the "
           "base). Otherwise like the cone."));
    actBrush3DTruncCone_ = addShape3D(
        MapEditor::Shape3DKind::TruncatedCone, tr("Truncated Cone"),
        tr("3D shape brush (Dam layer only): stamps a cone truncated at the "
           "upper part radius (top circle), optionally tilted with the upper "
           "part offset."));
    actBrush3DTruncPyramid_ = addShape3D(
        MapEditor::Shape3DKind::TruncatedPyramid, tr("Truncated Pyramid"),
        tr("3D shape brush (Dam layer only): stamps a pyramid truncated at "
           "the upper part side length (a smaller similar polygon on top), "
           "optionally shifted with the upper part offset."));
    actBrush3DTorus_ = addShape3D(
        MapEditor::Shape3DKind::Torus, tr("Half Torus"),
        tr("3D shape brush (Dam layer only): stamps the upper half of a "
           "torus — a ring ridge. The upper part radius is the ring's INNER "
           "radius; the base radius is the outer one."));
    actBrush3DEllipsoid_ = addShape3D(
        MapEditor::Shape3DKind::HalfEllipsoid, tr("Half Ellipsoid"),
        tr("3D shape brush (Dam layer only): stamps a half ellipsoid — a "
           "dome with a circular base."));
}

void BrushUiController::createBrushModeActions()
{
    // --- Mode: which terrain layer the height tools drive -------------
    QActionGroup *modeGroup = new QActionGroup(this);
    const auto addMode = [this, modeGroup](QIcon icon, const QString &text,
                                           const QString &toolTip,
                                           MapEditor::BrushMode mode) {
        QAction *action = new QAction(icon, text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);
        action->setChecked(brushTool_->params().mode == mode);
        connect(action, &QAction::triggered, this, [this, mode] {
            MapEditor::BrushParams params = brushTool_->params();
            params.mode = mode;
            // The zeroplast brushes are a Dam-mode tool (the zeroplast
            // belongs to the dam domain) — the UI keeps them disabled in the
            // Geo mode, so a type left active by the switch falls back to a
            // plain raise. Same for the scripted and sculpting brushes
            // (Dam mode only).
            if (mode == MapEditor::BrushMode::Geo &&
                (params.effect == MapEditor::BrushEffect::Align4ZP ||
                 params.effect == MapEditor::BrushEffect::ImageDecal ||
                 params.effect == MapEditor::BrushEffect::Script ||
                 params.effect == MapEditor::BrushEffect::Smooth ||
                 params.effect == MapEditor::BrushEffect::Flatten ||
                 params.effect == MapEditor::BrushEffect::Tricubic ||
                 params.effect == MapEditor::BrushEffect::Inflate ||
                 params.effect == MapEditor::BrushEffect::Deflate ||
                 params.effect == MapEditor::BrushEffect::GaussianBlur)) {
                params.effect = MapEditor::BrushEffect::ChangeHeight;
                params.direction = 1;
            }
            // The 3D shape brushes own the Dam layer like the sculpting
            // types — switching to Geo drops the solid back to a plain
            // raise. Same for the model-heightmap stamp.
            if (mode == MapEditor::BrushMode::Geo &&
                params.shape3d != MapEditor::Shape3DKind::None) {
                params.shape3d = MapEditor::Shape3DKind::None;
            }
            if (mode == MapEditor::BrushMode::Geo && params.modelBrush) {
                params.modelBrush = false;
            }
            brushTool_->setParams(params);
            syncControls();
        });
        modeGroup->addAction(action);
        return action;
    };

    actBrushGeo_ = addMode(
        makeGroundLetterIcon(QStringLiteral("G"), kInk), tr("Geo Layer"),
        tr("The height tools drive the geo layer (VxGBuf) only — a dam above "
           "the surface is never created or removed."),
        MapEditor::BrushMode::Geo);
    actBrushDam_ = addMode(
        makeGroundLetterIcon(QStringLiteral("D"), kAccent), tr("Dam Layer"),
        tr("The height tools drive the dam layer: raising seeds the dam, "
           "digging it down to the geo height removes it. The dam is always "
           "at or above the geo."),
        MapEditor::BrushMode::Dam);
}

void BrushUiController::createBrushGateActions()
{
    // --- Height-gate modifiers (Dam mode only) -------------------------
    // Two mutually exclusive PER-VOXEL conditions on top of any brush type:
    // the effect touches a covered voxel only when its own dam height is
    // above / below the gate height. Both unchecked = no gate. Enabling a
    // gate forces the Dam layer (the gate reads the dam height). The same
    // actions live on the Brush Modifiers toolbar and in the Edit menu.
    const auto addGate = [this](bool above) {
        QAction *action =
            new QAction(makeGateIcon(above),
                        above ? tr("Apply Above Height")
                              : tr("Apply Below Height"),
                        this);
        action->setToolTip(
            above
                ? tr("Height-gate modifier (Dam mode only): apply the brush "
                     "effect only to the voxels whose dam height is ABOVE the "
                     "gate height set next to it. Combines with every brush "
                     "type; each voxel is checked individually, so one stamp "
                     "can edit some cells and skip their neighbours. Enabling "
                     "it switches to the Dam layer.")
                : tr("Height-gate modifier (Dam mode only): apply the brush "
                     "effect only to the voxels whose dam height is BELOW the "
                     "gate height set next to it. Combines with every brush "
                     "type; each voxel is checked individually, so one stamp "
                     "can edit some cells and skip their neighbours. Enabling "
                     "it switches to the Dam layer."));
        action->setCheckable(true);
        connect(action, &QAction::triggered, this,
                [this, above](bool checked) {
                    MapEditor::BrushParams params = brushTool_->params();
                    if (checked) {
                        params.heightGate =
                            above ? MapEditor::BrushHeightGate::Above
                                  : MapEditor::BrushHeightGate::Below;
                        // The gate reads the dam height — it only makes
                        // sense on the Dam layer.
                        params.mode = MapEditor::BrushMode::Dam;
                    } else {
                        params.heightGate = MapEditor::BrushHeightGate::None;
                    }
                    brushTool_->setParams(params);
                    // Mutual exclusivity: the two conditions are alternatives
                    // (a voxel exactly at the gate height fails both, so both
                    // checked would not mean "apply everywhere").
                    QAction *other = above ? actGateBelow_ : actGateAbove_;
                    if (other && other->isChecked()) {
                        other->setChecked(false);
                    }
                    syncControls();
                });
        return action;
    };
    actGateAbove_ = addGate(true);
    actGateBelow_ = addGate(false);
}

void BrushUiController::createStampShapeActions()
{
    // T3.5 — load brush image from file. The loaded image becomes the stamp
    // mask of the CURRENT brush type (raise/lower/level/... — the image
    // pixel value is the per-cell influence) until a stamp shape is picked
    // explicitly.
    actLoadBrushImage_ = new QAction(makeLoadImageIcon(), tr("&Load Image..."),
                                      this);
    actLoadBrushImage_->setToolTip(
        tr("Load a TGA/PNG/BMP image as the brush stamp mask: the pixel "
           "brightness (0..255) controls the per-cell influence of the "
           "current brush type. Picking a stamp shape switches back to it."));
    connect(actLoadBrushImage_, &QAction::triggered,
            this, &BrushUiController::onLoadBrushImage);

    // --- Form: the stamp silhouette (T3.1 BrushShape) -----------------
    // T3.5 — picking a shape switches the stamp geometry back from a loaded
    // image mask to the generated shape mask (useImageMask = false).
    QActionGroup *shapeGroup = new QActionGroup(this);
    const auto addShape = [this, shapeGroup](MapEditor::BrushShape shape,
                                             const QString &text,
                                             const QString &toolTip) {
        QAction *action = new QAction(makeShapeIcon(shape), text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);
        action->setChecked(brushTool_->params().shape == shape);
        connect(action, &QAction::triggered, this, [this, shape] {
            MapEditor::BrushParams params = brushTool_->params();
            params.shape = shape;
            params.useImageMask = false;
            // A 2D stamp shape replaces the 3D solid and the model stamp.
            params.shape3d = MapEditor::Shape3DKind::None;
            params.modelBrush = false;
            brushTool_->setParams(params);
            syncControls();
        });
        shapeGroup->addAction(action);
        return action;
    };

    actShapeCircle_ = addShape(
        MapEditor::BrushShape::FilledCircle, tr("Circle"),
        tr("Filled circle stamp — the shape the game's terraformers influence. "
           "Radius is the disc half-size in map cells."));
    actShapeSquare_ = addShape(
        MapEditor::BrushShape::FilledSquare, tr("Square"),
        tr("Filled square stamp covering |dx| <= radius and |dy| <= radius."));
    actShapeCircleOutline_ = addShape(
        MapEditor::BrushShape::CircleOutline, tr("Circle Outline"),
        tr("One-cell ring exactly at the radius — draws circular borders."));
    actShapeSquareOutline_ = addShape(
        MapEditor::BrushShape::SquareOutline, tr("Square Outline"),
        tr("One-cell square border at the radius — draws rectangular borders."));
    actShapePolygon_ = addShape(
        MapEditor::BrushShape::FilledPolygon, tr("Polygon"),
        tr("Filled regular-polygon stamp inscribed into the radius circle. "
           "The vertex count and the first-vertex angle are set with the "
           "spinboxes next to it."));
    actShapePolygonOutline_ = addShape(
        MapEditor::BrushShape::PolygonOutline, tr("Polygon Outline"),
        tr("Regular-polygon border at the radius — the band grows inward "
           "with the stroke width, like the circle/square outlines."));
    actShapeLine_ = addShape(
        MapEditor::BrushShape::Line, tr("Line"),
        tr("Two-point stroke: press marks the start, release draws a "
           "Bresenham line stamped with the filled circle of the current "
           "radius — one undo entry per line. Works in both view modes."));
}

QAction* BrushUiController::addBrushTypeAction(QActionGroup* group, const QIcon& icon,
                                        const QString& text,
                                        const QString& toolTip,
                                        MapEditor::BrushEffect effect,
                                        int direction)
{
    QAction *action = new QAction(icon, text, this);
    action->setToolTip(toolTip);
    action->setCheckable(true);

    const MapEditor::BrushParams &current = brushTool_->params();
    action->setChecked(current.effect == effect &&
                       (effect != MapEditor::BrushEffect::ChangeHeight ||
                        current.direction == direction));

    connect(action, &QAction::triggered, this, [this, effect, direction] {
        MapEditor::BrushParams params = brushTool_->params();
        params.effect = effect;
        params.direction = direction;
        // A common brush type paints a flat mask — the 3D shape solid
        // and the model-heightmap stamp belong to their own buttons
        // (same rule as the image mask). Exception: Paint Color combines
        // with both (applyShape3DPaint / applyModelBrushPaint paint the
        // volume without touching heights), so they stay selected.
        if (effect != MapEditor::BrushEffect::SetColor) {
            params.shape3d = MapEditor::Shape3DKind::None;
            params.modelBrush = false;
        }
        brushTool_->setParams(params);
        // T4.2 — keep the interaction mode in step with the brush type:
        // the picking type (eyedropper) drives the Eyedropper mode, every
        // painting type drives the Brush mode. The View mode stays as the
        // user left it (the type buttons themselves are Edit actions).
        if (viewport_) {
            viewport_->setEditorMode(
                effect == MapEditor::BrushEffect::PickColor
                    ? MapEditor::ViewportWidget::EditorMode::Eyedropper
                    : MapEditor::ViewportWidget::EditorMode::Brush);
        }
        syncControls();
    });
    group->addAction(action);
    return action;
}

void BrushUiController::buildBrushToolbar()
{
    QToolBar *bar = window_->addToolBar(tr("Brush"));
    bar->setObjectName(QStringLiteral("BrushToolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(22, 22));
    bar->setStyleSheet(
        QStringLiteral("QToolButton:enabled:hover, QToolButton:enabled:checked "
                       "{ background: rgba(0, 96, 170, 35); border-radius: 4px; }"));

    // T4.2 — the editor interaction mode switch (View / Brush / Eyedropper);
    // picking the Eyedropper mode selects the PickColor brush type, picking
    // the Brush mode swaps it back to the painting color brush.
    bar->addAction(modeActionView_);
    bar->addAction(modeActionBrush_);
    bar->addAction(modeActionEyedropper_);
    bar->addAction(modeActionPlaceSpawn_);
    bar->addSeparator();

    // T3.1 form buttons: the stamp silhouette + the stamp half-size. The
    // silhouette is what the cursor preview shows and what a stamp paints.
    bar->addAction(actShapeCircle_);
    bar->addAction(actShapeSquare_);
    bar->addAction(actShapeCircleOutline_);
    bar->addAction(actShapeSquareOutline_);
    bar->addAction(actShapePolygon_);
    bar->addAction(actShapePolygonOutline_);
    bar->addAction(actShapeLine_);

    // Stamp half-size in map cells (BrushParams::radius, clamped by the tool
    // to BrushTool::maxRadius() = 512).
    brushRadiusSpin_ = new QSpinBox(bar);
    brushRadiusSpin_->setRange(0, MapEditor::BrushTool::maxRadius());
    brushRadiusSpin_->setSuffix(tr(" c"));
    brushRadiusSpin_->setToolTip(
        tr("Brush radius in map cells (the stamp half-size)."));
    connect(brushRadiusSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.radius = value;
        brushTool_->setParams(params);
        syncControls();
    });
    bar->addWidget(new QLabel(tr("Radius:"), bar));
    bar->addWidget(brushRadiusSpin_);

    // Stroke width in cells for the unfilled shapes (CircleOutline,
    // SquareOutline, Line). The band is inset from the outer edge, so it never
    // exceeds the stamp footprint. 0 = full stamp (the pre-thickness behaviour);
    // UI shows 1..maxThickness where 1 = original one-cell ring/border.
    brushThicknessSpin_ = new QSpinBox(bar);
    brushThicknessSpin_->setSuffix(tr(" c"));
    brushThicknessSpin_->setToolTip(
        tr("Stroke width in cells of the unfilled shapes (CircleOutline, "
           "SquareOutline, Line). The band grows inward from the outer edge, "
           "so a thicker stroke never edits outside the brush footprint. "
           "Ignored by the filled shapes. 0 = full stamp."));
    connect(brushThicknessSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.thickness = value;
        brushTool_->setParams(params);
        syncControls();
    });
    bar->addWidget(new QLabel(tr("Width:"), bar));
    bar->addWidget(brushThicknessSpin_);

    // Regular-polygon stamp params (FilledPolygon / PolygonOutline): the
    // vertex count and the first-vertex angle. Enabled only with a polygon
    // shape, like the stroke width is only enabled for the unfilled shapes.
    polygonSidesSpin_ = new QSpinBox(bar);
    polygonSidesSpin_->setObjectName(QStringLiteral("polygonSidesSpin"));
    polygonSidesSpin_->setRange(MapEditor::kMinPolygonSides,
                                MapEditor::kMaxPolygonSides);
    polygonSidesSpin_->setValue(6);
    polygonSidesSpin_->setToolTip(
        tr("Vertex count of the regular-polygon stamp (3..20). The polygon "
           "is inscribed into the radius circle, so the brush radius is the "
           "circumradius, exactly like the circle brush."));
    connect(polygonSidesSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        params.polygonSides = value;
        brushTool_->setParams(params);
        syncControls();
    });
    bar->addWidget(new QLabel(tr("Sides:"), bar));
    bar->addWidget(polygonSidesSpin_);

    polygonAngleSpin_ = new QDoubleSpinBox(bar);
    polygonAngleSpin_->setObjectName(QStringLiteral("polygonAngleSpin"));
    polygonAngleSpin_->setRange(0.0, 360.0);
    polygonAngleSpin_->setDecimals(1);
    polygonAngleSpin_->setSingleStep(15.0);
    polygonAngleSpin_->setWrapping(true);
    polygonAngleSpin_->setSuffix(tr("\u00B0"));
    polygonAngleSpin_->setToolTip(
        tr("Angle offset of the first polygon vertex in degrees (0..360, "
           "wraps 360\u00B0 \u2192 0\u00B0). 0\u00B0 points the vertex at map "
           "east (+x); the angle grows clockwise on screen."));
    connect(polygonAngleSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.polygonAngleDeg = value;
                brushTool_->setParams(params);
                syncControls();
            });
    bar->addWidget(new QLabel(tr("Angle:"), bar));
    bar->addWidget(polygonAngleSpin_);
    bar->addSeparator();

    // T3.2 type buttons: raise / lower / the two levellers.
    bar->addAction(actBrushRaise_);
    bar->addAction(actBrushLower_);
    bar->addAction(actBrushLevelZP_);
    bar->addAction(actBrushLevelHeight_);

    // The height value of the height tools — whole voxels (0..MAX_VX_HEIGHT_WHOLE). One
    // control, meaning per type: the raise/lower step (per stamp at full
    // influence) for Raise/Lower Terrain, the target height for "Level to
    // Height" and the value getCurrentHeight() reports for the scripted
    // brush (the domain BrushTool::clampHeightLevel enforces too).
    brushLevelSpin_ = new QSpinBox(bar);
    brushLevelSpin_->setRange(0, MAX_VX_HEIGHT_WHOLE);
    brushLevelSpin_->setSuffix(tr(" vox"));
    connect(brushLevelSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        MapEditor::BrushParams params = brushTool_->params();
        if (params.effect == MapEditor::BrushEffect::AlignVariableH ||
            params.effect == MapEditor::BrushEffect::Script ||
            (params.effect == MapEditor::BrushEffect::SetColor &&
             (params.shape3d != MapEditor::Shape3DKind::None ||
              params.modelBrush))) {
            // Set Color over a 3D shape / the model stamp: the level is
            // the volume base for the paint (applyShape3DPaint /
            // applyModelBrushPaint).
            params.heightLevel = value;
        } else {
            params.strength = MapEditor::BrushTool::clampStrength(value);
        }
        brushTool_->setParams(params);
    });
    bar->addWidget(new QLabel(tr("Level:"), bar));
    bar->addWidget(brushLevelSpin_);

    bar->addSeparator();

    // T3.2 mode buttons: Geo / Dam.
    bar->addAction(actBrushGeo_);
    bar->addAction(actBrushDam_);

    bar->addSeparator();

    // The color button: shows the current paint RGB as a swatch; clicking
    // opens the color picker dialog. Painting is true-color — the exact
    // picked RGB is written to the map (the nearest palette entry is kept
    // as the surface type).
    brushColorButton_ = new QToolButton(bar);
    brushColorButton_->setFixedSize(28, 24);
    brushColorButton_->setToolTip(
        tr("Paint color for the \"Paint Color\" brush — the exact 24-bit RGB "
           "is painted (click to change). The eyedropper picks into this."));
    connect(brushColorButton_, &QToolButton::clicked, this, [this]() {
        const MapEditor::BrushParams params = brushTool_->params();
        const QColor initial(
            static_cast<int>((params.colorRgb >> 16) & 0xFF),
            static_cast<int>((params.colorRgb >> 8) & 0xFF),
            static_cast<int>(params.colorRgb & 0xFF));
        const QColor chosen = QColorDialog::getColor(initial, window_,
                                                     tr("Paint Color"));
        if (!chosen.isValid()) {
            return;
        }
        MapEditor::BrushParams next = brushTool_->params();
        next.colorRgb = (static_cast<unsigned int>(chosen.red()) << 16) |
                        (static_cast<unsigned int>(chosen.green()) << 8) |
                        static_cast<unsigned int>(chosen.blue());
        brushTool_->setParams(next);
        syncControls();
    });
    bar->addWidget(new QLabel(tr("Color:"), bar));
    bar->addWidget(brushColorButton_);

    // Layer painting for the color brush: the height at which the surface
    // is painted (-1 = the column top = the default paint) and the paint
    // depth (1 = a single voxel layer). Below the surface the brush paints
    // internal voxel layers [height-depth+1 .. height]; at/above the
    // surface it is the default top paint.
    brushPaintHeightSpin_ = new QSpinBox(bar);
    brushPaintHeightSpin_->setRange(-1, MAX_VX_HEIGHT_WHOLE);
    brushPaintHeightSpin_->setValue(-1);
    brushPaintHeightSpin_->setSpecialValueText(tr("top"));
    brushPaintHeightSpin_->setSuffix(tr(" vox"));
    brushPaintHeightSpin_->setToolTip(
        tr("Paint height (whole voxels): -1 = the column top (default "
           "paint). A height below the surface paints internal voxel "
           "layers; at/above the surface it clamps to the top."));
    connect(brushPaintHeightSpin_, &QSpinBox::valueChanged, this,
            [this](int value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.paintHeight = value;
                brushTool_->setParams(params);
            });
    bar->addWidget(new QLabel(tr("H:"), bar));
    bar->addWidget(brushPaintHeightSpin_);

    brushPaintDepthSpin_ = new QSpinBox(bar);
    brushPaintDepthSpin_->setRange(1, MAX_VX_HEIGHT_WHOLE);
    brushPaintDepthSpin_->setValue(1);
    brushPaintDepthSpin_->setToolTip(
        tr("Paint depth: how many voxel layers downward a below-surface "
           "paint covers (1 = a single layer)."));
    connect(brushPaintDepthSpin_, &QSpinBox::valueChanged, this,
            [this](int value) {
                MapEditor::BrushParams params = brushTool_->params();
                params.paintDepth = value;
                brushTool_->setParams(params);
            });
    bar->addWidget(new QLabel(tr("Depth:"), bar));
    bar->addWidget(brushPaintDepthSpin_);

    // "Paint transparent": the layer range (2D brushes, the Image Decal, the
    // 3D shape / model volume paints) becomes EMPTY/TRANSPARENT voxels —
    // they carry no color, do not participate in terraforming and are never
    // the topmost voxel: stripped on top with an immediate height recalc
    // (vrtMap::normalizeTransparentColumn, game and editor).
    brushPaintTransparentCheck_ = new QCheckBox(tr("Transparent"), bar);
    brushPaintTransparentCheck_->setToolTip(
        tr("Paint the layer range as transparent (empty) voxels instead of "
           "a color: no color, no terraforming participation, units and "
           "line of sight pass through. A transparent voxel can never be "
           "the topmost one — painted onto the top it is destroyed and the "
           "terrain height recalculates to the nearest solid voxel; raising "
           "terrain through a transparent voxel makes it solid."));
    connect(brushPaintTransparentCheck_, &QCheckBox::toggled, this,
            [this](bool on) {
                MapEditor::BrushParams params = brushTool_->params();
                params.paintTransparent = on;
                brushTool_->setParams(params);
            });
    bar->addWidget(brushPaintTransparentCheck_);

    // T3.3/T4.2 — the paint/pick color types next to their color controls
    // (the same actions the Edit menu carries).
    bar->addAction(actBrushColor_);
    bar->addAction(actBrushPickColor_);

    // ImageDecal — the palette-limited image stamp (Dam layer only).
    bar->addAction(actBrushImageDecal_);

    bar->addSeparator();

    // T3.4 — soot/very-light-dam buttons.
    bar->addAction(actBrushSoot_);
    bar->addAction(actBrushVeryLightDam_);

    // Scripted brush (type + script editor window).
    bar->addAction(actBrushScript_);
    bar->addAction(actBrushScriptEdit_);

    bar->addSeparator();

    // T3.5 — load-image button: the image becomes the stamp mask of the
    // current brush type.
    bar->addAction(actLoadBrushImage_);

    // T3.5 — the image mask scale (percent of the loaded image size). The
    // scaled mask is recomputed from the stored original on every change.
    brushMaskScaleSpin_ = new QSpinBox(bar);
    brushMaskScaleSpin_->setRange(10, 800);
    brushMaskScaleSpin_->setValue(100);
    brushMaskScaleSpin_->setSuffix(tr(" %"));
    brushMaskScaleSpin_->setToolTip(
        tr("Scale of the loaded brush image relative to its original pixel "
           "size (the mask stays centred under the cursor)."));
    connect(brushMaskScaleSpin_, &QSpinBox::valueChanged, this, [this](int value) {
        maskScalePercent_ = value;
        applyMaskScale();
    });
    bar->addWidget(new QLabel(tr("Mask:"), bar));
    bar->addWidget(brushMaskScaleSpin_);

    syncControls();
    syncColorControls();
}

void BrushUiController::syncControls()
{
    const MapEditor::BrushParams params = brushTool_->params();

    // T4.2 — brush type buttons: the exclusive action group only auto-checks
    // user-triggered actions, so every programmatic effect change (eyedropper
    // pick, Eyedropper/Brush mode switch, ...) mirrors into the buttons here.
    const auto syncType = [params](QAction *action, bool active) {
        if (action) {
            action->setChecked(active);
        }
    };
    syncType(actBrushRaise_, params.effect == MapEditor::BrushEffect::ChangeHeight
                                 && params.direction > 0);
    syncType(actBrushLower_, params.effect == MapEditor::BrushEffect::ChangeHeight
                                 && params.direction < 0);
    syncType(actBrushLevelZP_, params.effect == MapEditor::BrushEffect::Align4ZP);
    syncType(actBrushLevelHeight_,
             params.effect == MapEditor::BrushEffect::AlignVariableH);
    syncType(actBrushColor_, params.effect == MapEditor::BrushEffect::SetColor);
    syncType(actBrushImageDecal_,
             params.effect == MapEditor::BrushEffect::ImageDecal);
    syncType(actBrushPickColor_,
             params.effect == MapEditor::BrushEffect::PickColor);
    syncType(actBrushSoot_, params.effect == MapEditor::BrushEffect::PlotSoot);
    syncType(actBrushVeryLightDam_,
             params.effect == MapEditor::BrushEffect::VeryLightDam);
    syncType(actBrushScript_, params.effect == MapEditor::BrushEffect::Script);
    syncType(actBrushSmooth_, params.effect == MapEditor::BrushEffect::Smooth);
    syncType(actBrushFlatten_, params.effect == MapEditor::BrushEffect::Flatten);
    syncType(actBrushTricubic_,
             params.effect == MapEditor::BrushEffect::Tricubic);
    syncType(actBrushInflate_,
             params.effect == MapEditor::BrushEffect::Inflate);
    syncType(actBrushDeflate_,
             params.effect == MapEditor::BrushEffect::Deflate);
    syncType(actBrushGaussianBlur_,
             params.effect == MapEditor::BrushEffect::GaussianBlur);
    syncType(actBrush3DCone_,
             params.shape3d == MapEditor::Shape3DKind::Cone);
    syncType(actBrush3DPyramid_,
             params.shape3d == MapEditor::Shape3DKind::Pyramid);
    syncType(actBrush3DTruncCone_,
             params.shape3d == MapEditor::Shape3DKind::TruncatedCone);
    syncType(actBrush3DTruncPyramid_,
             params.shape3d == MapEditor::Shape3DKind::TruncatedPyramid);
    syncType(actBrush3DTorus_, params.shape3d == MapEditor::Shape3DKind::Torus);
    syncType(actBrush3DEllipsoid_,
             params.shape3d == MapEditor::Shape3DKind::HalfEllipsoid);
    if (actModelBrush_) {
        actModelBrush_->setChecked(params.modelBrush);
    }
    if (actModelPlace_) {
        actModelPlace_->setChecked(params.modelMode == 0);
    }
    if (actModelAdd_) {
        actModelAdd_->setChecked(params.modelMode == 1);
    }
    if (actModelSubtract_) {
        actModelSubtract_->setChecked(params.modelMode == 2);
    }

    // 3D shape settings mirror the tool.
    const auto syncSpin = [](QSpinBox *spin, int shown) {
        if (spin && spin->value() != shown) {
            spin->blockSignals(true);
            spin->setValue(shown);
            spin->blockSignals(false);
        }
    };
    syncSpin(shapeHeightSpin_, params.shapeHeight);
    syncSpin(shapeTopOffsetXSpin_, qRound(params.shapeTopOffsetX));
    syncSpin(shapeTopOffsetYSpin_, qRound(params.shapeTopOffsetY));
    syncSpin(shapeTopRadiusSpin_, qRound(params.shapeTopRadius));
    syncSpin(shapeTopSideSpin_, qRound(params.shapeTopSide));
    // Model brush settings mirror the tool.
    syncSpin(modelHeightSpin_, params.modelHeight);
    const auto syncDoubleSpin = [](QDoubleSpinBox *spin, double shown) {
        if (spin && std::abs(spin->value() - shown) > 1e-9) {
            spin->blockSignals(true);
            spin->setValue(shown);
            spin->blockSignals(false);
        }
    };
    syncDoubleSpin(modelScaleXSpin_, params.modelScaleX);
    syncDoubleSpin(modelScaleYSpin_, params.modelScaleY);

    // Blur radius mirrors the tool; its maximum follows the stamp radius
    // (it must stay strictly smaller).
    if (blurRadiusSpin_) {
        const int maxBlur =
            MapEditor::BrushTool::clampBlurRadius(INT_MAX, params.radius);
        blurRadiusSpin_->setMaximum(maxBlur);
        if (blurRadiusSpin_->value() != params.blurRadius) {
            blurRadiusSpin_->blockSignals(true);
            blurRadiusSpin_->setValue(params.blurRadius);
            blurRadiusSpin_->blockSignals(false);
        }
    }

    // Sculpting fraction spinbox mirrors the tool (shared by both types).
    if (sculptEffectSpin_) {
        const double shown = MapEditor::BrushTool::clampSculptEffect(
            params.sculptEffect);
        if (std::abs(sculptEffectSpin_->value() - shown) > 1e-9) {
            sculptEffectSpin_->blockSignals(true);
            sculptEffectSpin_->setValue(shown);
            sculptEffectSpin_->blockSignals(false);
        }
    }

    // Height-gate modifiers: mirror the condition into the checkable actions
    // (the exclusive group only auto-checks user-triggered actions) and the
    // comparison height into the toolbar + menu spinboxes.
    if (actGateAbove_) {
        actGateAbove_->setChecked(params.heightGate ==
                                  MapEditor::BrushHeightGate::Above);
    }
    if (actGateBelow_) {
        actGateBelow_->setChecked(params.heightGate ==
                                  MapEditor::BrushHeightGate::Below);
    }
    for (QSpinBox *spin : {gateHeightSpin_, gateHeightMenuSpin_}) {
        if (spin && spin->value() != params.gateHeight) {
            spin->blockSignals(true);
            spin->setValue(params.gateHeight);
            spin->blockSignals(false);
        }
    }

    if (brushLevelSpin_) {
        // One height control for the height tools: the raise/lower step for
        // ChangeHeight, the target height for AlignVariableH (and the value
        // getCurrentHeight() reports for the scripted brush; every other
        // type does not consume it — the value display keeps the last
        // choice of the height tools). With a 3D shape or the model brush +
        // Paint Color the spin edits the volume base height (the paint
        // starts there) — the same heightLevel the shape's height tools use.
        const bool shapePaintBase =
            params.effect == MapEditor::BrushEffect::SetColor &&
            (params.shape3d != MapEditor::Shape3DKind::None ||
             params.modelBrush);
        const bool showsTarget =
            params.effect == MapEditor::BrushEffect::AlignVariableH ||
            params.effect == MapEditor::BrushEffect::Script || shapePaintBase;
        const int shown = static_cast<int>(std::lround(
            showsTarget ? params.heightLevel : params.strength));
        brushLevelSpin_->setToolTip(
            params.effect == MapEditor::BrushEffect::Script
                ? tr("Height value the scripted brush script reads through "
                     "getCurrentHeight() (whole voxels; 1 voxel = 1/32 m).")
                : shapePaintBase
                    ? tr("Base height of the 3D shape / the model stamp "
                         "(whole voxels; 1 voxel = 1/32 m): Paint Color "
                         "paints the volume upward from this level without "
                         "changing the terrain.")
                    : showsTarget
                        ? tr("Target height of the \"Level to Height\" brush "
                             "(whole voxels; 1 voxel = 1/32 m).")
                        : tr("Height change per stamp of the Raise/Lower Terrain "
                             "brush, at full mask influence (whole voxels; "
                             "1 voxel = 1/32 m)."));
        if (brushLevelSpin_->value() != shown) {
            brushLevelSpin_->blockSignals(true);
            brushLevelSpin_->setValue(shown);
            brushLevelSpin_->blockSignals(false);
        }
    }

    // The brush mode buttons are mirrored too: some brush types force a layer
    // (the zeroplast brushes and the ImageDecal decal are Dam-only), so a
    // programmatic mode change has to reach the exclusive group.
    if (actBrushGeo_) {
        actBrushGeo_->setChecked(params.mode == MapEditor::BrushMode::Geo);
    }
    if (actBrushDam_) {
        actBrushDam_->setChecked(params.mode == MapEditor::BrushMode::Dam);
    }

    // T3.1 — the stamp form drives the cursor preview silhouette; T3.5 — a
    // loaded image mask replaces the shapes (no shape button is checked
    // then); ImageDecal has its own rectangular geometry; T4.2 — the
    // eyedropper is a point operation, the radius then does not matter.
    if (brushRadiusSpin_) {
        if (brushRadiusSpin_->value() != params.radius) {
            brushRadiusSpin_->blockSignals(true);
            brushRadiusSpin_->setValue(params.radius);
            brushRadiusSpin_->blockSignals(false);
        }
    }

    // Thickness: only enabled for the unfilled shapes; the range is the stamp's
    // own extent (2*radius+1 cells) so a thicker band never leaves the stamp.
    // Internal 0 = "auto" (full stamp); UI maps 0 to 1 for outlines and to
    // maxThickness for the Line shape.
    const bool thicknessEnabled =
        MapEditor::BrushMasks::isUnfilledShape(params.shape) &&
        !params.useImageMask &&
        params.effect != MapEditor::BrushEffect::ImageDecal &&
        params.effect != MapEditor::BrushEffect::PickColor;
    if (brushThicknessSpin_) {
        const int maxThick = MapEditor::BrushTool::maxThickness(params.radius);
        brushThicknessSpin_->setRange(1, maxThick);
        brushThicknessSpin_->setEnabled(thicknessEnabled);
        // Map the internal 0 ("auto") to the shape's natural default.
        int shownThick = params.thickness;
        if (shownThick <= 0) {
            shownThick = params.shape == MapEditor::BrushShape::Line ? maxThick : 1;
        }
        shownThick = std::clamp(shownThick, 1, maxThick);
        if (brushThicknessSpin_->value() != shownThick) {
            brushThicknessSpin_->blockSignals(true);
            brushThicknessSpin_->setValue(shownThick);
            brushThicknessSpin_->blockSignals(false);
        }
    }
    const bool imageMaskActive = params.useImageMask && brushTool_->hasImageMask();
    const bool decalActive = params.effect == MapEditor::BrushEffect::ImageDecal &&
                             brushTool_->hasDecalImage();
    const bool shape3dActive = params.shape3d != MapEditor::Shape3DKind::None;
    const bool modelStampActive = params.modelBrush;
    if (actShapeCircle_) actShapeCircle_->setChecked(!imageMaskActive && !decalActive &&
        !shape3dActive && !modelStampActive && params.shape == MapEditor::BrushShape::FilledCircle);
    if (actShapeSquare_) actShapeSquare_->setChecked(!imageMaskActive && !decalActive &&
        !shape3dActive && !modelStampActive && params.shape == MapEditor::BrushShape::FilledSquare);
    if (actShapeCircleOutline_) actShapeCircleOutline_->setChecked(!imageMaskActive && !decalActive &&
        !shape3dActive && !modelStampActive && params.shape == MapEditor::BrushShape::CircleOutline);
    if (actShapeSquareOutline_) actShapeSquareOutline_->setChecked(!imageMaskActive && !decalActive &&
        !shape3dActive && !modelStampActive && params.shape == MapEditor::BrushShape::SquareOutline);
    if (actShapePolygon_) actShapePolygon_->setChecked(!imageMaskActive && !decalActive &&
        !shape3dActive && !modelStampActive && params.shape == MapEditor::BrushShape::FilledPolygon);
    if (actShapePolygonOutline_) actShapePolygonOutline_->setChecked(!imageMaskActive && !decalActive &&
        !shape3dActive && !modelStampActive && params.shape == MapEditor::BrushShape::PolygonOutline);
    if (actShapeLine_) actShapeLine_->setChecked(!imageMaskActive && !decalActive &&
        !shape3dActive && !modelStampActive && params.shape == MapEditor::BrushShape::Line);

    // Regular-polygon params mirror the tool (the shape buttons' exclusive
    // group only carries the shape itself).
    if (polygonSidesSpin_ && polygonSidesSpin_->value() != params.polygonSides) {
        polygonSidesSpin_->blockSignals(true);
        polygonSidesSpin_->setValue(params.polygonSides);
        polygonSidesSpin_->blockSignals(false);
    }
    if (polygonAngleSpin_) {
        const double shownAngle =
            std::round(MapEditor::BrushTool::clampPolygonAngleDeg(
                           params.polygonAngleDeg) *
                       10.0) /
            10.0;
        if (std::abs(polygonAngleSpin_->value() - shownAngle) > 1e-9) {
            polygonAngleSpin_->blockSignals(true);
            polygonAngleSpin_->setValue(shownAngle);
            polygonAngleSpin_->blockSignals(false);
        }
    }

    // Layer-paint controls follow params.
    if (brushPaintHeightSpin_ &&
        brushPaintHeightSpin_->value() != params.paintHeight) {
        brushPaintHeightSpin_->blockSignals(true);
        brushPaintHeightSpin_->setValue(params.paintHeight);
        brushPaintHeightSpin_->blockSignals(false);
    }
    if (brushPaintDepthSpin_ &&
        brushPaintDepthSpin_->value() != params.paintDepth) {
        brushPaintDepthSpin_->blockSignals(true);
        brushPaintDepthSpin_->setValue(params.paintDepth);
        brushPaintDepthSpin_->blockSignals(false);
    }
    if (brushPaintInteriorCheck_ &&
        brushPaintInteriorCheck_->isChecked() != params.paintInterior3d) {
        brushPaintInteriorCheck_->blockSignals(true);
        brushPaintInteriorCheck_->setChecked(params.paintInterior3d);
        brushPaintInteriorCheck_->blockSignals(false);
    }
    if (brushPaintTransparentCheck_ &&
        brushPaintTransparentCheck_->isChecked() != params.paintTransparent) {
        brushPaintTransparentCheck_->blockSignals(true);
        brushPaintTransparentCheck_->setChecked(params.paintTransparent);
        brushPaintTransparentCheck_->blockSignals(false);
    }
    // The color button is restyled from params by syncColorControls() below.

    // Mirror the tool into the viewport cursor preview: the square silhouette
    // for the square forms and for the image mask (a rectangle), the circle
    // otherwise; the radius is the mask half-size for the image mask. The
    // preview only follows the brush — the View/Eyedropper modes hide it in
    // the viewport (drawBrushPreview) anyway.
    if (viewport_) {
        const bool scriptPreview = !imageMaskActive && !decalActive &&
            params.effect == MapEditor::BrushEffect::Script;
        // 3D shapes preview as their BASEMENT (both view modes): the circle
        // or polygon footprint, the half torus with its inner ring.
        const bool shape3dPolygon = shape3dActive &&
            MapEditor::shape3DIsPolygonal(params.shape3d);
        const bool polygonPreview = !imageMaskActive && !decalActive &&
            (shape3dPolygon ||
             params.shape == MapEditor::BrushShape::FilledPolygon ||
             params.shape == MapEditor::BrushShape::PolygonOutline);
        const bool squarePreview = !shape3dActive &&
            (imageMaskActive || decalActive ||
             params.shape == MapEditor::BrushShape::FilledSquare ||
             params.shape == MapEditor::BrushShape::SquareOutline);
        int previewRadius = params.radius;
        if (imageMaskActive) {
            previewRadius = std::max(brushTool_->imageMask().sx,
                                     brushTool_->imageMask().sy) / 2;
        } else if (decalActive) {
            previewRadius = std::max(brushTool_->decalImage().width,
                                     brushTool_->decalImage().height) / 2;
        }
        // Inner ring of the band for the outline shapes; 0 = no inner ring.
        // thickness 0 ("auto") → the original one-cell ring, so no inner ring;
        // the Line shape's stroke is centred on the path, not a concentric
        // band, so its preview keeps the outer footprint only. The polygon
        // inner contour is the same polygon at the reduced radius — the
        // polygon band is distance-based, so that is the closest simple
        // inset the preview can draw. The half torus draws the ring's inner
        // circle as its basement hole.
        int previewInner = 0;
        if (!imageMaskActive && !decalActive && params.thickness > 0 &&
            (params.shape == MapEditor::BrushShape::CircleOutline ||
             params.shape == MapEditor::BrushShape::SquareOutline ||
             params.shape == MapEditor::BrushShape::PolygonOutline)) {
            previewInner = std::max(0, params.radius - params.thickness + 1);
        }
        if (shape3dActive && params.shape3d == MapEditor::Shape3DKind::Torus &&
            params.shapeTopRadius > 0) {
            previewInner = qRound(params.shapeTopRadius);
        }
        // The model brush previews its placement RECTANGLE (asymmetric half
        // extents in cells); 0/0 hands the preview back to the shapes.
        int previewRectX = 0;
        int previewRectY = 0;
        if (params.modelBrush && brushTool_->hasModelBrushData()) {
            const MapEditor::ModelBrush::ModelRaster& raster =
                brushTool_->modelRaster();
            previewRectX = std::max(
                1, static_cast<int>(
                       std::lround(raster.width * params.modelScaleX))) / 2;
            previewRectY = std::max(
                1, static_cast<int>(
                       std::lround(raster.height * params.modelScaleY))) / 2;
        }
        viewport_->setBrushPreview(
            scriptPreview
                ? (scriptStampShape_ == MapEditor::ScriptStampShape::Square
                       ? MapEditor::BrushPreviewShape::Square
                       : MapEditor::BrushPreviewShape::Circle)
            : (params.modelBrush && brushTool_->hasModelBrushData())
                    ? MapEditor::BrushPreviewShape::Square
            : polygonPreview ? MapEditor::BrushPreviewShape::Polygon
            : squarePreview ? MapEditor::BrushPreviewShape::Square
                            : MapEditor::BrushPreviewShape::Circle,
            previewRadius, previewInner);
        viewport_->setBrushPreviewPolygon(params.polygonSides,
                                          params.polygonAngleDeg);
        viewport_->setBrushPreviewRect(previewRectX, previewRectY);
    }

    // Enabled/disabled states depend on the effect AND on the session state
    // (a world must be loaded) — updateActionStates() is the single source
    // of truth for both.
    emit actionStatesNeedUpdate();

    // The color picker panel follows the brush: it always shows the color
    // the brush will paint (setColor only restyles the panel — no feedback
    // loop into the brush).
    emit syncBrushColor(params.colorRgb & 0x00FFFFFFu);

    syncColorControls();
}

void BrushUiController::syncColorControls()
{
    if (!brushTool_ || !brushColorButton_) {
        return;
    }

    const unsigned int rgb = brushTool_->params().colorRgb & 0x00FFFFFFu;
    const QColor color(static_cast<int>((rgb >> 16) & 0xFF),
                       static_cast<int>((rgb >> 8) & 0xFF),
                       static_cast<int>(rgb & 0xFF));
    brushColorButton_->setStyleSheet(
        QStringLiteral("QToolButton { border: 1px solid #808080; "
                       "background-color: %1; }")
            .arg(color.name()));
}

QString BrushUiController::loadBrushImage(const QString &filePath)
{
    if (!brushTool_) {
        return tr("The brush tool is not available.");
    }
    // Load via QImageReader for explicit format control.
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        return tr("Cannot load image '%1': %2")
            .arg(filePath, reader.errorString());
    }

    // Convert to brush mask — the alpha channel becomes the stamp influence.
    const MapEditor::BrushMask mask = imageToBrushMask(img);
    if (mask.empty()) {
        return tr("The loaded image produced an empty brush mask.");
    }

    // Keep the un-scaled original: the scale control recomputes the mask
    // from it, so repeated scale changes never blur the source.
    brushMaskOriginal_ = mask;
    brushTool_->setImageMask(mask);
    maskScalePercent_ = 100;
    if (brushMaskScaleSpin_) {
        brushMaskScaleSpin_->blockSignals(true);
        brushMaskScaleSpin_->setValue(100);
        brushMaskScaleSpin_->blockSignals(false);
    }

    // The image becomes the stamp mask of the current brush type (T3.5
    // rework): raise/lower/level/... keep working, only the stamp geometry
    // switches from the shape to the image. The eyedropper is a point
    // operation — fall back to a raise so the mask has an immediate effect,
    // and make sure the editor mode actually paints (a Load Image from the
    // View mode would otherwise do nothing on click).
    MapEditor::BrushParams params = brushTool_->params();
    params.useImageMask = true;
    params.shape3d = MapEditor::Shape3DKind::None;
    params.modelBrush = false;
    if (params.effect == MapEditor::BrushEffect::PickColor ||
        params.effect == MapEditor::BrushEffect::None) {
        params.effect = MapEditor::BrushEffect::ChangeHeight;
        params.direction = 1;
    }
    brushTool_->setParams(params);
    if (viewport_) {
        viewport_->setEditorMode(MapEditor::ViewportWidget::EditorMode::Brush);
    }
    syncControls();

    showStatus(
        tr("Brush image loaded: %1 (%2x%3)")
            .arg(QFileInfo(filePath).fileName())
            .arg(mask.sx)
            .arg(mask.sy),
        5000);
    return QString();
}

void BrushUiController::setMaskScalePercent(int percent)
{
    const int clamped = qBound(10, percent, 800);
    maskScalePercent_ = clamped;
    if (brushMaskScaleSpin_ && brushMaskScaleSpin_->value() != clamped) {
        brushMaskScaleSpin_->blockSignals(true);
        brushMaskScaleSpin_->setValue(clamped);
        brushMaskScaleSpin_->blockSignals(false);
    }
    applyMaskScale();
}

void BrushUiController::applyMaskScale()
{
    if (!brushTool_) {
        return;
    }
    const double factor = maskScalePercent_ / 100.0;
    // The same control scales whichever image stamp is active: the
    // influence mask of the terrain tools (T3.5) or the palette-index
    // decal image (ImageDecal). Both are recomputed from the un-scaled
    // original, so repeated changes never accumulate blur.
    if (brushTool_->params().effect == MapEditor::BrushEffect::ImageDecal) {
        if (!decalOriginal_.empty()) {
            brushTool_->setDecalImage(
                MapEditor::ImageDecal::scaleNearest(decalOriginal_, factor));
        }
    } else if (!brushMaskOriginal_.empty()) {
        brushTool_->setImageMask(scaleBrushMask(brushMaskOriginal_, factor));
    }
    syncControls();
}

void BrushUiController::onLoadBrushImage()
{
    const QString filter = tr("Images (*.png *.tga *.bmp *.jpg *.jpeg);;All Files (*)");
    const QString filePath = QFileDialog::getOpenFileName(
        window_, tr("Load Brush Image"), QString(), filter);
    if (filePath.isEmpty()) {
        return;
    }
    const QString error = loadBrushImage(filePath);
    if (!error.isEmpty()) {
        QMessageBox::warning(window_, tr("Image Load Failed"), error);
    }
}

void BrushUiController::onLoadDecalImage()
{
    if (!session_ || !session_->isWorldLoaded() || !brushTool_) {
        return;
    }

    const QString filter =
        tr("Images (*.png *.tga *.bmp);;All Files (*)");
    const QString filePath = QFileDialog::getOpenFileName(
        window_, tr("Load Image Decal"), QString(), filter);
    if (filePath.isEmpty()) {
        return;
    }

    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        QMessageBox::warning(window_, tr("Image Load Failed"),
            tr("Cannot load image '%1':\n%2")
                .arg(filePath, reader.errorString()));
        return;
    }

    // The decal is a brush stamp, not a map import: a huge image would make
    // one click repaint a large part of the map, so the size is capped.
    if (img.width() > MapEditor::ImageDecal::kMaxImageSize ||
        img.height() > MapEditor::ImageDecal::kMaxImageSize) {
        QMessageBox::warning(window_, tr("Image Too Large"),
            tr("The image is too large (%1x%2 px).\n"
               "The maximum size of an image is %3 px on each side.")
                .arg(img.width())
                .arg(img.height())
                .arg(MapEditor::ImageDecal::kMaxImageSize));
        return;
    }

    // Exact, un-premultiplied RGBA: the conversion matches the colours
    // against the palette and skips anything that is not fully opaque.
    const QImage rgbaImage = img.convertToFormat(QImage::Format_ARGB32);
    const int width = rgbaImage.width();
    const int height = rgbaImage.height();
    std::vector<unsigned char> rgba(
        static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        const QRgb *line =
            reinterpret_cast<const QRgb *>(rgbaImage.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const std::size_t base =
                (static_cast<std::size_t>(y) * width + x) * 4;
            rgba[base + 0] = static_cast<unsigned char>(qRed(line[x]));
            rgba[base + 1] = static_cast<unsigned char>(qGreen(line[x]));
            rgba[base + 2] = static_cast<unsigned char>(qBlue(line[x]));
            rgba[base + 3] = static_cast<unsigned char>(qAlpha(line[x]));
        }
    }

    const std::vector<unsigned char> palette =
        session_->paletteBytes(MapEditor::EditorSession::PaletteLayer::Dam);
    const MapEditor::ImageDecal::PrepareResult prepared =
        MapEditor::ImageDecal::prepare(rgba.data(), width, height,
                                       palette.data());
    if (!prepared.ok) {
        QMessageBox::warning(window_, tr("Image Decal"),
                             QString::fromStdString(prepared.error));
        return;
    }

    // Step 8 — the decal becomes the stamp of the ImageDecal brush. The
    // colors are exact 24-bit RGB: no palette writes, no quantization.
    decalOriginal_ = prepared.image;
    brushTool_->setDecalImage(prepared.image);
    if (brushMaskScaleSpin_) {
        brushMaskScaleSpin_->blockSignals(true);
        brushMaskScaleSpin_->setValue(100);
        brushMaskScaleSpin_->blockSignals(false);
    }

    MapEditor::BrushParams params = brushTool_->params();
    params.effect = MapEditor::BrushEffect::ImageDecal;
    params.mode = MapEditor::BrushMode::Dam;
    params.useImageMask = false;
    brushTool_->setParams(params);
    if (viewport_) {
        viewport_->setEditorMode(MapEditor::ViewportWidget::EditorMode::Brush);
    }
    syncControls();

    showStatus(tr("Image decal ready: %1x%2 px, true 24-bit colors."), 8000);
}

} // namespace MapEditor

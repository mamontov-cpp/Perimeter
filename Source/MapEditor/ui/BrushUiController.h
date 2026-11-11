#pragma once

#include <QObject>

#include <QString>

class QToolButton;

#include "BrushTool.h"
#include "ImageDecal.h"
#include "ScriptEditorDialog.h" // ScriptStampShape

class QAction;
class QActionGroup;
class QLabel;
class QMenu;
class QSpinBox;
class QDoubleSpinBox;
class QMainWindow;
class QToolBar;

namespace MapEditor {
class EditorSession;
class ViewportWidget;

/// The brush UI domain of the editor: the brush type/mode/stamp actions, the
/// Brush / Sculpting / Modifiers / Model toolbars with all their spinboxes,
/// the Edit-menu "Brush Type / Mode" + "Brush Modifiers" submenus, the
/// brush-control sync (syncBrushControls/syncColorControls) and the image /
/// decal / model import flows. Extracted from MainWindow (god-object
/// decomposition, docs/architecture.md §5.36) — the same pattern as
/// BattleMapController (§5.35).
///
/// Host wiring: the controller borrows the brush tool / session / window
/// (no ownership); the viewport arrives later via setViewport() (the
/// toolbars are built before the viewport widget exists). The editor-mode
/// actions (View / Brush / Eyedropper / Place Spawn) stay with the window —
/// the toolbar only embeds them (setEditorModeActions). Host services come
/// back as signals: statusMessage (status bar), actionStatesNeedUpdate
/// (MainWindow::updateActionStates), palettesNeedRefresh (repaint + palette
/// panel refresh after a decal/model import), syncPaletteSelection (the
/// palette panel highlight follows the brush color).
class BrushUiController : public QObject
{
    Q_OBJECT

public:
    BrushUiController(MapEditor::BrushTool *brushTool,
                      MapEditor::EditorSession *session,
                      QMainWindow *window, QObject *parent = nullptr);

    void setViewport(MapEditor::ViewportWidget *viewport) { viewport_ = viewport; }
    /// The toolbar embeds the window-owned editor-mode action switch.
    void setEditorModeActions(QAction *view, QAction *brush, QAction *eyedropper,
                              QAction *placeSpawn);

    // All brush actions + the exclusivity group (before the menus/toolbars
    // so both can embed them).
    void createActions();
    // The Brush / Sculpting / Modifiers / Model toolbars (own rows).
    void buildToolbars();
    // Edit → "Brush Type / Mode" + "Brush Modifiers" (the gate-height menu
    // spin mirror lives here).
    void addEditMenuSubMenus(QMenu *editMenu);

    // The brush block of MainWindow::updateActionStates: every brush action
    // and spinbox enablement, derived from the tool params + worldLoaded.
    void updateEnabledStates(bool worldLoaded);
    // Re-read the tool parameters into the buttons/spinbox state.
    void syncControls();
    /// The shared body of the Mask scale spin and setMaskScalePercent:
    /// recomputes the active image stamp from its un-scaled original at
    /// maskScalePercent_.
    void applyMaskScale();
    // The color button + swatch follow params().colorRgb.
    void syncColorControls();
    /// Push an RGB color into the color button without firing its signal.
    void setColorRgb(unsigned int rgb);
    /// Dialog-free image-mask loading, shared by the UI picker (onLoadBrushImage)
    /// and the MCP load_brush_image tool: the image's alpha channel becomes
    /// the stamp influence (fully transparent pixels do not stamp at all).
    /// Empty string on success, an agent/user-readable error otherwise.
    /// The un-scaled original is kept and the scale resets to 100%, exactly
    /// like the toolbar flow.
    QString loadBrushImage(const QString &filePath);
    /// Recomputes the active image stamp (brush mask or ImageDecal decal)
    /// at `percent` of the original size (10..800), centred under the cursor
    /// — the toolbar Mask spin body, callable headless (MCP apply_brush).
    void setMaskScalePercent(int percent);
    int maskScalePercent() const { return maskScalePercent_; }
    /// Convert the imported model colours into the stamp image (true 24-bit
    /// colors; the palette is consulted only for the nearest-entry SurBuf
    /// side band — the MCP import_model path sets the raster directly, then
    /// prepares colors through here).
    bool quantizeModelBrush();

signals:
    void statusMessage(const QString &message, int timeoutMs);
    void actionStatesNeedUpdate();
    /// The color picker panel follows the brush color.
    void syncBrushColor(unsigned int rgb);

private:
    void buildBrushToolbar();
    void buildSculptToolbar();
    void buildModifierToolbar();
    void buildModelToolbar();
    void createBrushTypeActions();
    void createSculptTypeActions();
    void createShape3DActions();
    void createBrushModeActions();
    void createBrushGateActions();
    void createStampShapeActions();
    /// Shared creator for the common brush-type actions (also used by the
    /// sculpting section).
    QAction *addBrushTypeAction(QActionGroup *group, const QIcon &icon,
                                const QString &text, const QString &toolTip,
                                MapEditor::BrushEffect effect, int direction);
    /// T3.5 — open a file dialog, load an image as brush mask.
    void onLoadBrushImage();
    /// ImageDecal — palette-limited image stamp.
    void onLoadDecalImage();
    /// Import a PLY model as the model-heightmap brush.
    void onImportModel();
    void showStatus(const QString &message, int timeoutMs);

    MapEditor::BrushTool *brushTool_ = nullptr;
    MapEditor::EditorSession *session_ = nullptr;
    MapEditor::ViewportWidget *viewport_ = nullptr;
    QMainWindow *window_ = nullptr;

    // Window-owned editor-mode actions, embedded into the Brush toolbar.
    QAction *modeActionView_ = nullptr;
    QAction *modeActionBrush_ = nullptr;
    QAction *modeActionEyedropper_ = nullptr;
    QAction *modeActionPlaceSpawn_ = nullptr;

    // T3.2 — brush type (effect) actions: raise / lower / level to
    // zeroplast / level to an arbitrary height.
    QAction *actBrushRaise_ = nullptr;
    QAction *actBrushLower_ = nullptr;
    QAction *actBrushLevelZP_ = nullptr;
    QAction *actBrushLevelHeight_ = nullptr;
    // T3.3 — color brush (SetSurf) action.
    QAction *actBrushColor_ = nullptr;
    // T4.2 — eyedropper as a brush type: the PickColor effect.
    QAction *actBrushPickColor_ = nullptr;
    // T3.4/T5 — dam-marking brushes (soot, very light dam).
    QAction *actBrushSoot_ = nullptr;
    QAction *actBrushVeryLightDam_ = nullptr;
    // Scripted brush: the type action and the script-editor action.
    QAction *actBrushScript_ = nullptr;
    QAction *actBrushScriptEdit_ = nullptr;
    // Sculpting brushes (Dam mode only).
    QAction *actBrushSmooth_ = nullptr;
    QAction *actBrushFlatten_ = nullptr;
    QAction *actBrushTricubic_ = nullptr;
    QAction *actBrushInflate_ = nullptr;
    QAction *actBrushDeflate_ = nullptr;
    QAction *actBrushGaussianBlur_ = nullptr;
    // 3D shape brushes (Dam mode only): cone / pyramid / truncated cone /
    // truncated pyramid / half torus / half ellipsoid.
    QAction *actBrush3DCone_ = nullptr;
    QAction *actBrush3DPyramid_ = nullptr;
    QAction *actBrush3DTruncCone_ = nullptr;
    QAction *actBrush3DTruncPyramid_ = nullptr;
    QAction *actBrush3DTorus_ = nullptr;
    QAction *actBrush3DEllipsoid_ = nullptr;
    // 3D shape settings (sculpting toolbar).
    QSpinBox *shapeHeightSpin_ = nullptr;
    QSpinBox *shapeTopOffsetXSpin_ = nullptr;
    QSpinBox *shapeTopOffsetYSpin_ = nullptr;
    QSpinBox *shapeTopRadiusSpin_ = nullptr;
    QSpinBox *shapeTopSideSpin_ = nullptr;
    // Model-heightmap brush (own toolbar).
    QAction *actImportModel_ = nullptr;
    QAction *actModelBrush_ = nullptr;
    QAction *actModelPlace_ = nullptr;
    QAction *actModelAdd_ = nullptr;
    QAction *actModelSubtract_ = nullptr;
    QSpinBox *modelHeightSpin_ = nullptr;
    QDoubleSpinBox *modelScaleXSpin_ = nullptr;
    QDoubleSpinBox *modelScaleYSpin_ = nullptr;
    // The last imported model path (status messages / re-quantization).
    QString modelImportPath_;
    // Gaussian-blur convolution radius (cells).
    QSpinBox *blurRadiusSpin_ = nullptr;
    // The script source kept by the controller (loaded into BrushParams::script).
    QString brushScript_;
    MapEditor::ScriptStampShape scriptStampShape_ =
        MapEditor::ScriptStampShape::Circle;
    // Sculpting fraction (0..1) shared by Smooth / Flatten.
    QDoubleSpinBox *sculptEffectSpin_ = nullptr;
    // Brush height-gate modifiers (Dam mode only).
    QAction *actGateAbove_ = nullptr;
    QAction *actGateBelow_ = nullptr;
    QSpinBox *gateHeightSpin_ = nullptr;
    QSpinBox *gateHeightMenuSpin_ = nullptr;
    // T3.5 — load-image action: the loaded image becomes the stamp mask.
    QAction *actLoadBrushImage_ = nullptr;
    // ImageDecal — palette-limited image stamp.
    QAction *actBrushImageDecal_ = nullptr;
    // T3.1 — brush form (stamp shape) actions.
    QAction *actShapeCircle_ = nullptr;
    QAction *actShapeSquare_ = nullptr;
    QAction *actShapeCircleOutline_ = nullptr;
    QAction *actShapeSquareOutline_ = nullptr;
    QAction *actShapePolygon_ = nullptr;
    QAction *actShapePolygonOutline_ = nullptr;
    QAction *actShapeLine_ = nullptr;
    // T3.1 — stamp half-size in map cells (BrushParams::radius).
    QSpinBox *brushRadiusSpin_ = nullptr;
    // Regular-polygon stamp params.
    QSpinBox *polygonSidesSpin_ = nullptr;
    QDoubleSpinBox *polygonAngleSpin_ = nullptr;
    // Stroke width in cells of the unfilled shapes.
    QSpinBox *brushThicknessSpin_ = nullptr;
    // T3.2 — brush mode actions: Geo / Dam.
    QAction *actBrushGeo_ = nullptr;
    QAction *actBrushDam_ = nullptr;
    // T3.2 — AlignVariableH target height (whole voxels 0..255).
    QSpinBox *brushLevelSpin_ = nullptr;
    // The color button: swatch showing the current paint RGB; clicking opens
    // the color picker dialog.
    QToolButton *brushColorButton_ = nullptr;
    // T3.5 — the image-mask scale control and the un-scaled original mask.
    QSpinBox *brushMaskScaleSpin_ = nullptr;
    MapEditor::BrushMask brushMaskOriginal_;
    int maskScalePercent_ = 100;
    // ImageDecal — the un-scaled true-color decal.
    MapEditor::ImageDecal::DecalImage decalOriginal_;
    // Exclusivity group of the common + sculpting brush-type actions.
    QActionGroup *brushTypeGroup_ = nullptr;
};

} // namespace MapEditor

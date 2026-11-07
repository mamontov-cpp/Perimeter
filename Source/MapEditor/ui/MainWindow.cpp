#include "MainWindow.h"
#include "BrushUiAssets.h"
#include "BrushUiController.h"
#include "ImageIoController.h"
#include "McpBridge.h"
#include "EditorMath.h"

#include <View3DRenderer.h>
#include <BrushTool.h>
#include <EditorSession.h>
#include <GameCameraPreset.h>
#include <GameDirManager.h>
#include <BattleMapPanel.h>
#include <HelpWindow.h>
#include <ImageDecal.h>
#include <MinimapWidget.h>
#include <NewWorldWizard.h>
#include <PalettePanel.h>
#include <RunScriptDialog.h>
#include <ScriptedBrush.h>
#include <ViewportRenderer.h>
#include <ViewportWidget.h>
#include <WorldFiles.h>
#include <WorldImages.h>
#include <WorldSettingsDialog.h>
#include <ValidationDialog.h>

#include <filesystem>
#include <algorithm>
#include <cmath>

#include "EditorTrace.h"
using MapEditor::editorTrace;

namespace fs = std::filesystem;

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QWidgetAction>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QPolygonF>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QTransform>
#include <QVBoxLayout>

namespace {

/// The script palette hook is a context-free function pointer; this points
/// it at the running MainWindow (set in the ctor, cleared in the dtor).
MainWindow* g_mainWindowForScriptHook = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------



MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("MainWindow"));
    setWindowTitle(QStringLiteral("Perimeter Map Editor"));
    resize(1280, 800);

    session_ = new MapEditor::EditorSession();
    gameDirMgr_ = new GameDirManager();

    // T3.1 — the single brush tool of the session; its default parameters
    // (filled circle, raise) let the viewport paint right away.
    brushTool_ = new MapEditor::BrushTool(*session_);

    // T3.2 — the whole brush UI (type/mode/stamp actions, the Brush /
    // Sculpting / Modifiers / Model toolbars, the image/model import flows)
    // lives in the BrushUiController (docs/architecture.md §5.36) — same
    // signal pattern as the BattleMapController below.
    brushUI_ = new MapEditor::BrushUiController(brushTool_, session_, this, this);
    connect(brushUI_, &MapEditor::BrushUiController::statusMessage, this,
            [this](const QString &message, int timeoutMs) {
                statusBar()->showMessage(message, timeoutMs);
            });
    connect(brushUI_,
            &MapEditor::BrushUiController::actionStatesNeedUpdate, this,
            [this]() { updateActionStates(); });
    connect(brushUI_, &MapEditor::BrushUiController::palettesNeedRefresh,
            this, [this]() {
                refreshPaletteRenderers();
                if (palettePanel_) {
                    palettePanel_->refresh();
                }
            });
    connect(brushUI_, &MapEditor::BrushUiController::syncPaletteSelection,
            this, [this](int index, bool dam) {
                if (palettePanel_) {
                    palettePanel_->selectIndex(index, dam);
                }
            });
    createBrushActions();

    // The scripted brush's showInfoMessageBox/showErrorMessageBox execute in
    // the Qt-free core — forward them to a QMessageBox here (information vs
    // critical mode). A non-capturing lambda converts to the function
    // pointer the hook takes.
    MapEditor::setScriptedBrushMessageHook(
        [](bool isError, const std::string& text) {
            const QString message = QString::fromStdString(text);
            if (isError) {
                QMessageBox::critical(nullptr, QStringLiteral("Scripted Brush"),
                                      message);
            } else {
                QMessageBox::information(nullptr,
                                         QStringLiteral("Scripted Brush"),
                                         message);
            }
        });

    // A script's setGeoPalette/setDamPalette edits the palettes in the
    // Qt-free core — the hook repaints the viewport (every pixel using the
    // colour re-tints) and refreshes the palette panel.
    g_mainWindowForScriptHook = this;
    MapEditor::setScriptPaletteHook([] {
        if (g_mainWindowForScriptHook) {
            g_mainWindowForScriptHook->refreshAfterScriptPaletteEdit();
        }
    });

    buildMenuBar();
    buildViewToolbar();
    addToolBarBreak();
    brushUI_->buildToolbars();
    buildStatusBar();
    buildViewport();
    // The toolbars were built before the viewport existed — hand it over
    // now (the brush controls push previews into it).
    brushUI_->setViewport(viewport_);
    // T6 — the image import/export flows (docs/architecture.md §5.37).
    imageIO_ = new MapEditor::ImageIoController(session_, viewport_, this, this);
    connect(imageIO_, &MapEditor::ImageIoController::statusMessage, this,
            [this](const QString &message, int timeoutMs) {
                statusBar()->showMessage(message, timeoutMs);
            });
    connect(imageIO_, &MapEditor::ImageIoController::statusBarNeedsUpdate,
            this, [this]() { updateStatusBar(); });

    // The toggle/spinbox states were set before the viewport existed — push
    // them now (they reach the renderer when a world is bound).
    if (viewport_) {
        if (actDimGeo_) {
            viewport_->setGeoDimmed(actDimGeo_->isChecked());
        }
        if (gridStepWSpin_ && gridStepHSpin_) {
            viewport_->setGridStep(gridStepWSpin_->value(),
                                   gridStepHSpin_->value());
        }
    }

    // The viewport was built silently — no camera signal has fired yet, so
    // fill the readout with the actual 2D zoom state instead of leaving it
    // blank until the first pan/zoom.
    updateViewReadout();
    updateStatusBar();
#ifdef MAP_EDITOR_MCP
    // Embedded MCP server for LLM agents (after everything it can touch
    // exists). A busy port only disables the server, never the editor. The
    // whole layer lives in the McpBridge (docs/architecture.md §5.37).
    mcpBridge_ = new MapEditor::McpBridge(session_, viewport_, brushTool_,
                                          battleMaps_, brushUI_, this);
    mcpBridge_->setWorldUnloadHook([this]() { onWorldUnload(); });
    mcpBridge_->setWorldLoadedHook([this]() { onWorldLoaded(); });
    mcpBridge_->setStatusBarHook([this]() { updateStatusBar(); });
    mcpBridge_->setPaletteRefreshHook([this]() { refreshPaletteRenderers(); });
    mcpBridge_->start();
#endif
    editorTrace("MainWindow: ctor done");
}

MainWindow::~MainWindow()
{
    editorTrace("MainWindow: dtor start");
#ifdef MAP_EDITOR_MCP
    delete mcpBridge_; // stops serving (the bridge owns the server) and
    mcpBridge_ = nullptr; // releases the port
#endif
    if (g_mainWindowForScriptHook == this) {
        g_mainWindowForScriptHook = nullptr;
        MapEditor::setScriptPaletteHook(nullptr);
    }
    delete brushTool_;
    delete session_;
    delete gameDirMgr_;
    // viewport_ is a child widget; deleted by the parent.
    editorTrace("MainWindow: dtor members deleted (viewport dies with parent)");
}

void MainWindow::debugOpenGameDir(const std::string& dir)
{
    editorTrace("autotest: setGameDir('%s')", dir.c_str());
    if (!session_->setGameDir(dir)) {
        editorTrace("autotest: setGameDir FAILED");
        return;
    }
    if (!session_->scanWorlds()) {
        editorTrace("autotest: scanWorlds FAILED");
        return;
    }
    updateStatusBar();
    editorTrace("autotest: game dir opened (%zu worlds)",
                session_->worldCount());
}

void MainWindow::debugLoadWorld(const std::string& worldName)
{
    editorTrace("autotest: loadWorldByName('%s')", worldName.c_str());
    if (!session_->loadWorldByName(worldName)) {
        editorTrace("autotest: loadWorldByName FAILED");
        return;
    }
    editorTrace("autotest: calling onWorldLoaded");
    onWorldLoaded();
    editorTrace("autotest: after onWorldLoaded, updating status bar");
    updateStatusBar();
    editorTrace("autotest: world loaded");
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------

void MainWindow::buildViewport()
{
    viewport_ = new MapEditor::ViewportWidget(this);

    // T3.1 — plug the brush tool in and mirror its shape/radius in the
    // cursor preview (the viewport draws the outline over the raster).
    viewport_->setBrushTool(brushTool_);
    // The editor mode actions were created before this widget existed — push
    // the checked state into the viewport now that it is here (the Brush
    // mode is the viewport default, so it needs no explicit set).
    if (viewport_ && actModeEyedropper_ && actModeView_) {
        if (actModeEyedropper_->isChecked()) {
            viewport_->setEditorMode(
                MapEditor::ViewportWidget::EditorMode::Eyedropper);
        } else if (actModeView_->isChecked()) {
            viewport_->setEditorMode(MapEditor::ViewportWidget::EditorMode::View);
        }
    }
    brushUI_->syncControls();
    viewport_->setBrushPreviewVisible(true);

    // Connect cursor position signal to status bar.
    connect(viewport_, &MapEditor::ViewportWidget::cursorMapPosition,
            this, &MainWindow::updateCursorInfo);

    // Brush edits change the modified state — keep the status bar/undo
    // actions live.
    connect(viewport_, &MapEditor::ViewportWidget::worldEdited,
            this, &MainWindow::updateStatusBar);

    // A failed scripted-brush stamp: critical message box per spec.
    connect(viewport_, &MapEditor::ViewportWidget::brushScriptError, this,
            &MainWindow::onBrushScriptError);

    // Keep the toolbar in sync with whatever the viewport does (mouse
    // orbit/pan/zoom change the camera too, not only the actions).
    connect(viewport_, &MapEditor::ViewportWidget::viewModeChanged, this,
            [this](MapEditor::ViewportWidget::ViewMode mode) {
                onViewportViewModeChanged(static_cast<int>(mode));
            });
    connect(viewport_, &MapEditor::ViewportWidget::renderStyleChanged, this,
            [this](MapEditor::ViewportWidget::RenderStyle style) {
                // Keep the View menu / View toolbar toggle pair in sync with
                // whatever set the style (the menu buttons are the only other
                // driver today).
                if (actChunkyRender_) {
                    actChunkyRender_->setChecked(
                        style == MapEditor::ViewportWidget::RenderStyle::Chunky);
                }
                if (actSmoothRender_) {
                    actSmoothRender_->setChecked(
                        style == MapEditor::ViewportWidget::RenderStyle::Smooth);
                }
            });
    connect(viewport_, &MapEditor::ViewportWidget::obliqueCameraChanged,
            this, &MainWindow::onObliqueCameraChanged);

    // T4.2 — editor mode follows the viewport too (mode actions keep their
    // check state when the interaction switches the mode side-effects).
    connect(viewport_, &MapEditor::ViewportWidget::editorModeChanged, this,
            [this](MapEditor::ViewportWidget::EditorMode mode) {
                onEditorModeChanged(static_cast<int>(mode));
            });

    // T3.3 — eyedropper signal: right-click in TopDown mode picks SurBuf.
    connect(viewport_, &MapEditor::ViewportWidget::eyedropperPicked,
            this, &MainWindow::onEyedropperPicked);

    // Hide the freeze-dam action when the GPU terrain path is available —
    // the feature is a CPU-path-only fallback (it depends on the torus
    // repetition logic in View3DRenderer, which sokol does not implement).
    connect(viewport_, &MapEditor::ViewportWidget::gpuViewReadyChanged, this,
            [this](bool gpuReady) {
                if (actFreezeDam_) {
                    actFreezeDam_->setVisible(!gpuReady);
                }
            });

    // Keep the camera preset pair (like in game / like in editor) in sync
    // when the viewport's game camera mode changes. The switch also changes
    // WHICH rotation the spinbox mirrors (the game preset psi=0 vs the
    // editor turnAngleDeg), so refresh it here.
    connect(viewport_, &MapEditor::ViewportWidget::gameCameraModeChanged,
            this, [this](bool gameMode) {
                if (actGameCamera_) {
                    actGameCamera_->setChecked(gameMode);
                }
                if (actEditorCamera_) {
                    actEditorCamera_->setChecked(!gameMode);
                }
                syncCameraRotationSpin();
            });

    /// Initial style button state (the viewport defaults to Smooth; post-T3.3
    /// chunky/smooth renderer toggle).
    actChunkyRender_->setChecked(viewport_->renderStyle() !=
        MapEditor::ViewportWidget::RenderStyle::Smooth);
    actSmoothRender_->setChecked(viewport_->renderStyle() ==
        MapEditor::ViewportWidget::RenderStyle::Smooth);

    setCentralWidget(viewport_);

    // T2.4 — the minimap goes into its own dock next to the central widget.
    buildMinimapDock();
    // T4.1 — the palette panel takes its place below the minimap dock.
    buildPaletteDock();
    // T8 — the battle-map domain (Battle Map / Buildings / Scum Nests /
    // Nature docks, the .spg flow, .gmp save/load) lives in the
    // BattleMapController (docs/architecture.md §5.35). The host services it
    // calls back into are the unsaved-changes guard, the post-world-load
    // hook, the status bar and the two full-refresh entry points.
    battleMaps_ = new MapEditor::BattleMapController(session_, viewport_,
                                                     minimap_, this, viewMenu_,
                                                     this);
    battleMaps_->setWorldSwitchGuard(
        [this]() { return confirmDiscardUnsavedChanges(); });
    battleMaps_->setWorldLoadedHook([this]() { onWorldLoaded(); });
    connect(battleMaps_, &MapEditor::BattleMapController::statusMessage, this,
            [this](const QString &message, int timeoutMs) {
                statusBar()->showMessage(message, timeoutMs);
            });
    connect(battleMaps_,
            &MapEditor::BattleMapController::actionStatesNeedUpdate, this,
            [this]() { updateActionStates(); });
    connect(battleMaps_, &MapEditor::BattleMapController::palettesNeedRefresh,
            this, [this]() { refreshPaletteRenderers(); });
    battleMaps_->buildDocks();
}

void MainWindow::buildMinimapDock()
{
    minimap_ = new MapEditor::MinimapWidget(this);

    minimapDock_ = new QDockWidget(tr("Minimap"), this);
    minimapDock_->setObjectName(QStringLiteral("MinimapDock"));
    minimapDock_->setWidget(minimap_);
    minimapDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                  Qt::RightDockWidgetArea |
                                  Qt::TopDockWidgetArea |
                                  Qt::BottomDockWidgetArea);
    minimapDock_->setFeatures(QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, minimapDock_);

    // The View menu owns the show/hide toggle (default: visible).
    if (viewMenu_) {
        viewMenu_->addSeparator();
        QAction *toggleAction = minimapDock_->toggleViewAction();
        toggleAction->setText(tr("&Minimap"));
        toggleAction->setToolTip(
            tr("Show or hide the minimap panel (M)"));
        toggleAction->setShortcut(QKeySequence(Qt::Key_M));
        viewMenu_->addAction(toggleAction);
    }

    // The view outline follows the viewport — whatever moves it
    // (pan, zoom, orbit, mode switch, resize, world load/unload) ends in
    // ViewportWidget::viewPolygonChanged after the next repaint. The polygon
    // carries the 3D camera azimuth/perspective that an axis-aligned rect
    // would flatten.
    connect(viewport_, &MapEditor::ViewportWidget::viewPolygonChanged,
            minimap_, &MapEditor::MinimapWidget::setViewPolygon);

    // The top view follows the terrain: every viewport repaint that repainted
    // cells (brush stamp, undo/redo, image import, palette edit) re-samples the
    // panel's raster from the live world — map.tga on disk only catches up on
    // save, so reading the file back after an edit would show the old surface.
    connect(viewport_, &MapEditor::ViewportWidget::terrainRepainted,
            minimap_, &MapEditor::MinimapWidget::refreshFromLiveTerrain);

    // Clicking the minimap navigates there instead.
    connect(minimap_, &MapEditor::MinimapWidget::cellClicked,
            this, &MainWindow::onMinimapCellClicked);

    // Load the image for the world that may already be bound and push the
    // current view rectangle once (subsequent updates come from the signal).
    minimap_->setSession(session_);
    minimap_->setViewPolygon(viewport_->currentViewPolygon());
}

void MainWindow::buildPaletteDock()
{
    palettePanel_ = new MapEditor::PalettePanel(this);

    paletteDock_ = new QDockWidget(tr("Palette"), this);
    paletteDock_->setObjectName(QStringLiteral("PaletteDock"));
    paletteDock_->setWidget(palettePanel_);
    paletteDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                  Qt::RightDockWidgetArea |
                                  Qt::TopDockWidgetArea |
                                  Qt::BottomDockWidgetArea);
    paletteDock_->setFeatures(QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable);
    // Stacks under the minimap dock on the same side.
    addDockWidget(Qt::RightDockWidgetArea, paletteDock_);

    // The View menu owns the show/hide toggle (default: visible).
    if (viewMenu_) {
        viewMenu_->addSeparator();
        QAction *toggleAction = paletteDock_->toggleViewAction();
        toggleAction->setText(tr("&Palette"));
        toggleAction->setToolTip(
            tr("Show or hide the palette panel (P)"));
        toggleAction->setShortcut(QKeySequence(Qt::Key_P));
        viewMenu_->addAction(toggleAction);
    }

    // A selected swatch becomes the color the brush paints: the palette the
    // swatch lives in follows the brush (Geo / Dam layer, T4.1 model — a
    // SurBuf value indexes the palette of the cell's layer directly).
    connect(palettePanel_, &MapEditor::PalettePanel::colorSelected,
            this, [this](int paletteIndex, bool dam) {
        if (brushTool_) {
            MapEditor::BrushParams params = brushTool_->params();
            params.mode = dam ? MapEditor::BrushMode::Dam
                              : MapEditor::BrushMode::Geo;
            params.colorIndex = paletteIndex;
            brushTool_->setParams(params);
        }
        brushUI_->setColorIndex(paletteIndex);
        brushUI_->syncControls();
        brushUI_->syncColorControls();
    });

    // An edited palette re-tints the whole world: rebuild both render paths.
    connect(palettePanel_, &MapEditor::PalettePanel::paletteEdited,
            this, [this](int, bool) {
        refreshPaletteRenderers();
        // The current brush color swatch may be the entry that was edited.
        brushUI_->syncColorControls();
    });

    // Push the session that may already be bound.
    palettePanel_->setSession(session_);
}

void MainWindow::onWorldLoaded()
{
    editorTrace("onWorldLoaded: start (viewport=%p, session=%p)",
                static_cast<void*>(viewport_), static_cast<void*>(session_));
    // Rebind the viewport to pick up the newly loaded world.
    if (viewport_ && session_) {
        editorTrace("onWorldLoaded: calling viewport->setSession");
        viewport_->setSession(session_);
        editorTrace("onWorldLoaded: viewport setSession done");
    }
    // Rebind the minimap image (map.tga of the new world) and reset the
    // outline until the viewport's next repaint pushes a rectangle.
    editorTrace("onWorldLoaded: setting minimap session");
    if (minimap_ && session_) {
        minimap_->setSession(session_);
        minimap_->setViewPolygon(viewport_ ? viewport_->currentViewPolygon()
                                           : QPolygonF());
    }
    editorTrace("onWorldLoaded: setting palette session");
    // Rebuild the palette panel grids (T4.1) — the palette belongs to the
    // newly loaded world. setSession() resets the swatch selection, so push
    // the brush state back in: the highlighted entry must keep showing the
    // color the brush will paint.
    if (palettePanel_ && session_) {
        palettePanel_->setSession(session_);
        const MapEditor::BrushParams &params = brushTool_->params();
        palettePanel_->selectIndex(params.colorIndex,
                                   params.mode == MapEditor::BrushMode::Dam);
    }
    editorTrace("onWorldLoaded: sync brush controls");
    // The brush-dependent controls (color swatch, spinboxes, cursor preview)
    // were only partially initialized before the world existed — re-read the
    // tool state now that the palettes are available.
    brushUI_->syncControls();
    editorTrace("onWorldLoaded: updating view readout");
    // The camera readout has never fired its signal yet on a fresh session.
    updateViewReadout();
    editorTrace("onWorldLoaded: updating window title");
    // The title carries the world name (also after "Save As", which
    // rebinds the session onto the renamed world and lands here).
    updateWindowTitle();
    editorTrace("onWorldLoaded: updating status bar");
    updateStatusBar();
    editorTrace("onWorldLoaded: done");
}

void MainWindow::onWorldUnload()
{
    if (viewport_ && session_) {
        viewport_->setSession(session_);
    }
    if (minimap_ && session_) {
        minimap_->setSession(session_);
        minimap_->setViewPolygon(QPolygonF());
    }
    if (palettePanel_ && session_) {
        palettePanel_->setSession(session_);
    }
    // The brush controls lose their world-bound state (color swatch RGB,
    // palette selection) — re-read the tool so nothing stale stays on screen.
    brushUI_->syncControls();
    updateWindowTitle();
    updateStatusBar();
}

void MainWindow::updateCursorInfo(int mapX, int mapY)
{
    // Both view modes report the cell under the cursor: TopDown maps the
    // screen linearly, Oblique ray-picks the rendered voxel surface.
    QString info = QString(tr("X: %1  Y: %2")).arg(mapX).arg(mapY);
    // T4.2 — also show the palette entry and RGB of the pixel: a SurBuf
    // value indexes the palette of the cell's own layer (T4.1 model), so the
    // readout resolves it the same way the renderer does (geo/dam).
    if (session_ && session_->isWorldLoaded() && mapX >= 0 && mapY >= 0) {
        const int surIndex = session_->surBufAt(mapX, mapY);
        const bool dam = session_->cellIsDam(mapX, mapY);
        const unsigned int rgb =
            session_->paletteRgb(dam ? MapEditor::EditorSession::PaletteLayer::Dam
                                     : MapEditor::EditorSession::PaletteLayer::Geo,
                                 surIndex);
        info += QString(tr("  Sur: %1 (%2)  %3"))
                    .arg(surIndex)
                    .arg(dam ? tr("Dam") : tr("Geo"))
                    .arg(rgb, 6, 16, QLatin1Char('0'));
    }
    statusBar()->showMessage(info, 3000);
}

void MainWindow::onZoomToFit()
{
    if (viewport_) {
        viewport_->zoomToFit();
    }
}

void MainWindow::onActionToggle3D()
{
    if (viewport_) {
        viewport_->toggleViewMode();
    }
}

void MainWindow::onActionGameCamera()
{
    if (!viewport_) {
        return;
    }

    // Aim at the first player of the loaded world — the camera state the
    // game itself starts a battle with (psi=0, theta=0, min zoom distance;
    // FORTOP and the other shipped worlds carry no camera splines, so the
    // frame anchor is the player view centre).
    QPointF presetCenter;
    if (session_ && session_->isWorldLoaded()) {
        
        // Used preset from FORTOP, since current can be also not present in world
        static constexpr MapEditor::GameCameraPreset default_preset{ 354.791000,306.425000,89.509186,17.999985,1000.000000,true };
        const MapEditor::GameCameraPreset preset = default_preset;
        presetCenter = QPointF(preset.centerX, preset.centerY);
    }

    // The game camera model (FOV frustum, terCameraType matrix) lives in
    // the GPU terrain path; without it there is nothing to switch to.
    if (!viewport_->setGameCameraMode(true, presetCenter)) {
        QMessageBox::warning(
            this, tr("Camera Like in Game"),
            tr("The game camera is only available in the GPU 3D view. "
               "The editor camera stays active."));
        if (actGameCamera_) {
            actGameCamera_->setChecked(false);
        }
        if (actEditorCamera_) {
            actEditorCamera_->setChecked(true);
        }
        return;
    }

    // Make sure the 3D view is visible — the game camera replaces the
    // editor 3D camera, switching from the 2D view otherwise shows nothing.
    if (viewport_->viewMode() != MapEditor::ViewportWidget::ViewMode::Oblique) {
        viewport_->setViewMode(MapEditor::ViewportWidget::ViewMode::Oblique);
    }
}

void MainWindow::onActionEditorCamera()
{
    if (viewport_) {
        viewport_->setGameCameraMode(false, QPointF());
    }
}

void MainWindow::onViewportViewModeChanged(int mode)
{
    if (actView3D_) {
        actView3D_->setChecked(mode ==
            static_cast<int>(MapEditor::ViewportWidget::ViewMode::Oblique));
    }
    updateViewReadout();
}

void MainWindow::onObliqueCameraChanged(const MapEditor::View3DCamera &)
{
    syncCameraRotationSpin();
    updateViewReadout();
}

void MainWindow::syncCameraRotationSpin()
{
    if (!camRotationSpin_ || !viewport_) {
        return;
    }
    // The spinbox shows a single decimal; compare at that precision so a
    // camera change below the display step does not re-write the control.
    const double shown =
        std::round(viewport_->rotationDeg() * 10.0) / 10.0;
    if (std::abs(camRotationSpin_->value() - shown) < 1e-9) {
        return;
    }
    camRotationSpin_->blockSignals(true);
    camRotationSpin_->setValue(shown);
    camRotationSpin_->blockSignals(false);
}

void MainWindow::onEditorModeChanged(int mode)
{
    using Viewport = MapEditor::ViewportWidget;
    // Only mirrors the viewport state into the exclusive mode actions —
    // the View mode stays reachable no matter how the mode was switched.
    if (actModeView_) {
        actModeView_->setChecked(mode == static_cast<int>(Viewport::EditorMode::View));
    }
    if (actModeBrush_) {
        actModeBrush_->setChecked(mode == static_cast<int>(Viewport::EditorMode::Brush));
    }
    if (actModeEyedropper_) {
        actModeEyedropper_->setChecked(
            mode == static_cast<int>(Viewport::EditorMode::Eyedropper));
    }
    if (actModePlaceSpawn_) {
        actModePlaceSpawn_->setChecked(
            mode == static_cast<int>(Viewport::EditorMode::PlaceSpawn));
    }
}

void MainWindow::onMinimapCellClicked(int mapX, int mapY)
{
    if (viewport_) {
        viewport_->centerOnCell(QPoint(mapX, mapY));
    }
}

void MainWindow::onBrushScriptError(const QString& message)
{
    // Spec: application errors of the scripted brush go to a critical
    // message box (syntax problems are caught by the editor dialog's
    // OK-button check; this path covers runtime errors, timeouts and
    // misconfiguration like an empty script).
    QMessageBox::critical(this, tr("Scripted Brush Error"), message);
}

void MainWindow::onEyedropperPicked(int mapX, int mapY, unsigned char surIndex)
{

    // T4.2 — BrushTool::pickColorAt already stored the sampled pixel and put
    // it into params().colorIndex; the eyedropper readout slots the color
    // brush in for painting the sampled color afterwards. The SurBuf value
    // indexes the palette of the sampled cell's layer — point the brush at
    // the same layer so the color readout and the paint resolve it exactly
    // like the renderer does (T4.1 model).
    const bool dam = session_ && session_->cellIsDam(mapX, mapY);
    MapEditor::BrushParams params = brushTool_->params();
    params.effect = MapEditor::BrushEffect::SetColor;
    params.colorIndex = surIndex;
    params.mode = dam ? MapEditor::BrushMode::Dam : MapEditor::BrushMode::Geo;
    brushTool_->setParams(params);

    brushUI_->setColorIndex(surIndex);
    brushUI_->syncColorControls();
    brushUI_->syncControls();

    // T4.1 — mirror the pick into the palette panel selection.
    if (palettePanel_) {
        palettePanel_->selectIndex(static_cast<int>(surIndex), dam);
    }

    statusBar()->showMessage(
        tr("Eyedropper: SurBuf(%1,%2) = %3")
            .arg(mapX)
            .arg(mapY)
            .arg(static_cast<int>(surIndex)),
        5000);
}

void MainWindow::updateViewReadout()
{
    if (!viewReadout_ || !viewport_) {
        return;
    }
    if (viewport_->viewMode() == MapEditor::ViewportWidget::ViewMode::Oblique) {
        const MapEditor::View3DCamera &cam = viewport_->obliqueCamera();
        // rot follows the ACTIVE camera model (game psi in the game camera
        // mode) so the readout agrees with the rotation spinbox; the other
        // fields are editor-camera knobs either way.
        viewReadout_->setText(tr("3D    rot %1\u00B0  tilt %2\u00B0  lift %3  focus %4")
                                  .arg(qRound(viewport_->rotationDeg()))
                                  .arg(qRound(cam.leanDeg))
                                  .arg(cam.cameraDZ)
                                  .arg(cam.focusC));
    } else {
        viewReadout_->setText(tr("2D    zoom %1%")
                                  .arg(qRound(viewport_->zoom() * 100)));
    }
}

// ---------------------------------------------------------------------------
// Menu / status bar
// ---------------------------------------------------------------------------

void MainWindow::buildMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));

    QAction *actSelDir = fileMenu->addAction(tr("Select &Game Directory..."));
    actSelDir->setToolTip(tr("Open the folder that contains RESOURCE/Worlds/"));
    connect(actSelDir, &QAction::triggered, this, &MainWindow::onActionSelectGameDir);

    fileMenu->addSeparator();

    QAction *actScan = fileMenu->addAction(tr("&Scan Worlds"));
    actScan->setShortcut(QKeySequence(Qt::Key_F5));
    connect(actScan, &QAction::triggered, this, &MainWindow::onActionScanWorlds);

    QAction *actNewWorld = fileMenu->addAction(tr("New &World..."));
    actNewWorld->setToolTip(tr("Create a new world folder (flat or generated terrain)"));
    connect(actNewWorld, &QAction::triggered, this, &MainWindow::onActionNewWorld);

    QAction *actLoad = fileMenu->addAction(tr("&Load World..."));
    actLoad->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    connect(actLoad, &QAction::triggered, this, &MainWindow::onActionLoadWorld);

    QAction *actUnload = fileMenu->addAction(tr("&Unload World"));
    actUnload->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    connect(actUnload, &QAction::triggered, this, &MainWindow::onActionUnloadWorld);

    fileMenu->addSeparator();

    actSave_ = fileMenu->addAction(tr("&Save"));
    actSave_->setShortcut(QKeySequence(QKeySequence::Save));
    actSave_->setToolTip(tr("Save the loaded world (Ctrl+S)"));
    actSave_->setEnabled(false);
    connect(actSave_, &QAction::triggered, this, &MainWindow::onActionSave);

    actSaveAs_ = fileMenu->addAction(tr("Save &As..."));
    actSaveAs_->setShortcut(QKeySequence(QKeySequence::SaveAs));
    actSaveAs_->setToolTip(tr("Save a copy of the loaded world as a new world folder (Ctrl+Shift+S)"));
    actSaveAs_->setEnabled(false);
    connect(actSaveAs_, &QAction::triggered, this, &MainWindow::onActionSaveAs);

    actWorldParams_ = fileMenu->addAction(tr("World &Parameters..."));
    actWorldParams_->setToolTip(tr("Zero-layer height, generation powers, storage compression, geo noise and the zeroplast color"));
    actWorldParams_->setEnabled(false);
    connect(actWorldParams_, &QAction::triggered, this, &MainWindow::onActionWorldParams);

    QMenu *imageMenu = fileMenu->addMenu(tr("&Images"));
    QAction *actImportMapImage = imageMenu->addAction(tr("Import Image onto &Map..."));
    actImportMapImage->setToolTip(tr("Paint the surface colors from an image (RGB -> nearest palette index, indexed TGA -> indices as-is)"));
    actImportMapImage->setEnabled(false);
    connect(actImportMapImage, &QAction::triggered, this, &MainWindow::onActionImportMapImage);
    importMapImageAction_ = actImportMapImage;

    QAction *actImportUpTga = imageMenu->addAction(tr("Import &up.tga (cover image)..."));
    actImportUpTga->setToolTip(tr("Scale any image to 512x512 and store it as the world's up.tga"));
    actImportUpTga->setEnabled(false);
    connect(actImportUpTga, &QAction::triggered, this, &MainWindow::onActionImportUpTga);
    importUpTgaAction_ = actImportUpTga;

    QAction *actImportLeveled = imageMenu->addAction(tr("Import &leveledSurfaceTexture.tga..."));
    actImportLeveled->setToolTip(tr("Replace the leveled-zones texture (validated: indexed TGA, power of two, <= map size)"));
    actImportLeveled->setEnabled(false);
    connect(actImportLeveled, &QAction::triggered, this, &MainWindow::onActionImportLeveledTexture);
    importLeveledAction_ = actImportLeveled;

    imageMenu->addSeparator();
    QAction *actExportHeightmap = imageMenu->addAction(tr("&Export Heightmap (TGA)..."));
    actExportHeightmap->setToolTip(tr("8-bit grayscale heightmap (SGetAlt >> 5, dams included)"));
    actExportHeightmap->setEnabled(false);
    connect(actExportHeightmap, &QAction::triggered, this, &MainWindow::onActionExportHeightmap);
    exportHeightmapAction_ = actExportHeightmap;

    QAction *actExportTopView = imageMenu->addAction(tr("Export &Top View (TGA)..."));
    actExportTopView->setToolTip(tr("The surface raster exactly as the viewport and cache.tga show it"));
    actExportTopView->setEnabled(false);
    connect(actExportTopView, &QAction::triggered, this, &MainWindow::onActionExportTopView);
    exportTopViewAction_ = actExportTopView;

    fileMenu->addSeparator();

    QAction *actOpenBattleMap = fileMenu->addAction(tr("Open Battle &Map (.spg)..."));
    actOpenBattleMap->setToolTip(tr("Load a battle map (RESOURCE/Battle, RESOURCE/Worlds, SCENARIO, SURVIVAL) and its world"));
    actOpenBattleMap->setEnabled(false);
    connect(actOpenBattleMap, &QAction::triggered, this, &MainWindow::onActionOpenBattleMap);
    actOpenBattleMap_ = actOpenBattleMap;

    QAction *actSaveBattleMap = fileMenu->addAction(tr("Save Battle &Map"));
    actSaveBattleMap->setToolTip(tr("Write the loaded battle map back to its .spg"));
    actSaveBattleMap->setEnabled(false);
    connect(actSaveBattleMap, &QAction::triggered, this, &MainWindow::onActionSaveBattleMap);
    actSaveBattleMap_ = actSaveBattleMap;

    QAction *actSaveBattleMapAs = fileMenu->addAction(tr("Save Battle Map &As..."));
    actSaveBattleMapAs->setToolTip(tr("Write the battle map under a new name (RESOURCE/Battle or RESOURCE/Worlds)"));
    actSaveBattleMapAs->setEnabled(false);
    connect(actSaveBattleMapAs, &QAction::triggered, this, &MainWindow::onActionSaveBattleMapAs);
    actSaveBattleMapAs_ = actSaveBattleMapAs;

    QAction *actNewBattleMap = fileMenu->addAction(tr("New Battle &Map..."));
    actNewBattleMap->setToolTip(tr("Create a battle map (.spg) for the loaded world: player spawns, cameras and player settings"));
    actNewBattleMap->setEnabled(false);
    connect(actNewBattleMap, &QAction::triggered, this, &MainWindow::onActionNewBattleMap);
    actNewBattleMap_ = actNewBattleMap;

    fileMenu->addSeparator();

    QAction *actSaveDiff = fileMenu->addAction(tr("Save &Difference (.gmp)..."));
    actSaveDiff->setToolTip(tr("Write the difference data (terrain changed in "
                               "Difference Mode) in the game's .gmp format — "
                               "next to the loaded battle map by default"));
    actSaveDiff->setEnabled(false);
    connect(actSaveDiff, &QAction::triggered, this, &MainWindow::onActionSaveDiff);
    actSaveDiff_ = actSaveDiff;

    QAction *actLoadDiff = fileMenu->addAction(tr("&Load Difference (.gmp)..."));
    actLoadDiff->setToolTip(tr("Load difference data from a .gmp file (the "
                               "game's format) and overlay it on the loaded "
                               "world — visible in Difference Mode"));
    actLoadDiff->setEnabled(false);
    connect(actLoadDiff, &QAction::triggered, this, &MainWindow::onActionLoadDiff);
    actLoadDiff_ = actLoadDiff;

    fileMenu->addSeparator();

    QAction *actQuit = fileMenu->addAction(tr("E&xit"));
    actQuit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    connect(actQuit, &QAction::triggered, this, &QApplication::quit);

    buildEditMenu(menuBar());
    buildViewMenu(menuBar());
    buildToolsMenu();
    buildHelpMenu();
}

void MainWindow::buildEditMenu(QMenuBar *menuBar)
{
    QMenu *editMenu = menuBar->addMenu(tr("&Edit"));

    actUndo_ = editMenu->addAction(tr("&Undo"));
    actUndo_->setToolTip(tr("Undo the last terrain edit (Ctrl+Z)"));
    actUndo_->setShortcut(QKeySequence::Undo);
    actUndo_->setEnabled(false);
    connect(actUndo_, &QAction::triggered, this, &MainWindow::onActionUndo);

    actRedo_ = editMenu->addAction(tr("&Redo"));
    actRedo_->setToolTip(tr("Redo the last undone terrain edit (Ctrl+R or Ctrl+Y)"));
    actRedo_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_R),
                            QKeySequence::Redo});
    actRedo_->setEnabled(false);
    connect(actRedo_, &QAction::triggered, this, &MainWindow::onActionRedo);

    editMenu->addSeparator();

    actClearUndoHistory_ = editMenu->addAction(tr("C&lear Undo History"));
    actClearUndoHistory_->setToolTip(tr("Discard the whole undo/redo history"));
    actClearUndoHistory_->setEnabled(false);
    connect(actClearUndoHistory_, &QAction::triggered,
            this, &MainWindow::onActionClearUndoHistory);

    editMenu->addSeparator();

    // T4.2 — the editor interaction modes (View / Brush / Eyedropper) as an
    // exclusive switch, exposed at the top of the Edit menu next to the
    // brush function they drive.
    QMenu *modeMenu = editMenu->addMenu(tr("Edito&r Mode"));
    modeMenu->addAction(actModeView_);
    modeMenu->addAction(actModeBrush_);
    modeMenu->addAction(actModeEyedropper_);

    // T3.2 — the brush type/mode + modifiers submenus carry the same actions
    // as the toolbars; the BrushUiController owns them (the gate-height menu
    // spin mirror lives there too).
    brushUI_->addEditMenuSubMenus(editMenu);
}

void MainWindow::buildViewMenu(QMenuBar *menuBar)
{
    QMenu *viewMenu = menuBar->addMenu(tr("&View"));
    viewMenu_ = viewMenu;   // T2.4 appends the minimap toggle later.
    const auto create = [&viewMenu]() { return viewMenu->addAction(QString()); };

    // ------------------------------------------------------------------
    // TopDown / common
    // ------------------------------------------------------------------
    actGrid_ = create();
    actGrid_->setText(tr("&Grid"));
    actGrid_->setCheckable(true);
    actGrid_->setChecked(false);
    actGrid_->setShortcut(QKeySequence(Qt::Key_G));
    connect(actGrid_, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setGridVisible(checked);
        }
    });

    // In-game look: the game dims the un-terraformed geo surface to shades
    // of grey; the editor renders full palette colors by default. This
    // toggle switches the composite view (2D raster + smooth 3D) to the
    // dimmed game look and back.
    actDimGeo_ = create();
    actDimGeo_->setText(tr("Dim &Geo Layer (Game Look)"));
    actDimGeo_->setToolTip(
        tr("Render cells without a dam as darkened shades of grey, like the "
           "game shows the un-terraformed surface (2D view and smooth 3D; "
           "the dam areas keep their colors)."));
    actDimGeo_->setCheckable(true);
    actDimGeo_->setChecked(true);
    connect(actDimGeo_, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setGeoDimmed(checked);
        }
    });

    actZoomToFit_ = create();
    actZoomToFit_->setText(tr("Zoom to &Fit"));
    actZoomToFit_->setIcon(MapEditor::brushui::makeResetIcon());
    actZoomToFit_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(actZoomToFit_, &QAction::triggered, this, &MainWindow::onZoomToFit);

    viewMenu->addSeparator();

    // ------------------------------------------------------------------
    // Oblique (3D) camera (T2.3)
    // ------------------------------------------------------------------
    actView3D_ = create();
    actView3D_->setText(tr("&3D View"));
    actView3D_->setIcon(MapEditor::brushui::makeModeIcon(true));
    actView3D_->setToolTip(
        tr("Switch the viewport between the flat 2D palette and the tilted "
           "3D voxel view (F9)"));
    actView3D_->setCheckable(true);
    actView3D_->setShortcut(QKeySequence(Qt::Key_F9));
    connect(actView3D_, &QAction::triggered, this, &MainWindow::onActionToggle3D);

    // Freeze-dam 3D scroll: the world repeats infinitely, but with the dam
    // frozen only the geo layer scrolls — the repeats render geo-only, so
    // the dam stays in its single original place.
    actFreezeDam_ = create();
    actFreezeDam_->setText(tr("&Freeze Dam Layer (3D Scroll)"));
    actFreezeDam_->setToolTip(
        tr("The 3D world repeats infinitely when scrolling. With the dam "
           "frozen, only the geo layer repeats — the dam stays in its one "
           "original place, which makes the real map edge easy to spot. "
           "Smooth renderer only."));
    actFreezeDam_->setCheckable(true);
    actFreezeDam_->setChecked(false);
    connect(actFreezeDam_, &QAction::toggled, this, [this](bool checked) {
        if (viewport_) {
            viewport_->setDamFrozen(checked);
        }
    });

    // ------------------------------------------------------------------
    // Renderer style: chunky (pixel-exact) vs smooth (bilinear, the
    // in-game camera look). Both actions share one exclusive group.
    // ------------------------------------------------------------------
    QActionGroup *styleGroup = new QActionGroup(this);
    actChunkyRender_ = new QAction(MapEditor::brushui::makeChunkyIcon(), tr("&Chunky Renderer"), this);
    actChunkyRender_->setToolTip(
        tr("Pixel-exact rendering: one map cell = one solid block, hard cell "
           "edges (the classic look; the 2D view stays at or below 1:1)."));
    actChunkyRender_->setCheckable(true);
    actSmoothRender_ = new QAction(MapEditor::brushui::makeSmoothIcon(), tr("&Smooth Renderer"), this);
    actSmoothRender_->setToolTip(
        tr("Bilinear rendering like the in-game camera: neighboring cells "
           "blend into smooth gradients; the 2D view may zoom above 1:1."));
    actSmoothRender_->setCheckable(true);
    styleGroup->addAction(actChunkyRender_);
    styleGroup->addAction(actSmoothRender_);
    connect(actChunkyRender_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->setRenderStyle(
            MapEditor::ViewportWidget::RenderStyle::Chunky);
    });
    connect(actSmoothRender_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->setRenderStyle(
            MapEditor::ViewportWidget::RenderStyle::Smooth);
    });

    viewMenu->addSeparator();

    actRotateLeft_ = create();
    actRotateLeft_->setText(tr("Rotate &Left"));
    actRotateLeft_->setIcon(MapEditor::brushui::makeRotateIcon(false));
    actRotateLeft_->setToolTip(tr("Rotate the 3D camera 15\u00B0 left (Ctrl+Left)"));
    actRotateLeft_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    connect(actRotateLeft_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueRotation(-15.0);
    });

    actRotateRight_ = create();
    actRotateRight_->setText(tr("Rotate &Right"));
    actRotateRight_->setIcon(MapEditor::brushui::makeRotateIcon(true));
    actRotateRight_->setToolTip(tr("Rotate the 3D camera 15\u00B0 right (Ctrl+Right)"));
    actRotateRight_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right));
    connect(actRotateRight_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueRotation(15.0);
    });

    actTiltUp_ = create();
    actTiltUp_->setText(tr("Tilt &Up"));
    actTiltUp_->setIcon(MapEditor::brushui::makeTiltIcon(true));
    actTiltUp_->setToolTip(
        tr("Look further along the horizon (Ctrl+Up); "
           "Ctrl+Up/Ctrl+Down change the camera pitch"));
    actTiltUp_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    connect(actTiltUp_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueTilt(5.0);
    });

    actTiltDown_ = create();
    actTiltDown_->setText(tr("Tilt &Down"));
    actTiltDown_->setIcon(MapEditor::brushui::makeTiltIcon(false));
    actTiltDown_->setToolTip(tr("Look more down onto the surface (Ctrl+Down)"));
    actTiltDown_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    connect(actTiltDown_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueTilt(-5.0);
    });

    actLiftUp_ = create();
    actLiftUp_->setText(tr("Camera &Higher"));
    actLiftUp_->setIcon(MapEditor::brushui::makeLiftIcon(true));
    actLiftUp_->setToolTip(tr("Lift the 3D camera one step (Page Up)"));
    actLiftUp_->setShortcut(QKeySequence(Qt::Key_PageUp));
    connect(actLiftUp_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueLift(25);
    });

    actLiftDown_ = create();
    actLiftDown_->setText(tr("Camera &Lower"));
    actLiftDown_->setIcon(MapEditor::brushui::makeLiftIcon(false));
    actLiftDown_->setToolTip(tr("Lower the 3D camera one step (Page Down)"));
    actLiftDown_->setShortcut(QKeySequence(Qt::Key_PageDown));
    connect(actLiftDown_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->adjustObliqueLift(-25);
    });

    viewMenu->addSeparator();

    actZoomIn_ = create();
    actZoomIn_->setText(tr("Zoom &In"));
    actZoomIn_->setIcon(MapEditor::brushui::makeZoomIcon(true));
    actZoomIn_->setToolTip(tr("Zoom in (Ctrl+=; the 3D view moves the camera closer)"));
    actZoomIn_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal));
    connect(actZoomIn_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->zoomStep(1.25);
    });

    actZoomOut_ = create();
    actZoomOut_->setText(tr("Zoom &Out"));
    actZoomOut_->setIcon(MapEditor::brushui::makeZoomIcon(false));
    actZoomOut_->setToolTip(tr("Zoom out (Ctrl+-; the 3D view moves the camera away)"));
    actZoomOut_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    connect(actZoomOut_, &QAction::triggered, this, [this] {
        if (viewport_) viewport_->zoomStep(0.8);
    });

    actResetView_ = create();
    actResetView_->setText(tr("&Reset View"));
    actResetView_->setToolTip(tr("2D — fit the map; 3D — default camera"));
    actResetView_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Home));
    connect(actResetView_, &QAction::triggered, this, [this] {
        if (!viewport_) return;
        if (viewport_->viewMode() == MapEditor::ViewportWidget::ViewMode::Oblique) {
            viewport_->resetObliqueView();
        } else {
            viewport_->zoomToFit();
        }
    });

    viewMenu->addSeparator();

    // Difference mode (.gmp): ON shows and edits BASE+DIFF (brushes write
    // difference data), OFF shows/edits the base terrain. Toggling swaps the
    // whole buffer content and clears the undo history.
    actDiffMode_ = create();
    actDiffMode_->setText(tr("Differenc&e Mode"));
    actDiffMode_->setToolTip(
        tr("Toggle the difference mode (F2). ON: the terrain shows the base "
           "world with the .gmp difference data applied, and every brush "
           "edit is captured as difference data (saved via File → Save "
           "Difference (.gmp)). OFF: the plain base world. Switching clears "
           "the undo history."));
    actDiffMode_->setCheckable(true);
    actDiffMode_->setShortcut(QKeySequence(Qt::Key_F2));
    connect(actDiffMode_, &QAction::triggered, this, [this](bool on) {
        if (!session_ || on == session_->isDiffMode()) {
            return;
        }
        if (!session_->isWorldLoaded()) {
            actDiffMode_->setChecked(false);
            return;
        }
        if (session_->setDiffMode(on)) {
            // Every cell may have changed: full 2D cache repaint + 3D redraw.
            refreshPaletteRenderers();
            statusBar()->showMessage(
                on ? tr("Difference mode ON: brushes write .gmp difference "
                        "data (undo history cleared).")
                   : tr("Difference mode OFF: showing the base terrain "
                        "(undo history cleared)."),
                6000);
        } else {
            actDiffMode_->setChecked(!on);
        }
        updateActionStates();
    });

    viewMenu->addSeparator();

    // ------------------------------------------------------------------
    // Camera preset pair (exclusive): the main game's camera model vs the
    // editor camera model. The game variant needs the GPU terrain path —
    // the game frustum/FOV lives in TerrainGpuView.
    // ------------------------------------------------------------------
    QActionGroup *cameraGroup = new QActionGroup(this);
    actGameCamera_ = create();
    actGameCamera_->setText(tr("Camera Like in &Game"));
    actGameCamera_->setToolTip(
        tr("Switch the 3D view to the main game's camera: psi/theta/distance "
           "model with the game FOV (HardwareCameraFocus), aimed at the first "
           "player of the loaded world — the view a player has at battle "
           "start. The minimap outline follows the game frustum."));
    actGameCamera_->setCheckable(true);
    actEditorCamera_ = create();
    actEditorCamera_->setText(tr("Camera Like in &Editor"));
    actEditorCamera_->setToolTip(
        tr("Switch the 3D view back to the editor camera (rotation/tilt/lift/"
           "focus with its own zoom model)."));
    actEditorCamera_->setCheckable(true);
    actEditorCamera_->setChecked(true);
    cameraGroup->addAction(actGameCamera_);
    cameraGroup->addAction(actEditorCamera_);
    connect(actGameCamera_, &QAction::triggered, this,
            &MainWindow::onActionGameCamera);
    connect(actEditorCamera_, &QAction::triggered, this,
            &MainWindow::onActionEditorCamera);
}

void MainWindow::buildViewToolbar()
{
    QToolBar *bar = addToolBar(tr("View"));
    bar->setObjectName(QStringLiteral("ViewToolbar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(22, 22));
    bar->setStyleSheet(
        QStringLiteral("QToolButton:enabled:hover, QToolButton:enabled:checked "
                       "{ background: rgba(0, 96, 170, 35); border-radius: 4px; }"));

    bar->addSeparator();
    bar->addAction(actGrid_);
    bar->addAction(actDimGeo_);
    // Editable grid cell size (map cells): the steps of the grid overlay in
    // the 2D lines and the 3D surface dots. Defaults to Terra's kmGrid (4).
    gridStepWSpin_ = new QSpinBox(bar);
    gridStepWSpin_->setRange(1, 1024);
    gridStepWSpin_->setValue(128);
    gridStepWSpin_->setSuffix(tr(" c"));
    gridStepWSpin_->setToolTip(
        tr("Grid cell width in map cells (the horizontal line spacing of "
           "the grid overlay, 2D and 3D)."));
    gridStepHSpin_ = new QSpinBox(bar);
    gridStepHSpin_->setRange(1, 1024);
    gridStepHSpin_->setValue(128);
    gridStepHSpin_->setSuffix(tr(" c"));
    gridStepHSpin_->setToolTip(
        tr("Grid cell height in map cells (the vertical line spacing of "
           "the grid overlay, 2D and 3D)."));
    const auto pushGridStep = [this](int) {
        if (viewport_) {
            viewport_->setGridStep(gridStepWSpin_->value(),
                                   gridStepHSpin_->value());
        }
    };
    connect(gridStepWSpin_, &QSpinBox::valueChanged, this, pushGridStep);
    connect(gridStepHSpin_, &QSpinBox::valueChanged, this, pushGridStep);
    bar->addWidget(new QLabel(tr("Grid W:"), bar));
    bar->addWidget(gridStepWSpin_);
    bar->addWidget(new QLabel(tr("Grid H:"), bar));
    bar->addWidget(gridStepHSpin_);
    bar->addAction(actZoomToFit_);
    bar->addSeparator();
    bar->addAction(actView3D_);
    bar->addAction(actFreezeDam_);
    bar->addAction(actChunkyRender_);
    bar->addAction(actSmoothRender_);
    bar->addSeparator();
    bar->addAction(actRotateLeft_);
    bar->addAction(actRotateRight_);
    // Absolute camera rotation (azimuth) of the 3D view, degrees — the same
    // knob the rotate buttons and the orbit drag move. Wraps across the
    // 360/0 edge like the buttons do; follows the active camera model (the
    // editor turnAngleDeg or the game camera psi, see ViewportWidget).
    camRotationSpin_ = new QDoubleSpinBox(bar);
    camRotationSpin_->setObjectName(QStringLiteral("camRotationSpin"));
    camRotationSpin_->setRange(0.0, 360.0);
    camRotationSpin_->setDecimals(1);
    camRotationSpin_->setSingleStep(5.0);
    camRotationSpin_->setWrapping(true);
    camRotationSpin_->setSuffix(tr("\u00B0"));
    camRotationSpin_->setToolTip(
        tr("3D camera rotation (azimuth) in degrees. Edits the same camera "
           "rotation as the rotate buttons and the Alt+left orbit drag; "
           "wraps 360\u00B0 \u2192 0\u00B0."));
    connect(camRotationSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) {
                if (viewport_) {
                    viewport_->setRotationDeg(value);
                }
            });
    bar->addWidget(new QLabel(tr("Turn:"), bar));
    bar->addWidget(camRotationSpin_);
    bar->addAction(actTiltUp_);
    bar->addAction(actTiltDown_);
    bar->addSeparator();
    bar->addAction(actLiftUp_);
    bar->addAction(actLiftDown_);
    bar->addSeparator();
    bar->addAction(actZoomIn_);
    bar->addAction(actZoomOut_);
    bar->addAction(actResetView_);
    bar->addSeparator();
    bar->addAction(actDiffMode_);

    // The T3.1 brush editing toggle moved to the dedicated Brush toolbar
    // (T3.2), where the brush type/mode buttons live next to it.

    // A compact live readout of the active camera.
    viewReadout_ = new QLabel(bar);
    viewReadout_->setObjectName(QStringLiteral("viewReadout"));
    viewReadout_->setStyleSheet(
        QStringLiteral("QLabel { color: #303030; padding: 0 10px; }"));
    // Spacing for the monospace-ish look without a fixed width.
    QFont font = viewReadout_->font();
    font.setPointSizeF(8.5);
    viewReadout_->setFont(font);
    bar->addWidget(viewReadout_);
}

// ---------------------------------------------------------------------------
// Brush actions — one creator per UI section, split out of the former
// 600-line createBrushActions monolith (see the code review in
// docs/architecture.md).
// ---------------------------------------------------------------------------

void MainWindow::createBrushActions()
{
    brushUI_->createActions();
    // T4.2 — the editor interaction modes stay with the window (the Edit
    // menu, updateActionStates and the autotest hooks keep them); the Brush
    // toolbar only embeds them.
    createEditorModeActions();
    brushUI_->setEditorModeActions(actModeView_, actModeBrush_,
                                   actModeEyedropper_, actModePlaceSpawn_);
}

void MainWindow::createEditorModeActions()
{
    using EditorMode = MapEditor::ViewportWidget::EditorMode;
    // --- Editor interaction mode: View / Brush / Eyedropper -------------
    // What a click in the world does (T4.2), in both the 2D and the 3D view:
    //   View  — full camera control (left drag pans/orbits);
    //   Brush — the click applies the brush (camera: pan/zoom, Alt+left orbit
    //           in the 3D view);
    //   Eyedropper — the click samples the Geo/Dam pixel through the PickColor
    //           brush type (same limited camera as Brush).
    QActionGroup *interactionGroup = new QActionGroup(this);
    const auto addEditorMode = [this, interactionGroup](
            QIcon icon, const QString &text, const QString &toolTip,
            const QKeySequence &shortcut, EditorMode mode) {
        QAction *action = new QAction(icon, text, this);
        action->setToolTip(toolTip);
        action->setCheckable(true);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, [this, mode] {
            if (!viewport_) {
                return;
            }
            // The Place Spawn mode needs a loaded battle map — there is no
            // spawn to place otherwise (updateActionStates also disables the
            // action; this guards the shortcut path).
            if (mode == EditorMode::PlaceSpawn && !battleMaps_->isLoaded()) {
                return;
            }
            // The brush type carries which tool the interaction works with:
            // entering the Eyedropper mode selects the PickColor type, and
            // returning to the Brush mode turns it back into an actually
            // painting color brush (SetBrush params note in BrushTool.h).
            MapEditor::BrushParams params = brushTool_->params();
            if (mode == EditorMode::Eyedropper) {
                params.effect = MapEditor::BrushEffect::PickColor;
                brushTool_->setParams(params);
            } else if (mode == EditorMode::Brush
                       && params.effect == MapEditor::BrushEffect::PickColor) {
                params.effect = MapEditor::BrushEffect::SetColor;
                brushTool_->setParams(params);
            }
            viewport_->setEditorMode(mode);
            brushUI_->syncControls();
        });
        interactionGroup->addAction(action);
        return action;
    };

    actModeView_ = addEditorMode(
        MapEditor::brushui::makeViewModeIcon(), tr("&View Mode"),
        tr("Navigate only: left drag pans the 2D view / orbits the 3D view, "
           "middle/right drag pans, wheel zooms (V). The brush does nothing."),
        QKeySequence(Qt::Key_V), EditorMode::View);
    actModeBrush_ = addEditorMode(
        MapEditor::brushui::makeBrushIcon(), tr("&Brush"),
        tr("Apply the brush with a left click/drag in the 2D and the 3D view "
           "(T). Camera changes stay limited: pan and zoom are always "
           "available, Alt+left still orbits in the 3D view."),
        QKeySequence(Qt::Key_T), EditorMode::Brush);
    actModeEyedropper_ = addEditorMode(
        MapEditor::brushui::makeEyedropperIcon(), tr("&Eyedropper"),
        tr("Sample the surface color instead of painting: a click/drag picks "
           "the Geo/Dam palette pixel under the cursor through the PickColor "
           "brush type (E). Same limited camera as the Brush mode."),
        QKeySequence(Qt::Key_E), EditorMode::Eyedropper);
    actModePlaceSpawn_ = addEditorMode(
        MapEditor::brushui::makePlaceSpawnIcon(), tr("Place &Spawn"),
        tr("Move the selected battle-map player's spawn to the clicked cell: "
           "frame, squad, units and camera relocate together, the height is "
           "corrected by the terrain difference, and the move is one undo "
           "step (S). Available with a battle map loaded — the world itself "
           "is not touched."),
        QKeySequence(Qt::Key_S), EditorMode::PlaceSpawn);
    actModeBrush_->setChecked(true);
}

void MainWindow::refreshPaletteRenderers()
{
    if (!viewport_) {
        return;
    }
    // Every pixel's color may have changed: the 2D cache is repainted as a
    // whole, and the 3D view re-seeds its mip pyramid from the same cache
    // (which invalidateAll() starts by marking the map fully dirty).
    if (MapEditor::ViewportRenderer *renderer = viewport_->renderer()) {
        renderer->invalidateAll();
    }
    viewport_->setNeeds3DRedraw();
    viewport_->update();
}

void MainWindow::buildStatusBar()
{
    statusBar()->showMessage(tr("Map editor"));
}

// ---------------------------------------------------------------------------
// Window title
// ---------------------------------------------------------------------------

void MainWindow::updateWindowTitle()
{
    if (session_ && session_->isWorldLoaded()) {
        setWindowTitle(tr("Perimeter Map Editor - %1")
                           .arg(QString::fromStdString(session_->currentWorldName())));
    } else {
        setWindowTitle(tr("Perimeter Map Editor"));
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onActionSelectGameDir()
{
    // Switching the game directory unloads the current world — offer to save.
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    const QString autoDetected = gameDirMgr_->autoDetect();

    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Game Directory"), autoDetected,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (dir.isEmpty()) {
        return;
    }

    if (!GameDirManager::isValidGameDir(dir)) {
        QMessageBox::warning(this, tr("Invalid Directory"),
            tr("The selected directory does not contain RESOURCE/Worlds/.\n"
               "Please select the Perimeter game root directory."));
        return;
    }

    if (!session_->setGameDir(dir.toStdString())) {
        QMessageBox::critical(this, tr("Error"),
            tr("Failed to set game directory: %1").arg(dir));
        return;
    }

    gameDirMgr_->saveGameDir(dir);
    // The nature model list comes from the game folder — rescan on switch.
    battleMaps_->invalidateNatureModels();
    // The session unloaded the world when the directory switched — drop the
    // viewport raster binding and the minimap image with it.
    onWorldUnload();

    // Scan the worlds right away so the status bar reflects the selected
    // directory immediately instead of only after a world is loaded
    // (File → Scan Worlds stays available as an explicit re-scan, e.g.
    // after new worlds appear in the folder).
    if (!session_->scanWorlds()) {
        QMessageBox::warning(this, tr("Scan Failed"),
            tr("The game directory was selected, but no worlds were found "
               "in RESOURCE/Worlds/."));
    }
    updateStatusBar();
}

void MainWindow::onActionScanWorlds()
{
    if (!session_->hasGameDir()) {
        QMessageBox::information(this, tr("No Game Directory"),
            tr("Please select a game directory first via File \u2192 Select Game Directory."));
        return;
    }

    if (!session_->scanWorlds()) {
        QMessageBox::critical(this, tr("Scan Failed"),
            tr("Failed to scan worlds in the game directory."));
        return;
    }

    updateStatusBar();
}

void MainWindow::showWorldListDialog()
{
    if (!session_->hasGameDir()) {
        return;
    }

    if (session_->worldCount() == 0) {
        if (!session_->scanWorlds()) {
            QMessageBox::critical(this, tr("Scan Failed"),
                tr("Failed to scan worlds. Try File \u2192 Select Game Directory first."));
            return;
        }
    }

    const std::vector<MapEditor::WorldInfo> &worlds = session_->getWorldList();
    if (worlds.empty()) {
        QMessageBox::information(this, tr("No Worlds"),
            tr("No worlds found. Make sure the game directory contains RESOURCE/Worlds/."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Select World"));
    dialog.resize(400, 300);

    auto *listWidget = new QListWidget(&dialog);
    for (const auto &w : worlds) {
        // Mod worlds are shown as "<Mod> / <World>" to tell them apart from
        // base game worlds (and from a same-named base world).
        listWidget->addItem(QString::fromStdString(w.displayName()));
    }

    const int curIdx = session_->currentWorldIndex();
    if (curIdx >= 0 && curIdx < listWidget->count()) {
        listWidget->setCurrentRow(curIdx);
    }

    auto *btnLoad   = new QPushButton(tr("Load"),   &dialog);
    auto *btnCancel = new QPushButton(tr("Cancel"), &dialog);

    connect(btnLoad,   &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(btnCancel, &QPushButton::clicked, &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(listWidget);

    auto *btnBox = new QHBoxLayout();
    btnBox->addWidget(btnLoad);
    btnBox->addWidget(btnCancel);
    layout->addLayout(btnBox);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const int row = listWidget->currentRow();
    if (row < 0 || row >= static_cast<int>(worlds.size())) {
        return;
    }

    if (!session_->loadWorld(static_cast<size_t>(row))) {
        QMessageBox::critical(this, tr("Load Failed"),
            tr("Failed to load world '%1'.")
                .arg(QString::fromStdString(worlds[static_cast<size_t>(row)].name)));
    }

    updateStatusBar();
}

void MainWindow::onActionLoadWorld()
{
    // Loading another world discards the current one — offer to save first.
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    showWorldListDialog();
    if (session_->isWorldLoaded()) {
        onWorldLoaded();
    }
}

void MainWindow::onActionUnloadWorld()
{
    // Unloading discards every change made since the last save — offer to
    // save first.
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    session_->unloadWorld();
    onWorldUnload();
}

void MainWindow::onActionSave()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    // Save writes the BASE terrain — difference data never merges into the
    // world (that is what File → Save Difference (.gmp) is for), even in
    // Difference Mode.
    if (!session_->saveWorld()) {
        QMessageBox::critical(this, tr("Save Failed"),
            tr("Failed to save world '%1'. See the log for details.")
                .arg(QString::fromStdString(session_->currentWorldName())));
        return;
    }
    // map.tga was regenerated by the save — keep the minimap image in sync.
    if (minimap_) {
        minimap_->reload();
    }
    updateStatusBar();
}

void MainWindow::onActionSaveAs()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString name = askSaveAsName();
    if (name.isEmpty()) {
        return; // cancelled by the user
    }
    if (!session_->saveWorldAs(name.toStdString())) {
        QMessageBox::critical(this, tr("Save As Failed"),
            tr("Failed to save world '%1' as '%2'.\n"
               "A world with that name may already exist in RESOURCE/Worlds/.")
                .arg(QString::fromStdString(session_->currentWorldName()), name));
        return;
    }
    // saveWorldAs() reloaded the session onto the new world — rebind the
    // viewport and refresh the world list-driven UI state.
    onWorldLoaded();
}

void MainWindow::onActionNewWorld()
{
    if (!session_->hasGameDir() || session_->worldCount() == 0) {
        QMessageBox::warning(this, tr("New World"),
                             tr("Select a game directory with at least one "
                                "world first (it donates the palettes and "
                                "textures for the new world)."));
        return;
    }
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    NewWorldWizard wizard(this, session_->getWorldList(),
                          session_->getGameDir());
    if (wizard.exec() != QDialog::Accepted) {
        return;
    }
    const MapEditor::WorldCreateResult result = MapEditor::createNewWorld(
        *session_, wizard.wizardParams(),
        wizard.templateWorldName().toStdString());
    if (!result.ok) {
        QMessageBox::critical(this, tr("New World"),
                              tr("Failed to create the world:\n%1")
                                  .arg(QString::fromStdString(result.error)));
        return;
    }
    // createNewWorld loaded and saved the new world — rebind the viewport.
    onWorldLoaded();
}

void MainWindow::onActionWorldParams()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const std::optional<MapEditor::WorldParams> current =
        session_->currentWorldParams();
    if (!current) {
        QMessageBox::warning(this, tr("World Parameters"),
                             tr("Cannot read the parameters of the loaded world."));
        return;
    }
    WorldSettingsDialog dialog(this, WorldSettingsPanel::Mode::ExistingWorld,
                               *current);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (!session_->applyWorldSettings(dialog.params())) {
        QMessageBox::warning(this, tr("World Parameters"),
                             tr("Some parameters could not be written "
                                "(see the log for details)."));
    }
    // hZeroPlast drives the 3D grid reference plane; the 2D raster is
    // unaffected (palette colors did not change).
    if (viewport_) {
        viewport_->setNeeds3DRedraw();
        viewport_->update();
    }
    updateStatusBar();
}

// ---------------------------------------------------------------------------
// T6 — image import/export: thin wrappers over ImageIoController (the flows
// moved there — docs/architecture.md §5.37).
// ---------------------------------------------------------------------------

void MainWindow::onActionImportMapImage()
{
    imageIO_->importMapImage();
}

void MainWindow::onActionImportUpTga()
{
    imageIO_->importUpTga();
}

void MainWindow::onActionImportLeveledTexture()
{
    imageIO_->importLeveledTexture();
}

void MainWindow::onActionExportHeightmap()
{
    imageIO_->exportHeightmap();
}

void MainWindow::onActionExportTopView()
{
    imageIO_->exportTopView();
}

// ---------------------------------------------------------------------------
// T9 — validation + autofixes.
// ---------------------------------------------------------------------------

void MainWindow::buildToolsMenu()
{
    QMenu *toolsMenu = menuBar()->addMenu(tr("&Tools"));

    QAction *actRunScript = toolsMenu->addAction(tr("&Run Script..."));
    actRunScript->setToolTip(tr("Write and run a JavaScript with the editor "
                                "brush API (the same functions the scripted "
                                "brush runs) — one run is one undo step"));
    actRunScript->setEnabled(false);
    connect(actRunScript, &QAction::triggered, this,
            &MainWindow::onActionRunScript);
    actRunScript_ = actRunScript;

    toolsMenu->addSeparator();

    QAction *actValidateWorld = toolsMenu->addAction(tr("&Validate World..."));
    actValidateWorld->setToolTip(tr("Files, sizes, palettes, textures and the volume balance of the loaded world"));
    actValidateWorld->setEnabled(false);
    connect(actValidateWorld, &QAction::triggered, this, &MainWindow::onActionValidateWorld);
    actValidateWorld_ = actValidateWorld;

    QAction *actValidateMap = toolsMenu->addAction(tr("Validate &Battle Map..."));
    actValidateMap->setToolTip(tr("World reference, spawn bounds, cameras, shuffling indices of the loaded battle map"));
    actValidateMap->setEnabled(false);
    connect(actValidateMap, &QAction::triggered, this, &MainWindow::onActionValidateBattleMap);
    actValidateBattleMap_ = actValidateMap;

    toolsMenu->addSeparator();

    QAction *actFixShuffling = toolsMenu->addAction(tr("Autofix: &Normalize Shuffling Indices"));
    actFixShuffling->setEnabled(false);
    connect(actFixShuffling, &QAction::triggered, this, &MainWindow::onActionAutofixShuffling);
    actFixShuffling_ = actFixShuffling;

    QAction *actFixZP = toolsMenu->addAction(tr("Autofix: Set zeroLayerHeight from &World"));
    actFixZP->setEnabled(false);
    connect(actFixZP, &QAction::triggered, this, &MainWindow::onActionAutofixZeroLayerHeight);
    actFixZeroLayer_ = actFixZP;

    QAction *actRegenImages = toolsMenu->addAction(tr("Autofix: &Regenerate cache.tga / map.tga"));
    actRegenImages->setEnabled(false);
    connect(actRegenImages, &QAction::triggered, this, &MainWindow::onActionRegenerateImages);
    actRegenerateImages_ = actRegenImages;
}

void MainWindow::showValidationReport(const QString &title,
                                      const MapEditor::ValidationReport &report)
{
    MapEditor::ValidationDialog dialog(this, title, report);
    if (dialog.exec() == QDialog::Accepted) {
        int x = -1;
        int y = -1;
        dialog.currentIssueCell(x, y);
        if (x >= 0 && y >= 0 && viewport_) {
            viewport_->centerOnCell(QPoint(x, y));
            viewport_->update();
        }
    }
}

// ---------------------------------------------------------------------------
// T8 — battle maps (.spg): thin wrappers over BattleMapController (the whole
// domain moved there — docs/architecture.md §5.35).
// ---------------------------------------------------------------------------

void MainWindow::onActionOpenBattleMap()
{
    battleMaps_->openBattleMap();
}

void MainWindow::onActionSaveBattleMap()
{
    battleMaps_->saveBattleMap();
}

void MainWindow::onActionSaveBattleMapAs()
{
    battleMaps_->saveBattleMapAs();
}

void MainWindow::onActionNewBattleMap()
{
    battleMaps_->newBattleMap();
}

void MainWindow::onActionSaveDiff()
{
    battleMaps_->saveDiffData();
}

void MainWindow::onActionLoadDiff()
{
    battleMaps_->loadDiffData();
}

void MainWindow::onActionValidateWorld()
{
    const auto files = session_ ? session_->currentWorldFiles() : std::nullopt;
    if (!files || !session_->isWorldLoaded()) {
        return;
    }
    MapEditor::ValidationReport report =
        MapEditor::validateWorldFiles(*files);
    long long fill = 0;
    long long dig = 0;
    report.issues.push_back(
        MapEditor::validateVolumeBalance(MapEditor::VolumeBalanceOptions{},
                                         fill, dig));
    showValidationReport(
        tr("World Validation: %1")
            .arg(QString::fromStdString(session_->currentWorldName())),
        report);
}

void MainWindow::onActionValidateBattleMap()
{
    if (!battleMaps_->isLoaded() || !session_ || !session_->hasGameDir()) {
        return;
    }
    const fs::path worldsRoot =
        fs::u8path(session_->getGameDir()) / "RESOURCE" / "Worlds";
    showValidationReport(
        tr("Battle Map Validation"),
        MapEditor::validateBattleMap(battleMaps_->spgFile(), worldsRoot));
}

void MainWindow::onActionAutofixShuffling()
{
    if (!battleMaps_->isLoaded()) {
        return;
    }
    if (MapEditor::autofixShufflingIndices(battleMaps_->spgFile())) {
        battleMaps_->refreshUI();
        statusBar()->showMessage(
            tr("playersShufflingIndices normalized."), 5000);
    }
}

void MainWindow::onActionAutofixZeroLayerHeight()
{
    if (!battleMaps_->isLoaded() || !session_->isWorldLoaded()) {
        return;
    }
    if (MapEditor::autofixZeroLayerHeight(battleMaps_->spgFile(),
                                          session_->zeroLayerHeight())) {
        battleMaps_->refreshUI();
        statusBar()->showMessage(
            tr("zeroLayerHeight set to the world hZeroPlast (%1).")
                .arg(session_->zeroLayerHeight()),
            5000);
    }
}

void MainWindow::onActionRegenerateImages()
{
    if (!session_ || !session_->isWorldLoaded()) {
        return;
    }
    const bool ok = session_->regenerateWorldImages();
    statusBar()->showMessage(
        ok ? tr("cache.tga / map.tga regenerated.")
           : tr("Image regeneration failed."),
        5000);
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

void MainWindow::buildHelpMenu()
{
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *actHelp = helpMenu->addAction(tr("Map Editor &Help..."));
    actHelp->setToolTip(tr("The editor manual (create a skirmish map, brushes, view, images...)"));
    actHelp->setShortcut(QKeySequence(Qt::Key_F1));
    connect(actHelp, &QAction::triggered, this, &MainWindow::onActionHelp);
}

void MainWindow::onActionHelp()
{
    // Non-modal: the manual stays open while the editor is being used.
    MapEditor::HelpWindow::openHelp(this);
}

void MainWindow::refreshAfterScriptPaletteEdit()
{
    // Every pixel may have changed colour: full 2D cache repaint + 3D redraw.
    refreshPaletteRenderers();
    if (palettePanel_) {
        palettePanel_->refresh();
    }
}

void MainWindow::onActionRunScript()
{
    if (!session_ || !session_->isWorldLoaded()) {
        statusBar()->showMessage(
            tr("Load a world first — the script works on the loaded world."),
            5000);
        return;
    }
    if (runScript_.isEmpty()) {
        // A starter example the first time the window opens.
        runScript_ = QStringLiteral(
            "// Run Script: runs once with the editor brush API.\n"
            "// getCurrentPos() reports the cursor cell (map centre off-cursor).\n"
            "//   raiseHeight(pos, height, radius, square) / lowerHeight(...)\n"
            "//   setHeight(pos, height, radius, square) / setToZP(pos, r, sq)\n"
            "//   paintSoot(pos, r, sq) / paintVeryLightDam(pos, r, sq)\n"
            "//   paintColor(pos, red, green, blue, radius, square)\n"
            "//   applyBlur(pos, radius, blurRadius, effect, square) / applySmooth(pos, radius, effect, sq)\n"
            "//   setGeoPalette(index, {r, g, b}) / setDamPalette(index, {r, g, b})\n"
            "//   getDamHeight(pos) / getGeoColor(pos) / getDamColor(pos)\n"
            "\n"
            "var p = getCurrentPos();\n"
            "// Example: a red Geo palette entry and a pad at the cursor.\n"
            "setGeoPalette(200, {r: 220, g: 40, b: 40});\n"
            "setHeight({x: p.x, y: p.y}, 30, 8, false);\n");
    }
    MapEditor::RunScriptDialog dialog(this, runScript_);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    runScript_ = dialog.script();

    // No stamp: the API's "current position" is the cursor cell, falling
    // back to the map centre.
    int cx = static_cast<int>(session_->mapWidth()) / 2;
    int cy = static_cast<int>(session_->mapHeight()) / 2;
    if (viewport_) {
        const QPoint cell = viewport_->cursorCell();
        if (cell.x() >= 0 && cell.y() >= 0) {
            cx = cell.x();
            cy = cell.y();
        }
    }

    std::string error;
    if (!MapEditor::runEditorScript(*session_, *brushTool_,
                                    brushTool_->params(), cx, cy,
                                    runScript_.toStdString(), error)) {
        QMessageBox::critical(this, tr("Run Script"),
                              QString::fromStdString(error));
        return;
    }
    // A script may have edited the terrain (dirty rects repaint) and/or the
    // palettes (every pixel re-tints) — refresh both views fully.
    refreshPaletteRenderers();
    updateStatusBar();
    statusBar()->showMessage(tr("Script executed (one undo step)."), 4000);
}

void MainWindow::onActionUndo()
{
    bool undone = session_->undo();
    if (!undone && battleMaps_->canUndoPlayerMove() &&
        battleMaps_->undoPlayerMove()) {
        // A spawn relocation (Place Spawn mode) — BattleMapController::
        // undoPlayerMove() already refreshed the panel list and the
        // viewport overlay (they read the anchors).
        undone = true;
    }
    if (undone) {
        // regRender() marked the affected area; a repaint drains the dirty
        // rectangles and refreshes the raster.
        if (viewport_) {
            viewport_->setNeeds3DRedraw();
            viewport_->update();
        }
        updateStatusBar();
        updateActionStates();
    }
}

void MainWindow::onActionRedo()
{
    bool redone = session_->redo();
    if (!redone && battleMaps_->canRedoPlayerMove() &&
        battleMaps_->redoPlayerMove()) {
        redone = true;
    }
    if (redone) {
        if (viewport_) {
            viewport_->setNeeds3DRedraw();
            viewport_->update();
        }
        updateStatusBar();
        updateActionStates();
    }
}

void MainWindow::onActionClearUndoHistory()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    session_->clearUndoHistory();
    updateStatusBar();
}

// ---------------------------------------------------------------------------
// Unsaved changes / Save As helpers (T1.4)
// ---------------------------------------------------------------------------

bool MainWindow::confirmDiscardUnsavedChanges()
{
    if (session_ == nullptr) {
        return true;
    }
    if (session_->hasUnsavedChanges()) {
        const QString message = QString::fromStdString(
            session_->getUnsavedChangesWarning());
        const QMessageBox::StandardButton chosen = QMessageBox::question(
            this, tr("Unsaved Changes"), message,
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        if (chosen == QMessageBox::Cancel) {
            return false;
        }
        if (chosen == QMessageBox::Save) {
            if (!session_->saveWorld()) {
                QMessageBox::critical(this, tr("Save Failed"),
                    tr("Failed to save world '%1'. See the log for details.")
                        .arg(QString::fromStdString(session_->currentWorldName())));
                return false;
            }
        }
    }

    // Unsaved DIFFERENCE data is saved to the .gmp — never merged into the
    // world from this prompt.
    if (session_->hasUnsavedDiffChanges()) {
        const QMessageBox::StandardButton chosen = QMessageBox::question(
            this, tr("Unsaved Difference Data"),
            tr("The terrain difference data (.gmp) has unsaved changes.\n"
               "Save them now?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (chosen == QMessageBox::Cancel) {
            return false;
        }
        if (chosen == QMessageBox::Save) {
            onActionSaveDiff();
            if (session_->hasUnsavedDiffChanges()) {
                // The save failed or was cancelled — do not close.
                return false;
            }
        }
    }
    return true;
}

QString MainWindow::askSaveAsName()
{
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Save World As"));
    dialog.setLabelText(
        tr("Name of the new world folder inside RESOURCE/Worlds/:"));
    QString defaultName = QString::fromStdString(session_->currentWorldName());
    defaultName += QStringLiteral("_copy");
    dialog.setTextValue(defaultName);

    for (;;) {
        if (dialog.exec() != QDialog::Accepted) {
            return QString();
        }
        const QString name = dialog.textValue().trimmed();

        if (name.isEmpty()) {
            QMessageBox::warning(this, tr("Invalid Name"),
                tr("The world name must not be empty."));
            continue;
        }
        if (name.contains('/') || name.contains('\\') ||
            name == QStringLiteral(".") || name == QStringLiteral("..")) {
            QMessageBox::warning(this, tr("Invalid Name"),
                tr("The world name must not be a path — only the folder name "
                   "of the new world."));
            continue;
        }
        if (dialog.textValue() != name) {
            // Whitespace was trimmed; show the cleaned value so the user sees
            // what will actually be created before it is written.
            dialog.setTextValue(name);
        }
        return name;
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscardUnsavedChanges()) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

// ---------------------------------------------------------------------------
// Actions state
// ---------------------------------------------------------------------------

void MainWindow::updateActionStates()
{
    const bool worldLoaded = session_ != nullptr && session_->isWorldLoaded();
    // T8/T9 — battle map and validation actions: opening a battle map needs
    // a game directory only (its world may be loaded later or not at all);
    // the battle-map tools need a loaded .spg, the world-dependent ones both.
    const bool gameDirSet = session_ != nullptr && session_->hasGameDir();
    // battleMaps_ is created in buildViewport (after the toolbars) — the
    // toolbar spinboxes fire valueChanged during construction and reach
    // here through BrushUiController::syncControls before the controller
    // exists.
    const bool battleMapLoaded = battleMaps_ && battleMaps_->isLoaded();

    // The bulk of the actions is gated on a single session predicate —
    // group them into tables instead of an if-chain per action (the
    // brush-action table lives in the BrushUiController).
    const auto setEnabled = [](QAction *action, bool enabled) {
        if (action) {
            action->setEnabled(enabled);
        }
    };
    for (QAction *action : {actSave_, actSaveAs_, actWorldParams_,
                            actValidateWorld_, actRegenerateImages_, actRunScript_,
                            actSaveDiff_, actLoadDiff_,
                            importMapImageAction_, importUpTgaAction_,
                            importLeveledAction_, exportHeightmapAction_,
                            exportTopViewAction_, actClearUndoHistory_}) {
        setEnabled(action, worldLoaded);
    }
    // View actions make sense only when there is something to look at.
    for (QAction *action : {actZoomToFit_, actView3D_, actDimGeo_,
                            actFreezeDam_, actRotateLeft_, actRotateRight_,
                            actTiltUp_, actTiltDown_, actLiftUp_, actLiftDown_,
                            actZoomIn_, actZoomOut_, actResetView_}) {
        setEnabled(action, worldLoaded);
    }
    // Battle-map tools need a loaded .spg.
    for (QAction *action : {actSaveBattleMapAs_, actValidateBattleMap_,
                            actFixShuffling_}) {
        setEnabled(action, battleMapLoaded);
    }
    setEnabled(actOpenBattleMap_, gameDirSet);
    // Saving writes to the loaded map's own file — a freshly created map
    // (new_battle_map) has none until "Save Battle Map As".
    setEnabled(actSaveBattleMap_, battleMapLoaded &&
                                      !battleMaps_->path().isEmpty());
    // Creating a battle map builds it for the currently loaded world.
    setEnabled(actNewBattleMap_, gameDirSet && worldLoaded);
    setEnabled(actFixZeroLayer_, battleMapLoaded && worldLoaded);
    // Difference mode (.gmp): the toggle and the gmp save/load need a loaded
    // world; the checked state mirrors the session.
    if (actDiffMode_) {
        actDiffMode_->setEnabled(worldLoaded);
        if (actDiffMode_->isChecked() != session_->isDiffMode()) {
            actDiffMode_->blockSignals(true);
            actDiffMode_->setChecked(session_->isDiffMode());
            actDiffMode_->blockSignals(false);
        }
    }
    if (actModePlaceSpawn_) {
        actModePlaceSpawn_->setEnabled(battleMapLoaded);
        // The mode is meaningless without a battle map — drop back to the
        // Brush mode when the map is closed while the mode is active.
        if (!battleMapLoaded && viewport_ &&
            viewport_->editorMode() ==
                MapEditor::ViewportWidget::EditorMode::PlaceSpawn) {
            viewport_->setEditorMode(MapEditor::ViewportWidget::EditorMode::Brush);
        }
    }
    // Terrain history first, then the battle-map spawn-move history
    // (both are reverted by the same Ctrl+Z). battleMaps_ appears during
    // buildViewport — before that there is no spawn history either.
    const bool canUndoSpawnMove = battleMapLoaded && battleMaps_->canUndoPlayerMove();
    const bool canRedoSpawnMove = battleMapLoaded && battleMaps_->canRedoPlayerMove();
    if (actUndo_) {
        actUndo_->setEnabled((worldLoaded && session_->canUndo()) ||
                             canUndoSpawnMove);
    }
    if (actRedo_) {
        actRedo_->setEnabled((worldLoaded && session_->canRedo()) ||
                             canRedoSpawnMove);
    }
    // The grid cell size drives the grid overlay — needs a world too.
    if (gridStepWSpin_) {
        gridStepWSpin_->setEnabled(worldLoaded);
    }
    if (gridStepHSpin_) {
        gridStepHSpin_->setEnabled(worldLoaded);
    }
    // T3.1/T4.2 — the editor interaction mode: navigation works everywhere,
    // painting and picking need a world.
    setEnabled(actModeView_, true);
    setEnabled(actModeBrush_, worldLoaded);
    setEnabled(actModeEyedropper_, worldLoaded);
    // T3.x — every brush action and spinbox follows the tool parameters
    // (Dam-only types, the decal/script/sculpt/3D/model exclusions, the
    // gate/polygon/shape predicates) — the whole block lives in the
    // BrushUiController.
    brushUI_->updateEnabledStates(worldLoaded);
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void MainWindow::updateStatusBar()
{
    QString text;

    switch (session_->state()) {
    case MapEditor::SessionState::NoGame:
        // The session stays in NoGame both before any directory is picked
        // and when the picked directory has no worlds — phrase accordingly.
        text = session_->hasGameDir()
            ? tr("Game directory selected, but no worlds found "
                 "(File \u2192 Scan Worlds).")
            : tr("No game directory selected.");
        break;
    case MapEditor::SessionState::GameReady:
        text = tr("Game ready \u2014 %1 world(s) found.").arg(
            static_cast<int>(session_->worldCount()));
        break;
    case MapEditor::SessionState::WorldLoaded:
        text = tr("World: %1%2").arg(
            QString::fromStdString(session_->currentWorldName()),
            session_->isModified() ? QStringLiteral(" [modified]") : QString());
        break;
    }

    statusBar()->showMessage(text);

    // Keep the Edit/File actions in sync with the session state.
    updateActionStates();
}

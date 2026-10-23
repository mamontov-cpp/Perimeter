#pragma once

#include <QWidget>
#include <unordered_map>
#include <utility>
#include <vector>

class QLabel;
class QPushButton;

namespace MapEditor {

class EditorSession;

// ============================================================================
// MapEditor UI — PalettePanel (T4.1, T4.4).
//
// The palette panel of the brush color model: two grids of colored swatches,
// one per material layer (SurBuf values index the palette of the cell's layer
// directly — EditorSession::paletteRgb for the layout details) —
//
//   * Geo  — vrtMap::GeoPal (the inGeo.act / geoPal.xml palette), entries
//            0..255, plus the custom (extended) entries below them;
//   * Dam  — vrtMap::DamPal (the inDam.act palette), entries 0..255, plus
//            the custom (extended) entries below them.
//
// Custom entries (T4.4) live in the extended palette of the world sidecar;
// their indices are global (>=256) and each grid has an "Add custom color…"
// button that appends one.
//
// Interactions:
//   * single click selects a swatch and emits colorSelected(index, dam)
//     (MainWindow feeds it into the brush colorIndex and switches the brush
//     to that layer, so the swatch, the cursor readout and the paint all
//     resolve the index through the same palette);
//   * double click opens the color dialog and writes the new RGB through
//     EditorSession::setPaletteRgb()/applyPalettes() (base entries) or
//     setExtendedPaletteRgb() (custom entries), emitting paletteEdited()
//     (the caller then has to refresh the renderers — the palette re-tints
//     every surface pixel);
//   * "Add custom color…" appends a new custom entry through
//     EditorSession::addPaletteColor().
//
// The widget never touches Terra itself: palette bytes go through
// EditorSession (Qt-free helpers).
// ============================================================================

class PalettePanel : public QWidget
{
    Q_OBJECT

public:
    explicit PalettePanel(QWidget *parent = nullptr);

    /// Bind the session and rebuild the swatch grids for the loaded world.
    /// Safe to call when no game directory/world is available — the panel
    /// then shows a hint and disables the grids.
    void setSession(EditorSession *session);

    /// Re-read the palette bytes from the session (after a palette edit that
    /// changed colors elsewhere, or after a world reload).
    void refresh();

    /// Highlight the given palette entry as the current one (feeds back the
    /// eyedropper picks so the panel always shows the color the brush will
    /// paint; `dam` selects the grid).
    void selectIndex(int paletteIndex, bool dam);
    int selectedIndex() const { return selectedIndex_; }
    bool selectedIsDam() const { return selectedIsDam_; }

signals:
    /// Emitted when the user single-clicks a swatch (`dam` — the swatch's
    /// palette layer).
    void colorSelected(int paletteIndex, bool dam);
    /// Emitted after the session changed a palette entry through the color
    /// dialog. Consumers should rebuild the renderer caches; `paletteIndex`
    /// is the entry that was edited in palette `dam`.
    void paletteEdited(int paletteIndex, bool dam);

protected:
    /// Swatch clicks/double-clicks are routed through a shared event filter.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// Rebuild the whole grid contents (base entries + T4.4 custom entries +
    /// "Add custom color…" buttons) from the session state. Wholesale: every
    /// palette extension changes the swatch count.
    void rebuildGrids();
    /// True when the built grid layout no longer matches the session's
    /// extended entry counts (needs rebuildGrids).
    bool gridCountsStale() const;
    /// Push current palette colors/labels/tooltips into the swatches.
    void updateSwatches();
    /// Apply the selection outline to the swatch of `index` (and drop it
    /// from the previously selected one).
    void applySelectionStyle(int index, bool dam);
    /// Open the color dialog for the given entry and write the result into
    /// the session, emitting paletteEdited(). Base entries go through
    /// setPaletteRgb()+applyPalettes(), custom entries (>=256) through
    /// setExtendedPaletteRgb().
    void editIndex(int paletteIndex, bool dam);
    /// T4.4 — append a new custom entry to the layer's palette via
    /// EditorSession::addPaletteColor(), select it and make it the brush
    /// color. Shows a message when the 16-bit index space is exhausted.
    void addCustomColor(bool dam);

    bool hasPalette() const;

    EditorSession *session_ = nullptr;

    /// Shown instead of both grids when no world is loaded.
    QLabel *hint_ = nullptr;
    QWidget *gridsHost_ = nullptr;
    /// The "Add custom color…" buttons (one per layer), kept for enabling.
    QPushButton *addGeoButton_ = nullptr;
    QPushButton *addDamButton_ = nullptr;

    /// The swatch widgets in build order (256 base per layer + custom).
    /// The assigned palette entry is also stored as the "paletteIndex"
    /// property of every swatch; "paletteDam" is 1 for the dam grid.
    std::vector<QWidget *> swatches_;
    /// entry/dam -> swatch lookup (key = entry*2 + (dam?1:0); base geo keys
    /// 0..510, base dam 1..511, custom keys >= 512 — collision-free).
    std::unordered_map<int, QWidget *> swatchByKey_;
    /// The custom-entry counts the current grid layout was built with.
    int builtGeoCustom_ = 0;
    int builtDamCustom_ = 0;

    int selectedIndex_ = -1;
    bool selectedIsDam_ = false;
};

} // namespace MapEditor

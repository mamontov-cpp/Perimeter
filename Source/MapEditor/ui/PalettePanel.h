#pragma once

#include <QWidget>
#include <vector>

class QLabel;

namespace MapEditor {

class EditorSession;

// ============================================================================
// MapEditor UI — PalettePanel (T4.1).
//
// The palette panel of the brush color model: two grids of colored swatches,
// one per material layer (SurBuf values index the palette of the cell's layer
// directly — EditorSession::paletteRgb for the layout details) —
//
//   * Geo  — vrtMap::GeoPal (the inGeo.act / geoPal.xml palette), entries
//            0..255;
//   * Dam  — vrtMap::DamPal (the inDam.act palette), entries 0..255.
//
// Interactions:
//   * single click selects a swatch and emits colorSelected(index, dam)
//     (MainWindow feeds it into the brush colorIndex and switches the brush
//     to that layer, so the swatch, the cursor readout and the paint all
//     resolve the index through the same palette);
//   * double click opens the color dialog and writes the new RGB through
//     EditorSession::setPaletteRgb()+applyPalettes(), emitting paletteEdited()
//     (the caller then has to refresh the renderers — the palette re-tints
//     every surface pixel).
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
    /// Push current palette colors/labels/tooltips into the swatches.
    void updateSwatches();
    /// Apply the selection outline to the swatch of `index` (and drop it
    /// from the previously selected one).
    void applySelectionStyle(int index, bool dam);
    /// Open the color dialog for the given entry and write the result into
    /// the session (setPaletteRgb + applyPalettes), emitting paletteEdited().
    void editIndex(int paletteIndex, bool dam);

    bool hasPalette() const;

    EditorSession *session_ = nullptr;

    /// Shown instead of both grids when no world is loaded.
    QLabel *hint_ = nullptr;
    QWidget *gridsHost_ = nullptr;

    /// The swatch widgets: [0..255] geo entries, [256..511] dam entries.
    /// The assigned palette entry is also stored as the "paletteIndex"
    /// property of every swatch; "paletteDam" is 1 for the dam grid.
    std::vector<QWidget *> swatches_;

    int selectedIndex_ = -1;
    bool selectedIsDam_ = false;
};

} // namespace MapEditor

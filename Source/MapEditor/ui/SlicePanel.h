#pragma once

#include <QWidget>

class QCheckBox;
class QPushButton;
class QSpinBox;

namespace MapEditor {

class EditorSession;

/// ===========================================================================
/// MapEditor UI — the "Slice" panel: show colors at a height.
///
/// When enabled and APPLIED, the map renders clipped at the chosen height —
/// every cell shows the color of the voxel layer at that height (voxels
/// above the slice are not rendered; the appearance of the layer is
/// projected onto the surface). Changing the controls only marks the change
/// pending: the apply button runs the (expensive) full-map re-render, so the
/// 3D view is not rebuilt on every spinbox step. The optional "paint at this
/// height" checkbox makes the 2D brushes paint at the slice height (to the
/// brush paint depth); it takes effect immediately (it only sets the brush
/// paint height).
/// ===========================================================================
class SlicePanel : public QWidget
{
    Q_OBJECT

public:
    explicit SlicePanel(QWidget* parent = nullptr);

    void setSession(EditorSession* session);

    bool sliceEnabled() const;
    int sliceHeight() const;
    /// The 2D brushes paint at the slice height (only meaningful while the
    /// slice is enabled).
    bool paintAtSliceHeight() const;

signals:
    /// Emitted on any control change (the pending state may have changed).
    void sliceChanged();
    /// Emitted when paintAtSliceHeight toggles on and on Apply while
    /// painting-at-slice is active.
    void paintAtSliceRequested(int height);

private:
    /// Runs the expensive part: commits the slice height to the session
    /// (full-map re-render) and pushes the brush paint height.
    void applySlice();
    /// Enables/disables the sub-controls and greys the Apply button out
    /// while the panel state matches the applied one.
    void refreshState();

    EditorSession* session_ = nullptr;
    QCheckBox* enable_ = nullptr;
    QSpinBox* height_ = nullptr;
    QCheckBox* paintAt_ = nullptr;
    QPushButton* apply_ = nullptr;
};

} // namespace MapEditor

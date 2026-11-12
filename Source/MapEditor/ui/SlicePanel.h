#pragma once

#include <QWidget>

class QCheckBox;
class QSpinBox;

namespace MapEditor {

class EditorSession;

/// ===========================================================================
/// MapEditor UI — the "Slice" panel: show colors at a height.
///
/// When enabled, the map renders clipped at the chosen height — every cell
/// shows the color of the voxel layer at that height (voxels above the slice
/// are not rendered; the appearance of the layer is projected onto the
/// surface). The optional "paint at this height" checkbox makes the 2D
/// brushes paint at the slice height (to the brush paint depth).
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
    /// Emitted on any control change.
    void sliceChanged();
    /// Emitted when paintAtSliceHeight toggles or the height changes while
    /// painting-at-slice is active.
    void paintAtSliceRequested(int height);

private:
    EditorSession* session_ = nullptr;
    QCheckBox* enable_ = nullptr;
    QSpinBox* height_ = nullptr;
    QCheckBox* paintAt_ = nullptr;
};

} // namespace MapEditor

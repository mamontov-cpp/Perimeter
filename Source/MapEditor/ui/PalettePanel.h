#pragma once

#include <QWidget>

class QLabel;

namespace MapEditor {

class EditorSession;
class KritaColorPicker;

/// ===========================================================================
/// MapEditor UI — color picker panel.
///
/// A Krita-style advanced color picker: three gradient strips (Hue /
/// Saturation / Value). Each strip is a gradient of ITS channel with the
/// other two channels pinned to the current color — clicking a strip sets
/// that channel from the position and the other strips re-render from the
/// new color (exactly Krita's HSV slider selector). Replaces the two
/// 256-entry palette grids the panel used to carry (the palettes still
/// exist in the world files as the fallback/material layer; painting itself
/// is true-color, see docs/map_logic.md).
///
/// - User edits emit colorChanged() — the brush paints that exact RGB.
/// - setColor() reflects a color picked elsewhere (eyedropper, brush
///   state restore) without re-emitting.
/// ===========================================================================
class PalettePanel : public QWidget
{
    Q_OBJECT

public:
    explicit PalettePanel(QWidget *parent = nullptr);

    /// Bind the session; without a loaded world the picker is disabled.
    void setSession(EditorSession *session);

    /// The current picker color, 0x00RRGGBB.
    unsigned int color() const { return currentRgb_; }

public slots:
    /// Show `rgb` in the picker (no colorChanged emission).
    void setColor(unsigned int rgb);

signals:
    /// The user changed the color in the picker.
    void colorChanged(unsigned int rgb);

private:
    EditorSession *session_ = nullptr;
    KritaColorPicker *picker_ = nullptr;
    QLabel *hint_ = nullptr;
    unsigned int currentRgb_ = 0x00FFFFFF;
};

} // namespace MapEditor

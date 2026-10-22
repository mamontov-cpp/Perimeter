// ============================================================================
// MapEditor UI — PalettePanel implementation (T4.1). See PalettePanel.h.
// ============================================================================

#include "PalettePanel.h"

#include "EditorSession.h"

#include <QColor>
#include <QColorDialog>
#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace MapEditor {

namespace {

/// Swatches per grid row (16 rows of 16 per 256-entry palette).
constexpr int kColumnsPerRow = 16;
constexpr int kSwatchSize = 18; // px

/// Object properties identifying the palette entry of a swatch widget.
constexpr char kPaletteIndexProperty[] = "paletteIndex";
constexpr char kPaletteDamProperty[] = "paletteDam";

/// Slot index of a swatch widget inside swatches_.
inline int swatchSlot(int paletteIndex, bool dam)
{
    return dam ? 256 + paletteIndex : paletteIndex;
}

inline bool slotIsDam(int slot)
{
    return slot >= 256;
}

/// Frame/background stylesheet of a swatch (2px blue border when selected).
inline QString swatchStyleSheet(bool selected, unsigned int rgb)
{
    const QString style = selected
        ? QStringLiteral("QToolButton { border: 2px solid #0078d7; "
                         "background-color: #%1; }")
        : QStringLiteral("QToolButton { border: 1px solid #909090; "
                         "background-color: #%1; }");
    return style.arg(rgb, 6, 16, QLatin1Char('0'));
}

} // namespace

// ---------------------------------------------------------------------------

PalettePanel::PalettePanel(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    gridsHost_ = new QWidget(this);
    {
        QVBoxLayout *grids = new QVBoxLayout(gridsHost_);
        grids->setContentsMargins(0, 0, 0, 0);
        grids->setSpacing(6);

        // Both grids are built identically, one per palette layer.
        const char *labels[2] = {
            QT_TRANSLATE_NOOP(
                "PalettePanel", "Geo palette (GeoPal) — entries 0..255"),
            QT_TRANSLATE_NOOP(
                "PalettePanel", "Dam palette (DamPal) — entries 0..255"),
        };
        for (int grid = 0; grid < 2; ++grid) {
            const bool dam = grid == 1;
            QLabel *title = new QLabel(tr(labels[grid]), gridsHost_);
            grids->addWidget(title);
            QGridLayout *gridLayout = new QGridLayout;
            gridLayout->setSpacing(1);
            for (int entry = 0; entry < 256; ++entry) {
                QToolButton *swatch = new QToolButton(gridsHost_);
                swatch->setProperty(kPaletteIndexProperty, entry);
                swatch->setProperty(kPaletteDamProperty, dam ? 1 : 0);
                swatch->setFixedSize(kSwatchSize, kSwatchSize);
                swatch->setCheckable(false);
                swatch->setFocusPolicy(Qt::NoFocus);
                // Click + double-click routing through the panel's filter —
                // keeps the swatches plain QToolButtons (no Q_OBJECT subclass).
                swatch->installEventFilter(this);
                gridLayout->addWidget(swatch,
                                      entry / kColumnsPerRow,
                                      entry % kColumnsPerRow);
                swatches_.push_back(swatch);
            }
            grids->addLayout(gridLayout);
        }
        grids->addStretch(1);
    }

    // The 512 swatches scroll vertically; the container grows above them.
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidget(gridsHost_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    hint_ = new QLabel(tr("No world loaded."), this);
    hint_->setAlignment(Qt::AlignCenter);

    layout->addWidget(hint_);
    layout->addWidget(scroll, 1);
}

// ---------------------------------------------------------------------------

bool PalettePanel::hasPalette() const
{
    return session_ && session_->isWorldLoaded();
}

void PalettePanel::setSession(EditorSession *session)
{
    session_ = session;

    const bool has = hasPalette();
    gridsHost_->setEnabled(has);
    hint_->setVisible(!has);
    selectedIndex_ = -1;
    selectedIsDam_ = false;
    if (has) {
        updateSwatches();
    } else {
        // No palette — drop the stale colors and keep only the frames.
        for (QWidget *swatch : swatches_) {
            swatch->setEnabled(false);
            swatch->setStyleSheet(QString());
            swatch->setToolTip(QString());
        }
    }
}

void PalettePanel::refresh()
{
    if (hasPalette()) {
        updateSwatches();
        applySelectionStyle(selectedIndex_, selectedIsDam_);
    }
}

void PalettePanel::selectIndex(int paletteIndex, bool dam)
{
    const int newSlot = (paletteIndex >= 0 && paletteIndex < 256 && hasPalette())
                            ? swatchSlot(paletteIndex, dam)
                            : -1;
    const bool newDam = slotIsDam(newSlot);
    const int newEntry =
        newSlot < 0 ? -1 : swatches_[static_cast<size_t>(newSlot)]
                               ->property(kPaletteIndexProperty)
                               .toInt();
    if (newSlot == swatchSlot(selectedIndex_, selectedIsDam_) &&
        selectedIndex_ >= 0) {
        applySelectionStyle(selectedIndex_, selectedIsDam_);
        return;
    }
    if (selectedIndex_ >= 0) {
        const int oldSlot =
            swatchSlot(selectedIndex_, selectedIsDam_);
        const int oldEntry =
            swatches_[static_cast<size_t>(oldSlot)]
                ->property(kPaletteIndexProperty)
                .toInt();
        const bool oldDam = slotIsDam(oldSlot);
        const unsigned int oldRgb =
            session_->paletteRgb(oldDam ? EditorSession::PaletteLayer::Dam
                                        : EditorSession::PaletteLayer::Geo,
                                 oldEntry);
        swatches_[static_cast<size_t>(oldSlot)]
            ->setStyleSheet(swatchStyleSheet(false, oldRgb));
    }
    selectedIndex_ = newSlot < 0 ? -1 : newEntry;
    selectedIsDam_ = newSlot < 0 ? false : newDam;
    applySelectionStyle(selectedIndex_, selectedIsDam_);
}

void PalettePanel::applySelectionStyle(int index, bool dam)
{
    const bool has = hasPalette();
    if (has && index >= 0 && index < 256) {
        QWidget *swatch =
            swatches_[static_cast<size_t>(swatchSlot(index, dam))];
        const unsigned int rgb =
            session_->paletteRgb(dam ? EditorSession::PaletteLayer::Dam
                                     : EditorSession::PaletteLayer::Geo,
                                 index);
        swatch->setStyleSheet(swatchStyleSheet(true, rgb));
        swatch->update();
    }
}

void PalettePanel::updateSwatches()
{
    if (!hasPalette()) {
        return;
    }
    for (int slot = 0; slot < 512; ++slot) {
        QWidget *swatch = swatches_[static_cast<size_t>(slot)];
        const int entry =
            static_cast<int>(swatch->property(kPaletteIndexProperty).toInt());
        const bool dam = swatch->property(kPaletteDamProperty).toInt() != 0;
        const unsigned int rgb =
            session_->paletteRgb(dam ? EditorSession::PaletteLayer::Dam
                                     : EditorSession::PaletteLayer::Geo,
                                 entry);
        swatch->setEnabled(true);
        swatch->setStyleSheet(
            slot == swatchSlot(selectedIndex_, selectedIsDam_)
                ? swatchStyleSheet(true, rgb)
                : swatchStyleSheet(false, rgb));
        swatch->setToolTip(tr("Index %1 — #%2 (%3)")
                               .arg(entry)
                               .arg(rgb, 6, 16, QLatin1Char('0'))
                               .arg(dam ? tr("Dam") : tr("Geo")));
    }
}

void PalettePanel::editIndex(int paletteIndex, bool dam)
{
    if (!hasPalette() || paletteIndex < 0 || paletteIndex >= 256) {
        return;
    }

    const EditorSession::PaletteLayer layer =
        dam ? EditorSession::PaletteLayer::Dam
            : EditorSession::PaletteLayer::Geo;
    const unsigned int oldRgb = session_->paletteRgb(layer, paletteIndex);
    const QColor initial(static_cast<int>((oldRgb >> 16) & 0xFF),
                         static_cast<int>((oldRgb >> 8) & 0xFF),
                         static_cast<int>(oldRgb & 0xFF));
    const QColor chosen = QColorDialog::getColor(
        initial, this, tr("Palette entry %1 (%2)")
                           .arg(paletteIndex)
                           .arg(dam ? tr("Dam") : tr("Geo")));
    if (!chosen.isValid()) {
        return;
    }
    const unsigned int rgb = (static_cast<unsigned int>(chosen.red()) << 16) |
                             (static_cast<unsigned int>(chosen.green()) << 8) |
                             static_cast<unsigned int>(chosen.blue());
    if (rgb == oldRgb) {
        return;
    }

    // T4.1 — RGB editing: write the bytes, rebuild the surface color tables
    // (convertPal2TableSurCol, exactly what the game runs after loading).
    if (!session_->setPaletteRgb(layer, paletteIndex, rgb) ||
        !session_->applyPalettes()) {
        return;
    }
    refresh();
    emit paletteEdited(paletteIndex, dam);
}

bool PalettePanel::eventFilter(QObject *watched, QEvent *event)
{
    QWidget *swatch = qobject_cast<QWidget *>(watched);
    if (swatch && swatch->property(kPaletteIndexProperty).isValid()) {
        const int entry =
            swatch->property(kPaletteIndexProperty).toInt();
        const bool dam =
            swatch->property(kPaletteDamProperty).toInt() != 0;
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                selectIndex(entry, dam);
                emit colorSelected(entry, dam);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                selectIndex(entry, dam);
                emit colorSelected(entry, dam);
                editIndex(entry, dam);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace MapEditor

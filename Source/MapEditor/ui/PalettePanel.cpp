// ============================================================================
// MapEditor UI — PalettePanel implementation (T4.1, T4.4). See PalettePanel.h.
// ============================================================================

#include "PalettePanel.h"

#include "EditorSession.h"

#include <QColor>
#include <QColorDialog>
#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
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

/// Key of a swatch inside swatchByKey_ (entry*2 + dam).
inline int swatchKey(int paletteIndex, bool dam)
{
    return paletteIndex * 2 + (dam ? 1 : 0);
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

    // The swatch grids scroll vertically; the container grows above them.
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidget(gridsHost_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    hint_ = new QLabel(tr("No world loaded."), this);
    hint_->setAlignment(Qt::AlignCenter);

    layout->addWidget(hint_);
    layout->addWidget(scroll, 1);

    rebuildGrids();
}

// ---------------------------------------------------------------------------

bool PalettePanel::hasPalette() const
{
    return session_ && session_->isWorldLoaded();
}

void PalettePanel::rebuildGrids()
{
    // Wipe the previous contents wholesale — every palette extension changes
    // the swatch count, and 512+ QToolButtons rebuild quickly.
    swatches_.clear();
    swatchByKey_.clear();
    addGeoButton_ = addDamButton_ = nullptr;
    // Delete the whole layout tree in one go: QLayout's destructor removes
    // nested layouts recursively (exactly once). Widgets live in those
    // layouts, but they are parented to gridsHost_, not owned by the
    // layout, so they are removed separately below.
    //
    // NOTE: do not walk the tree with takeAt() — for a nested layout Qt
    // returns the layout object itself as the item, so "delete nested;
    // delete item;" is a double delete (crashed on every rebuild).
    delete gridsHost_->layout();
    const QList<QWidget *> staleChildren = gridsHost_->findChildren<QWidget *>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *child : staleChildren) {
        child->deleteLater();
    }

    QVBoxLayout *grids = new QVBoxLayout(gridsHost_);
    grids->setContentsMargins(0, 0, 0, 0);
    grids->setSpacing(6);

    // Both sections are built identically, one per palette layer.
    const struct {
        const char *title;
        bool dam;
    } sections[2] = {
        {QT_TRANSLATE_NOOP("PalettePanel",
                           "Geo palette (GeoPal) — entries 0..255"), false},
        {QT_TRANSLATE_NOOP("PalettePanel",
                           "Dam palette (DamPal) — entries 0..255"), true},
    };

    const int geoCustom = hasPalette()
        ? session_->extendedPaletteSize(EditorSession::PaletteLayer::Geo) : 0;
    const int damCustom = hasPalette()
        ? session_->extendedPaletteSize(EditorSession::PaletteLayer::Dam) : 0;

    for (const auto &section : sections) {
        const bool dam = section.dam;
        QLabel *title = new QLabel(tr(section.title), gridsHost_);
        grids->addWidget(title);

        // Base entries 0..255.
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
            swatchByKey_[swatchKey(entry, dam)] = swatch;
        }
        grids->addLayout(gridLayout);

        // T4.4 — custom (extended) entries; global indices >=256, Geo first.
        QGridLayout *customLayout = new QGridLayout;
        customLayout->setSpacing(1);
        const int customCount = dam ? damCustom : geoCustom;
        for (int k = 0; k < customCount; ++k) {
            const int entry = 256 + (dam ? geoCustom : 0) + k;
            QToolButton *swatch = new QToolButton(gridsHost_);
            swatch->setProperty(kPaletteIndexProperty, entry);
            swatch->setProperty(kPaletteDamProperty, dam ? 1 : 0);
            swatch->setFixedSize(kSwatchSize, kSwatchSize);
            swatch->setCheckable(false);
            swatch->setFocusPolicy(Qt::NoFocus);
            swatch->installEventFilter(this);
            customLayout->addWidget(swatch,
                                    k / kColumnsPerRow,
                                    k % kColumnsPerRow);
            swatches_.push_back(swatch);
            swatchByKey_[swatchKey(entry, dam)] = swatch;
        }
        grids->addLayout(customLayout);

        // T4.4 — the "Add custom color…" button appends a new extended entry.
        QPushButton *addButton = new QPushButton(tr("Add custom color…"),
                                                 gridsHost_);
        addButton->setFocusPolicy(Qt::NoFocus);
        connect(addButton, &QPushButton::clicked, this,
                [this, dam]() { addCustomColor(dam); });
        grids->addWidget(addButton);
        if (dam) {
            addDamButton_ = addButton;
        } else {
            addGeoButton_ = addButton;
        }
    }
    grids->addStretch(1);

    builtGeoCustom_ = geoCustom;
    builtDamCustom_ = damCustom;
}

bool PalettePanel::gridCountsStale() const
{
    const int geoCustom = hasPalette()
        ? session_->extendedPaletteSize(EditorSession::PaletteLayer::Geo) : 0;
    const int damCustom = hasPalette()
        ? session_->extendedPaletteSize(EditorSession::PaletteLayer::Dam) : 0;
    return geoCustom != builtGeoCustom_ || damCustom != builtDamCustom_;
}

void PalettePanel::setSession(EditorSession *session)
{
    session_ = session;

    const bool has = hasPalette();
    gridsHost_->setEnabled(has);
    hint_->setVisible(!has);
    if (gridCountsStale()) {
        rebuildGrids();
    }
    selectedIndex_ = -1;
    selectedIsDam_ = false;
    updateSwatches();
}

void PalettePanel::refresh()
{
    if (!hasPalette()) {
        return;
    }
    if (gridCountsStale()) {
        rebuildGrids();
    }
    updateSwatches();
    applySelectionStyle(selectedIndex_, selectedIsDam_);
}

static QWidget *swatchForIndex(const std::unordered_map<int, QWidget *> &byKey,
                               int paletteIndex, bool dam)
{
    if (paletteIndex < 0) {
        return nullptr;
    }
    const auto it = byKey.find(swatchKey(paletteIndex, dam));
    return it == byKey.end() ? nullptr : it->second;
}

void PalettePanel::selectIndex(int paletteIndex, bool dam)
{
    QWidget *newSwatch = swatchForIndex(swatchByKey_, paletteIndex, dam);
    QWidget *oldSwatch =
        swatchForIndex(swatchByKey_, selectedIndex_, selectedIsDam_);
    if (newSwatch != nullptr && newSwatch == oldSwatch) {
        applySelectionStyle(selectedIndex_, selectedIsDam_);
        return;
    }
    if (oldSwatch != nullptr) {
        const unsigned int oldRgb = session_->paletteRgb(
            selectedIsDam_ ? EditorSession::PaletteLayer::Dam
                           : EditorSession::PaletteLayer::Geo,
            selectedIndex_);
        oldSwatch->setStyleSheet(swatchStyleSheet(false, oldRgb));
    }
    if (newSwatch != nullptr) {
        selectedIndex_ = paletteIndex;
        selectedIsDam_ = dam;
    } else {
        selectedIndex_ = -1;
        selectedIsDam_ = false;
    }
    applySelectionStyle(selectedIndex_, selectedIsDam_);
}

void PalettePanel::applySelectionStyle(int index, bool dam)
{
    QWidget *swatch = swatchForIndex(swatchByKey_, index, dam);
    if (hasPalette() && swatch != nullptr) {
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
    const bool selValid =
        swatchForIndex(swatchByKey_, selectedIndex_, selectedIsDam_) != nullptr;
    for (QWidget *swatch : swatches_) {
        const int entry =
            static_cast<int>(swatch->property(kPaletteIndexProperty).toInt());
        const bool dam = swatch->property(kPaletteDamProperty).toInt() != 0;
        const unsigned int rgb =
            hasPalette()
                ? session_->paletteRgb(
                    dam ? EditorSession::PaletteLayer::Dam
                        : EditorSession::PaletteLayer::Geo,
                    entry)
                : 0;
        swatch->setEnabled(hasPalette());
        const bool isSel = selValid && entry == selectedIndex_ && dam == selectedIsDam_;
        swatch->setStyleSheet(
            isSel ? swatchStyleSheet(true, rgb)
                  : swatchStyleSheet(false, rgb));
        swatch->setToolTip(hasPalette()
            ? tr("Index %1 — #%2 (%3)")
                  .arg(entry)
                  .arg(rgb, 6, 16, QLatin1Char('0'))
                  .arg(dam ? tr("Dam") : tr("Geo"))
            : QString());
    }
}

void PalettePanel::editIndex(int paletteIndex, bool dam)
{
    if (!hasPalette() || paletteIndex < 0) {
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

    if (paletteIndex >= 256) {
        // T4.4 — custom entry: rewrite the sidecar-palette bytes and rebuild
        // the extended color tables.
        if (!session_->setExtendedPaletteRgb(layer, paletteIndex, rgb)) {
            return;
        }
    } else {
        // T4.1 — RGB editing: write the bytes, rebuild the surface color
        // tables (convertPal2TableSurCol, exactly what the game runs after
        // loading).
        if (!session_->setPaletteRgb(layer, paletteIndex, rgb) ||
            !session_->applyPalettes()) {
            return;
        }
    }
    refresh();
    emit paletteEdited(paletteIndex, dam);
}

void PalettePanel::addCustomColor(bool dam)
{
    if (!hasPalette()) {
        return;
    }
    const QColor chosen = QColorDialog::getColor(
        Qt::white, this,
        tr("New custom %1 palette color").arg(dam ? tr("Dam") : tr("Geo")));
    if (!chosen.isValid()) {
        return;
    }
    const unsigned int rgb = (static_cast<unsigned int>(chosen.red()) << 16) |
                             (static_cast<unsigned int>(chosen.green()) << 8) |
                             static_cast<unsigned int>(chosen.blue());
    const int entry = session_->addPaletteColor(
        dam ? EditorSession::PaletteLayer::Dam
            : EditorSession::PaletteLayer::Geo,
        rgb);
    if (entry < 0) {
        QMessageBox::information(
            this, tr("Extended palette is full"),
            tr("The extended palette reached the 16-bit index limit "
               "(65536 entries per world). Existing entries can still be "
               "edited — double-click a swatch."));
        return;
    }
    rebuildGrids();
    updateSwatches();
    selectIndex(entry, dam);
    emit colorSelected(entry, dam);
    emit paletteEdited(entry, dam);
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

// ============================================================================
// MapEditor UI — SlicePanel implementation. See SlicePanel.h.
// ============================================================================

#include "SlicePanel.h"

#include "EditorSession.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif
#include "terra.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>

namespace MapEditor {

SlicePanel::SlicePanel(QWidget* parent)
    : QWidget(parent)
{
    QFormLayout* form = new QFormLayout(this);
    form->setContentsMargins(6, 6, 6, 6);

    enable_ = new QCheckBox(tr("Show colors at a height"), this);
    enable_->setToolTip(tr("Clip the whole map to the chosen height: every "
                           "cell shows the color of the voxel layer at that "
                           "height (voxels above the slice are not "
                           "rendered). Takes effect on Apply — the "
                           "re-render is expensive."));
    form->addRow(enable_);

    height_ = new QSpinBox(this);
    height_->setRange(0, MAX_VX_HEIGHT_WHOLE);
    height_->setSuffix(tr(" vox"));
    height_->setToolTip(tr("The slice height in whole voxels. Editing does "
                           "not re-render — press Apply."));
    form->addRow(tr("Height:"), height_);

    paintAt_ = new QCheckBox(tr("2D brushes paint at this height"), this);
    paintAt_->setToolTip(tr("The 2D brushes paint at the slice height (to "
                            "the paint depth). Takes effect immediately — "
                            "it only sets the brush paint height."));
    form->addRow(paintAt_);

    apply_ = new QPushButton(tr("Apply"), this);
    apply_->setToolTip(tr("Re-render the map at the chosen slice height. "
                          "Enabled while the panel state differs from the "
                          "applied one — the re-render is expensive, so it "
                          "never runs on spinbox edits."));
    form->addRow(apply_);

    height_->setEnabled(false);
    paintAt_->setEnabled(false);
    apply_->setEnabled(false);

    connect(enable_, &QCheckBox::toggled, this, [this]() { refreshState(); });
    connect(height_, &QSpinBox::valueChanged, this,
            [this](int) { refreshState(); });
    connect(paintAt_, &QCheckBox::toggled, this, [this](bool on) {
        // Cheap: only the brush paint height follows — no re-render.
        if (on && enable_->isChecked()) {
            emit paintAtSliceRequested(height_->value());
        }
        refreshState();
    });
    connect(apply_, &QPushButton::clicked, this, [this]() { applySlice(); });
}

void SlicePanel::setSession(EditorSession* session)
{
    session_ = session;
    const bool has = session_ && session_->isWorldLoaded();
    setEnabled(has);
    refreshState();
}

bool SlicePanel::sliceEnabled() const
{
    return enable_->isChecked();
}

int SlicePanel::sliceHeight() const
{
    return height_->value();
}

bool SlicePanel::paintAtSliceHeight() const
{
    return enable_->isChecked() && paintAt_->isChecked();
}

void SlicePanel::applySlice()
{
    if (!session_) {
        return;
    }
    // The expensive call: a full-map regRender that rebuilds the 2D cache
    // and the GPU tilemap (TerrainGpuView). Deliberately NOT hooked to the
    // spinbox — see the header.
    session_->setViewSliceHeight(enable_->isChecked() ? height_->value()
                                                      : -1);
    if (enable_->isChecked() && paintAt_->isChecked()) {
        emit paintAtSliceRequested(height_->value());
    }
    refreshState();
}

void SlicePanel::refreshState()
{
    const bool on = enable_->isChecked();
    height_->setEnabled(on);
    paintAt_->setEnabled(on);
    const int want = on ? height_->value() : -1;
    const int applied = (session_ && session_->isWorldLoaded())
                            ? session_->viewSliceHeight()
                            : -1;
    apply_->setEnabled(session_ && session_->isWorldLoaded() &&
                       want != applied);
    emit sliceChanged();
}

} // namespace MapEditor

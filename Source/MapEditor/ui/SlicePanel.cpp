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
                           "rendered)."));
    form->addRow(enable_);

    height_ = new QSpinBox(this);
    height_->setRange(0, MAX_VX_HEIGHT_WHOLE);
    height_->setSuffix(tr(" vox"));
    height_->setToolTip(tr("The slice height in whole voxels."));
    form->addRow(tr("Height:"), height_);

    paintAt_ = new QCheckBox(tr("2D brushes paint at this height"), this);
    paintAt_->setToolTip(tr("While the slice is shown, the 2D brushes paint "
                            "at the slice height (to the paint depth)."));
    form->addRow(paintAt_);

    height_->setEnabled(false);
    paintAt_->setEnabled(false);

    auto updateState = [this]() {
        height_->setEnabled(enable_->isChecked());
        paintAt_->setEnabled(enable_->isChecked());
        if (session_) {
            session_->setViewSliceHeight(enable_->isChecked()
                                             ? height_->value()
                                             : -1);
        }
        emit sliceChanged();
        if (enable_->isChecked() && paintAt_->isChecked()) {
            emit paintAtSliceRequested(height_->value());
        }
    };
    connect(enable_, &QCheckBox::toggled, this, updateState);
    connect(height_, &QSpinBox::valueChanged, this, updateState);
    connect(paintAt_, &QCheckBox::toggled, this, updateState);
}

void SlicePanel::setSession(EditorSession* session)
{
    session_ = session;
    const bool has = session_ && session_->isWorldLoaded();
    setEnabled(has);
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

} // namespace MapEditor

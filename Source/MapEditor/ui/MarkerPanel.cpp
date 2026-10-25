// ============================================================================
// MarkerPanel implementation (T7.2).
// ============================================================================

#include "MarkerPanel.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace MapEditor {

namespace {

const char *const kMarkerTypes[] = {"point", "zone", "unit", "building"};

QString typeDisplay(const QString &type)
{
    if (type == QLatin1String("zone")) return QObject::tr("Zone");
    if (type == QLatin1String("unit")) return QObject::tr("Unit");
    if (type == QLatin1String("building")) return QObject::tr("Building");
    return QObject::tr("Point");
}

QColor colorOf(unsigned int rgb)
{
    return QColor(static_cast<int>((rgb >> 16) & 0xFF),
                  static_cast<int>((rgb >> 8) & 0xFF),
                  static_cast<int>(rgb & 0xFF));
}

} // namespace

MarkerPanel::MarkerPanel(MapEditor::MarkerStore &store, QWidget *parent)
    : QWidget(parent), store_(store)
{
    auto *layout = new QVBoxLayout(this);

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list_);

    auto *buttons = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("Add"), this);
    editButton_ = new QPushButton(tr("Edit..."), this);
    deleteButton_ = new QPushButton(tr("Delete"), this);
    goButton_ = new QPushButton(tr("Go To"), this);
    buttons->addWidget(addButton);
    buttons->addWidget(editButton_);
    buttons->addWidget(deleteButton_);
    buttons->addWidget(goButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(addButton, &QPushButton::clicked, this, &MarkerPanel::onAdd);
    connect(editButton_, &QPushButton::clicked, this, &MarkerPanel::onEdit);
    connect(deleteButton_, &QPushButton::clicked, this, &MarkerPanel::onDelete);
    connect(goButton_, &QPushButton::clicked, this, &MarkerPanel::onGoTo);
    connect(list_, &QListWidget::itemSelectionChanged, this,
            &MarkerPanel::onSelectionChanged);
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *) { onGoTo(); });

    onSelectionChanged();
}

void MarkerPanel::refresh()
{
    list_->clear();
    for (const MapEditor::MapMarkerData &marker : store_.markers()) {
        QString text = QStringLiteral("[%1] %2 (%3, %4)")
                           .arg(typeDisplay(QString::fromStdString(marker.type)),
                                QString::fromStdString(marker.label))
                           .arg(marker.x)
                           .arg(marker.y);
        if (marker.radius > 0) {
            text += QStringLiteral(" r%1").arg(marker.radius);
        }
        auto *item = new QListWidgetItem(text, list_);
        item->setData(Qt::UserRole,
                      QVariant::fromValue<qlonglong>(
                          static_cast<qlonglong>(marker.id)));
        item->setForeground(colorOf(marker.colorRgb));
    }
    onSelectionChanged();
}

void MarkerPanel::onSelectionChanged()
{
    const bool has = list_->currentItem() != nullptr;
    editButton_->setEnabled(has);
    deleteButton_->setEnabled(has);
    goButton_->setEnabled(has);
}

void MarkerPanel::onAdd()
{
    MapEditor::MapMarkerData marker;
    marker.x = defaultX_;
    marker.y = defaultY_;
    marker.label = tr("Marker %1").arg(store_.size() + 1).toStdString();
    if (!runEditorDialog(marker, true)) {
        return;
    }
    store_.add(marker);
    refresh();
    emit markersChanged();
}

void MarkerPanel::onEdit()
{
    const QListWidgetItem *item = list_->currentItem();
    if (!item) {
        return;
    }
    const auto id = static_cast<std::uint64_t>(
        item->data(Qt::UserRole).value<qlonglong>());
    const MapEditor::MapMarkerData *existing = store_.find(id);
    if (!existing) {
        return;
    }
    MapEditor::MapMarkerData marker = *existing;
    if (!runEditorDialog(marker, false)) {
        return;
    }
    store_.update(marker);
    refresh();
    emit markersChanged();
}

void MarkerPanel::onDelete()
{
    QListWidgetItem *item = list_->currentItem();
    if (!item) {
        return;
    }
    const auto id = static_cast<std::uint64_t>(
        item->data(Qt::UserRole).value<qlonglong>());
    if (store_.remove(id)) {
        refresh();
        emit markersChanged();
    }
}

void MarkerPanel::onGoTo()
{
    const QListWidgetItem *item = list_->currentItem();
    if (!item) {
        return;
    }
    const auto id = static_cast<std::uint64_t>(
        item->data(Qt::UserRole).value<qlonglong>());
    const MapEditor::MapMarkerData *marker = store_.find(id);
    if (marker) {
        emit goToMarker(marker->x, marker->y);
    }
}

bool MarkerPanel::runEditorDialog(MapEditor::MapMarkerData &marker, bool isNew)
{
    QDialog dialog(this);
    dialog.setWindowTitle(isNew ? tr("Add Marker") : tr("Edit Marker"));
    auto *form = new QFormLayout(&dialog);

    auto *labelEdit = new QLineEdit(QString::fromStdString(marker.label),
                                    &dialog);
    form->addRow(tr("Label:"), labelEdit);

    auto *typeCombo = new QComboBox(&dialog);
    for (const char *type : kMarkerTypes) {
        typeCombo->addItem(typeDisplay(QString::fromLatin1(type)),
                           QString::fromLatin1(type));
    }
    const int typeIndex = typeCombo->findData(
        QString::fromStdString(marker.type));
    typeCombo->setCurrentIndex(typeIndex < 0 ? 0 : typeIndex);
    form->addRow(tr("Type:"), typeCombo);

    auto *noteEdit = new QLineEdit(QString::fromStdString(marker.note),
                                   &dialog);
    form->addRow(tr("Note:"), noteEdit);

    auto *xSpin = new QSpinBox(&dialog);
    xSpin->setRange(0, 1 << 14);
    xSpin->setValue(marker.x);
    auto *ySpin = new QSpinBox(&dialog);
    ySpin->setRange(0, 1 << 14);
    ySpin->setValue(marker.y);
    auto *posRow = new QHBoxLayout;
    posRow->addWidget(new QLabel(tr("X:"), &dialog));
    posRow->addWidget(xSpin);
    posRow->addWidget(new QLabel(tr("Y:"), &dialog));
    posRow->addWidget(ySpin);
    form->addRow(tr("Position:"), posRow);

    auto *radiusSpin = new QSpinBox(&dialog);
    radiusSpin->setRange(0, 4096);
    radiusSpin->setValue(marker.radius);
    radiusSpin->setSpecialValueText(tr("(point)"));
    form->addRow(tr("Zone radius:"), radiusSpin);

    QColor color = colorOf(marker.colorRgb);
    auto *colorButton = new QPushButton(&dialog);
    auto updateColorButton = [colorButton, &color]() {
        colorButton->setStyleSheet(
            QStringLiteral("background-color: %1").arg(color.name()));
    };
    updateColorButton();
    connect(colorButton, &QPushButton::clicked, &dialog, [&]() {
        const QColor picked = QColorDialog::getColor(color, &dialog);
        if (picked.isValid()) {
            color = picked;
            updateColorButton();
        }
    });
    form->addRow(tr("Color:"), colorButton);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    marker.label = labelEdit->text().toStdString();
    marker.type = typeCombo->currentData().toString().toStdString();
    marker.note = noteEdit->text().toStdString();
    marker.x = xSpin->value();
    marker.y = ySpin->value();
    marker.radius = radiusSpin->value();
    marker.colorRgb = (static_cast<unsigned>(color.red()) << 16) |
                      (static_cast<unsigned>(color.green()) << 8) |
                      static_cast<unsigned>(color.blue());
    return true;
}

} // namespace MapEditor

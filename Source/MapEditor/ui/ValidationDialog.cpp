// ============================================================================
// ValidationDialog implementation (T9.1).
// ============================================================================

#include "ValidationDialog.h"

#include <QColor>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace MapEditor {

namespace {

QString severityText(Severity severity)
{
    switch (severity) {
    case Severity::Error: return QObject::tr("Error");
    case Severity::Warning: return QObject::tr("Warning");
    case Severity::Info: return QObject::tr("Info");
    }
    return QObject::tr("?");
}

QColor severityColor(Severity severity)
{
    switch (severity) {
    case Severity::Error: return QColor(220, 60, 60);
    case Severity::Warning: return QColor(214, 150, 0);
    case Severity::Info: return QColor(120, 120, 120);
    }
    return QColor(0, 0, 0);
}

} // namespace

ValidationDialog::ValidationDialog(QWidget *parent, const QString &title,
                                   const ValidationReport &report)
    : QDialog(parent)
{
    setWindowTitle(title);
    setModal(true);
    resize(640, 420);

    auto *layout = new QVBoxLayout(this);

    const int errors = report.count(Severity::Error);
    const int warnings = report.count(Severity::Warning);
    const int infos = report.count(Severity::Info);
    auto *summary = new QLabel(
        tr("%1 error(s), %2 warning(s), %3 info").arg(errors).arg(warnings).arg(infos),
        this);
    layout->addWidget(summary);

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels({tr("Severity"), tr("Message"), tr("Cell")});
    tree_->header()->resizeSection(0, 90);
    tree_->header()->resizeSection(2, 110);
    tree_->setRootIsDecorated(false);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(tree_);

    for (const ValidationIssue &issue : report.issues) {
        auto *item = new QTreeWidgetItem(tree_);
        item->setText(0, severityText(issue.severity));
        item->setForeground(0, severityColor(issue.severity));
        item->setText(1, QString::fromStdString(issue.message));
        if (issue.x >= 0 && issue.y >= 0) {
            item->setText(2, QStringLiteral("%1, %2").arg(issue.x).arg(issue.y));
            item->setData(2, Qt::UserRole, QPoint(issue.x, issue.y));
        }
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

void ValidationDialog::currentIssueCell(int &x, int &y) const
{
    x = y = -1;
    QTreeWidgetItem *item = tree_->currentItem();
    if (!item) {
        return;
    }
    const QVariant data = item->data(2, Qt::UserRole);
    if (data.isValid()) {
        const QPoint cell = data.toPoint();
        x = cell.x();
        y = cell.y();
    }
}

} // namespace MapEditor

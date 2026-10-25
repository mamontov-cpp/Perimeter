#pragma once

#include <QDialog>

#include "Validation.h"

class QTreeWidget;

// ============================================================================
// ValidationDialog (T9.1) — the report of Validation: one tree with
// error / warning / info entries (color coded) and a "Go To" button for
// issues that carry a map cell (the caller wires it to the viewport).
// ============================================================================
namespace MapEditor {

class ValidationDialog : public QDialog
{
    Q_OBJECT

public:
    ValidationDialog(QWidget *parent, const QString &title,
                     const ValidationReport &report);

    /// The cell of the currently selected issue (-1,-1 when it has none).
    void currentIssueCell(int &x, int &y) const;

private:
    QTreeWidget *tree_ = nullptr;
};

} // namespace MapEditor

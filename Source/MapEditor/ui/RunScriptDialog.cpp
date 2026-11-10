// ============================================================================
// MapEditor UI — the "Run Script" dialog (see the header).
// ============================================================================

#include "RunScriptDialog.h"

#include "ScriptEditorDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "ScriptedBrush.h"

namespace MapEditor {

RunScriptDialog::RunScriptDialog(QWidget* parent, const QString& script)
    : QDialog(parent)
{
    setWindowTitle(tr("Run Script"));
    setMinimumSize(560, 480);

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        tr("JavaScript (duktape) with the editor brush API — the same "
           "functions the scripted brush runs. No stamp: getCurrentPos() "
           "reports the cursor cell (the map centre when the cursor is off "
           "the map). The whole run is ONE undo step; a 60 s time limit "
           "applies. API functions are highlighted in blue."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    edit_ = new ScriptEdit(this);
    edit_->setPlainText(script);
    layout->addWidget(edit_, 1);

    // Run compiles the text first (syntax gate): on error the message is
    // shown and the dialog stays open; Cancel discards. The Run button IS
    // the box's Ok (AcceptRole) under a relabelled text — an Apply button
    // would carry ApplyRole and the accepted() signal would never fire.
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel,
                                         this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Run"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        std::string error;
        if (!checkScriptSyntax(edit_->toPlainText().toStdString(), error)) {
            QMessageBox::warning(this, tr("Script Syntax Error"),
                                 tr("The script does not compile:\n\n%1")
                                     .arg(QString::fromStdString(error)));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* bottom = new QHBoxLayout();
    bottom->addStretch(1);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);
}

QString RunScriptDialog::script() const
{
    return edit_->toPlainText();
}

} // namespace MapEditor

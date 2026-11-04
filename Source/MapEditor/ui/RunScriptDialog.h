#pragma once

#include <QDialog>

// ============================================================================
// MapEditor UI — the "Run Script" dialog (Tools → Run Script...).
//
// The same multiline JavaScript editor as the scripted-brush script editor
// (shared ScriptEdit/ScriptHighlighter from ScriptEditorDialog.h) — but no
// stamp preview picker: Run compiles the text (syntax check; on error the
// box shows the message and the dialog stays open) and, on success, leaves
// the script in script() for the caller to execute through
// runEditorScript() (the same API as the scripted brush).
// ============================================================================

namespace MapEditor {

class ScriptEdit;

class RunScriptDialog : public QDialog
{
    Q_OBJECT
public:
    /// `script` seeds the editor.
    RunScriptDialog(QWidget* parent, const QString& script);

    /// The script to run (valid after a successful exec()).
    QString script() const;

private:
    ScriptEdit* edit_;
};

} // namespace MapEditor

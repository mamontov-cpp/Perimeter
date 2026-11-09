#pragma once

#include <QDialog>

#include <QString>

class QTextBrowser;

// ============================================================================
// HelpWindow — the editor manual viewer (Help menu, F1).
//
// The manual itself is separate Markdown files deployed next to the
// executable by the build: map_editor_help.md (Russian, the original) and
// map_editor_help_en.md (English translation); the window renders them with
// Qt's markdown support. A language switcher at the top flips between the
// two files (the choice persists in QSettings). Non-modal, so the editor
// stays usable while reading.
// ============================================================================
namespace MapEditor {

class HelpWindow : public QDialog
{
    Q_OBJECT

public:
    /// Opens (or raises an already-open) help window. The window deletes
    /// itself on close.
    static void openHelp(QWidget *parent);

private:
    explicit HelpWindow(QWidget *parent);

    /// The help file name for a language code ("ru"/anything else — the
    /// Russian original; "en" — the translation).
    static QString fileNameForLanguage(const QString &language);

    /// Finds the given help file next to the executable (build tree and
    /// install layouts) or in the working directory; empty when not found.
    static QString locateHelpFile(const QString &fileName);

    /// Reloads the browser from the language's file (falling back to the
    /// other language when the file is missing) and persists the choice.
    void loadDocument(const QString &language);

    QTextBrowser *browser_ = nullptr;
    QString language_;
};

} // namespace MapEditor

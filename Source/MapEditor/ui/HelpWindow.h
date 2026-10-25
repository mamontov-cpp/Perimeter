#pragma once

#include <QDialog>

class QTextBrowser;

// ============================================================================
// HelpWindow — the editor manual viewer (Help menu, F1).
//
// The manual itself is a separate Markdown file
// (Source/MapEditor/data/map_editor_help.md) deployed next to the
// executable by the build; the window renders it with Qt's markdown
// support. Non-modal, so the editor stays usable while reading.
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

    /// Finds map_editor_help.md next to the executable (build tree and
    /// install layouts) or in the working directory; empty when not found.
    static QString locateHelpFile();
};

} // namespace MapEditor

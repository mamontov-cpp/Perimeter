#pragma once

#include <QDialog>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <vector>

class QComboBox;
class QPlainTextEdit;
class QTextDocument;
class QWidget;

namespace MapEditor {

// ============================================================================
// MapEditor UI — the scripted-brush script editor dialog.
//
// A modal dialog with a MULTILINE JavaScript editor (QPlainTextEdit):
//   * syntax highlighting — the editor API names, JS keywords, literals,
//     comments (ScriptHighlighter below);
//   * a line-number gutter (LineNumberArea, the classic Qt code-editor
//     pattern);
//   * a stamp-form picker (circle/square) choosing the cursor preview
//     outline while the scripted brush is active;
//   * OK runs the core syntax check (duktape compile): on error a warning
//     box shows the message (with the brush.js line number) and the dialog
//     stays open; Cancel discards the edits.
// ============================================================================

/// The stamp forms the picker offers — kept as an enum of our own so the
/// dialog does not pull the viewport header in.
enum class ScriptStampShape
{
    Circle,
    Square,
};

/// The syntax highlighter: API function names get a distinct bold colour,
/// JS keywords italic, numbers/strings literal colours, // and /* */
/// comments grey.
class ScriptHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    explicit ScriptHighlighter(QTextDocument* parent = nullptr);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule
    {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    std::vector<Rule> rules_;
    QTextCharFormat apiFormat_;
    QTextCharFormat commentFormat_;
};

/// The line-number gutter widget (paired with the editor via resize/paint
/// requests, mirroring the Qt "Code Editor Example").
class LineNumberArea : public QWidget
{
    Q_OBJECT
public:
    explicit LineNumberArea(class ScriptEdit* editor);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    ScriptEdit* editor_;
};

/// The multiline script editor (line numbers on the left gutter).
class ScriptEdit : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit ScriptEdit(QWidget* parent = nullptr);

    void lineNumberAreaPaintEvent(QPaintEvent* event);
    int lineNumberAreaWidth() const;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    LineNumberArea* lineNumberArea_;
};

class ScriptEditorDialog : public QDialog
{
    Q_OBJECT
public:
    /// `script` seeds the editor; `shape` seeds the stamp-form picker.
    ScriptEditorDialog(QWidget* parent, const QString& script,
                       ScriptStampShape shape);

    /// The edited script (valid after a successful exec()).
    QString script() const;
    /// The picked stamp preview form.
    ScriptStampShape stampShape() const;

private:
    ScriptEdit* edit_;
    QComboBox* shapeCombo_;
};

} // namespace MapEditor

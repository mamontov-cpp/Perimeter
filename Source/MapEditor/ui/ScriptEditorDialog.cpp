// ============================================================================
// MapEditor UI — the scripted-brush script editor dialog (see the header).
// ============================================================================

#include "ScriptEditorDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTextEdit>
#include <QVBoxLayout>

#include "ScriptedBrush.h"

namespace MapEditor {

// ---------------------------------------------------------------------------
// Syntax highlighting
// ---------------------------------------------------------------------------

ScriptHighlighter::ScriptHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    apiFormat_.setForeground(QColor(0x1f, 0x6f, 0xc4));
    apiFormat_.setFontWeight(QFont::Bold);

    struct Entry
    {
        const char* pattern; // raw regex, anchored at a word boundary by us
        QTextCharFormat format;
    };
    QTextCharFormat keywordFormat;
    keywordFormat.setForeground(QColor(0x7f, 0x2f, 0xa0));
    keywordFormat.setFontItalic(true);
    QTextCharFormat literalFormat;
    literalFormat.setForeground(QColor(0xa3, 0x5e, 0x12));
    commentFormat_.setForeground(QColor(0x80, 0x80, 0x80));

    const QStringList keywords{
        QStringLiteral("break"),  QStringLiteral("case"),    QStringLiteral("catch"),
        QStringLiteral("const"),  QStringLiteral("continue"), QStringLiteral("debugger"),
        QStringLiteral("default"), QStringLiteral("delete"),  QStringLiteral("do"),
        QStringLiteral("else"),   QStringLiteral("finally"),  QStringLiteral("for"),
        QStringLiteral("function"), QStringLiteral("if"),     QStringLiteral("in"),
        QStringLiteral("instanceof"), QStringLiteral("let"), QStringLiteral("new"),
        QStringLiteral("return"), QStringLiteral("switch"),   QStringLiteral("this"),
        QStringLiteral("throw"),  QStringLiteral("try"),      QStringLiteral("typeof"),
        QStringLiteral("var"),    QStringLiteral("void"),     QStringLiteral("while"),
        QStringLiteral("with"),
    };
    const QStringList api{
        QStringLiteral("getCurrentPos"),       QStringLiteral("getCurrentRadius"),
        QStringLiteral("getCurrentThickness"), QStringLiteral("getCurrentHeight"),
        QStringLiteral("raiseHeight"),         QStringLiteral("lowerHeight"),
        QStringLiteral("setHeight"),           QStringLiteral("setToZP"),
        QStringLiteral("paintSoot"),           QStringLiteral("paintVeryLightDam"),
        QStringLiteral("paintColor"),
        QStringLiteral("applyBlur"),           QStringLiteral("applySmooth"),
        QStringLiteral("getDamHeight"),        QStringLiteral("getGeoColor"),
        QStringLiteral("getDamColor"),         QStringLiteral("setGeoPalette"),
        QStringLiteral("setDamPalette"),       QStringLiteral("showInfoMessageBox"),
        QStringLiteral("showErrorMessageBox"),
    };

    // Order matters: API names before keywords (both word-anchored, no
    // overlap in practice), comments handled separately below.
    for (const QString& word : api) {
        rules_.push_back(
            {QRegularExpression(QStringLiteral("\\b") + word + QStringLiteral("\\b")),
             apiFormat_});
    }
    for (const QString& word : keywords) {
        rules_.push_back(
            {QRegularExpression(QStringLiteral("\\b") + word + QStringLiteral("\\b")),
             keywordFormat});
    }
    rules_.push_back({QRegularExpression(QStringLiteral("\\b\\d+(\\.\\d+)?\\b")),
                      literalFormat});
    rules_.push_back({QRegularExpression(QStringLiteral("(\"[^\"]*\")|('[^']*')")),
                      literalFormat});
    rules_.push_back({QRegularExpression(QStringLiteral("//[^\n]*")), commentFormat_});
}

void ScriptHighlighter::highlightBlock(const QString& text)
{
    for (const Rule& rule : rules_) {
        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), rule.format);
        }
    }
    // /* ... */ comments may span blocks — track the state.
    setCurrentBlockState(0);
    int start = 0;
    if (previousBlockState() != 1) {
        start = text.indexOf(QStringLiteral("/*"));
    }
    while (start >= 0) {
        int end = text.indexOf(QStringLiteral("*/"), start + 2);
        int length = 0;
        if (end == -1) {
            setCurrentBlockState(1);
            length = text.length() - start;
        } else {
            length = end - start + 2;
        }
        setFormat(start, length, commentFormat_);
        start = text.indexOf(QStringLiteral("/*"), start + length);
    }
}

// ---------------------------------------------------------------------------
// Line numbers
// ---------------------------------------------------------------------------

LineNumberArea::LineNumberArea(ScriptEdit* editor)
    : QWidget(editor)
    , editor_(editor)
{
}

QSize LineNumberArea::sizeHint() const
{
    return QSize(editor_->lineNumberAreaWidth(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent* event)
{
    editor_->lineNumberAreaPaintEvent(event);
}

ScriptEdit::ScriptEdit(QWidget* parent)
    : QPlainTextEdit(parent)
    , lineNumberArea_(new LineNumberArea(this))
{
    new ScriptHighlighter(document());
    connect(this, &ScriptEdit::blockCountChanged, this,
            [this](int) {
                setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
            });
    connect(this, &ScriptEdit::updateRequest, this,
            [this](const QRect& rect, int dy) {
                if (dy != 0) {
                    lineNumberArea_->scroll(0, dy);
                } else {
                    lineNumberArea_->update(0, rect.y(),
                                            lineNumberArea_->width(),
                                            rect.height());
                }
                if (rect.contains(viewport()->rect())) {
                    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
                }
            });
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    setPlaceholderText(QStringLiteral(
        "// JavaScript (duktape). The editor brush API:\n"
        "//   getCurrentPos()  -> {x, y}\n"
        "//   getCurrentRadius() / getCurrentThickness() / getCurrentHeight()\n"
        "//   raiseHeight(pos, height, radius, square)\n"
        "//   lowerHeight(pos, height, radius, square)\n"
        "//   setHeight(pos, height, radius, square)\n"
        "//   setToZP(pos, radius, square)\n"
        "//   paintSoot(pos, radius, square)\n"
        "//   paintVeryLightDam(pos, radius, square)\n"
        "//   paintColor(pos, red, green, blue, radius, square)\n"
        "//   applyBlur(pos, radius, blurRadius, effect, square)\n"
        "//   applySmooth(pos, radius, effect, square)\n"
        "//   getDamHeight(pos) -> int        getGeoColor(pos) / getDamColor(pos)"
        " -> {r,g,b}\n"
        "//   setGeoPalette(index, {r, g, b}) / setDamPalette(index, {r, g, b})\n"
        "//   showInfoMessageBox(text) / showErrorMessageBox(text)\n"
        "// pos = {x: int, y: int}; radius >= 1; effect 0..1; square = boolean. "
        "Dam layer only."));
}

int ScriptEdit::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    const int space = 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    return space;
}

void ScriptEdit::lineNumberAreaPaintEvent(QPaintEvent* event)
{
    QPainter painter(lineNumberArea_);
    painter.fillRect(event->rect(), QColor(0xf0, 0xf0, 0xf0));
    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            if (blockNumber == current) {
                painter.setPen(QColor(0x20, 0x60, 0xa0));
                painter.setFont(QFont(font().family(), font().pointSizeF(), QFont::Bold));
            } else {
                painter.setPen(QColor(0x90, 0x90, 0x90));
                painter.setFont(font());
            }
            painter.drawText(0, top, lineNumberArea_->width() - 4,
                             fontMetrics().height(), Qt::AlignRight, number);
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void ScriptEdit::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    lineNumberArea_->setGeometry(QRect(cr.left(), cr.top(),
                                       lineNumberAreaWidth(), cr.height()));
}

// ---------------------------------------------------------------------------
// The dialog
// ---------------------------------------------------------------------------

ScriptEditorDialog::ScriptEditorDialog(QWidget* parent, const QString& script,
                                       ScriptStampShape shape)
    : QDialog(parent)
{
    setWindowTitle(tr("Scripted Brush — Script Editor"));
    setMinimumSize(560, 480);

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        tr("JavaScript (duktape), Dam layer only. The whole evaluation of one "
           "stamp is a single Undo step; a 60 s time limit applies. The brush "
           "API functions are highlighted in blue."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    edit_ = new ScriptEdit(this);
    edit_->setPlainText(script);
    layout->addWidget(edit_, 1);

    // Bottom row: stamp preview shape picker + the OK/Cancel buttons.
    auto* bottom = new QHBoxLayout();
    bottom->addWidget(new QLabel(tr("Stamp preview:"), this));
    shapeCombo_ = new QComboBox(this);
    shapeCombo_->addItem(tr("Circle (filled)"),
                         static_cast<int>(ScriptStampShape::Circle));
    shapeCombo_->addItem(tr("Square (filled)"),
                         static_cast<int>(ScriptStampShape::Square));
    shapeCombo_->setCurrentIndex(shape == ScriptStampShape::Square ? 1 : 0);
    shapeCombo_->setToolTip(
        tr("The cursor preview outline while the scripted brush is active "
           "(the script itself picks a shape per call)."));
    bottom->addWidget(shapeCombo_);
    bottom->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        // The OK-button gate: compile (syntax check) before accepting; on
        // error show the message and keep the dialog open.
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
    bottom->addWidget(buttons);
    layout->addLayout(bottom);
}

QString ScriptEditorDialog::script() const
{
    return edit_->toPlainText();
}

ScriptStampShape ScriptEditorDialog::stampShape() const
{
    return static_cast<ScriptStampShape>(
        shapeCombo_->currentData().toInt());
}

} // namespace MapEditor

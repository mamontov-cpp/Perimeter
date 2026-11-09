// ============================================================================
// HelpWindow implementation.
// ============================================================================

#include "HelpWindow.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace MapEditor {

namespace {
const char kSettingsKey[] = "help/language";
const char kLangRussian[] = "ru";
const char kLangEnglish[] = "en";
} // namespace

QString HelpWindow::fileNameForLanguage(const QString &language)
{
    return language == QLatin1String(kLangEnglish)
               ? QStringLiteral("map_editor_help_en.md")
               : QStringLiteral("map_editor_help.md");
}

QString HelpWindow::locateHelpFile(const QString &fileName)
{
    const QString exeDir = QCoreApplication::applicationDirPath();

    // Next to the executable (build tree + install layout) and a few levels
    // up (in case the binary sits in a Release/ subfolder of the game dir).
    QStringList candidates;
    candidates << exeDir + QLatin1Char('/') + fileName;
    candidates << exeDir + QStringLiteral("/../../") + fileName;
    candidates << exeDir + QStringLiteral("/../../../") + fileName;
    candidates << exeDir + QStringLiteral("/../../../../") + fileName;
    candidates << QDir::currentPath() + QLatin1Char('/') + fileName;

    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return QString();
}

void HelpWindow::openHelp(QWidget *parent)
{
    // One live window per editor: raise the existing one on repeat presses.
    const auto existing = parent->findChild<HelpWindow *>();
    if (existing) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    HelpWindow *window = new HelpWindow(parent);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
}

HelpWindow::HelpWindow(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Perimeter Map Editor — Help"));
    resize(860, 640);

    auto *layout = new QVBoxLayout(this);

    // Language switcher: the manual ships as two Markdown files (Russian
    // original + English translation) deployed next to the executable.
    auto *topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(tr("Language:"), this));
    auto *languageBox = new QComboBox(this);
    languageBox->addItem(tr("Russian"), QString::fromLatin1(kLangRussian));
    languageBox->addItem(tr("English"), QString::fromLatin1(kLangEnglish));
    topBar->addWidget(languageBox);
    topBar->addStretch(1);
    layout->addLayout(topBar);

    browser_ = new QTextBrowser(this);
    browser_->setOpenExternalLinks(false);
    browser_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(browser_);

    // The persisted choice (Russian by default — the original manual).
    QSettings settings;
    const QString saved = settings.value(QString::fromLatin1(kSettingsKey),
                                         QString::fromLatin1(kLangRussian))
                              .toString();
    languageBox->setCurrentIndex(saved == QLatin1String(kLangEnglish) ? 1 : 0);
    loadDocument(saved);

    connect(languageBox, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                loadDocument(index == 1 ? QString::fromLatin1(kLangEnglish)
                                        : QString::fromLatin1(kLangRussian));
            });
}

void HelpWindow::loadDocument(const QString &language)
{
    language_ = language;
    QSettings settings;
    settings.setValue(QString::fromLatin1(kSettingsKey), language);

    // Keep the reading position across the language switch (proportional to
    // the document length — the two files do not align line by line).
    const int oldScroll = browser_->verticalScrollBar()->value();
    const int oldMax = browser_->verticalScrollBar()->maximum();

    QString path = locateHelpFile(fileNameForLanguage(language));
    if (path.isEmpty()) {
        // Fall back to the other language when the requested file is
        // missing (e.g. an old install without the translation).
        const QString other = language == QLatin1String(kLangEnglish)
                                  ? QString::fromLatin1(kLangRussian)
                                  : QString::fromLatin1(kLangEnglish);
        path = locateHelpFile(fileNameForLanguage(other));
    }
    if (path.isEmpty()) {
        browser_->setPlainText(
            tr("Neither 'map_editor_help.md' nor 'map_editor_help_en.md' was "
               "found.\nThey are deployed next to the editor executable; "
               "search paths start at:\n")
            + QCoreApplication::applicationDirPath());
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Help"),
                             tr("Cannot read '%1'.").arg(path));
        return;
    }
    browser_->document()->setMarkdown(QString::fromUtf8(file.readAll()));
    const int newMax = browser_->verticalScrollBar()->maximum();
    browser_->verticalScrollBar()->setValue(
        oldMax > 0 ? static_cast<int>(static_cast<long long>(oldScroll) *
                                      newMax / oldMax)
                   : 0);
}

} // namespace MapEditor

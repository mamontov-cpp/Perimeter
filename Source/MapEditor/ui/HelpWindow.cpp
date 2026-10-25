// ============================================================================
// HelpWindow implementation.
// ============================================================================

#include "HelpWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QScrollBar>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace MapEditor {

QString HelpWindow::locateHelpFile()
{
    const QString fileName = QStringLiteral("map_editor_help.md");
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
    auto *browser = new QTextBrowser(this);
    browser->setOpenExternalLinks(false);
    browser->setFrameShape(QFrame::NoFrame);
    layout->addWidget(browser);

    const QString path = locateHelpFile();
    if (path.isEmpty()) {
        browser->setPlainText(
            tr("The help file 'map_editor_help.md' was not found.\n"
               "It is deployed next to the editor executable; search paths:\n")
            + QCoreApplication::applicationDirPath());
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Help"),
                             tr("Cannot read '%1'.").arg(path));
        return;
    }
    browser->document()->setMarkdown(QString::fromUtf8(file.readAll()));
    browser->verticalScrollBar()->setValue(0);
}

} // namespace MapEditor

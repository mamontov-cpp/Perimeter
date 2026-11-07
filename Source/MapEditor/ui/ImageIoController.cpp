// ============================================================================
// ImageIoController — the T6 image import/export flows (docs/architecture.md
// §5.37). Moved nearly verbatim from MainWindow.cpp; the host-service
// indirections follow the same pattern as the other controllers: statusBar()
// -> statusMessage, updateStatusBar() -> statusBarNeedsUpdate, dialog
// parents this -> window_.
// ============================================================================

#include "ImageIoController.h"

#include "EditorSession.h"
#include "ViewportWidget.h"
#include "WorldFiles.h"
#include "WorldImages.h"

#include <vector>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QImage>
#include <QMainWindow>
#include <QMessageBox>

namespace MapEditor {

ImageIoController::ImageIoController(MapEditor::EditorSession *session,
                                     MapEditor::ViewportWidget *viewport,
                                     QMainWindow *window, QObject *parent)
    : QObject(parent),
      session_(session),
      viewport_(viewport),
      window_(window)
{
}

void ImageIoController::importMapImage()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        window_, tr("Import Image onto Map"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tga);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    // Options: which layers take the paint.
    QDialog optionsDialog(window_);
    optionsDialog.setWindowTitle(tr("Import Options"));
    auto *form = new QFormLayout(&optionsDialog);
    auto *geoBox = new QCheckBox(tr("Paint geo cells (no dam)"), &optionsDialog);
    auto *damBox = new QCheckBox(tr("Paint dam cells"), &optionsDialog);
    geoBox->setChecked(true);
    damBox->setChecked(true);
    form->addRow(geoBox);
    form->addRow(damBox);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel,
                                         &optionsDialog);
    connect(buttons, &QDialogButtonBox::accepted, &optionsDialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &optionsDialog, &QDialog::reject);
    form->addRow(buttons);
    if (optionsDialog.exec() != QDialog::Accepted) {
        return;
    }
    MapEditor::EditorSession::SurfaceImportOptions options;
    options.geoCells = geoBox->isChecked();
    options.damCells = damBox->isChecked();
    if (!options.geoCells && !options.damCells) {
        return;
    }

    bool ok = false;
    // Indexed TGA: the indices go into SurBuf as-is (any size — the core
    // samples nearest). Everything else: RGB -> nearest palette entry.
    if (path.endsWith(QStringLiteral(".tga"), Qt::CaseInsensitive)) {
        unsigned width = 0;
        unsigned height = 0;
        std::vector<unsigned char> indices;
        std::vector<unsigned char> palette;
        if (MapEditor::WorldImages::readIndexedTga(
                path.toStdWString(), width, height, indices, palette)) {
            ok = session_->importSurfaceImageIndexed(width, height, indices,
                                                     options);
        }
    }
    if (!ok) {
        QImage image(path);
        if (image.isNull()) {
            QMessageBox::warning(window_, tr("Import Image onto Map"),
                                 tr("Cannot read the image file."));
            return;
        }
        image = image.convertToFormat(QImage::Format_RGB888)
                    .scaled(static_cast<int>(session_->mapWidth()),
                            static_cast<int>(session_->mapHeight()),
                            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const int w = image.width();
        const int h = image.height();
        std::vector<unsigned char> bgr(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            const uchar *line = image.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                // RGB888 -> BGR (the game/TGA byte order).
                bgr[(static_cast<size_t>(y) * w + x) * 3 + 0] = line[x * 3 + 2];
                bgr[(static_cast<size_t>(y) * w + x) * 3 + 1] = line[x * 3 + 1];
                bgr[(static_cast<size_t>(y) * w + x) * 3 + 2] = line[x * 3 + 0];
            }
        }
        ok = session_->importSurfaceImageRgb(static_cast<unsigned>(w),
                                             static_cast<unsigned>(h), bgr,
                                             options);
    }
    if (!ok) {
        QMessageBox::warning(window_, tr("Import Image onto Map"),
                             tr("The image could not be applied to the map."));
        return;
    }
    if (viewport_) {
        viewport_->setNeeds3DRedraw();
        viewport_->update();
    }
    emit statusBarNeedsUpdate();
}

void ImageIoController::importUpTga()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        window_, tr("Import up.tga (Cover Image)"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tga);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QImage image(path);
    if (image.isNull()) {
        QMessageBox::warning(window_, tr("Import up.tga"),
                             tr("Cannot read the image file."));
        return;
    }
    image = image.convertToFormat(QImage::Format_RGB888)
                .scaled(static_cast<int>(MapEditor::WorldImages::kUpTgaSize),
                        static_cast<int>(MapEditor::WorldImages::kUpTgaSize),
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<unsigned char> bgr(static_cast<size_t>(
        MapEditor::WorldImages::kUpTgaSize *
        MapEditor::WorldImages::kUpTgaSize * 3));
    for (int y = 0; y < image.height(); ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            bgr[(static_cast<size_t>(y) * image.width() + x) * 3 + 0] =
                line[x * 3 + 2];
            bgr[(static_cast<size_t>(y) * image.width() + x) * 3 + 1] =
                line[x * 3 + 1];
            bgr[(static_cast<size_t>(y) * image.width() + x) * 3 + 2] =
                line[x * 3 + 0];
        }
    }
    const auto files = session_->currentWorldFiles();
    if (!files || !MapEditor::WorldImages::writeUpTga(files->worldDir(), bgr)) {
        QMessageBox::warning(window_, tr("Import up.tga"),
                             tr("Failed to write up.tga."));
        return;
    }
    showStatus(tr("up.tga imported (512x512, 24-bit)."), 5000);
}

void ImageIoController::importLeveledTexture()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        window_, tr("Import leveledSurfaceTexture.tga"), QString(),
        tr("TGA images (*.tga);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(window_, tr("Import leveledSurfaceTexture"),
                             tr("Cannot read the file."));
        return;
    }
    const QByteArray bytes = file.readAll();
    const auto files = session_->currentWorldFiles();
    if (!files) {
        return;
    }
    const std::vector<std::string> errors =
        MapEditor::WorldFiles::validateLeveledTexture(
            std::vector<unsigned char>(bytes.constData(),
                                       bytes.constData() + bytes.size()),
            static_cast<int>(session_->mapWidth()),
            static_cast<int>(session_->mapHeight()));
    if (!errors.empty()) {
        QString message = tr("The texture is not valid:\n");
        for (const std::string &error : errors) {
            message += QStringLiteral("\n- ") + QString::fromStdString(error);
        }
        QMessageBox::warning(window_, tr("Import leveledSurfaceTexture"), message);
        return;
    }
    if (!files->writeLeveledTexture(
            std::vector<unsigned char>(bytes.constData(),
                                       bytes.constData() + bytes.size()),
            static_cast<int>(session_->mapWidth()),
            static_cast<int>(session_->mapHeight()))) {
        QMessageBox::warning(window_, tr("Import leveledSurfaceTexture"),
                             tr("Failed to write the texture."));
        return;
    }
    // Reload the texture into the running world so the viewport shows it.
    if (session_->reloadLeveledTextureLive()) {
        showStatus(
            tr("leveledSurfaceTexture.tga imported and reloaded."), 5000);
    } else {
        showStatus(
            tr("leveledSurfaceTexture.tga imported (reload the world to see "
               "it)."), 5000);
    }
    if (viewport_) {
        viewport_->setNeeds3DRedraw();
        viewport_->update();
    }
}

void ImageIoController::exportHeightmap()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        window_, tr("Export Heightmap"), QStringLiteral("heightmap.tga"),
        tr("TGA images (*.tga)"));
    if (path.isEmpty()) {
        return;
    }
    if (MapEditor::WorldImages::exportHeightmapTga(path.toStdWString())) {
        showStatus(tr("Heightmap exported to %1.").arg(path), 5000);
    } else {
        QMessageBox::warning(window_, tr("Export Heightmap"),
                             tr("Failed to write the heightmap."));
    }
}

void ImageIoController::exportTopView()
{
    if (!session_->isWorldLoaded()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        window_, tr("Export Top View"), QStringLiteral("topview.tga"),
        tr("TGA images (*.tga)"));
    if (path.isEmpty()) {
        return;
    }
    if (session_->exportTopView(path.toStdString())) {
        showStatus(tr("Top view exported to %1.").arg(path), 5000);
    } else {
        QMessageBox::warning(window_, tr("Export Top View"),
                             tr("Failed to write the top view."));
    }
}

} // namespace MapEditor

#pragma once

#include <QObject>

class QMainWindow;

namespace MapEditor {
class EditorSession;
class ViewportWidget;

/// The T6 image import/export flows of the editor: paint the surface from
/// an image (indexed TGA or RGB -> nearest palette), replace up.tga, import
/// leveledSurfaceTexture.tga with validation, export the heightmap and the
/// top view. Extracted from MainWindow (god-object decomposition,
/// docs/architecture.md §5.37) — same signal pattern as the other
/// controllers: statusMessage replaces the statusBar() calls and
/// statusBarNeedsUpdate mirrors the window's readout refresh.
class ImageIoController : public QObject
{
    Q_OBJECT

public:
    ImageIoController(MapEditor::EditorSession *session,
                      MapEditor::ViewportWidget *viewport,
                      QMainWindow *window, QObject *parent = nullptr);

    void importMapImage();
    void importUpTga();
    void importLeveledTexture();
    void exportHeightmap();
    void exportTopView();

signals:
    void statusMessage(const QString &message, int timeoutMs);
    void statusBarNeedsUpdate();

private:
    void showStatus(const QString &message, int timeoutMs)
    {
        emit statusMessage(message, timeoutMs);
    }

    MapEditor::EditorSession *session_ = nullptr;
    MapEditor::ViewportWidget *viewport_ = nullptr;
    QMainWindow *window_ = nullptr;
};

} // namespace MapEditor

// ============================================================================
// MapEditor UI — GameDirManager implementation.
// ============================================================================

#include "GameDirManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GameDirManager::GameDirManager() = default;

// ---------------------------------------------------------------------------
// QSettings helpers
// ---------------------------------------------------------------------------

QString GameDirManager::savedGameDir() const
{
    QSettings settings;
    const QString dir = settings.value(QStringLiteral("game/dir")).toString();
    if (!dir.isEmpty() && isValidGameDir(dir)) {
        return dir;
    }
    return {};
}

void GameDirManager::saveGameDir(const QString& dir)
{
    QSettings settings;
    settings.setValue(QStringLiteral("game/dir"), dir);
    addRecent(dir);
}

// ---------------------------------------------------------------------------
// Auto-detect
// ---------------------------------------------------------------------------

QString GameDirManager::autoDetect() const
{
    // 1. Previously saved and still valid.
    const QString saved = savedGameDir();
    if (!saved.isEmpty()) {
        return saved;
    }

    // 2. Current working directory.
    const QString cwd = QDir::currentPath();
    if (hasWorldsSubdir(cwd)) {
        return QDir(cwd).absolutePath();
    }

    // 3. Executable directory.
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (hasWorldsSubdir(exeDir)) {
        return QDir(exeDir).absolutePath();
    }

    // 4. Dev build tree: walk up from the exe directory looking for a
    //    folder that contains RESOURCE/Worlds.  This covers the common
    //    layout  <repo>/build/Source/Debug/map_editor.exe  where the game
    //    resources are deployed.
    QDir dir(exeDir);
    for (int i = 0; i < 6; ++i) {
        if (hasWorldsSubdir(dir.absolutePath())) {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) {
            break;
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// Recent directories
// ---------------------------------------------------------------------------

QStringList GameDirManager::recentDirs() const
{
    QSettings settings;
    return settings.value(QStringLiteral("game/recent")).toStringList();
}

void GameDirManager::addRecent(const QString& dir)
{
    QSettings settings;
    QStringList list = settings.value(QStringLiteral("game/recent")).toStringList();

    // Remove duplicates and move to front.
    list.removeAll(dir);
    list.prepend(dir);

    // Trim to maximum size.
    while (list.size() > kMaxRecent) {
        list.removeLast();
    }

    settings.setValue(QStringLiteral("game/recent"), list);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool GameDirManager::isValidGameDir(const QString& dir)
{
    return hasWorldsSubdir(dir);
}

bool GameDirManager::hasWorldsSubdir(const QString& dir)
{
    if (dir.isEmpty()) {
        return false;
    }
    const QString worldsPath = QDir(dir).filePath(QStringLiteral("RESOURCE/Worlds"));
    return QFileInfo::exists(worldsPath);
}

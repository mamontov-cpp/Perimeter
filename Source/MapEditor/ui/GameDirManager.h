#pragma once

#include <QString>
#include <QStringList>
#include <optional>

// ============================================================================
// MapEditor UI — GameDirManager.
//
// Qt helper that:
//   - Persists the last selected game folder via QSettings.
//   - Auto-detects the game folder from well-known build output locations.
//   - Maintains a list of recently used game directories.
// ============================================================================

class GameDirManager
{
public:
    GameDirManager();

    /// Return the last saved game directory from QSettings, or empty string
    /// if none was saved or the saved path no longer exists.
    QString savedGameDir() const;

    /// Save the game directory to QSettings (key "game/dir").
    void saveGameDir(const QString& dir);

    /// Try to auto-detect the game directory.
    /// Search order:
    ///   1. QSettings saved path (if still valid).
    ///   2. Current working directory (if it has RESOURCE/Worlds/).
    ///   3. Directory of the executable (same check).
    ///   4. build/Source/Debug relative to the executable (dev tree layout).
    /// Returns empty string if nothing found.
    QString autoDetect() const;

    /// Return a list of recently used game directories (most-recent first).
    QStringList recentDirs() const;

    /// Add a directory to the recent list (deduplicates, keeps max 8 entries).
    void addRecent(const QString& dir);

    /// Validate that the given directory looks like a Perimeter game folder
    /// (contains RESOURCE/Worlds/).  Returns true when valid.
    static bool isValidGameDir(const QString& dir);

private:
    static constexpr int kMaxRecent = 8;

    /// Check whether a path contains RESOURCE/Worlds/ subdirectory.
    static bool hasWorldsSubdir(const QString& dir);
};

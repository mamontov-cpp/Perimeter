#pragma once

#include "WorldFiles.h"

#include <optional>
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// MapEditorCore — EditorSession.
//
// A non-Qt session manager that wraps vMap lifecycle:
//   - Validate and set the game directory.
//   - Scan RESOURCE/Worlds/* for available world folders.
//   - Load a world via Terra API (prepare → selectUsedWorld → fullLoad).
//   - Unload the world (worldFree).
//   - Track whether the loaded world has been modified (vMap.worldChanged).
//   - Provide file-level access to a world folder (WorldFiles).
// ============================================================================

namespace MapEditor {

/// State of the editor session.
enum class SessionState
{
    NoGame,           // No game directory selected.
    GameReady,        // Game directory is set and worlds are scanned, no world loaded.
    WorldLoaded,      // A world is loaded via Terra API.
};

/// Describes the result of a world scan.
struct WorldInfo
{
    std::string name;   // folder name inside RESOURCE/Worlds/
    std::string dirKey; // key used by Terra's wTable (full content-path)
};

/// Session manager for the map editor.  All public methods are safe to call
/// from the UI thread; Terra is NOT thread-safe and must only be accessed
/// on the UI thread.
class EditorSession
{
public:
    EditorSession();
    ~EditorSession();

    // Prevent copying — the session owns state.
    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    // -----------------------------------------------------------------------
    // Game directory management
    // -----------------------------------------------------------------------

    /// Set the game directory path.  Returns false if the path does not exist
    /// or does not contain RESOURCE/Worlds/.
    bool setGameDir(const std::string& gameDir);

    /// Return the currently set game directory, or empty string.
    const std::string& getGameDir() const;

    /// Return true if a game directory has been set and validated.
    bool hasGameDir() const;

    // -----------------------------------------------------------------------
    // World scanning
    // -----------------------------------------------------------------------

    /// Scan RESOURCE/Worlds/ and populate the world list.
    /// Requires a valid game directory; returns false and clears the list
    /// if the worlds directory does not exist.
    bool scanWorlds();

    /// Return the list of worlds discovered in the last scan.
    const std::vector<WorldInfo>& getWorldList() const;

    /// Return the number of scanned worlds.
    size_t worldCount() const;

    // -----------------------------------------------------------------------
    // World loading / unloading
    // -----------------------------------------------------------------------

    /// Load the world at the given index in the scanned list.
    /// Internally calls: prepare("RESOURCE/Worlds") → selectUsedWorld(idx) → fullLoad().
    /// Returns false on failure (error is logged via ErrH).
    bool loadWorld(size_t worldIndex);

    /// Load a world by its folder name.
    bool loadWorldByName(const std::string& worldName);

    /// Unload the current world (worldFree).  Safe to call when no world is loaded.
    void unloadWorld();

    /// Return true if a world is currently loaded (vMap.VxGBuf != nullptr).
    bool isWorldLoaded() const;

    // -----------------------------------------------------------------------
    // Change tracking
    // -----------------------------------------------------------------------

    /// Return true if the loaded world has been modified (vMap.worldChanged).
    bool isModified() const;

    /// Set the modified flag explicitly.
    void setModified(bool modified);

    // -----------------------------------------------------------------------
    // Accessors for the underlying vMap (caller must ensure a world is loaded).
    // -----------------------------------------------------------------------

    /// Return a pointer to the vMap instance used by this session.
    void* getVMap() const;

    /// Return the currently loaded world index (-1 if no world loaded).
    int32_t currentWorldIndex() const;

    /// Return the name of the currently loaded world (empty if none).
    const std::string& currentWorldName() const;

    /// Return the current session state.
    SessionState state() const;

    // -----------------------------------------------------------------------
    // File-level access to world folders (WorldFiles)
    // -----------------------------------------------------------------------

    /// WorldFiles for a scanned world folder (RESOURCE/Worlds/<name>).
    /// Returns std::nullopt when no game directory is set or the index is out
    /// of range.
    std::optional<WorldFiles> worldFilesFor(size_t worldIndex) const;

    /// WorldFiles for the world that is currently loaded/selected.
    std::optional<WorldFiles> currentWorldFiles() const;

    // -----------------------------------------------------------------------
    // World saving (T1.3) / Save As (T1.4)
    // -----------------------------------------------------------------------

    /// Save the currently loaded world using Terra's save pipeline plus
    /// editor-side updates (images, ini normalization). Returns true on success.
    bool saveWorld();

    /// Save the currently loaded world as a copy to a new world folder.
    /// Creates the target folder under RESOURCE/Worlds/<newWorldName> in the
    /// current game directory, copies all existing files from the source world,
    /// switches the session to operate on the new world and saves it. Returns
    /// true on success.
    bool saveWorldAs(const std::string& newWorldName);

    /// Check if there are unsaved changes that require warning before discarding.
    bool hasUnsavedChanges() const;
    /// Get a warning message for discarding unsaved changes.
    std::string getUnsavedChangesWarning() const;

    // -----------------------------------------------------------------------
    // Undo / Redo (T1.4)
    // -----------------------------------------------------------------------

    /// Undo the last terrain edit operation.
    void undo();
    /// Redo the last undone operation.
    void redo();
    /// Discard all undo/redo history.
    void clearUndoHistory();
    /// Return true if there is at least one operation to undo.
    bool canUndo() const;
    /// Return true if there is at least one operation to redo.
    bool canRedo() const;

private:
    /// Populate vMap.wTable for RESOURCE/Worlds. Terra's prepare() may only be
    /// called once per process (its landPrepare() accumulates global radius
    /// tables), so the editor calls it once and rebuilds wTable itself
    /// afterwards — including when the game directory changes.
    void refreshWorldTable();

    std::string gameDir_;
    std::vector<WorldInfo> worldList_;
    int32_t currentWorldIndex_ = -1;
    bool modified_ = false;
    SessionState state_ = SessionState::NoGame;
    std::string loadedWorldFolderName_;
};

} // namespace MapEditor

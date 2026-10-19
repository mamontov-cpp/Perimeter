#pragma once

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

private:
    std::string gameDir_;
    std::vector<WorldInfo> worldList_;
    int32_t currentWorldIndex_ = -1;
    bool modified_ = false;
    SessionState state_ = SessionState::NoGame;
};

} // namespace MapEditor

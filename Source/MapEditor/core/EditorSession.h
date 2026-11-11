#pragma once

#include "WorldFiles.h"
#include "WorldParams.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
    /// Non-empty when the world comes from a mod (Mods/<modName>/...).
    std::string modName;
    /// "Mod / World" for mod worlds, plain name for base game worlds.
    std::string displayName() const
    {
        return modName.empty() ? name : modName + " / " + name;
    }
};

/// Snapshot of the four voxel buffers (VxG, VxD, Atr, Sur) over one rectangle.
/// Made *before* the rectangle is edited; undo() swaps it back (mirrors Terra's
/// sPreChangedArea, See docs/context.md — Terra's own recording is
/// `_SURMAP_`-only and therefore unusable inside the editor).
struct UndoAreaSnapshot
{
    int x = 0;
    int y = 0;
    int sx = 0; // clamped width  in cells (0..H_SIZE)
    int sy = 0; // clamped height in cells (0..V_SIZE)
    std::vector<uint16_t> geo; // целые высоты гео-слоя (VxWholeHeight)
    std::vector<uint16_t> dam; // целые высоты дам-слоя (VxWholeHeight)
    std::vector<unsigned char> atr;
    std::vector<unsigned char> sur;
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
    bool isWorldLoaded() const noexcept;

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
    /// Name of the currently loaded world (empty when none). Returned BY
    /// VALUE on purpose: a reference into worldList_ would dangle when
    /// scanWorlds() rebuilds the list while a caller holds it.
    std::string currentWorldName() const;

    /// Map dimensions of the loaded world (0 when none is loaded).
    unsigned mapWidth() const;
    unsigned mapHeight() const;

    /// T6.4 — reload leveledSurfaceTexture.tga into the loaded world after
    /// the file was replaced (delLeveledTexture + loadLeveledTexture).
    /// Returns true when the texture is back in memory.
    bool reloadLeveledTextureLive();

    /// T8.3 — visible surface height at a map cell (SGetAlt >> VX_FRACTION,
    /// whole voxels, dam priority) for the .spg spawn z-correction.
    /// Returns -1 when no world is loaded or the cell is out of range.
    int terrainHeightAt(int x, int y) const;

    /// The exact visible surface height in VOXELS as a double — the raw
    /// 1/32 fixed-point value (Terra's VX_FRACTION), dam priority. For
    /// readouts that show the fraction (e.g. the status-bar cursor info).
    /// Returns -1 when no world is loaded or the cell is out of range.
    double terrainHeightExact(int x, int y) const;

    /// One-cell terrain sample for the MCP server / scripting UIs: whole
    /// voxel geo height, the top-surface height while a dam layer exists
    /// (0 where dam-free), the SurBuf surface index with its Geo/Dam
    /// palette RGB, and the raw attribute byte. False when no world is
    /// loaded or (x, y) is outside the map.
    struct TerrainSample
    {
        int geoHeight = 0;
        int damHeight = 0;   ///< 0 where the cell has no dam layer.
        int surfaceIndex = 0;
        unsigned int attributes = 0;
        unsigned int geoRgb = 0; ///< 0xRRGGBB of surfaceIndex in the geo palette.
        unsigned int damRgb = 0; ///< 0xRRGGBB of surfaceIndex in the dam palette.
    };
    bool terrainSample(int x, int y, TerrainSample& out) const;

    /// T8.4 — the loaded world's zero-layer height (vMap.hZeroPlast), used
    /// as the initial manualData.zeroLayerHeight of a new battle map.
    /// Returns -1 when no world is loaded.
    int zeroLayerHeight() const;

    /// Return the current session state.
    SessionState state() const noexcept;

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
    [[nodiscard]] bool saveWorld();

    /// Save the currently loaded world as a copy to a new world folder.
    /// Creates the target folder under RESOURCE/Worlds/<newWorldName> in the
    /// current game directory, copies all existing files from the source world,
    /// switches the session to operate on the new world and saves it. Returns
    /// true on success.
    [[nodiscard]] bool saveWorldAs(const std::string& newWorldName);

    /// Check if there are unsaved changes that require warning before discarding.
    bool hasUnsavedChanges() const;
    /// Get a warning message for discarding unsaved changes.
    std::string getUnsavedChangesWarning() const;

    // -----------------------------------------------------------------------
    // Undo / Redo (T1.4)
    //
    // The history lives in the session (not in Terra): Terra records
    // pre-changed areas only under _SURMAP_, which the editor cannot define.
    // Tools that edit the world (T3 brushes and later image import) must call
    // recordUndoArea() with the rectangle they are about to touch; undo()/redo()
    // then swap the whole pre-change snapshot back, exactly like Terra's own
    // dispatcher swaps sPreChangedArea contents.
    // -----------------------------------------------------------------------

    /// Snapshot the current voxel buffers over the given rectangle (clipped to
    /// the map). Call this BEFORE modifying VxGBuf/VxDBuf/AtrBuf/SurBuf.
    /// Any snapshot previously undone is discarded (it becomes unreachable).
    /// Consecutive calls land in ONE undo transaction while they arrive within
    /// the coalescing window (see setUndoCoalescingWindowMs()) — a stroke of
    /// brush stamps is undone by a single undo().
    void recordUndoArea(int x, int y, int width, int height);

    /// Begin an undo group: recordUndoArea() calls collect their snapshots
    /// instead of pushing them, and endUndoGroup() merges them into ONE undo
    /// entry covering the union rectangle (earliest-covering snapshot wins
    /// per cell, so the merged entry restores the exact pre-group state).
    /// A new group discards the redo tail, exactly like a fresh edit.
    /// Used by the scripted brush: one stroke = one undo step no matter how
    /// many world edits the script performs.
    void beginUndoGroup();
    /// Merge the collected snapshots into a single undo entry. No-op (returns
    /// false) when the group recorded nothing. Must follow beginUndoGroup().
    /// With a non-zero coalescing window the entry stays "pending" afterwards,
    /// so edits that follow within the window join the same transaction.
    bool endUndoGroup();
    /// True between beginUndoGroup() and endUndoGroup().
    bool isUndoGroupActive() const;

    /// Close the currently pending undo transaction (if any) so the next
    /// recordUndoArea() starts a fresh one regardless of the 500 ms window.
    /// Called implicitly on undo()/redo(); useful as an explicit boundary
    /// (e.g. between two logically separate edits that happen to be close in
    /// time, or from tests).
    void breakUndoCoalescing();

    /// Width of the undo coalescing window in milliseconds: undoable actions
    /// that follow one another within this period form ONE undo transaction.
    /// 500 by default; 0 disables coalescing (every recordUndoArea() is its
    /// own undo entry — the pre-coalescing behaviour, used by the tests).
    void setUndoCoalescingWindowMs(int ms);
    int undoCoalescingWindowMs() const;

    /// Undo the last terrain edit operation (restore the last not-yet-undone
    /// snapshot and mark the area for re-render). Returns true if an operation
    /// was undone.
    bool undo();
    /// Redo the last undone operation. Returns true if it was redone.
    bool redo();
    /// Discard all undo/redo history.
    void clearUndoHistory();
    /// Return true if there is at least one operation to undo.
    bool canUndo() const;
    /// Return true if there is at least one operation to redo.
    bool canRedo() const;

    /// regRender() wrapper used for every world edit/restore in the editor:
    /// Terra's RenderStr renders the shadow continuation westward beyond a
    /// dirty rect on its own (while prevShadow persists, LAND.cpp:1469), but
    /// those extra cells never enter changedAreas and are never re-rendered
    /// when the terrain drops back — undoing a raise leaves stale dark
    /// lighting (At_SHADOW-flagged) there, which shows as black stripes in
    /// both 3D views (they read RnrBuf directly). This helper walks the same
    /// At_SHADOW trail west of the rect per row and marks the trail region
    /// for re-render as well, so the stale shadow cells get recomputed and
    /// repainted together with the edited area.
    void regRenderWithShadowTrail(int lowX, int lowY, int hiX, int hiY);

    // -----------------------------------------------------------------------
    // Difference mode (.gmp) — terrain differences carried beside the base
    // world, in the game's legacy .gmp format (see loadGameMap/saveGameMap,
    // VMAP.cpp:952-1090).
    //
    // OFF (default): vMap buffers hold the BASE terrain; brushes edit the
    // base world itself.
    // ON: vMap buffers hold BASE+DIFF (what the game would show with the
    // .gmp applied); every terrain edit re-syncs the difference model over
    // the edited rect (via the regRenderWithShadowTrail hook), so brushes
    // effectively write difference data. Toggling swaps the whole buffer
    // content and CLEARS the undo history (snapshots from the other state
    // would corrupt the display).
    // -----------------------------------------------------------------------

    /// Switch the difference mode. Returns false when no world is loaded.
    bool setDiffMode(bool on);
    bool isDiffMode() const { return diffMode_; }
    /// True when the difference model holds any cells.
    bool hasDiffData() const { return !diffAreas_.empty(); }
    /// True when the difference data was edited (or loaded) since the last
    /// .gmp save — what the exit flow offers to save instead of the world.
    bool hasUnsavedDiffChanges() const { return diffModified_; }

    /// Write the current difference model in the game's .gmp format
    /// (works in both modes). False on write error. Clears the unsaved-
    /// difference flag.
    bool saveDiffToGmp(const std::string& path);
    /// Replace the difference model with a .gmp file (the game's format;
    /// absolute per-area values, overlaid on the base terrain). When the
    /// mode is ON the display updates immediately. False on parse error.
    bool loadDiffFromGmp(const std::string& path);

    /// Export the top-down preview TGA of the BASE world (what the game's
    /// map-selection screen shows — it never applies .gmp), regardless of
    /// the difference mode. False on write error.
    bool exportTopView(const std::string& path);
    /// Regenerate cache.tga/map.tga from the BASE terrain (see exportTopView).
    bool regenerateWorldImages();

    // -----------------------------------------------------------------------
    // T5.1 — world parameters (WorldParams / WorldSettingsDialog)
    // -----------------------------------------------------------------------

    /// Snapshot of the current parameters of the loaded world: live values
    /// (hZeroPlast from vMap, f3d scaling/function) mixed with the file state
    /// (generation powers, storage compression from the output.vmp header).
    /// std::nullopt when no world is loaded or world.ini is unreadable.
    std::optional<WorldParams> currentWorldParams() const;

    /// Apply the parameters the World Settings dialog changed:
    ///   hZeroPlast  -> vMap (live, marks the world modified; saveWorld
    ///                  persists it into world.ini)
    ///   3DF params  -> f3d.setKM/setFunction live + f3d.saveVariable()
    ///                  (world.ini via Terra's own writer)
    ///   powers      -> world.ini only: the loaded buffers keep their sizes,
    ///                  new values take effect on reload / terrain generation
    ///   compression -> session target: output.vmp is converted immediately
    ///                  and every save re-applies the format after save3Buf()
    /// Returns false when no world is loaded or a file write failed.
    [[nodiscard]] bool applyWorldSettings(const WorldParams& params);

    /// Target output.vmp compression (0 = S2T0, 1 = S2T1) that saveWorld()
    /// re-applies after Terra writes the buffers as S2T0.
    int vmpCompressionTarget() const { return vmpCompressionTarget_; }

    // -----------------------------------------------------------------------
    // T6.3 — surface image import. The image is sampled nearest per map cell
    // (scale it to the map size in the UI beforehand for smooth results);
    // every cell receives the nearest palette entry of its own layer
    // (GeoPal for geo cells / DamPal for dam cells — the T4.3 gating).
    // The whole map is recorded as one undo step before the change.
    // -----------------------------------------------------------------------

    struct SurfaceImportOptions
    {
        bool geoCells = true; ///< paint cells without dams.
        bool damCells = true; ///< paint dam cells.
    };

    /// 24-bit BGR source (top-left origin, imgWidth x imgHeight): every
    /// pixel maps to the nearest palette index of the cell's layer.
    bool importSurfaceImageRgb(unsigned imgWidth, unsigned imgHeight,
                               const std::vector<unsigned char>& bgr,
                               const SurfaceImportOptions& options);

    /// Indexed source (imgWidth x imgHeight palette indices): indices are
    /// written into SurBuf as-is.
    bool importSurfaceImageIndexed(unsigned imgWidth, unsigned imgHeight,
                                   const std::vector<unsigned char>& indices,
                                   const SurfaceImportOptions& options);

    // -----------------------------------------------------------------------
    // T3.3 — SurBuf / palette read helpers for the Qt UI (eyedropper,
    // color swatch). These avoid pulling terra.h into Qt translation units.
    // -----------------------------------------------------------------------

    /// Read the SurBuf palette index at a map cell (0..255).
    /// Returns 0 when the world is not loaded or the cell is out of range.
    unsigned char surBufAt(int x, int y) const;

    /// Palette of one material layer (T4.1). A SurBuf value indexes the
    /// palette of the cell's own layer directly — the render tables
    /// Sur2Col32[SurBuf][...] are built exactly that way
    /// (vrtMap::convertPal2TableSurCol, VMAP.cpp:1367): dam-free cells take
    /// GeoPal[SurBuf], dam cells (VxDBuf != 0) take DamPal[SurBuf]. Makes
    /// no sense to bake a 0..127/128..255 split into the index — real dam
    /// SurBuf values cover the whole 0..255 range in shipped worlds — so the
    /// helpers below take the layer explicitly.
    enum class PaletteLayer
    {
        Geo, ///< vrtMap::GeoPal (inGeo.act + geoPal.xml).
        Dam, ///< vrtMap::DamPal (inDam.act).
    };

    /// Read RGB of one palette entry as 0x00RRGGBB.
    /// Returns 0 when the world is not loaded or the index is out of range.
    unsigned int paletteRgb(PaletteLayer layer, int paletteIndex) const;

    /// The raw palette bytes of one layer (768 RGB bytes, the layout of
    /// vrtMap::GeoPal/DamPal — `palette[i*3+0]` is red). Empty when no world
    /// is loaded. Used by ImageDecal::prepare(), which needs the whole table.
    std::vector<unsigned char> paletteBytes(PaletteLayer layer) const;

    /// ImageDecal — 256 flags: 1 when the loaded world's dam layer paints at
    /// least one cell with that Dam palette entry. The decal conversion never
    /// overwrites a colour that is in use, and needs to know how many entries
    /// are free before it can add new ones. All zeros when no world is loaded.
    std::vector<unsigned char> damPaletteUsage() const;

    /// The layer whose palette the SurBuf value at the cell belongs to
    /// (dam ⇔ VxDBuf != 0). Returns false / Geo when out of range/not loaded.
    bool cellIsDam(int x, int y) const;

    // -----------------------------------------------------------------------
    // T4.1 — palette editing. The palette buffers live in vrtMap (GeoPal /
    // DamPal); the Qt UI reaches them only through these helpers.
    // -----------------------------------------------------------------------

    /// Write RGB (0x00RRGGBB) into the palette entry of `layer` (0..255).
    /// Only records the change — rebuild the surface color tables with
    /// applyPalettes() to make the viewport show it. Returns false when the
    /// world is not loaded or the index is out of range.
    bool setPaletteRgb(PaletteLayer layer, int paletteIndex, unsigned int rgb);

    /// Rebuild vrtMap's surface color tables from the current GeoPal/DamPal
    /// bytes (vrtMap::convertPal2TableSurCol for both palettes — the same
    /// call loadGeoDamPal performs after loading). The next viewport repaint
    /// then shows the edited palette. Returns false when no world is loaded.
    bool applyPalettes();

    /// True when the palette was changed since the last load/save (palette
    /// edits are outside the undo history, so this is tracked separately).
    bool isPaletteModified() const noexcept { return paletteModified_; }

private:
    /// Re-derive modified_ from the undo position vs. the position recorded at
    /// the last successful save. Called after every undo/redo.
    /// All undo-state access (undo stack, pending group, positions) is
    /// serialized through undoMutex_ — recordUndoArea()/undo()/redo() may be
    /// invoked while the world is being edited, so the history must stay
    /// consistent even if a caller runs outside the UI thread.
    void markModifiedAgainstSavedState();

    /// Merge the pending transaction (undoGroup_) into ONE undo-stack entry
    /// and push it. No-op when nothing is pending. Caller must hold
    /// undoMutex_.
    void finalizePendingUndoGroup();

    /// Difference model: one entry per coarse 64x64 area (kmGridChA) that
    /// holds at least one cell differing from the base terrain. The blocks
    /// are ABSOLUTE buffer values (the .gmp format), keyed by the area
    /// index (row-major over the coarse grid). Geo/dam are 16-bit whole
    /// heights (VxWholeHeight) so difference data survives worlds above
    /// 255 voxels; the on-disk legacy section stays 8-bit, full heights go
    /// to an "XH16" trailer (saveDiffToGmp, mirrors vrtMap::saveGameMap).
    struct DiffArea
    {
        std::vector<uint16_t> geo;
        std::vector<uint16_t> dam;
        std::vector<unsigned char> atr;
        std::vector<unsigned char> sur;
    };
    std::map<int, DiffArea> diffAreas_;
    /// The difference data changed since the last saveDiffToGmp (or load).
    bool diffModified_ = false;
    /// Base terrain snapshot, kept while the difference mode is ON (vMap
    /// buffers then carry BASE+DIFF). Geo/dam are 16-bit (VxWholeHeight).
    std::vector<uint16_t> baseGeo_;
    std::vector<uint16_t> baseDam_;
    std::vector<unsigned char> baseAtr_;
    std::vector<unsigned char> baseSur_;
    bool diffMode_ = false;

    /// Re-sync the difference model with the current buffers over the rect
    /// (only meaningful while the mode is ON).
    void resyncDiffRect(int x0, int y0, int x1, int y1);
    /// Buffers := base snapshot + difference model (full re-render needed).
    void applyDiffToBuffers();
    /// Drop the difference state (world unload/switch).
    void clearDiffState();
    /// The Terra/WorldFiles part of saveWorld: buffers -> output.vmp etc.
    /// Caller guarantees the buffers carry the BASE terrain (saveWorld runs
    /// this through runWithBaseTerrain).
    bool writeWorldFiles(const WorldFiles& files,
                         const std::filesystem::path& worldDir);
    /// Run `fn` with vMap buffers holding the BASE terrain. In difference
    /// mode the effective content is swapped out for the duration and
    /// restored afterwards (full re-render); with the mode off `fn` runs
    /// as-is. Used for the top-down previews (map.tga / Export Top View):
    /// the game's map-selection screen shows the base world and never
    /// applies .gmp, so the previews must not carry difference data.
    void runWithBaseTerrain(const std::function<void()>& fn);

    /// Total snapshot cells currently collected in the pending transaction.
    /// Caller must hold undoMutex_.
    std::size_t pendingGroupCells() const;

    /// Populate vMap.wTable for RESOURCE/Worlds. Terra's prepare() may only be
    /// called once per process (its landPrepare() accumulates global radius
    /// tables), so the editor calls it once and rebuilds wTable itself
    /// afterwards — including when the game directory changes.
    void refreshWorldTable();

    /// Enumerates the world folders of the base game PLUS every mod
    /// (Mods/<Mod>/RESOURCE/Worlds/<World>); `fn(name, dirKey, modName)` runs
    /// per world (modName empty for base worlds). The content map already
    /// holds the mod folders under "mods/..." keys after the "./" scan, so
    /// dirKey is the exact key Terra resolves the world's files through.
    void forEachWorldFolder(
        const std::function<void(const std::string&, const std::string&,
                                 const std::string&)>& fn) const;

    std::string gameDir_;
    std::vector<WorldInfo> worldList_;
    int32_t currentWorldIndex_ = -1;
    bool modified_ = false;
    /// T4.1 — the palette was edited since the last load/save (outside the
    /// undo history; synced to inGeo.act/inDam.act + geoPal.xml on save).
    bool paletteModified_ = false;
    /// T5.1 — output.vmp compression to (re)apply on save (0 = S2T0,
    /// 1 = S2T1); initialised from the loaded world's file header.
    int vmpCompressionTarget_ = 0;
    SessionState state_ = SessionState::NoGame;
    std::string loadedWorldFolderName_;

    // Editor-side undo stack (T1.4). undoPos_ is the number of snapshots that
    // are currently "applied" (not undone); entries at/after undoPos_ hold
    // their original (pre-change) bytes when undone and post-change bytes when
    // redone — the same swap semantics as vrtMap's preCAs list.
    // Everything in this block is guarded by undoMutex_.
    mutable std::mutex undoMutex_;
    std::vector<UndoAreaSnapshot> undoStack_;
    size_t undoPos_ = 0;
    /// Undo-stack position (undoPos_) recorded at the last successful save.
    /// Valid only while hasSavedUndoPos_ is true.
    size_t savedUndoPos_ = 0;
    bool hasSavedUndoPos_ = false;
    /// Undo-group collection (see beginUndoGroup()): while active,
    /// recordUndoArea() appends here instead of pushing entries.
    bool undoGroupActive_ = false;
    std::vector<UndoAreaSnapshot> undoGroup_;
    /// Pending undo transaction: with a non-zero coalescing window the
    /// snapshots of undoGroup_ are NOT pushed as their own stack entry but
    /// kept here, so further undoable actions arriving within the window
    /// merge into the same transaction. The transaction is closed (merged
    /// into undoStack_ by finalizePendingUndoGroup()) when the window
    /// expires, on undo()/redo(), on clearUndoHistory() or when it grows past
    /// the memory budget.
    bool undoPendingOpen_ = false;
    std::chrono::steady_clock::time_point lastUndoActionTime_{};
    /// Coalescing window width, milliseconds (0 = every record is its own
    /// entry). See setUndoCoalescingWindowMs().
    int undoCoalescingWindowMs_ = 500;

    /// Bound the undo stack by kMaxUndoCells: drop the oldest entries in ONE
    /// range erase (per-entry erase(begin) would re-move the tail on every
    /// iteration) and shift undoPos_/savedUndoPos_ accordingly.
    void trimUndoStack();
};

} // namespace MapEditor

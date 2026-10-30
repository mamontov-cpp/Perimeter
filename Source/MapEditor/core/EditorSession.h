#pragma once

#include "WorldFiles.h"
#include "WorldParams.h"

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
    std::vector<unsigned char> geo;
    std::vector<unsigned char> dam;
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

    /// T8.4 — the loaded world's zero-layer height (vMap.hZeroPlast), used
    /// as the initial manualData.zeroLayerHeight of a new battle map.
    /// Returns -1 when no world is loaded.
    int zeroLayerHeight() const;

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
    void recordUndoArea(int x, int y, int width, int height);

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
    bool applyWorldSettings(const WorldParams& params);

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
    bool isPaletteModified() const { return paletteModified_; }

private:
    /// Re-derive modified_ from the undo position vs. the position recorded at
    /// the last successful save. Called after every undo/redo.
    void markModifiedAgainstSavedState();

    /// Populate vMap.wTable for RESOURCE/Worlds. Terra's prepare() may only be
    /// called once per process (its landPrepare() accumulates global radius
    /// tables), so the editor calls it once and rebuilds wTable itself
    /// afterwards — including when the game directory changes.
    void refreshWorldTable();

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
    std::vector<UndoAreaSnapshot> undoStack_;
    size_t undoPos_ = 0;
    /// Undo-stack position (undoPos_) recorded at the last successful save.
    /// Valid only while hasSavedUndoPos_ is true.
    size_t savedUndoPos_ = 0;
    bool hasSavedUndoPos_ = false;
};

} // namespace MapEditor

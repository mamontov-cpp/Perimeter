// ============================================================================
// MapEditorCore — EditorSession implementation.
// ============================================================================

#include "EditorSession.h"
#include "PaletteOps.h"
#include "ViewportRenderer.h"
#include "WorldImages.h"
#include "WorldParams.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "EditorTrace.h"

// Terra / XTool headers — needed for vMap and content-path API.
// _PERIMETER_ and _TX3D_LIBRARY_ are defined by CMake.
#include "terra.h"
#include "files/files.h"
#include "xerrhand.h"

namespace fs = std::filesystem;

namespace MapEditor {

// init_content_root is provided by shim/content_root.cpp (MapEditorSupport).
extern void init_content_root();

namespace {

// Terra's vrtMap::prepare() calls landPrepare(), which accumulates the global
// radius tables (maxRad[]) and leaks the previous xRad/yRad heaps. Calling it a
// second time therefore overflows those heaps and crashes the process, so it may
// run at most once per process.
//
// We cannot rely on a flag of our own: other code (the game, tests) may call
// vMap.prepare() directly, bypassing us. Instead we detect Terra's own state —
// landPrepare() leaves xRad[0] pointing at the heap it built — so the decision
// is shared with every caller in the process. See docs/context.md.
bool terraLandPrepared()
{
    return xRad[0] != nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

EditorSession::EditorSession() = default;

EditorSession::~EditorSession()
{
    editorTrace("EditorSession: dtor start");
    // Unload any open world before destruction so Terra frees its buffers cleanly.
    unloadWorld();
    editorTrace("EditorSession: dtor done");
}

// ---------------------------------------------------------------------------
// Game directory management
// ---------------------------------------------------------------------------

bool EditorSession::setGameDir(const std::string& gameDir)
{
    // Validate the directory exists.
    if (!fs::exists(gameDir)) {
        fprintf(stderr, "EditorSession: game directory does not exist: %s\n", gameDir.c_str());
        return false;
    }
    if (!fs::is_directory(gameDir)) {
        fprintf(stderr, "EditorSession: game path is not a directory: %s\n", gameDir.c_str());
        return false;
    }

    // Must contain RESOURCE/Worlds.
    const fs::path worldsPath = fs::path(gameDir) / "RESOURCE" / "Worlds";
    if (!fs::exists(worldsPath) || !fs::is_directory(worldsPath)) {
        fprintf(stderr, "EditorSession: RESOURCE/Worlds not found in: %s\n", gameDir.c_str());
        return false;
    }

    // Unload any currently loaded world before switching directory.
    unloadWorld();

    gameDir_ = fs::absolute(fs::u8path(gameDir)).lexically_normal().u8string();
    state_ = SessionState::NoGame;
    worldList_.clear();

    printf("EditorSession: game directory set to '%s'\n", gameDir_.c_str());
    return true;
}

const std::string& EditorSession::getGameDir() const
{
    return gameDir_;
}

bool EditorSession::hasGameDir() const
{
    return !gameDir_.empty();
}

// ---------------------------------------------------------------------------
// World scanning
// ---------------------------------------------------------------------------

bool EditorSession::scanWorlds()
{
    worldList_.clear();

    if (!hasGameDir()) {
        fprintf(stderr, "EditorSession: cannot scan worlds — no game directory set.\n");
        return false;
    }

    // Change the working directory to the game root so that content-root
    // scanning finds RESOURCE/.  This mirrors what the test harness does.
    fs::current_path(fs::u8path(gameDir_));
    init_content_root();

    // Base worlds plus mod worlds (Mods/<Mod>/RESOURCE/Worlds/<World>).
    // Uses Terra's content-entry API like vrtMap::prepare("RESOURCE/Worlds").
    forEachWorldFolder([this](const std::string& name, const std::string& key,
                              const std::string& modName) {
        WorldInfo wi;
        wi.name = name;
        wi.dirKey = key;
        wi.modName = modName;
        worldList_.push_back(std::move(wi));
    });

    // Sort by mod then name for a stable UI list (base worlds first).
    std::sort(worldList_.begin(), worldList_.end(),
              [](const WorldInfo& a, const WorldInfo& b) {
                  if (a.modName != b.modName) {
                      return a.modName < b.modName;
                  }
                  return a.name < b.name;
              });

    if (worldList_.empty()) {
        fprintf(stderr, "EditorSession: no worlds found in RESOURCE/Worlds of '%s'\n",
                gameDir_.c_str());
        return false;
    }

    state_ = SessionState::GameReady;
    printf("EditorSession: found %zu world(s)\n", worldList_.size());
    return true;
}

void EditorSession::forEachWorldFolder(
    const std::function<void(const std::string&, const std::string&,
                             const std::string&)>& fn) const
{
    // Base game worlds.
    for (const auto& entry : get_content_entries_directory("RESOURCE/Worlds")) {
        if (!entry->is_directory) {
            continue;
        }
        fn(fs::u8path(entry->path_content).filename().u8string(), entry->key,
           std::string());
    }

    // Mod worlds: Mods/<ModName>/RESOURCE/Worlds/<World>. The Mods folder may
    // be absent entirely. The content map already indexed it under
    // "mods/<mod>/resource/worlds/..." keys during the "./" scan, which also
    // makes the keys resolvable at load time. "Publish" is the game's own
    // staging folder, not a mod.
    const fs::path modsRoot = fs::u8path(gameDir_) / "Mods";
    std::error_code ec;
    if (!fs::is_directory(modsRoot, ec)) {
        return;
    }
    for (const auto& modIt : fs::directory_iterator(modsRoot, ec)) {
        std::error_code typeEc;
        if (!modIt.is_directory(typeEc)) {
            continue;
        }
        const std::string modName = modIt.path().filename().u8string();
        std::string modLower = modName;
        std::transform(modLower.begin(), modLower.end(), modLower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (modLower == "publish") {
            continue;
        }
        for (const auto& entry :
             get_content_entries_directory("mods/" + modLower +
                                           "/resource/worlds")) {
            if (!entry->is_directory) {
                continue;
            }
            fn(fs::u8path(entry->path_content).filename().u8string(),
               entry->key, modName);
        }
    }
}

const std::vector<WorldInfo>& EditorSession::getWorldList() const
{
    return worldList_;
}

size_t EditorSession::worldCount() const
{
    return worldList_.size();
}

// ---------------------------------------------------------------------------
// World loading / unloading
// ---------------------------------------------------------------------------

void EditorSession::refreshWorldTable()
{
    // Terra's prepare() initialises the circular-brush radius tables through
    // landPrepare(), which leaks and accumulates global state (maxRad) and may
    // therefore run only once per process — so it is called at most once,
    // only for those tables.
    if (!terraLandPrepared()) {
        vMap.prepare("RESOURCE/Worlds");
    }

    // wTable is ALWAYS rebuilt here: prepare() enumerates only the base
    // RESOURCE/Worlds, while the editor's list also carries mod worlds
    // (Mods/<Mod>/RESOURCE/Worlds). A mod world loaded as the FIRST world of
    // the process must resolve just like any other.
    vMap.wTable.clear();
    forEachWorldFolder([](const std::string& name, const std::string& key,
                          const std::string&) {
        vMap.wTable.emplace_back(name, key);
    });
    vMap.maxWorld = vMap.wTable.size();
}

bool EditorSession::loadWorld(size_t worldIndex)
{
    editorTrace("EditorSession: loadWorld(%zu) start", worldIndex);
    if (worldIndex >= worldList_.size()) {
        fprintf(stderr, "EditorSession: world index %zu out of range (%zu worlds)\n",
                worldIndex, worldList_.size());
        editorTrace("EditorSession: loadWorld FAILED — index out of range");
        return false;
    }

    // Unload the previous world if any.
    if (isWorldLoaded()) {
        editorTrace("EditorSession: loadWorld — unloading previous world");
        unloadWorld();
    }

    // Ensure the content root points to the game directory.
    editorTrace("EditorSession: loadWorld setting content root");
    fs::current_path(fs::u8path(gameDir_));
    init_content_root();

    // Populate Terra's world table (safe to call repeatedly).
    editorTrace("EditorSession: loadWorld refreshWorldTable");
    refreshWorldTable();
    editorTrace("EditorSession: loadWorld maxWorld=%d", vMap.maxWorld);
    if (vMap.maxWorld < 1) {
        fprintf(stderr, "EditorSession: no worlds found in RESOURCE/Worlds.\n");
        editorTrace("EditorSession: loadWorld FAILED — no worlds");
        return false;
    }

    // Find the Terra index for our world. Match by the content key first —
    // a mod world may share its folder name with a base world, so the
    // name-based getWorldID is only a fallback.
    const std::string& wantedName = worldList_[worldIndex].name;
    const std::string& wantedKey = worldList_[worldIndex].dirKey;
    int terraIdx = -1;
    for (int i = 0; i < vMap.maxWorld; ++i) {
        if (vMap.wTable[static_cast<std::size_t>(i)].dir == wantedKey) {
            terraIdx = i;
            break;
        }
    }
    if (terraIdx < 0) {
        terraIdx = vMap.getWorldID(wantedName.c_str());
    }
    editorTrace("EditorSession: loadWorld lookup('%s')=%d", wantedName.c_str(), terraIdx);
    if (terraIdx < 0) {
        fprintf(stderr, "EditorSession: world '%s' not found in Terra wTable.\n",
                wantedName.c_str());
        editorTrace("EditorSession: loadWorld FAILED — lookup returned -1");
        return false;
    }

    // Load the world: analyzeINI + worldPrepare + load data.
    editorTrace("EditorSession: selectUsedWorld(%d)", terraIdx);
    vMap.selectUsedWorld(terraIdx);
    editorTrace("EditorSession: fullLoad('%s') start", wantedName.c_str());
    vMap.fullLoad();
    editorTrace("EditorSession: fullLoad done (IsFullLoad=%d)",
                vMap.IsFullLoad() ? 1 : 0);

    if (!vMap.IsFullLoad()) {
        fprintf(stderr, "EditorSession: fullLoad() failed for world '%s'.\n",
                wantedName.c_str());
        return false;
    }

    // Compute RnrBuf lighting and At_SHADOW exactly like
    // terUniverse::universalLoad does after loading a world
    // (Source/Game/Universe.cpp): fullLoad() itself never runs the renderer —
    // its RenderStr() loop is commented out (VMAP.cpp:924) — so without this
    // call getColor32() (viewport, cache.tga) would read uninitialised
    // lighting. See docs/context.md.
    editorTrace("EditorSession: WorldRender start");
    vMap.WorldRender();
    editorTrace("EditorSession: WorldRender done");

    clearUndoHistory();
    clearDiffState(); // difference data belongs to the previous world
    currentWorldIndex_ = static_cast<int32_t>(worldIndex);
    loadedWorldFolderName_ = wantedName;
    modified_ = false;
    paletteModified_ = false;
    vMap.worldChanged = false;
    state_ = SessionState::WorldLoaded;

    // T5.1 — remember the on-disk output.vmp compression so Save keeps it.
    editorTrace("EditorSession: loadWorld getting currentWorldFiles");
    if (const auto files = currentWorldFiles()) {
        editorTrace("EditorSession: loadWorld vmpCompressionOfFile");
        vmpCompressionTarget_ = std::max(
            0, vmpCompressionOfFile(files->filePath(WorldFiles::OutputVmpFile)));
    } else {
        editorTrace("EditorSession: loadWorld no currentWorldFiles");
        vmpCompressionTarget_ = 0;
    }

    editorTrace("EditorSession: loadWorld done, world '%s' (%u x %u)",
            wantedName.c_str(), vMap.H_SIZE, vMap.V_SIZE);
    printf("EditorSession: loaded world '%s' (%u x %u)\n",
            wantedName.c_str(), vMap.H_SIZE, vMap.V_SIZE);
    return true;
}

bool EditorSession::loadWorldByName(const std::string& worldName)
{
    // Prefer the base game world on a name clash with a mod world.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < worldList_.size(); ++i) {
            if (worldList_[i].name == worldName &&
                (pass == 1 || worldList_[i].modName.empty())) {
                return loadWorld(i);
            }
        }
    }
    fprintf(stderr, "EditorSession: world '%s' not in scanned list.\n", worldName.c_str());
    return false;
}

void EditorSession::unloadWorld()
{
    clearDiffState();
    if (vMap.VxGBuf != nullptr) {
        // Mirror vrtMap's own teardown of a loaded world. worldFree() alone only
        // frees the net maps, so the voxel/surface buffers must be released here
        // (delLeveledTexture() has to run before VxDBuf goes away). The one-time
        // circular-brush tables from prepare() are deliberately kept.
        editorTrace("EditorSession: unloadWorld (world loaded) — delLeveledTexture");
        vMap.delLeveledTexture();
        vMap.releaseMem4Buf();
        vMap.worldFree();
        editorTrace("EditorSession: unloadWorld — vMap buffers released");
    }
    clearUndoHistory();
    currentWorldIndex_ = -1;
    loadedWorldFolderName_.clear();
    modified_ = false;
    paletteModified_ = false;
    if (isWorldLoaded() && vMap.VxGBuf) {
        // Just in case, clear flag if still set
        vMap.worldChanged = false;
    }
    if (hasGameDir() && !worldList_.empty()) {
        state_ = SessionState::GameReady;
    } else {
        state_ = SessionState::NoGame;
    }
}

bool EditorSession::isWorldLoaded() const noexcept
{
    // vMap.VxGBuf is non-null only after a successful fullLoad().
    return vMap.VxGBuf != nullptr;
}

// ---------------------------------------------------------------------------
// Change tracking
// ---------------------------------------------------------------------------

bool EditorSession::isModified() const
{
    if (!isWorldLoaded()) {
        return false;
    }
    // In difference mode vMap.worldChanged cannot be trusted: Terra's own
    // RenderPrepare1 (LAND.cpp:947) sets it on every brush stamp, while the
    // stamps write DIFFERENCE data and the world file stays untouched.
    return (diffMode_ ? false : vMap.worldChanged) || modified_;
}

void EditorSession::setModified(bool modified)
{
    modified_ = modified;
    if (isWorldLoaded()) {
        vMap.worldChanged = modified;
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

void* EditorSession::getVMap() const
{
    return &vMap;
}

int32_t EditorSession::currentWorldIndex() const
{
    return currentWorldIndex_;
}

std::string EditorSession::currentWorldName() const
{
    if (currentWorldIndex_ < 0 ||
        static_cast<size_t>(currentWorldIndex_) >= worldList_.size()) {
        return std::string();
    }
    return worldList_[static_cast<size_t>(currentWorldIndex_)].name;
}

unsigned EditorSession::mapWidth() const
{
    return isWorldLoaded() ? static_cast<unsigned>(vMap.H_SIZE) : 0u;
}

unsigned EditorSession::mapHeight() const
{
    return isWorldLoaded() ? static_cast<unsigned>(vMap.V_SIZE) : 0u;
}

bool EditorSession::reloadLeveledTextureLive()
{
    if (!isWorldLoaded()) {
        return false;
    }
    vMap.delLeveledTexture();
    return vMap.loadLeveledTexture() != 0;
}

int EditorSession::zeroLayerHeight() const
{
    return isWorldLoaded() ? static_cast<int>(vMap.hZeroPlast) : -1;
}

int EditorSession::terrainHeightAt(int x, int y) const
{
    const unsigned w = mapWidth();
    const unsigned h = mapHeight();
    if (w == 0 || x < 0 || y < 0 ||
        static_cast<unsigned>(x) >= w || static_cast<unsigned>(y) >= h) {
        return -1;
    }
    return vMap.SGetAlt(x, y) >> VX_FRACTION;
}

SessionState EditorSession::state() const noexcept
{
    return state_;
}

// ---------------------------------------------------------------------------
// File-level access to world folders (WorldFiles)
// ---------------------------------------------------------------------------

std::optional<WorldFiles> EditorSession::worldFilesFor(size_t worldIndex) const
{
    if (!hasGameDir() || worldIndex >= worldList_.size()) {
        return std::nullopt;
    }
    // dirKey is the content-relative path Terra reports for the folder, e.g.
    // "RESOURCE/Worlds/COMPASS".
    const fs::path dir = fs::u8path(gameDir_) / fs::u8path(worldList_[worldIndex].dirKey);
    return WorldFiles(dir);
}

std::optional<WorldFiles> EditorSession::currentWorldFiles() const
{
    if (currentWorldIndex_ < 0) {
        return std::nullopt;
    }
    return worldFilesFor(static_cast<size_t>(currentWorldIndex_));
}

bool EditorSession::saveWorld()
{
    if (!isWorldLoaded()) {
        fprintf(stderr, "EditorSession::saveWorld: no world loaded.\n");
        return false;
    }

    // Terra resolves every world file through the content root and vMap.cWorld,
    // so make sure both still point at the world we loaded.
    if (hasGameDir()) {
        fs::current_path(fs::u8path(gameDir_));
    }
    init_content_root();

    const std::optional<WorldFiles> files = currentWorldFiles();
    if (!files) {
        fprintf(stderr, "EditorSession::saveWorld: no world folder resolved.\n");
        return false;
    }
    const fs::path worldDir = files->worldDir();

    // The world file must carry the BASE terrain — NEVER the difference
    // data (that is what .gmp is for). In difference mode the effective
    // BASE+DIFF buffers are swapped out for the duration of the write.
    bool written = false;
    runWithBaseTerrain([&] {
        written = writeWorldFiles(*files, worldDir);
    });
    if (!written) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(undoMutex_);
        hasSavedUndoPos_ = true;
        // A pending coalescing transaction is not part of the stack yet; its
        // edits are applied to the buffers but absent from the saved file, so
        // the checkpoint correctly excludes it (finalize pushes it past
        // savedUndoPos_ and the world reads as modified again).
        savedUndoPos_ = undoPos_;
    }
    modified_ = false;
    paletteModified_ = false;
    vMap.worldChanged = false;
    printf("EditorSession: saved world '%s' to '%s'\n",
           currentWorldName().c_str(), worldDir.u8string().c_str());
    return true;
}

bool EditorSession::writeWorldFiles(const WorldFiles& files,
                                    const fs::path& worldDir)
{
    // ------------------------------------------------------------------
    // 1. Buffers, palettes and [3DF Parameters] via Terra's own pipeline.
    // ------------------------------------------------------------------
    vMap.save3Buf();

    // hardness.bin is optional: refresh it only when the world already has one,
    // so "Save" never adds files the author did not ask for.
    if (files.exists(WorldFiles::HardnessFile)) {
        vMap.saveHardness();
    }

    // ------------------------------------------------------------------
    // 1b. Edited palettes (T4.1): inGeo.act/inDam.act via Terra, plus the
    // geoPal.xml color table — in _TX3D_LIBRARY_ builds the game reloads the
    // geo palette from the XML, not from inGeo.act, so saving only the .act
    // files would silently drop the edits on the next load.
    // ------------------------------------------------------------------
    if (paletteModified_) {
        vMap.saveGeoDamPal();
        if (!files.writeGeoPalXml(PaletteOps::geoPalXml(vMap.GeoPal))) {
            fprintf(stderr, "EditorSession::saveWorld: geoPal.xml sync failed.\n");
            return false;
        }
    }

    // ------------------------------------------------------------------
    // 2. world.ini — size, zero-layer height and storage version.
    // ------------------------------------------------------------------
    // Map Power is authoritative for the map size the game allocates.
    files.writeMapPower(static_cast<int>(vMap.H_SIZE_POWER),
                        static_cast<int>(vMap.V_SIZE_POWER));
    files.writeHZeroPlast(static_cast<int>(vMap.hZeroPlast));
    if (const auto version = files.readStorageVersion()) {
        files.writeStorageVersion(*version); // keep the existing version
    } else {
        files.writeStorageVersion("1.4");
    }

    // ------------------------------------------------------------------
    // 2b. T5.1 — output.vmp compression: save3Buf() always writes S2T0,
    // so the session target (from the World Settings dialog, initialised
    // from the file header on load) is re-applied here.
    // ------------------------------------------------------------------
    files.writeStorageCompressedFormat(vmpCompressionTarget_);
    if (vmpCompressionTarget_ != 0 &&
        !convertVmpCompression(worldDir / WorldFiles::OutputVmpFile,
                               vmpCompressionTarget_)) {
        fprintf(stderr, "EditorSession::saveWorld: output.vmp compression failed.\n");
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Images the game and its shell read. (Already on the BASE terrain
    // via the caller's runWithBaseTerrain: the shell's map-selection screen
    // shows the base world's map.tga and never applies .gmp data.)
    // ------------------------------------------------------------------
    if (!WorldImages::generateCacheTga(worldDir)) {
        fprintf(stderr, "EditorSession::saveWorld: cache.tga generation failed.\n");
        return false;
    }
    if (!WorldImages::generateMapTga(worldDir)) {
        fprintf(stderr, "EditorSession::saveWorld: map.tga generation failed.\n");
        return false;
    }
    return true;
}

bool EditorSession::saveWorldAs(const std::string& newWorldName)
{
    if (!isWorldLoaded() || newWorldName.empty()) {
        fprintf(stderr, "EditorSession::saveWorldAs: invalid state or empty name.\n");
        return false;
    }
    if (!hasGameDir()) {
        fprintf(stderr, "EditorSession::saveWorldAs: no game directory set.\n");
        return false;
    }

    const fs::path worldsRoot = fs::u8path(gameDir_) / "RESOURCE" / "Worlds";
    if (!fs::exists(worldsRoot) || !fs::is_directory(worldsRoot)) {
        fprintf(stderr, "EditorSession::saveWorldAs: RESOURCE/Worlds not found.\n");
        return false;
    }

    // Prevent overwriting existing world.
    const fs::path destDir = worldsRoot / fs::u8path(newWorldName);
    if (fs::exists(destDir)) {
        fprintf(stderr, "EditorSession::saveWorldAs: target world already exists: %s\n",
                destDir.u8string().c_str());
        return false;
    }

    // Copy source world folder.
    const std::optional<WorldFiles> srcFiles = currentWorldFiles();
    if (!srcFiles) {
        fprintf(stderr, "EditorSession::saveWorldAs: cannot resolve source world folder.\n");
        return false;
    }
    const fs::path srcDir = srcFiles->worldDir();
    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        fprintf(stderr, "EditorSession::saveWorldAs: cannot create directory '%s': %s\n",
                destDir.u8string().c_str(), ec.message().c_str());
        return false;
    }

    // Copy all entries (files and subdirectories), keeping the structure.
    // One recursive fs::copy instead of a hand-rolled iterator loop — the
    // copy_options cover the directory/file cases the loop spelled out.
    ec.clear();
    fs::copy(srcDir, destDir,
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
        fprintf(stderr, "EditorSession::saveWorldAs: copying '%s' failed: %s\n",
                srcDir.u8string().c_str(), ec.message().c_str());
        return false;
    }

    // Update world.ini in the copy to reflect the new folder name if needed? 
    // Terra uses folder name as world name; world.ini has [Storage] File Name but
    // the actual loaded name is from folder/wTable. Copy is fine; we'll reload.
    {
        WorldFiles destFiles(destDir);
        // Ensure Storage/File Name matches folder name if present
        if (auto ini = destFiles.readIni()) {
            // writeIniString/writeIniInt do read-modify-write; use generic write via helper style
            destFiles.writeIniString(WorldIni::Storage, WorldIni::FileName, newWorldName);
            // Also ensure map power/zero layer copied values are consistent if desired (copied already)
        }
    }

    // ------------------------------------------------------------------
    // T4.1 — carry in-memory palette edits into the copy: the reload below
    // reads the palettes back from the new world's files, so the edited
    // buffers must be written there before unloadWorld() discards the
    // in-memory state.
    // ------------------------------------------------------------------
    if (paletteModified_) {
        WorldFiles destFiles(destDir);
        GeoDamPalettes pal;
        std::copy(vMap.GeoPal, vMap.GeoPal + GeoDamPalettes::SizeBytes,
                  pal.geo.begin());
        std::copy(vMap.DamPal, vMap.DamPal + GeoDamPalettes::SizeBytes,
                  pal.dam.begin());
        if (!destFiles.writePalettes(pal) ||
            !destFiles.writeGeoPalXml(PaletteOps::geoPalXml(vMap.GeoPal))) {
            fprintf(stderr,
                    "EditorSession::saveWorldAs: cannot transfer the edited palettes.\n");
            return false;
        }
    }

    // Switch session to the new world: reload world list and load by name.
    // Unload current first.
    unloadWorld();
    if (!scanWorlds()) {
        fprintf(stderr, "EditorSession::saveWorldAs: failed to rescan worlds after copy.\n");
        return false;
    }
    if (!loadWorldByName(newWorldName)) {
        fprintf(stderr, "EditorSession::saveWorldAs: failed to load copied world '%s'.\n",
                newWorldName.c_str());
        return false;
    }

    // Save to regenerate cache.tga/map.tga and normalize via Terra pipeline.
    if (!saveWorld()) {
        fprintf(stderr, "EditorSession::saveWorldAs: save failed after copy.\n");
        return false;
    }

    printf("EditorSession: saved world as '%s'\n", newWorldName.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Undo / Redo (T1.4)
//
// Terra's UndoDispatcher_PutPreChangedArea() only records under _SURMAP_, so
// in the editor build nothing ever enters vrtMap::preCAs and the dispatcher
// would be permanently empty. The session therefore keeps its own history with
// the same swap semantics: every snapshot stores the buffer bytes from *before*
// an edit; undo()/redo() swap snapshot <-> buffers, then regRender() marks the
// area so the viewport repaints it from dirty rectangles.
// ---------------------------------------------------------------------------

namespace {

/// Total buffer cells kept across all snapshots (same budget as Terra's
/// UNDO_REDO_BUFFER_SIZE accounting: 4096*4096 cells x 4 buffers).
constexpr std::size_t kMaxUndoCells = 4096u * 4096u;

/// Total cells a single pending undo transaction may collect before it is
/// closed even though the coalescing window is still running — an
/// uninterrupted stroke must not collect snapshots without limit.
constexpr std::size_t kMaxPendingUndoCells = kMaxUndoCells / 2;

/// Swap the snapshot bytes with the live buffers over the snapshot rectangle.
/// After a call the snapshot holds whatever was in the buffers before — the
/// caller decides whether that means "restore" (undo) or "re-apply" (redo).
void swapSnapshotWithBuffers(UndoAreaSnapshot& snap)
{
    const std::size_t cells = static_cast<std::size_t>(snap.sx) * snap.sy;
    if (snap.geo.size() != cells || snap.dam.size() != cells ||
        snap.atr.size() != cells || snap.sur.size() != cells) {
        return;
    }
    const int H = vMap.H_SIZE;
    std::size_t cnt = 0;
    for (int j = 0; j < snap.sy; ++j) {
        const int row = (snap.y + j) * H + snap.x;
        for (int i = 0; i < snap.sx; ++i, ++cnt) {
            const int off = row + i;
            unsigned char t;
            t = vMap.VxGBuf[off]; vMap.VxGBuf[off] = snap.geo[cnt]; snap.geo[cnt] = t;
            t = vMap.VxDBuf[off]; vMap.VxDBuf[off] = snap.dam[cnt]; snap.dam[cnt] = t;
            t = vMap.AtrBuf[off]; vMap.AtrBuf[off] = snap.atr[cnt]; snap.atr[cnt] = t;
            t = vMap.SurBuf[off]; vMap.SurBuf[off] = snap.sur[cnt]; snap.sur[cnt] = t;
        }
    }
}

} // namespace

void EditorSession::regRenderWithShadowTrail(int lowX, int lowY, int hiX, int hiY)
{
    if (!isWorldLoaded()) {
        return;
    }
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    const int yBeg = std::max(0, std::min(lowY, mapH - 1));
    const int yEnd = std::max(0, std::min(hiY, mapH - 1));
    const int xBeg = std::max(0, std::min(lowX, mapW - 1));

    // Walk the At_SHADOW (== At_SHADOWV, 0x20) trail west of the rect, row
    // by row: RenderStr flags every cell it rendered in shadow, so the trail
    // marks exactly the cells the incremental lighting touched outside the
    // previously rendered area (stale after the terrain dropped under them).
    int trailX = xBeg;
    for (int y = yBeg; y <= yEnd; ++y) {
        const int rowBase = y * mapW;
        for (int x = xBeg - 1; x >= 0; --x) {
            if (!(vMap.AtrBuf[rowBase + x] & At_SHADOW)) {
                // One unflagged cell past the trail is rendered too —
                // RenderStr seeds its shading from the row ahead.
                trailX = std::min(trailX, x);
                break;
            }
            trailX = std::min(trailX, x);
        }
    }

    vMap.regRender(trailX, lowY, hiX, hiY);

    // Difference mode: every world edit/restore funnels through this hook —
    // re-sync the difference model over the touched rect so the .gmp data
    // always mirrors the effective terrain.
    if (diffMode_) {
        const int mapW = static_cast<int>(vMap.H_SIZE);
        const int mapH = static_cast<int>(vMap.V_SIZE);
        resyncDiffRect(std::max(0, std::min(lowX, trailX)),
                       std::max(0, std::min(lowY, mapH - 1)),
                       std::min(mapW, std::max(hiX, 0)),
                       std::min(mapH, std::max(hiY, 0)));
    }
}

// ---------------------------------------------------------------------------
// Difference mode (.gmp)
// ---------------------------------------------------------------------------

void EditorSession::resyncDiffRect(int x0, int y0, int x1, int y1)
{
    if (!isWorldLoaded() || !diffMode_ || baseGeo_.empty()) {
        return;
    }
    const int gw = static_cast<int>(vMap.H_SIZE) >> kmGridChA;
    const int gh = static_cast<int>(vMap.V_SIZE) >> kmGridChA;
    const int ax0 = std::max(0, x0 >> kmGridChA);
    const int ay0 = std::max(0, y0 >> kmGridChA);
    const int ax1 = std::min(gw - 1, (std::max(x0, x1 - 1)) >> kmGridChA);
    const int ay1 = std::min(gh - 1, (std::max(y0, y1 - 1)) >> kmGridChA);
    const int stride = static_cast<int>(vMap.H_SIZE);
    const unsigned char* bases[4] = {baseGeo_.data(), baseDam_.data(),
                                     baseAtr_.data(), baseSur_.data()};
    const unsigned char* live[4] = {vMap.VxGBuf, vMap.VxDBuf, vMap.AtrBuf,
                                    vMap.SurBuf};
    for (int ay = ay0; ay <= ay1; ++ay) {
        for (int ax = ax0; ax <= ax1; ++ax) {
            const int area = ay * gw + ax;
            const int baseOff = (ay << kmGridChA) * stride + (ax << kmGridChA);
            bool differs = false;
            for (int b = 0; b < 4 && !differs; ++b) {
                for (int k = 0; k < sizeCellGridCA && !differs; ++k) {
                    const int row = baseOff + k * stride;
                    for (int m = 0; m < sizeCellGridCA; ++m) {
                        if (live[b][row + m] != bases[b][row + m]) {
                            differs = true;
                            break;
                        }
                    }
                }
            }
            if (!differs) {
                if (diffAreas_.erase(area) > 0) {
                    diffModified_ = true;
                }
                continue;
            }
            DiffArea entry;
            for (int b = 0; b < 4; ++b) {
                entry.buf[static_cast<std::size_t>(b)].resize(
                    sizeCellGridCA * sizeCellGridCA);
                for (int k = 0; k < sizeCellGridCA; ++k) {
                    const int row = baseOff + k * stride;
                    std::memcpy(entry.buf[static_cast<std::size_t>(b)].data() +
                                    k * sizeCellGridCA,
                                live[b] + row, sizeCellGridCA);
                }
            }
            diffAreas_[area] = std::move(entry);
            diffModified_ = true;
        }
    }
}

void EditorSession::applyDiffToBuffers()
{
    const std::size_t cells =
        static_cast<std::size_t>(vMap.H_SIZE) * vMap.V_SIZE;
    std::memcpy(vMap.VxGBuf, baseGeo_.data(), cells);
    std::memcpy(vMap.VxDBuf, baseDam_.data(), cells);
    std::memcpy(vMap.AtrBuf, baseAtr_.data(), cells);
    std::memcpy(vMap.SurBuf, baseSur_.data(), cells);
    const int gw = static_cast<int>(vMap.H_SIZE) >> kmGridChA;
    const int stride = static_cast<int>(vMap.H_SIZE);
    for (const auto& [area, entry] : diffAreas_) {
        const int ax = area % gw;
        const int ay = area / gw;
        const int baseOff = (ay << kmGridChA) * stride + (ax << kmGridChA);
        for (int b = 0; b < 4; ++b) {
            const unsigned char* src = entry.buf[static_cast<std::size_t>(b)].data();
            unsigned char* dst[4] = {vMap.VxGBuf, vMap.VxDBuf, vMap.AtrBuf,
                                     vMap.SurBuf};
            for (int k = 0; k < sizeCellGridCA; ++k) {
                std::memcpy(dst[b] + baseOff + k * stride,
                            src + k * sizeCellGridCA, sizeCellGridCA);
            }
        }
    }
    // Full re-render: every cell may have changed.
    vMap.regRender(0, 0, static_cast<int>(vMap.H_SIZE),
                   static_cast<int>(vMap.V_SIZE));
}

void EditorSession::clearDiffState()
{
    diffAreas_.clear();
    diffModified_ = false;
    baseGeo_.clear();
    baseDam_.clear();
    baseAtr_.clear();
    baseSur_.clear();
    diffMode_ = false;
}

void EditorSession::runWithBaseTerrain(const std::function<void()>& fn)
{
    if (!diffMode_) {
        fn();
        return;
    }
    const std::size_t cells =
        static_cast<std::size_t>(vMap.H_SIZE) * vMap.V_SIZE;
    std::vector<unsigned char> effG(vMap.VxGBuf, vMap.VxGBuf + cells);
    std::vector<unsigned char> effD(vMap.VxDBuf, vMap.VxDBuf + cells);
    std::vector<unsigned char> effA(vMap.AtrBuf, vMap.AtrBuf + cells);
    std::vector<unsigned char> effS(vMap.SurBuf, vMap.SurBuf + cells);
    std::memcpy(vMap.VxGBuf, baseGeo_.data(), cells);
    std::memcpy(vMap.VxDBuf, baseDam_.data(), cells);
    std::memcpy(vMap.AtrBuf, baseAtr_.data(), cells);
    std::memcpy(vMap.SurBuf, baseSur_.data(), cells);
    fn();
    std::memcpy(vMap.VxGBuf, effG.data(), cells);
    std::memcpy(vMap.VxDBuf, effD.data(), cells);
    std::memcpy(vMap.AtrBuf, effA.data(), cells);
    std::memcpy(vMap.SurBuf, effS.data(), cells);
    // The effective content is back: repaint everything.
    vMap.regRender(0, 0, static_cast<int>(vMap.H_SIZE),
                   static_cast<int>(vMap.V_SIZE));
}

bool EditorSession::exportTopView(const std::string& path)
{
    if (!isWorldLoaded()) {
        return false;
    }
    bool ok = false;
    runWithBaseTerrain([&] {
        ok = WorldImages::exportTopViewTga(fs::u8path(path));
    });
    return ok;
}

bool EditorSession::regenerateWorldImages()
{
    const std::optional<WorldFiles> files = currentWorldFiles();
    if (!isWorldLoaded() || !files) {
        return false;
    }
    bool ok = false;
    runWithBaseTerrain([&] {
        ok = WorldImages::generateCacheTga(files->worldDir()) &&
             WorldImages::generateMapTga(files->worldDir());
    });
    return ok;
}

bool EditorSession::setDiffMode(bool on)
{
    if (!isWorldLoaded()) {
        return false;
    }
    if (on == diffMode_) {
        return true;
    }
    const std::size_t cells =
        static_cast<std::size_t>(vMap.H_SIZE) * vMap.V_SIZE;
    if (on) {
        // Snapshot the base terrain, then overlay the difference model.
        baseGeo_.assign(vMap.VxGBuf, vMap.VxGBuf + cells);
        baseDam_.assign(vMap.VxDBuf, vMap.VxDBuf + cells);
        baseAtr_.assign(vMap.AtrBuf, vMap.AtrBuf + cells);
        baseSur_.assign(vMap.SurBuf, vMap.SurBuf + cells);
        diffMode_ = true;
        applyDiffToBuffers();
    } else {
        // Buffers still hold BASE+DIFF: capture the difference, then restore
        // the base terrain for display/editing.
        resyncDiffRect(0, 0, static_cast<int>(vMap.H_SIZE),
                       static_cast<int>(vMap.V_SIZE));
        std::memcpy(vMap.VxGBuf, baseGeo_.data(), cells);
        std::memcpy(vMap.VxDBuf, baseDam_.data(), cells);
        std::memcpy(vMap.AtrBuf, baseAtr_.data(), cells);
        std::memcpy(vMap.SurBuf, baseSur_.data(), cells);
        baseGeo_.clear();
        baseDam_.clear();
        baseAtr_.clear();
        baseSur_.clear();
        diffMode_ = false;
        vMap.regRender(0, 0, static_cast<int>(vMap.H_SIZE),
                       static_cast<int>(vMap.V_SIZE));
    }
    // Snapshots recorded in the other buffer state would corrupt the
    // display — the mode switch is a hard reset of the history.
    clearUndoHistory();
    // Terra sets vMap.worldChanged on every stamp (RenderPrepare1) — with
    // the mode OFF the flag must reflect the editor's world-modified state.
    vMap.worldChanged = modified_;
    return true;
}

bool EditorSession::saveDiffToGmp(const std::string& path)
{
    if (!isWorldLoaded()) {
        return false;
    }
    const int gw = static_cast<int>(vMap.H_SIZE) >> kmGridChA;
    const int gh = static_cast<int>(vMap.V_SIZE) >> kmGridChA;
    std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "EditorSession::saveDiffToGmp: cannot open '%s'.\n",
                path.c_str());
        return false;
    }
    // The game's legacy format (loadGameMap, VMAP.cpp:952): world index,
    // the coarse changed-area flags, then per flagged area the four absolute
    // buffer blocks (geo, dam, atr, sur).
    int worldIndex = vMap.cWorld;
    out.write(reinterpret_cast<const char*>(&worldIndex), sizeof(worldIndex));
    std::vector<unsigned char> flags(static_cast<std::size_t>(gw) * gh, 0);
    for (const auto& [area, entry] : diffAreas_) {
        if (area >= 0 && area < gw * gh &&
            entry.buf[0].size() ==
                static_cast<std::size_t>(sizeCellGridCA) * sizeCellGridCA) {
            flags[static_cast<std::size_t>(area)] = 1;
        }
    }
    out.write(reinterpret_cast<const char*>(flags.data()),
              static_cast<std::streamsize>(flags.size()));
    for (int area = 0; area < gw * gh; ++area) {
        const auto it = diffAreas_.find(area);
        if (it == diffAreas_.end()) {
            continue;
        }
        for (int b = 0; b < 4; ++b) {
            out.write(reinterpret_cast<const char*>(
                          it->second.buf[static_cast<std::size_t>(b)].data()),
                      sizeCellGridCA * sizeCellGridCA);
        }
    }
    if (!out.good()) {
        fprintf(stderr, "EditorSession::saveDiffToGmp: write failed '%s'.\n",
                path.c_str());
        return false;
    }
    diffModified_ = false; // the difference data is saved
    return true;
}

bool EditorSession::loadDiffFromGmp(const std::string& path)
{
    if (!isWorldLoaded()) {
        return false;
    }
    const int gw = static_cast<int>(vMap.H_SIZE) >> kmGridChA;
    const int gh = static_cast<int>(vMap.V_SIZE) >> kmGridChA;
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) {
        fprintf(stderr, "EditorSession::loadDiffFromGmp: cannot open '%s'.\n",
                path.c_str());
        return false;
    }
    int worldIndex = 0;
    in.read(reinterpret_cast<char*>(&worldIndex), sizeof(worldIndex));
    std::vector<unsigned char> flags(static_cast<std::size_t>(gw) * gh, 0);
    in.read(reinterpret_cast<char*>(flags.data()),
            static_cast<std::streamsize>(flags.size()));
    if (!in) {
        fprintf(stderr, "EditorSession::loadDiffFromGmp: truncated header in "
                "'%s'.\n", path.c_str());
        return false;
    }
    std::map<int, DiffArea> loaded;
    const std::streamsize block = sizeCellGridCA * sizeCellGridCA;
    for (int area = 0; area < gw * gh; ++area) {
        if (flags[static_cast<std::size_t>(area)] != 1) {
            continue;
        }
        DiffArea entry;
        bool ok = true;
        for (int b = 0; b < 4 && ok; ++b) {
            entry.buf[static_cast<std::size_t>(b)].resize(
                static_cast<std::size_t>(block));
            in.read(reinterpret_cast<char*>(
                        entry.buf[static_cast<std::size_t>(b)].data()),
                    block);
            ok = static_cast<bool>(in);
        }
        if (!ok) {
            fprintf(stderr, "EditorSession::loadDiffFromGmp: truncated area "
                    "data in '%s'.\n", path.c_str());
            return false;
        }
        loaded[area] = std::move(entry);
    }
    diffAreas_ = std::move(loaded);
    diffModified_ = false; // freshly loaded == the saved state
    clearUndoHistory();
    if (diffMode_) {
        applyDiffToBuffers();
    }
    printf("EditorSession: loaded difference data from '%s' (%zu area(s)).\n",
           path.c_str(), diffAreas_.size());
    return true;
}

void EditorSession::recordUndoArea(int x, int y, int width, int height)
{
    if (!isWorldLoaded() || width <= 0 || height <= 0) {
        return;
    }

    // Clip the rectangle to the map (the editor view is bounded; Torus wrap
    // behaviour is intentionally not used here).
    const int xBeg = std::max(0, x);
    const int yBeg = std::max(0, y);
    const int xEnd = std::min(static_cast<int>(vMap.H_SIZE), x + width);
    const int yEnd = std::min(static_cast<int>(vMap.V_SIZE), y + height);
    if (xBeg >= xEnd || yBeg >= yEnd) {
        return;
    }
    const int sx = xEnd - xBeg;
    const int sy = yEnd - yBeg;

    UndoAreaSnapshot snapshot;
    snapshot.x = xBeg;
    snapshot.y = yBeg;
    snapshot.sx = sx;
    snapshot.sy = sy;
    const std::size_t cells = static_cast<std::size_t>(sx) * sy;
    snapshot.geo.resize(cells);
    snapshot.dam.resize(cells);
    snapshot.atr.resize(cells);
    snapshot.sur.resize(cells);

    std::size_t cnt = 0;
    for (int j = 0; j < sy; ++j) {
        const int row = (yBeg + j) * static_cast<int>(vMap.H_SIZE) + xBeg;
        for (int i = 0; i < sx; ++i, ++cnt) {
            const int off = row + i;
            snapshot.geo[cnt] = vMap.VxGBuf[off];
            snapshot.dam[cnt] = vMap.VxDBuf[off];
            snapshot.atr[cnt] = vMap.AtrBuf[off];
            snapshot.sur[cnt] = vMap.SurBuf[off];
        }
    }

    // Inside an explicit undo group, or inside a pending coalescing-window
    // transaction, the snapshot is collected, not pushed — the transaction
    // merges into ONE undo-stack entry when it is closed.
    std::lock_guard<std::mutex> lock(undoMutex_);
    const auto now = std::chrono::steady_clock::now();
    const bool withinWindow =
        undoPendingOpen_ && undoCoalescingWindowMs_ > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastUndoActionTime_).count() <= undoCoalescingWindowMs_;
    if (undoGroupActive_ || withinWindow) {
        if (!undoGroupActive_ &&
            pendingGroupCells() + cells > kMaxPendingUndoCells) {
            // The pending transaction outgrew the memory budget (a long
            // uninterrupted stroke): close it and start a fresh one.
            finalizePendingUndoGroup();
            undoGroup_.push_back(std::move(snapshot));
            undoPendingOpen_ = true;
        } else {
            undoGroup_.push_back(std::move(snapshot));
        }
        lastUndoActionTime_ = now;
        // In difference mode the world file stays untouched — the edit
        // belongs to the difference data.
        if (diffMode_) {
            diffModified_ = true;
        } else {
            setModified(true);
        }
        return;
    }

    // A new edit discards every undone snapshot after the current position
    // (they no longer correspond to any reachable world state). Closing the
    // previous pending transaction first commits it at the current position,
    // so it survives and is undoable separately.
    finalizePendingUndoGroup();
    undoStack_.resize(undoPos_);
    if (undoCoalescingWindowMs_ > 0) {
        undoGroup_.push_back(std::move(snapshot));
        undoPendingOpen_ = true;
    } else {
        undoStack_.push_back(std::move(snapshot));
        ++undoPos_;
        // Bound the memory usage by dropping the oldest snapshots; the same
        // trade-off Terra's dispatcher makes inside UndoDispatcher_PutPreChangedArea.
        trimUndoStack();
    }
    lastUndoActionTime_ = now;

    // A recorded area happens right before an edit, so the world counts as
    // modified from this point on. undo()/redo() re-derive the flag
    // exactly against the last saved position afterwards. In difference
    // mode the world file stays untouched — the edit belongs to the
    // difference data.
    if (diffMode_) {
        diffModified_ = true;
    } else {
        setModified(true);
    }
}

void EditorSession::beginUndoGroup()
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    if (undoGroupActive_) {
        return; // nested begin: collecting already
    }
    if (!undoPendingOpen_) {
        undoGroup_.clear();
        // The group as a whole is one fresh edit — drop the redo tail now, the
        // same thing recordUndoArea() does for a standalone snapshot.
        undoStack_.resize(undoPos_);
    }
    // When a transaction is pending (coalescing window), an explicit group
    // continues collecting into it: the merged entry may still join the
    // following edits that arrive within the window.
    undoGroupActive_ = true;
}

bool EditorSession::endUndoGroup()
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    undoGroupActive_ = false;
    if (undoGroup_.empty()) {
        undoPendingOpen_ = false;
        return false;
    }

    lastUndoActionTime_ = std::chrono::steady_clock::now();
    // The collected snapshots form one entry either way: with a coalescing
    // window the entry stays pending so edits that follow within the window
    // join the same undo transaction; without a window it commits now.
    undoPendingOpen_ = true;
    if (undoCoalescingWindowMs_ == 0) {
        finalizePendingUndoGroup();
    }
    return true;
}

std::size_t EditorSession::pendingGroupCells() const
{
    std::size_t total = 0;
    for (const UndoAreaSnapshot& snap : undoGroup_) {
        total += static_cast<std::size_t>(snap.sx) * snap.sy;
    }
    return total;
}

void EditorSession::finalizePendingUndoGroup()
{
    if (!undoPendingOpen_ || undoGroup_.empty()) {
        undoPendingOpen_ = false;
        return;
    }

    // Merge the collected snapshots into one entry over the union
    // rectangle. The snapshots were captured in edit order, so for every
    // cell the EARLIEST snapshot covering it holds the pre-transaction state
    // (later snapshots may already contain earlier edits of this group).
    const UndoAreaSnapshot& first = undoGroup_.front();
    int x0 = first.x;
    int y0 = first.y;
    int x1 = first.x + first.sx;
    int y1 = first.y + first.sy;
    for (const UndoAreaSnapshot& snap : undoGroup_) {
        x0 = std::min(x0, snap.x);
        y0 = std::min(y0, snap.y);
        x1 = std::max(x1, snap.x + snap.sx);
        y1 = std::max(y1, snap.y + snap.sy);
    }
    const int sx = x1 - x0;
    const int sy = y1 - y0;

    UndoAreaSnapshot merged;
    merged.x = x0;
    merged.y = y0;
    merged.sx = sx;
    merged.sy = sy;
    const std::size_t cells = static_cast<std::size_t>(sx) * sy;
    // Cells no snapshot covers were never edited by the transaction
    // (mask-clipped or render-side effects only) — their pre-state equals the
    // CURRENT buffer content, so seed the merged entry with it and overwrite
    // the covered cells with the captured pre-states below.
    merged.geo.resize(cells);
    merged.dam.resize(cells);
    merged.atr.resize(cells);
    merged.sur.resize(cells);
    std::size_t seed = 0;
    for (int j = 0; j < sy; ++j) {
        const int row = (y0 + j) * static_cast<int>(vMap.H_SIZE) + x0;
        for (int i = 0; i < sx; ++i, ++seed) {
            const int off = row + i;
            merged.geo[seed] = vMap.VxGBuf[off];
            merged.dam[seed] = vMap.VxDBuf[off];
            merged.atr[seed] = vMap.AtrBuf[off];
            merged.sur[seed] = vMap.SurBuf[off];
        }
    }

    std::vector<bool> covered(cells, false);
    for (const UndoAreaSnapshot& snap : undoGroup_) {
        for (int j = 0; j < snap.sy; ++j) {
            const int my = snap.y + j - y0;
            for (int i = 0; i < snap.sx; ++i) {
                const std::size_t mIdx =
                    static_cast<std::size_t>(my) * sx + (snap.x + i - x0);
                if (covered[mIdx]) {
                    continue; // an earlier snapshot already has the pre-state
                }
                const std::size_t sIdx =
                    static_cast<std::size_t>(j) * snap.sx + i;
                merged.geo[mIdx] = snap.geo[sIdx];
                merged.dam[mIdx] = snap.dam[sIdx];
                merged.atr[mIdx] = snap.atr[sIdx];
                merged.sur[mIdx] = snap.sur[sIdx];
                covered[mIdx] = true;
            }
        }
    }

    undoGroup_.clear();
    undoStack_.push_back(std::move(merged));
    ++undoPos_;

    // The same memory bound as recordUndoArea().
    trimUndoStack();
    undoPendingOpen_ = false;
}

void EditorSession::breakUndoCoalescing()
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    finalizePendingUndoGroup();
}

void EditorSession::setUndoCoalescingWindowMs(int ms)
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    undoCoalescingWindowMs_ = std::max(0, ms);
    if (undoCoalescingWindowMs_ == 0) {
        // Window disabled: commit whatever is pending so the history keeps
        // one-entry-per-record semantics from here on.
        finalizePendingUndoGroup();
    }
}

int EditorSession::undoCoalescingWindowMs() const
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    return undoCoalescingWindowMs_;
}

void EditorSession::trimUndoStack()
{
    // Caller must hold undoMutex_ (all callers do — it is only invoked from
    // the recording/commit paths, never on its own).
    std::size_t totalCells = 0;
    for (const UndoAreaSnapshot& snap : undoStack_) {
        totalCells += static_cast<std::size_t>(snap.sx) * snap.sy;
    }
    if (totalCells <= kMaxUndoCells) {
        return;
    }
    // Count how many front entries must go, then drop them in ONE range
    // erase — per-entry erase(begin) would re-move the whole tail on every
    // iteration (O(n*k) instead of O(n)).
    std::size_t drop = 0;
    std::size_t freed = 0;
    while (totalCells - freed > kMaxUndoCells && drop < undoStack_.size()) {
        freed += static_cast<std::size_t>(undoStack_[drop].sx) *
                 undoStack_[drop].sy;
        ++drop;
    }
    if (drop == 0) {
        return;
    }
    undoStack_.erase(undoStack_.begin(), undoStack_.begin() + drop);
    // undoPos_/savedUndoPos_ count entries from the front — shift them by
    // the dropped amount; a saved mark inside the dropped range is gone.
    if (savedUndoPos_ >= drop) {
        savedUndoPos_ -= drop;
    } else {
        savedUndoPos_ = 0;
        hasSavedUndoPos_ = false;
    }
    undoPos_ = undoPos_ >= drop ? undoPos_ - drop : 0;
}

bool EditorSession::undo()
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    // Close the pending transaction (if any): undo must revert the last
    // COMPLETED group of actions, and it finalizes the window.
    finalizePendingUndoGroup();
    if (!isWorldLoaded() || undoPos_ == 0) {
        return false;
    }
    swapSnapshotWithBuffers(undoStack_[undoPos_ - 1]);
    --undoPos_;
    const UndoAreaSnapshot& snap = undoStack_[undoPos_];
    regRenderWithShadowTrail(snap.x, snap.y, snap.x + snap.sx, snap.y + snap.sy);
    markModifiedAgainstSavedState();
    printf("EditorSession: undo (stack position %zu/%zu)\n",
           undoPos_, undoStack_.size());
    return true;
}

bool EditorSession::redo()
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    // Same as undo(): a pending transaction is closed first, so redo applies
    // to committed history only.
    finalizePendingUndoGroup();
    if (!isWorldLoaded() || undoPos_ >= undoStack_.size()) {
        return false;
    }
    swapSnapshotWithBuffers(undoStack_[undoPos_]);
    const UndoAreaSnapshot& snap = undoStack_[undoPos_];
    ++undoPos_;
    regRenderWithShadowTrail(snap.x, snap.y, snap.x + snap.sx, snap.y + snap.sy);
    markModifiedAgainstSavedState();
    printf("EditorSession: redo (stack position %zu/%zu)\n",
           undoPos_, undoStack_.size());
    return true;
}

void EditorSession::clearUndoHistory()
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    undoStack_.clear();
    undoPos_ = 0;
    savedUndoPos_ = 0;
    hasSavedUndoPos_ = false;
    undoGroup_.clear();
    undoGroupActive_ = false;
    undoPendingOpen_ = false;
}

bool EditorSession::isUndoGroupActive() const
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    return undoGroupActive_;
}

bool EditorSession::canUndo() const
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    return isWorldLoaded() &&
           (undoPos_ > 0 || (undoPendingOpen_ && !undoGroup_.empty()));
}

bool EditorSession::canRedo() const
{
    std::lock_guard<std::mutex> lock(undoMutex_);
    return isWorldLoaded() && undoPos_ < undoStack_.size();
}

void EditorSession::markModifiedAgainstSavedState()
{
    if (diffMode_) {
        // In difference mode the undo history belongs to the DIFFERENCE
        // data — undo/redo must not flag the world file as modified.
        diffModified_ = true;
        return;
    }
    // The world matches the last checkpoint exactly when the undo position
    // equals the position it had at save time.
    modified_ = !hasSavedUndoPos_ || undoPos_ != savedUndoPos_;
    if (isWorldLoaded()) {
        vMap.worldChanged = modified_;
    }
}

bool EditorSession::hasUnsavedChanges() const
{
    // Palette edits (T4.1) live outside the undo history that isModified()
    // tracks, so they are reported separately.
    return isModified() || (isWorldLoaded() && isPaletteModified());
}

std::string EditorSession::getUnsavedChangesWarning() const
{
    if (!isWorldLoaded()) {
        return std::string();
    }
    const std::string name = currentWorldName();
    if (name.empty()) {
        return "The current world has unsaved changes. Do you want to save them?";
    }
    return "World \"" + name + "\" has unsaved changes. Do you want to save them?";
}

// ---------------------------------------------------------------------------
// T3.3/T4.1 — SurBuf / palette helpers for the Qt UI.
//
// Model (verified against the game code and the shipped worlds — see
// docs/context.md): a SurBuf value indexes the palette of the cell's own
// layer directly. The render tables Sur2Col32[SurBuf][...] are built by
// convertPal2TableSurCol (VMAP.cpp:1367) from palBuf[i*3+0]=R, +1=G, +2=B:
// dam-free cells take GeoPal[SurBuf] (the geo conversion, columns 0..126),
// dam cells VxDBuf != 0 take DamPal[SurBuf] (the dam conversion, columns
// 128..254; RnrBuf bit 7 carries the layer). Real dam SurBuf values cover
// the whole 0..255 range, so the old "0..127 geo / 128..255 dam" index
// split was wrong and the helpers take the layer explicitly.
// ---------------------------------------------------------------------------

namespace {

unsigned char* paletteBuffer(EditorSession::PaletteLayer layer)
{
    return layer == EditorSession::PaletteLayer::Geo ? vMap.GeoPal : vMap.DamPal;
}

} // namespace

unsigned char EditorSession::surBufAt(int x, int y) const
{
    if (!isWorldLoaded()) {
        return 0;
    }
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    if (x < 0 || y < 0 || x >= mapW || y >= mapH) {
        return 0;
    }
    const int off = y * mapW + x;
    return vMap.SurBuf[off];
}

unsigned int EditorSession::paletteRgb(PaletteLayer layer, int paletteIndex) const
{
    if (!isWorldLoaded() || paletteIndex < 0 || paletteIndex >= 256) {
        return 0;
    }
    const unsigned char* pal = paletteBuffer(layer);
    const int off = paletteIndex * 3;
    return (static_cast<unsigned int>(pal[off + 0]) << 16) |
           (static_cast<unsigned int>(pal[off + 1]) << 8) |
           static_cast<unsigned int>(pal[off + 2]);
}

std::vector<unsigned char> EditorSession::paletteBytes(PaletteLayer layer) const
{
    if (!isWorldLoaded()) {
        return {};
    }
    const unsigned char* pal = paletteBuffer(layer);
    return std::vector<unsigned char>(pal, pal + 256 * 3);
}

std::vector<unsigned char> EditorSession::damPaletteUsage() const
{
    std::vector<unsigned char> used(256, 0);
    if (!isWorldLoaded()) {
        return used;
    }
    const std::size_t cells =
        static_cast<std::size_t>(vMap.H_SIZE) * vMap.V_SIZE;
    for (std::size_t i = 0; i < cells; ++i) {
        if (vMap.VxDBuf[i] != 0) {
            used[vMap.SurBuf[i]] = 1;
        }
    }
    return used;
}

bool EditorSession::cellIsDam(int x, int y) const
{
    if (!isWorldLoaded()) {
        return false;
    }
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);
    if (x < 0 || y < 0 || x >= mapW || y >= mapH) {
        return false;
    }
    return vMap.VxDBuf[y * mapW + x] != 0;
}

// ---------------------------------------------------------------------------
// T4.1 — palette editing.
// ---------------------------------------------------------------------------

bool EditorSession::setPaletteRgb(PaletteLayer layer, int paletteIndex,
                                  unsigned int rgb)
{
    if (!isWorldLoaded() || paletteIndex < 0 || paletteIndex >= 256) {
        return false;
    }

    unsigned char* pal = paletteBuffer(layer);
    const int off = paletteIndex * 3;
    // Plain RGB byte order — convertPal2TableSurCol reads palBuf[i*3+0]
    // as red (VMAP.cpp:1347).
    pal[off + 0] = static_cast<unsigned char>((rgb >> 16) & 0xFF);
    pal[off + 1] = static_cast<unsigned char>((rgb >> 8) & 0xFF);
    pal[off + 2] = static_cast<unsigned char>(rgb & 0xFF);
    paletteModified_ = true;
    return true;
}

bool EditorSession::applyPalettes()
{
    if (!isWorldLoaded()) {
        return false;
    }

    // Exactly what loadGeoDamPal() runs after filling GeoPal/DamPal: rebuild
    // every Sur2Col*/Tex2Col*/SurZP2Col* table, and (for the dam palette)
    // re-derive veryLightDam. The next viewport repaint re-renders the world
    // through these tables.
    vMap.convertPal2TableSurCol(vMap.GeoPal, vrtMap::Geologic);
    vMap.convertPal2TableSurCol(vMap.DamPal, vrtMap::Damming);
    return true;
}

// ---------------------------------------------------------------------------
// T5.1 — world parameters.
// ---------------------------------------------------------------------------

std::optional<WorldParams> EditorSession::currentWorldParams() const
{
    const std::optional<WorldFiles> files = currentWorldFiles();
    if (!files || !isWorldLoaded()) {
        return std::nullopt;
    }
    std::optional<WorldParams> params =
        WorldParams::readFromWorld(*files);
    if (!params) {
        return std::nullopt;
    }
    // Live state wins over the files for everything the session holds in
    // memory (edited but not yet saved values included).
    params->hZeroPlast = static_cast<int>(vMap.hZeroPlast);
    params->f3dKmx = f3d.kmx;
    params->f3dKmy = f3d.kmy;
    params->f3dKmz = f3d.kmz;
    params->f3dFunction = f3d.currentFunction;
    return params;
}

bool EditorSession::applyWorldSettings(const WorldParams& params)
{
    if (!isWorldLoaded()) {
        return false;
    }
    const std::optional<WorldFiles> files = currentWorldFiles();
    if (!files || !hasGameDir()) {
        return false;
    }
    const std::optional<WorldParams> current = currentWorldParams();
    if (!current) {
        return false;
    }

    // Terra resolves world.ini writes through the content root.
    fs::current_path(fs::u8path(gameDir_));

    bool ok = true;

    // Zero-layer height — a live scalar; saveWorld() persists it with the
    // buffers. Drives the ZP brushes and the 3D grid reference plane.
    const int hZeroPlast = std::clamp(params.hZeroPlast, 0, 255);
    if (hZeroPlast != static_cast<int>(vMap.hZeroPlast)) {
        vMap.hZeroPlast = static_cast<unsigned char>(hZeroPlast);
        setModified(true);
    }

    // 3DF geo-noise parameters — applied live and persisted through Terra's
    // own world.ini writer (f3d.saveVariable writes the current world).
    if (params.f3dKmx != current->f3dKmx || params.f3dKmy != current->f3dKmy ||
        params.f3dKmz != current->f3dKmz ||
        params.f3dFunction != current->f3dFunction) {
        auto clampKm = [](int value) {
            return std::clamp(value, 1, 1000000); // 0 would divide by zero
        };
        f3d.setKM(clampKm(params.f3dKmx), clampKm(params.f3dKmy),
                  clampKm(params.f3dKmz));
        f3d.setFunction(params.f3dFunction != 0 ? 1 : 0);
        f3d.saveVariable();
    }

    // Generation powers — world.ini only. The loaded buffers were sized by
    // the values in effect at load time; the new powers take effect when the
    // world is reloaded or its terrain is regenerated.
    if (params.geoNetPower != current->geoNetPower ||
        params.sectionSizePower != current->sectionSizePower ||
        params.minimalSquarePower != current->minimalSquarePower) {
        auto clampPower = [](int value) {
            return std::clamp(value, 0, 12);
        };
        ok &= files->writeIniInt(WorldIni::GlobalParameters,
                                 WorldIni::GeoNetPower,
                                 clampPower(params.geoNetPower));
        ok &= files->writeIniInt(WorldIni::GlobalParameters,
                                 WorldIni::SectionSizePower,
                                 clampPower(params.sectionSizePower));
        ok &= files->writeIniInt(WorldIni::GlobalParameters,
                                 WorldIni::MinimalSquarePower,
                                 clampPower(params.minimalSquarePower));
    }

    // output.vmp compression — convert the stored file right away and let
    // saveWorld() re-apply the format after every save3Buf().
    const int format = params.compressedFormat == 1 ? 1 : 0;
    if (format != vmpCompressionTarget_) {
        ok &= files->writeStorageCompressedFormat(format);
        ok &= convertVmpCompression(files->worldDir() / WorldFiles::OutputVmpFile,
                                    format);
        vmpCompressionTarget_ = format;
    }

    return ok;
}

// ---------------------------------------------------------------------------
// T6.3 — surface image import.
// ---------------------------------------------------------------------------

namespace {

/// Nearest palette entry (squared RGB distance) for a 0x00BBGGRR color.
unsigned char nearestPaletteIndex(const unsigned char* palette,
                                  unsigned int bgr)
{
    const int r = (bgr >> 16) & 0xFF;
    const int g = (bgr >> 8) & 0xFF;
    const int b = bgr & 0xFF;
    int bestIndex = 0;
    long bestDist = LONG_MAX;
    for (int i = 0; i < 256; ++i) {
        const int dr = static_cast<int>(palette[i * 3 + 0]) - r;
        const int dg = static_cast<int>(palette[i * 3 + 1]) - g;
        const int db = static_cast<int>(palette[i * 3 + 2]) - b;
        const long dist = static_cast<long>(dr) * dr +
                          static_cast<long>(dg) * dg +
                          static_cast<long>(db) * db;
        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = i;
            if (dist == 0) {
                break;
            }
        }
    }
    return static_cast<unsigned char>(bestIndex);
}

/// Shared per-cell import loop; `sample` maps image pixel -> SurBuf byte.
template <typename Sample>
bool importSurfaceImage(EditorSession& session, unsigned imgWidth,
                        unsigned imgHeight, Sample sample,
                        const EditorSession::SurfaceImportOptions& options)
{
    if (!session.isWorldLoaded() || imgWidth == 0 || imgHeight == 0) {
        return false;
    }
    const int mapW = static_cast<int>(vMap.H_SIZE);
    const int mapH = static_cast<int>(vMap.V_SIZE);

    // One undo step for the whole map.
    session.recordUndoArea(0, 0, mapW, mapH);

    for (int y = 0; y < mapH; ++y) {
        const unsigned sy = std::min<unsigned>(
            static_cast<unsigned>(y) * imgHeight / mapH, imgHeight - 1);
        for (int x = 0; x < mapW; ++x) {
            const unsigned sx = std::min<unsigned>(
                static_cast<unsigned>(x) * imgWidth / mapW, imgWidth - 1);
            const int off = y * mapW + x;
            const bool dam = vMap.VxDBuf[off] != 0;
            if (dam ? !options.damCells : !options.geoCells) {
                continue;
            }
            vMap.SurBuf[off] = sample(sx, sy, dam);
        }
    }

    session.regRenderWithShadowTrail(0, 0, mapW - 1, mapH - 1);
    return true;
}

} // namespace

bool EditorSession::importSurfaceImageRgb(unsigned imgWidth,
                                          unsigned imgHeight,
                                          const std::vector<unsigned char>& bgr,
                                          const SurfaceImportOptions& options)
{
    if (bgr.size() < static_cast<std::size_t>(imgWidth) * imgHeight * 3) {
        fprintf(stderr, "EditorSession: import image buffer too small.\n");
        return false;
    }
    return importSurfaceImage(
        *this, imgWidth, imgHeight,
        [&bgr, imgWidth](unsigned sx, unsigned sy, bool dam) {
            const std::size_t off =
                (static_cast<std::size_t>(sy) * imgWidth + sx) * 3;
            // The buffer is BGR, the palette compare wants 0x00BBGGRR.
            const unsigned int color = (static_cast<unsigned int>(bgr[off + 2]) << 16) |
                                       (static_cast<unsigned int>(bgr[off + 1]) << 8) |
                                       bgr[off + 0];
            return nearestPaletteIndex(dam ? vMap.DamPal : vMap.GeoPal, color);
        },
        options);
}

bool EditorSession::importSurfaceImageIndexed(
    unsigned imgWidth, unsigned imgHeight,
    const std::vector<unsigned char>& indices,
    const SurfaceImportOptions& options)
{
    if (indices.size() < static_cast<std::size_t>(imgWidth) * imgHeight) {
        fprintf(stderr, "EditorSession: import index buffer too small.\n");
        return false;
    }
    return importSurfaceImage(
        *this, imgWidth, imgHeight,
        [&indices, imgWidth](unsigned sx, unsigned sy, bool) {
            return indices[static_cast<std::size_t>(sy) * imgWidth + sx];
        },
        options);
}

} // namespace MapEditor

// ============================================================================
// MapEditorCore — EditorSession implementation.
// ============================================================================

#include "EditorSession.h"
#include "PaletteOps.h"
#include "ViewportRenderer.h"
#include "WorldImages.h"
#include "WorldParams.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

    // Use Terra's content-entry API to enumerate directories under RESOURCE/Worlds.
    // This matches the logic in vrtMap::prepare("RESOURCE/Worlds").
    for (const auto& entry : get_content_entries_directory("RESOURCE/Worlds")) {
        if (!entry->is_directory) {
            continue;
        }
        WorldInfo wi;
        const fs::path realPath = fs::u8path(entry->path_content);
        wi.name = realPath.filename().u8string();
        wi.dirKey = entry->key;
        worldList_.push_back(std::move(wi));
    }

    // Sort by name for a stable UI list.
    std::sort(worldList_.begin(), worldList_.end(),
              [](const WorldInfo& a, const WorldInfo& b) { return a.name < b.name; });

    if (worldList_.empty()) {
        fprintf(stderr, "EditorSession: no worlds found in RESOURCE/Worlds of '%s'\n",
                gameDir_.c_str());
        return false;
    }

    state_ = SessionState::GameReady;
    printf("EditorSession: found %zu world(s)\n", worldList_.size());
    return true;
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
    // therefore run only once per process. Call it the first time, then keep
    // wTable in sync ourselves.
    if (!terraLandPrepared()) {
        vMap.prepare("RESOURCE/Worlds");
        return;
    }

    // Same enumeration vrtMap::prepare() performs, minus landPrepare().
    vMap.wTable.clear();
    for (const auto& entry : get_content_entries_directory("RESOURCE/Worlds")) {
        if (!entry->is_directory) {
            continue;
        }
        const fs::path path = fs::u8path(entry->path_content);
        vMap.wTable.emplace_back(path.filename().u8string(), entry->key);
    }
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

    // Find the Terra index for our world by name (case-insensitive comparison
    // matches Terra's own getWorldID logic).
    const std::string& wantedName = worldList_[worldIndex].name;
    int terraIdx = vMap.getWorldID(wantedName.c_str());
    editorTrace("EditorSession: loadWorld getWorldID('%s')=%d", wantedName.c_str(), terraIdx);
    if (terraIdx < 0) {
        fprintf(stderr, "EditorSession: world '%s' not found in Terra wTable.\n",
                wantedName.c_str());
        editorTrace("EditorSession: loadWorld FAILED — getWorldID returned -1");
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
    for (size_t i = 0; i < worldList_.size(); ++i) {
        if (worldList_[i].name == worldName) {
            return loadWorld(i);
        }
    }
    fprintf(stderr, "EditorSession: world '%s' not in scanned list.\n", worldName.c_str());
    return false;
}

void EditorSession::unloadWorld()
{
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

bool EditorSession::isWorldLoaded() const
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
    return vMap.worldChanged || modified_;
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

const std::string& EditorSession::currentWorldName() const
{
    static const std::string empty;
    if (currentWorldIndex_ < 0 ||
        static_cast<size_t>(currentWorldIndex_) >= worldList_.size()) {
        return empty;
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

SessionState EditorSession::state() const
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

    // ------------------------------------------------------------------
    // 1. Buffers, palettes and [3DF Parameters] via Terra's own pipeline.
    // ------------------------------------------------------------------
    vMap.save3Buf();

    // hardness.bin is optional: refresh it only when the world already has one,
    // so "Save" never adds files the author did not ask for.
    if (files->exists(WorldFiles::HardnessFile)) {
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
        if (!files->writeGeoPalXml(PaletteOps::geoPalXml(vMap.GeoPal))) {
            fprintf(stderr, "EditorSession::saveWorld: geoPal.xml sync failed.\n");
            return false;
        }
    }

    // ------------------------------------------------------------------
    // 2. world.ini — size, zero-layer height and storage version.
    // ------------------------------------------------------------------
    // Map Power is authoritative for the map size the game allocates.
    files->writeMapPower(static_cast<int>(vMap.H_SIZE_POWER),
                         static_cast<int>(vMap.V_SIZE_POWER));
    files->writeHZeroPlast(static_cast<int>(vMap.hZeroPlast));
    if (const auto version = files->readStorageVersion()) {
        files->writeStorageVersion(*version); // keep the existing version
    } else {
        files->writeStorageVersion("1.4");
    }

    // ------------------------------------------------------------------
    // 2b. T5.1 — output.vmp compression: save3Buf() always writes S2T0,
    // so the session target (from the World Settings dialog, initialised
    // from the file header on load) is re-applied here.
    // ------------------------------------------------------------------
    files->writeStorageCompressedFormat(vmpCompressionTarget_);
    if (vmpCompressionTarget_ != 0 &&
        !convertVmpCompression(worldDir / WorldFiles::OutputVmpFile,
                               vmpCompressionTarget_)) {
        fprintf(stderr, "EditorSession::saveWorld: output.vmp compression failed.\n");
        return false;
    }

    // ------------------------------------------------------------------
    // 3. Images the game and its shell read.
    // ------------------------------------------------------------------
    if (!WorldImages::generateCacheTga(worldDir)) {
        fprintf(stderr, "EditorSession::saveWorld: cache.tga generation failed.\n");
        return false;
    }
    if (!WorldImages::generateMapTga(worldDir)) {
        fprintf(stderr, "EditorSession::saveWorld: map.tga generation failed.\n");
        return false;
    }

    hasSavedUndoPos_ = true;
    savedUndoPos_ = undoPos_;
    modified_ = false;
    paletteModified_ = false;
    vMap.worldChanged = false;
    printf("EditorSession: saved world '%s' to '%s'\n",
           currentWorldName().c_str(), worldDir.u8string().c_str());
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

    // Copy all entries (files and subdirectories) except maybe nothing - keep same structure.
    for (const auto& entry : fs::directory_iterator(srcDir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        const auto& p = entry.path();
        const auto rel = p.filename();
        const fs::path target = destDir / rel;
        if (entry.is_directory(ec)) {
            if (ec) continue;
            fs::copy(entry.path(), target,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        } else if (entry.is_regular_file(ec)) {
            if (ec) continue;
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
        }
        // Skip symlinks/others if any.
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

    // A new edit discards every undone snapshot after the current position
    // (they no longer correspond to any reachable world state).
    undoStack_.resize(undoPos_);
    undoStack_.push_back(std::move(snapshot));
    ++undoPos_;

    // Bound the memory usage by dropping the oldest snapshots; the same
    // trade-off Terra's dispatcher makes inside UndoDispatcher_PutPreChangedArea.
    std::size_t totalCells = 0;
    for (const UndoAreaSnapshot& snap : undoStack_) {
        totalCells += static_cast<std::size_t>(snap.sx) * snap.sy;
    }
    while (totalCells > kMaxUndoCells && !undoStack_.empty()) {
        totalCells -= static_cast<std::size_t>(undoStack_.front().sx) *
                      undoStack_.front().sy;
        undoStack_.erase(undoStack_.begin());
        if (savedUndoPos_ > 0) {
            --savedUndoPos_;
        } else {
            // The "saved" position was at/before the stack front and is gone —
            // fall back to always reporting modified after undo/redo.
            hasSavedUndoPos_ = false;
        }
        if (undoPos_ > 0) {
            --undoPos_;
        }
    }

    // A recorded area happens right before an edit, so the world counts as
    // modified from this point on. undo()/redo() re-derive the flag
    // exactly against the last saved position afterwards.
    setModified(true);
}

bool EditorSession::undo()
{
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
    undoStack_.clear();
    undoPos_ = 0;
    savedUndoPos_ = 0;
    hasSavedUndoPos_ = false;
}

bool EditorSession::canUndo() const
{
    return isWorldLoaded() && undoPos_ > 0;
}

bool EditorSession::canRedo() const
{
    return isWorldLoaded() && undoPos_ < undoStack_.size();
}

void EditorSession::markModifiedAgainstSavedState()
{
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

// ============================================================================
// MapEditorSupport — stubs for symbols that normally live in the game / plugins.
//
// Terra and tx3d (both reused by the editor) reference a handful of symbols that
// are defined outside of the libraries the editor links:
//
//   * UpdateRegionMap        — Game/Universe.cpp (minimap/radar bookkeeping)
//   * ZIPStream              — PluginMAX/ZIPStream.cpp (zip overlay for streams)
//   * global_time, sdlWindow, terVisGeneric, terRenderDevice,
//     terCamera, gameShell, HTManager::self, applicationHasFocus,
//     getGameContentEnumName, terGameContentBase
//     — Util/Game symbols needed by Util.lib when linked into map_editor.
//   * MTConfig::multithreading/setMultithreading — HT/ht.cpp; the render
//     stack (MapEditorRender) calls it, HT itself is not linked.
//
// Neither is needed for headless world processing, so we provide minimal
// implementations instead of dragging the whole game/plugin link graph in.
// ============================================================================

#include "shims.h"

#include <string>

// tx3d::TGA uses ZIPStream; including the real header guarantees the mangled
// names match the ones tx3d was compiled against.
#include "ZIPStream.h"
#include "files/files.h"

// ---------------------------------------------------------------------------
// UpdateRegionMap — game-side radar/minimap update. No-op in the editor.
// ---------------------------------------------------------------------------

void UpdateRegionMap(int /*x1*/, int /*y1*/, int /*x2*/, int /*y2*/)
{
}

// ---------------------------------------------------------------------------
// ZIPStream — the editor works with plain files on disk, so the stream simply
// opens the converted path through the base XStream. ZIP overlays are not used.
// ---------------------------------------------------------------------------

ZIPStream::ZIPStream() : XStream(0)
{
}

int ZIPStream::open(const char* name, unsigned f)
{
    if (!name) {
        return 0;
    }
    std::string path = convert_path_content(name);
    if (path.empty()) {
        path = convert_path_native(name);
    }
    return XStream::open(path.c_str(), f) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Util / Game symbols needed when Util.lib is linked (XPrmArchive, DebugUtil,
// SystemUtil).  The editor does not use threading, timers, or the game shell —
// minimal stubs that satisfy the linker.
// ---------------------------------------------------------------------------

// MT_IS_LOGIC / MT_IS_GRAPH — provided by the real render stack now
// (VisGeneric.cpp in MapEditorRender); the stub was removed when the GPU
// viewport started linking the game's VisGeneric. They consult MTConfig.

// MTConfig — lives in Source/HT/ht.cpp in the game. HT is not linked into the
// editor (its HTManager::self stub below is what the Util lib needs), so the
// two functions are stubbed: the editor is strictly single-threaded, and the
// render stack's MT_IS_GRAPH()/MT_IS_LOGIC() then always return true.
namespace MTConfig
{
bool multithreading()
{
    return false;
}

void setMultithreading(bool /*value*/)
{
}
}

// hWndVisGeneric — Game/Runtime.cpp global; VisGeneric.cpp only reports
// debug memory info through it. The editor has no game window.
struct HWND__;
HWND__* hWndVisGeneric = nullptr;

// ---------------------------------------------------------------------------
// PluginMAX mesh storage — cMeshFile/cMeshScene/sSubTexmap symbols referenced
// by ObjLibrary::LoadM3D. The editor never loads 3D models, so the PluginMAX
// link unit is not pulled in and these entry points are stubbed out.
// Only the symbols ObjLibrary references are declared, with the same public
// access/signatures so the mangled names match; the stubs never touch
// members (the real objects only exist in ObjLibrary.cpp dead code) and the
// parse entry points just report an error.
// ---------------------------------------------------------------------------

// From PluginMAX/Inc/meshfile.h (not included: its BaseClass.h chain needs
// more game context than the stub TU provides). Stable format constant.
#ifndef MESHFILE_ERROR
#define MESHFILE_ERROR 0x00000005
#endif

class cMeshFile;

struct sSubTexmap
{
    void Release();
};

class cMeshScene
{
public:
    int Read(cMeshFile& f);
};

class cMeshFile
{
public:
    cMeshFile();
    ~cMeshFile();
    int OpenRead(void* buf, int size);
    void Close();
    int ReadHeaderFile();
};

bool ZIPIsOpen()
{
    return false;
}

cMeshFile::cMeshFile() {}

cMeshFile::~cMeshFile() {}

int cMeshFile::OpenRead(void* /*buf*/, int /*size*/)
{
    return MESHFILE_ERROR;
}

void cMeshFile::Close() {}

int cMeshFile::ReadHeaderFile()
{
    return MESHFILE_ERROR;
}

void sSubTexmap::Release() {}

int cMeshScene::Read(cMeshFile& /*f*/)
{
    return MESHFILE_ERROR;
}

// global_time — SyncroTimer. Editor doesn't use timers; return zero.
class SyncroTimer { unsigned int time = 0; unsigned int delta() const { return 0; } float deltaf() const { return 0; } };
SyncroTimer global_time;

// sdlWindow — SDL window pointer. Not used in headless editor.
struct SDL_Window {};
SDL_Window* sdlWindow = nullptr;

// terVisGeneric, terRenderDevice, terCamera — renderer/camera globals. Not used.
class cVisGeneric {};
class cInterfaceRenderDevice {};
class terCameraType {};
cVisGeneric* terVisGeneric = nullptr;
cInterfaceRenderDevice* terRenderDevice = nullptr;
terCameraType* terCamera = nullptr;

// gameShell — game shell. Not used.
class GameShell {};
GameShell* gameShell = nullptr;

// HTManager::self — hotkey manager singleton. Not used.
// HTManager is a global-namespace class with a protected static member.
class HTManager
{
protected:
    static HTManager* self;
};
HTManager* HTManager::self = nullptr;

// applicationHasFocus — input focus check. Always true in editor.
bool applicationHasFocus()
{
    return true;
}

// getGameContentEnumName — converts GAME_CONTENT enum to string.
enum GAME_CONTENT { GC_NONE, GC_GAME, GC_EDITOR };
std::string getGameContentEnumName(const GAME_CONTENT&)
{
    return "map_editor";
}

// terGameContentBase — global game content enum.
GAME_CONTENT terGameContentBase = GC_EDITOR;

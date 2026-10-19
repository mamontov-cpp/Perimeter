// ============================================================================
// MapEditorSupport — stubs for symbols that normally live in the game / plugins.
//
// Terra and tx3d (both reused by the editor) reference a handful of symbols that
// are defined outside of the libraries the editor links:
//
//   * UpdateRegionMap        — Game/Universe.cpp (minimap/radar bookkeeping)
//   * ZIPStream              — PluginMAX/ZIPStream.cpp (zip overlay for streams)
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

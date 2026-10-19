// ============================================================================
// MapEditorSupport — content-root initialization.
// Minimal version of Game/GameContent.cpp findGameContent():
//   set_content_root_path + scan_resource_paths("./").
// ============================================================================

#include "shims.h"

#include <filesystem>
#include <cstdio>
#include <string>

#include "files/files.h"

namespace MapEditor {

void init_content_root()
{
    // Use current working directory as content root (the game folder).
    std::filesystem::path rootPath = std::filesystem::current_path();
    printf("MapEditor: Setting content root to '%s'\n", rootPath.u8string().c_str());

    clear_content_entries();
    set_content_root_path(rootPath);

    if (!scan_resource_paths("./")) {
        fprintf(stderr, "MapEditor: Failed to scan resource paths at '%s'\n",
                rootPath.u8string().c_str());
        return;
    }

    printf("MapEditor: Content root initialized: %s\n",
           get_content_root_path_str().c_str());
}

} // namespace MapEditor

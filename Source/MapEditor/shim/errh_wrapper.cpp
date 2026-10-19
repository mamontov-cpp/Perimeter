// ============================================================================
// MapEditorSupport — ErrH crash wrapper implementation.
// ============================================================================

#include "errh_wrapper.h"

#include <atomic>
#include <cstdio>

// Terra / XTool — needed for vMap and ErrH.
// NOTE: _PERIMETER_ and _TX3D_LIBRARY_ are already defined via CMake ADD_DEFINITIONS.
#include "terra.h"
#include "xerrhand.h"

namespace MapEditor {

// Flag to prevent recursive crash-handler invocation.
static std::atomic<int> crashInFlight{0};

static void errhCrashHandler()
{
    // Prevent recursive calls.
    if (crashInFlight.exchange(1) != 0) {
        return;
    }

    fprintf(stderr, "\n[MapEditor] Crash handler: attempting emergency save...\n");
    fflush(stderr);

    // Attempt to save the 3-buffers (VxGBuf, VxDBuf, AtrBuf, SurBuf).
    // If vMap is not yet loaded (vMap.VxGBuf == nullptr), there is nothing to save.
    if (vMap.VxGBuf != nullptr) {
        try {
            vMap.save3BufOnly();
            fprintf(stderr, "[MapEditor] Emergency save completed.\n");
        } catch (...) {
            fprintf(stderr, "[MapEditor] Emergency save threw exception.\n");
        }
    } else {
        fprintf(stderr, "[MapEditor] No world loaded — nothing to save.\n");
    }

    fflush(stderr);
    crashInFlight.store(0);
}

void setupErrHWrapper()
{
    // Set a distinctive prefix so error messages identify the editor.
    ErrH.SetPrefix("MapEditor");

    // Register the crash handler.
    ErrH.SetCrash(errhCrashHandler);
}

} // namespace MapEditor

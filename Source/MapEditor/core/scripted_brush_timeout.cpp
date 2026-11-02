// MapEditorCore — duktape execution-timeout check (see the header).
// Compiled as C++ for std::chrono::steady_clock (wall time — a CPU-time
// clock would freeze if an engine call ever blocked). The functions keep
// C linkage: duktape (compiled as C) calls mapeditorTimeoutCheck() through
// the DUK_USE_EXEC_TIMEOUT_CHECK config macro.
#include "scripted_brush_timeout.h"

#include <chrono>

extern "C" void mapeditorTimeoutSetDeadline(MapEditorScriptTimeout* timeout,
                                            long long timeoutMs)
{
    timeout->hit = 0;
    if (timeoutMs < 0) {
        timeout->deadlineMs = -1;
        return;
    }
    const long long nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    timeout->deadlineMs = nowMs + timeoutMs;
}

extern "C" int mapeditorTimeoutCheck(void* udata)
{
    MapEditorScriptTimeout* timeout =
        static_cast<MapEditorScriptTimeout*>(udata);
    if (!timeout) {
        return 0;
    }
    if (timeout->hit) {
        return 1; /* keep throwing until duktape has fully unwound */
    }
    if (timeout->deadlineMs >= 0) {
        const long long nowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (nowMs > timeout->deadlineMs) {
            timeout->hit = 1;
            return 1;
        }
    }
    return 0;
}

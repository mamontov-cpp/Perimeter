// ============================================================================
// EditorTrace — lifecycle tracing for crash hunting (debug instrumentation).
//
// Enabled by the MAP_EDITOR_TRACE environment variable; lines append
// unbuffered to %TEMP%\map_editor_trace.log with a relative millisecond
// timestamp, so the last logged point survives a hard crash. All calls are
// no-ops when the variable is unset.
// ============================================================================
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace MapEditor {

inline void editorTrace(const char* fmt, ...)
{
    static FILE* file = []() -> FILE* {
        const char* on = std::getenv("MAP_EDITOR_TRACE");
        if (!on || !*on) {
            return nullptr;
        }
        const char* temp = std::getenv("TEMP");
        char path[512];
        if (temp && *temp) {
            std::snprintf(path, sizeof(path), "%s\\map_editor_trace.log", temp);
        } else {
            std::snprintf(path, sizeof(path), "map_editor_trace.log");
        }
        FILE* f = std::fopen(path, "a");
        if (f) {
            std::setvbuf(f, nullptr, _IONBF, 0);
        }
        return f;
    }();
    if (!file) {
        return;
    }

    static const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    std::fprintf(file, "[%9lld ms] ", ms);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fputc('\n', file);
}

} // namespace MapEditor

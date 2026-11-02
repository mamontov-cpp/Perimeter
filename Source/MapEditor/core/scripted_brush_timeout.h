// ============================================================================
// MapEditorCore — duktape execution-timeout state (shared by the C++ runner
// and the timeout check that the duktape executor calls through the
// DUK_USE_EXEC_TIMEOUT_CHECK config macro; see ScriptedBrush.cpp).
//
// The heap user data of the per-run duktape heap points at this struct;
// mapeditorTimeoutCheck() runs on the executor interrupt interval and
// returns non-zero once the deadline passed — duktape then raises its
// "execution timeout" RangeError.
//
// The deadline is a std::chrono::steady_clock epoch millisecond value (wall
// time): CPU-time clocks would freeze if an engine call ever blocked, and
// scripts must be bounded in wall time.
// ============================================================================
#ifndef MAP_EDITOR_DUKTAPE_TIMEOUT_H
#define MAP_EDITOR_DUKTAPE_TIMEOUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MapEditorScriptTimeout
{
    long long deadlineMs; /* steady_clock epoch ms; < 0 = no limit */
    int hit;              /* sticky: keep reporting the timeout until unwind */
} MapEditorScriptTimeout;

/* Arms the deadline: `timeoutMs` from now (< 0 disables the limit). */
void mapeditorTimeoutSetDeadline(MapEditorScriptTimeout* timeout,
                                 long long timeoutMs);

/* Returns non-zero when the deadline has passed (sticky). */
int mapeditorTimeoutCheck(void* udata);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAP_EDITOR_DUKTAPE_TIMEOUT_H */

/* ============================================================================
 * MapEditorCore — duktape execution-timeout state (shared by the C++ runner
 * and the C timeout check that the duktape executor calls through the
 * DUK_USE_EXEC_TIMEOUT_CHECK config macro; see ScriptedBrush.cpp).
 *
 * The heap user data of the per-run duktape heap points at this struct;
 * mapeditorTimeoutCheck() runs on the executor interrupt interval and
 * returns non-zero once the deadline passed — duktape then raises its
 * "execution timeout" RangeError.
 * ========================================================================== */
#ifndef MAP_EDITOR_DUKTAPE_TIMEOUT_H
#define MAP_EDITOR_DUKTAPE_TIMEOUT_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MapEditorScriptTimeout
{
    clock_t deadline; /* (clock_t)-1 = no limit */
    int hit;          /* sticky: keep reporting the timeout until unwind */
} MapEditorScriptTimeout;

/* Returns non-zero when the deadline has passed (sticky). */
int mapeditorTimeoutCheck(void* udata);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAP_EDITOR_DUKTAPE_TIMEOUT_H */

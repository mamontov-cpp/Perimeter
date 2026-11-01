/* MapEditorCore — duktape execution-timeout check (see the header). */
#include "scripted_brush_timeout.h"

int mapeditorTimeoutCheck(void* udata)
{
    MapEditorScriptTimeout* timeout =
        (MapEditorScriptTimeout*)udata;
    if (!timeout) {
        return 0;
    }
    if (timeout->hit) {
        return 1; /* keep throwing until duktape has fully unwound */
    }
    if (timeout->deadline != (clock_t)-1 && clock() > timeout->deadline) {
        timeout->hit = 1;
        return 1;
    }
    return 0;
}

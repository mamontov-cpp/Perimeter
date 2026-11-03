// ============================================================================
// MapEditorCore — the scripted brush implementation (duktape glue).
//
// The API exposed to scripts (besides plain ECMAScript):
//
//   getCurrentPos()                    -> {x, y}   stamp centre (map cells)
//   getCurrentRadius()                 -> int     brush radius (cells)
//   getCurrentThickness()              -> int     brush stroke width (cells)
//   getCurrentHeight()                 -> int     brush height value (voxels)
//   raiseHeight(pos, height, radius, square)
//   lowerHeight(pos, height, radius, square)
//   setHeight(pos, height, radius, square)
//   setToZP(pos, radius, square)
//   paintSoot(pos, radius, square)
//   paintVeryLightDam(pos, radius, square)
//   paintColor(pos, red, green, blue, radius, square)
//
//   getDamHeight(pos)                  -> int     dam height (whole voxels,
//                                                 0 where no dam)
//   getGeoColor(pos)                   -> {r,g,b} SurBuf index resolved
//   getDamColor(pos)                   -> {r,g,b} through the Geo/Dam palette
//   showInfoMessageBox(text)           -> the UI message hook (info mode)
//   showErrorMessageBox(text)           -> the UI message hook (error mode)
//
//   pos    = {x: int, y: int}             map cell
//   radius = int >= 1 (circle radius or square half-side), clamped to the
//            editor brush radius cap
//   square = boolean: true → filled square stamp, false → filled circle
//   height = whole voxels, behaves like the corresponding editor brush
//            (raise/lower step per call; setHeight/setToZP absolute)
//   red/green/blue = 0..255; the nearest Dam-palette entry is painted
//
// Every op routes through BrushTool::applyMask with the Dam mode, so each
// behaves exactly like the corresponding editor brush (layer gating,
// hardness, SurBuf re-derivation, grid/render updates). All ops of one
// evaluation share ONE undo entry via the session undo group. The read*
// getters range-check the position (a pos outside the map throws). The
// message boxes are forwarded to the hook installed with
// setScriptedBrushMessageHook() — the Qt-free core never touches QMessageBox
// itself.
// ============================================================================

#include "ScriptedBrush.h"

#include <climits>

#include "duktape.h"

#include "BrushTool.h"
#include "EditorSession.h"

#include "terra.h"

// Global (non-namespaced) C linkage — must stay outside namespace MapEditor.
#include "scripted_brush_timeout.h"

namespace MapEditor {

namespace {

/// Everything the script bindings need, reachable from the duktape heap
/// through the global stash.
struct ScriptEnv
{
    EditorSession* session = nullptr;
    BrushTool* brush = nullptr;
    const BrushParams* params = nullptr;
    int centerX = 0;
    int centerY = 0;
    MapEditorScriptTimeout* timeout = nullptr;
};

ScriptEnv* getEnv(duk_context* ctx)
{
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "meScriptEnv");
    ScriptEnv* env = static_cast<ScriptEnv*>(duk_get_pointer(ctx, -1));
    duk_pop_2(ctx);
    return env;
}

/// Nearest palette entry (squared RGB distance) in a 768-byte RGB table.
unsigned char nearestPaletteEntry(const unsigned char* palette, int r, int g,
                                  int b)
{
    int bestIndex = 0;
    long bestDist = LONG_MAX;
    for (int i = 0; i < 256; ++i) {
        const int dr = static_cast<int>(palette[i * 3 + 0]) - r;
        const int dg = static_cast<int>(palette[i * 3 + 1]) - g;
        const int db = static_cast<int>(palette[i * 3 + 2]) - b;
        const long dist =
            static_cast<long>(dr) * dr + static_cast<long>(dg) * dg +
            static_cast<long>(db) * db;
        if (dist < bestDist) {
            bestDist = dist;
            bestIndex = i;
        }
    }
    return static_cast<unsigned char>(bestIndex);
}

/// Reads the {x, y} position object at `idx`.
bool readPosition(duk_context* ctx, int idx, int& x, int& y)
{
    if (!duk_is_object(ctx, idx)) {
        return false;
    }
    duk_get_prop_string(ctx, idx, "x");
    x = duk_to_int(ctx, -1);
    duk_pop(ctx);
    duk_get_prop_string(ctx, idx, "y");
    y = duk_to_int(ctx, -1);
    duk_pop(ctx);
    return true;
}

/// Applies one op through the brush: a filled circle/square mask of the
/// given radius at the position, driven with the Dam mode and the effect
/// the API call names. Shared tail of all *Height/paint* bindings.
duk_ret_t applyOp(duk_context* ctx, BrushEffect effect, int value,
                  bool valueIsColor)
{
    ScriptEnv* env = getEnv(ctx);
    const int nargs = duk_get_top(ctx);
    // Stack layouts:
    //   (pos, height, radius, square)            — raise/lower/setHeight
    //   (pos, radius, square)                    — setToZP/soot/vld
    //   (pos, red, green, blue, radius, square)  — paintColor
    const int radiusIndex = valueIsColor ? 4 : (nargs >= 4 ? 2 : 1);
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, 0, x, y)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "first argument must be a position {x, y}");
    }
    int radius = duk_to_int(ctx, radiusIndex);
    if (radius < 1) {
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "radius must be a number >= 1, got %d", radius);
    }
    radius = BrushTool::clampRadius(radius);
    const bool square = duk_to_boolean(ctx, radiusIndex + 1);

    BrushMask mask = square ? BrushMasks::filledSquare(radius)
                            : BrushMasks::filledCircle(radius);
    mask.translate(x, y);

    BrushParams op = *env->params;
    op.mode = BrushMode::Dam; // the scripted brush operates on the dam layer
    op.effect = effect;
    op.useImageMask = false;  // explicit geometry, not the loaded image
    switch (effect) {
    case BrushEffect::ChangeHeight:
        op.direction = value < 0 ? -1 : 1;
        op.strength = BrushTool::clampStrength(value < 0 ? -value : value);
        break;
    case BrushEffect::AlignVariableH:
        op.heightLevel = BrushTool::clampHeightLevel(value);
        break;
    case BrushEffect::SetColor: {
        const std::vector<unsigned char> pal =
            env->session->paletteBytes(EditorSession::PaletteLayer::Dam);
        if (pal.size() == 768) {
            op.colorIndex = nearestPaletteEntry(pal.data(), (value >> 16) & 0xFF,
                                                (value >> 8) & 0xFF, value & 0xFF);
        }
        break;
    }
    default:
        break;
    }
    // Out-of-map positions clip away inside applyMask; that is not an error.
    env->brush->applyMask(mask, op);
    return 0;
}

duk_ret_t apiGetCurrentPos(duk_context* ctx)
{
    const ScriptEnv* env = getEnv(ctx);
    duk_idx_t obj = duk_push_object(ctx);
    duk_push_int(ctx, env->centerX);
    duk_put_prop_string(ctx, obj, "x");
    duk_push_int(ctx, env->centerY);
    duk_put_prop_string(ctx, obj, "y");
    return 1;
}

duk_ret_t apiGetCurrentRadius(duk_context* ctx)
{
    duk_push_int(ctx, getEnv(ctx)->params->radius);
    return 1;
}

duk_ret_t apiGetCurrentThickness(duk_context* ctx)
{
    duk_push_int(ctx, getEnv(ctx)->params->thickness);
    return 1;
}

duk_ret_t apiGetCurrentHeight(duk_context* ctx)
{
    duk_push_int(ctx, getEnv(ctx)->params->heightLevel);
    return 1;
}

duk_ret_t apiRaiseHeight(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::ChangeHeight, duk_to_int(ctx, 1), false);
}

duk_ret_t apiLowerHeight(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::ChangeHeight, -duk_to_int(ctx, 1), false);
}

duk_ret_t apiSetHeight(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::AlignVariableH, duk_to_int(ctx, 1), false);
}

duk_ret_t apiSetToZP(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::Align4ZP, 0, false);
}

duk_ret_t apiPaintSoot(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::PlotSoot, 0, false);
}

duk_ret_t apiPaintVeryLightDam(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::VeryLightDam, 0, false);
}

duk_ret_t apiPaintColor(duk_context* ctx)
{
    const int r = duk_to_int(ctx, 1) & 0xFF;
    const int g = duk_to_int(ctx, 2) & 0xFF;
    const int b = duk_to_int(ctx, 3) & 0xFF;
    return applyOp(ctx, BrushEffect::SetColor, (r << 16) | (g << 8) | b, true);
}

/// Shared tail of the read* bindings: reads the {x, y} position and
/// range-checks it against the loaded map. Throws a duktape error (which
/// aborts the evaluation with a clear brush.js message) when the argument
/// is not a position object or lies outside the map.
void readMapPositionOrThrow(duk_context* ctx, int idx, int& off)
{
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, idx, x, y)) {
        duk_error(ctx, DUK_ERR_TYPE_ERROR,
                  "first argument must be a position {x, y}");
    }
    if (x < 0 || y < 0 || x >= static_cast<int>(vMap.H_SIZE) ||
        y >= static_cast<int>(vMap.V_SIZE)) {
        duk_error(ctx, DUK_ERR_RANGE_ERROR,
                  "position (%d, %d) is outside the map (%d x %d)", x, y,
                  static_cast<int>(vMap.H_SIZE),
                  static_cast<int>(vMap.V_SIZE));
    }
    off = y * static_cast<int>(vMap.H_SIZE) + x;
}

duk_ret_t apiGetDamHeight(duk_context* ctx)
{
    int off = 0;
    readMapPositionOrThrow(ctx, 0, off);
    // The dam height is the top surface while a dam is deployed; a dam-free
    // cell has no dam layer at all — report 0 there (whole voxels, like the
    // heights the height API takes).
    const int height = vMap.VxDBuf[off] != 0
        ? static_cast<int>(vMap.SGetAlt(off)) >> VX_FRACTION
        : 0;
    duk_push_int(ctx, height);
    return 1;
}

/// Shared tail of getGeoColor/getDamColor: the SurBuf index of the cell
/// resolved through the requested layer's palette (the game model: one
/// index per cell, GeoPal or DamPal depending on the layer — vrtMap::
/// convertPal2TableSurCol, VMAP.cpp:1367).
duk_ret_t pushLayerColor(duk_context* ctx, EditorSession::PaletteLayer layer)
{
    int off = 0;
    readMapPositionOrThrow(ctx, 0, off);
    const unsigned int rgb =
        getEnv(ctx)->session->paletteRgb(layer, vMap.SurBuf[off]);
    duk_idx_t obj = duk_push_object(ctx);
    duk_push_int(ctx, static_cast<int>((rgb >> 16) & 0xFF));
    duk_put_prop_string(ctx, obj, "r");
    duk_push_int(ctx, static_cast<int>((rgb >> 8) & 0xFF));
    duk_put_prop_string(ctx, obj, "g");
    duk_push_int(ctx, static_cast<int>(rgb & 0xFF));
    duk_put_prop_string(ctx, obj, "b");
    return 1;
}

duk_ret_t apiGetGeoColor(duk_context* ctx)
{
    return pushLayerColor(ctx, EditorSession::PaletteLayer::Geo);
}

duk_ret_t apiGetDamColor(duk_context* ctx)
{
    return pushLayerColor(ctx, EditorSession::PaletteLayer::Dam);
}

/// The message-box hook (installed by the Qt UI; see
/// setScriptedBrushMessageHook). Null = messages drop silently.
void (*g_messageBoxHook)(bool isError, const std::string& text) = nullptr;

duk_ret_t apiShowMessageBox(duk_context* ctx, bool isError)
{
    // duk_safe_to_string: any JS value becomes readable text.
    const std::string text = duk_safe_to_string(ctx, 0);
    if (g_messageBoxHook) {
        g_messageBoxHook(isError, text);
    }
    return 0;
}

duk_ret_t apiShowInfoMessageBox(duk_context* ctx)
{
    return apiShowMessageBox(ctx, false);
}

duk_ret_t apiShowErrorMessageBox(duk_context* ctx)
{
    return apiShowMessageBox(ctx, true);
}

/// Registers one global function.
void registerBinding(duk_context* ctx, const char* name, duk_c_function fn,
                     duk_idx_t nargs)
{
    duk_push_c_function(ctx, fn, nargs);
    duk_put_global_string(ctx, name);
}

/// The interrupt callback is compiled into duktape itself via the
/// DUK_USE_EXEC_TIMEOUT_CHECK config macro (scripted_brush_timeout.c) and
/// fires the heap's MapEditorScriptTimeout — see runScriptedBrush().

/// Compiles `script` (program) and leaves the function on the stack.
/// Returns false and fills errorOut with the duktape message.
bool compileScript(duk_context* ctx, const std::string& script,
                   std::string& errorOut)
{
    duk_push_string(ctx, script.c_str());
    duk_push_string(ctx, "brush.js");
    // Flags 0 = program (global) code — duktape has no DUK_COMPILE_PROGRAM;
    // EVAL/FUNCTION are opt-in bits.
    if (duk_pcompile(ctx, 0) != DUK_EXEC_SUCCESS) {
        errorOut = duk_safe_to_string(ctx, -1);
        duk_pop(ctx);
        return false;
    }
    return true;
}

} // namespace

bool checkScriptSyntax(const std::string& script, std::string& errorOut)
{
    if (script.empty()) {
        errorOut = "the script is empty";
        return false;
    }
    duk_context* ctx = duk_create_heap_default();
    if (!ctx) {
        errorOut = "cannot create the script engine";
        return false;
    }
    const bool ok = compileScript(ctx, script, errorOut);
    duk_destroy_heap(ctx);
    return ok;
}

bool runScriptedBrush(EditorSession& session, BrushTool& brush,
                      const BrushParams& params, int cx, int cy,
                      std::string& errorOut, int timeoutMs)
{
    if (params.mode != BrushMode::Dam) {
        errorOut = "the scripted brush operates on the Dam layer only "
                   "(switch the mode to Dam)";
        return false;
    }
    if (params.script.empty()) {
        errorOut = "no script configured — use the script editor button "
                   "(pencil next to the scripted brush) to write one";
        return false;
    }
    if (!session.isWorldLoaded()) {
        errorOut = "no world loaded";
        return false;
    }

    // Every world edit during the evaluation — however many ops the script
    // runs — merges into ONE undo entry.
    session.beginUndoGroup();

    // The timeout state is the heap user data: the executor calls
    // mapeditorTimeoutCheck(heap_udata) on its interrupt interval (compiled
    // into duktape via DUK_USE_EXEC_TIMEOUT_CHECK). Wall-clock deadline —
    // see scripted_brush_timeout.h.
    MapEditorScriptTimeout timeout;
    mapeditorTimeoutSetDeadline(&timeout, timeoutMs);

    ScriptEnv env;
    env.session = &session;
    env.brush = &brush;
    env.params = &params;
    env.centerX = cx;
    env.centerY = cy;
    env.timeout = &timeout;

    duk_context* ctx = duk_create_heap(nullptr, nullptr, nullptr, &timeout,
                                       nullptr);
    bool ok = false;
    if (!ctx) {
        errorOut = "cannot create the script engine";
    } else {
        duk_push_global_stash(ctx);
        duk_push_pointer(ctx, &env);
        duk_put_prop_string(ctx, -2, "meScriptEnv");
        duk_pop(ctx);

        registerBinding(ctx, "getCurrentPos", apiGetCurrentPos, 0);
        registerBinding(ctx, "getCurrentRadius", apiGetCurrentRadius, 0);
        registerBinding(ctx, "getCurrentThickness", apiGetCurrentThickness, 0);
        registerBinding(ctx, "getCurrentHeight", apiGetCurrentHeight, 0);
        registerBinding(ctx, "raiseHeight", apiRaiseHeight, 4);
        registerBinding(ctx, "lowerHeight", apiLowerHeight, 4);
        registerBinding(ctx, "setHeight", apiSetHeight, 4);
        registerBinding(ctx, "setToZP", apiSetToZP, 3);
        registerBinding(ctx, "paintSoot", apiPaintSoot, 3);
        registerBinding(ctx, "paintVeryLightDam", apiPaintVeryLightDam, 3);
        registerBinding(ctx, "paintColor", apiPaintColor, 6);
        registerBinding(ctx, "getDamHeight", apiGetDamHeight, 1);
        registerBinding(ctx, "getGeoColor", apiGetGeoColor, 1);
        registerBinding(ctx, "getDamColor", apiGetDamColor, 1);
        registerBinding(ctx, "showInfoMessageBox", apiShowInfoMessageBox, 1);
        registerBinding(ctx, "showErrorMessageBox", apiShowErrorMessageBox, 1);

        if (compileScript(ctx, params.script, errorOut)) {
            if (duk_pcall(ctx, 0) == DUK_EXEC_SUCCESS) {
                ok = true;
            } else if (timeout.hit) {
                errorOut = "the script exceeded the " +
                           std::to_string(timeoutMs) +
                           " ms time limit and was aborted";
            } else {
                errorOut = duk_safe_to_string(ctx, -1);
            }
        }
        duk_destroy_heap(ctx);
    }

    // Merge whatever was recorded (possibly nothing on a syntax error)
    // into the single undo entry.
    session.endUndoGroup();
    return ok;
}

void setScriptedBrushMessageHook(void (*hook)(bool isError,
                                              const std::string& text))
{
    g_messageBoxHook = hook;
}

} // namespace MapEditor

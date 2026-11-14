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
//   applyBlur(pos, radius, blurRadius, effect, square)
//   applySmooth(pos, radius, effect, square)
//   applyHeightFn(pos, radius, fn, square)
//
//   getDamHeight(pos)                  -> int     dam height (whole voxels,
//                                                 0 where no dam)
//   getGeoColor(pos)                   -> {r,g,b} SurBuf index resolved
//   getDamColor(pos)                   -> {r,g,b} through the Geo/Dam palette
//   setGeoPalette(index, {r, g, b})    -> set a Geo palette entry (0..255)
//   setDamPalette(index, {r, g, b})    -> set a Dam palette entry (0..255)
//   showInfoMessageBox(text)           -> the UI message hook (info mode)
//   showErrorMessageBox(text)           -> the UI message hook (error mode)
//
//   pos    = {x: int, y: int}             map cell
//   radius = int >= 1 (circle radius or square half-side), clamped to the
//            editor brush radius cap
//   blurRadius = int >= 1 (Gaussian Blur kernel radius in cells, clamped to
//            [1, radius - 1] like the sculpting toolbar brush)
//   effect = number 0..1 (clamped) — the fraction of the way each covered
//            cell moves towards the sculpting reference in one stamp; the
//            same value the Sculpting spinbox sets in the UI (0.5 = 50 %)
//   fn     = function(x, y, h) -> number: applyHeightFn evaluates it for
//            EVERY covered cell against the PRE-EDIT dam surface (h =
//            current height in voxels, fractional, 0 where no dam) and
//            writes the returned ABSOLUTE heights in one pass — covered
//            cells never influence each other (unlike overlapping
//            raise/lower stamps, which add up on the overlap)
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
#include <cmath>
#include <cstring>

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

/// Defined below (after the API bindings) — the binding callbacks capture
/// script errors as text and raise through this formatter.
std::string describeError(duk_context* ctx);

/// The message-box hook (installed by the Qt UI; see
/// setScriptedBrushMessageHook). Null = messages drop silently.
void (*g_messageBoxHook)(bool isError, const std::string& text) = nullptr;

/// The palette-edit hook (installed by the Qt UI; see setScriptPaletteHook):
/// fired after a script's setGeoPalette/setDamPalette so the UI repaints the
/// viewport (every pixel re-tints) and refreshes the palette widgets. Null =
/// the edit still lands, only the views stay stale until the next repaint.
void (*g_paletteHook)() = nullptr;

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
/// `value` is double: the height arguments accept FRACTIONAL voxels and are
/// converted to Terra's 1/32 fixed point at stamp time (the packed RGB color
/// of paintColor also rides in it).
duk_ret_t applyOp(duk_context* ctx, BrushEffect effect, double value,
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
    case BrushEffect::SetColor:
        // Exact 24-bit RGB — no palette quantization: the value carries the
        // packed color straight from paintColor(red, green, blue).
        op.colorRgb = static_cast<std::uint32_t>(value) & 0x00FFFFFFu;
        break;
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
    // Number (may be fractional — the scripted brush height is fixed-point
    // under the hood since the MCP/script fractional-height support).
    duk_push_number(ctx, getEnv(ctx)->params->heightLevel);
    return 1;
}

// Height arguments are read as NUMBERS: fractional voxels (10.4) stay
// fractional through to Terra's 1/32 fixed point instead of truncating.
duk_ret_t apiRaiseHeight(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::ChangeHeight, duk_to_number(ctx, 1), false);
}

duk_ret_t apiLowerHeight(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::ChangeHeight, -duk_to_number(ctx, 1), false);
}

duk_ret_t apiSetHeight(duk_context* ctx)
{
    return applyOp(ctx, BrushEffect::AlignVariableH, duk_to_number(ctx, 1), false);
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

/// paintColorAt(pos, red, green, blue, height, depth, radius, square) —
/// paint the internal voxel layers: [height-depth+1 .. height] at every
/// covered cell (height -1 = the column top = the default paint). A height
/// at/above the surface clamps to the top. When a brush image is loaded
/// (the toolbar Load Brush Image button / the MCP load_brush_image) it owns
/// the stamp geometry exactly like the UI: the pixel alpha is the
/// influence, fully transparent pixels are skipped, and radius/square are
/// ignored.
duk_ret_t apiPaintColorAt(duk_context* ctx)
{
    ScriptEnv* env = getEnv(ctx);
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, 0, x, y)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "first argument must be a position {x, y}");
    }
    const std::uint32_t rgb =
        (static_cast<std::uint32_t>(duk_to_int(ctx, 1) & 0xFF) << 16) |
        (static_cast<std::uint32_t>(duk_to_int(ctx, 2) & 0xFF) << 8) |
        static_cast<std::uint32_t>(duk_to_int(ctx, 3) & 0xFF);
    const int height = BrushTool::clampPaintHeight(duk_to_int(ctx, 4));
    const int depth = BrushTool::clampPaintDepth(duk_to_int(ctx, 5));
    const int radius = BrushTool::clampRadius(duk_to_int(ctx, 6));
    if (radius < 1) {
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "radius must be a number >= 1");
    }
    const bool square = duk_to_boolean(ctx, 7);

    BrushParams op = *env->params;
    BrushMask mask;
    if (op.useImageMask && env->brush->hasImageMask()) {
        mask = env->brush->imageMask();
        mask.translate(x, y);
    } else {
        mask = square ? BrushMasks::filledSquare(radius)
                      : BrushMasks::filledCircle(radius);
        mask.translate(x, y);
    }
    op.mode = BrushMode::Dam;
    op.effect = BrushEffect::SetColor;
    op.useImageMask = false; // the mask is applied explicitly above
    op.colorRgb = rgb;
    op.paintHeight = height;
    op.paintDepth = depth;
    env->brush->applyMask(mask, op);
    return 0;
}

/// paintColorShape(pos, red, green, blue, shape, height, shapeHeight,
///                   radius, paintInterior, depth) — paint the voxel volume
/// of a 3D solid WITHOUT changing any heights. `shape` is one of "cone",
/// "pyramid", "truncatedCone", "truncatedPyramid", "torus",
/// "halfEllipsoid". Every covered column gets the layers [height ..
/// height + profile*shapeHeight] (Paint interior semantics); a negative
/// `height` paints from 0 (the same base as the shape's height tools).
/// paintInterior false paints only the top slab of `depth` layers at the
/// shape surface. Polygonal solids (pyramid/frustum) take the polygon
/// side count and angle from the current brush UI settings.
duk_ret_t apiPaintColorShape(duk_context* ctx)
{
    ScriptEnv* env = getEnv(ctx);
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, 0, x, y)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "first argument must be a position {x, y}");
    }
    const std::uint32_t rgb =
        (static_cast<std::uint32_t>(duk_to_int(ctx, 1) & 0xFF) << 16) |
        (static_cast<std::uint32_t>(duk_to_int(ctx, 2) & 0xFF) << 8) |
        static_cast<std::uint32_t>(duk_to_int(ctx, 3) & 0xFF);
    const char* shapeName = duk_safe_to_string(ctx, 4);
    static const std::pair<const char*, Shape3DKind> kShapes[] = {
        {"cone", Shape3DKind::Cone},
        {"pyramid", Shape3DKind::Pyramid},
        {"truncatedCone", Shape3DKind::TruncatedCone},
        {"truncatedPyramid", Shape3DKind::TruncatedPyramid},
        {"torus", Shape3DKind::Torus},
        {"halfEllipsoid", Shape3DKind::HalfEllipsoid},
    };
    Shape3DKind kind = Shape3DKind::None;
    for (const auto& entry : kShapes) {
        if (std::strcmp(shapeName, entry.first) == 0) {
            kind = entry.second;
            break;
        }
    }
    if (kind == Shape3DKind::None) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "unknown shape '%s' (cone, pyramid, truncatedCone, "
                         "truncatedPyramid, torus, halfEllipsoid)",
                         shapeName);
    }
    const int height = BrushTool::clampPaintHeight(duk_to_int(ctx, 5));
    const int shapeHeight = std::clamp(duk_to_int(ctx, 6), 0,
                                       MAX_VX_HEIGHT_WHOLE);
    int radius = BrushTool::clampRadius(duk_to_int(ctx, 7));
    if (radius < 1) {
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "radius must be a number >= 1");
    }
    const bool paintInterior = duk_to_boolean(ctx, 8);
    const int depth = BrushTool::clampPaintDepth(duk_to_int(ctx, 9));

    BrushParams op = *env->params;
    op.mode = BrushMode::Dam;
    op.effect = BrushEffect::SetColor;
    op.useImageMask = false;
    op.modelBrush = false;
    op.colorRgb = rgb;
    op.shape3d = kind;
    op.paintHeight = height;
    op.shapeHeight = shapeHeight;
    op.paintInterior3d = paintInterior;
    op.paintDepth = depth;
    BrushMask mask =
        shape3DIsPolygonal(kind)
            ? BrushMasks::filledPolygon(radius, op.polygonSides,
                                        op.polygonAngleDeg)
            : BrushMasks::filledCircle(radius);
    mask.translate(x, y);
    env->brush->applyMask(mask, op);
    return 0;
}

/// paintColorModel(pos, red, green, blue, height, modelHeight,
///                   paintInterior, depth) — paint the voxel volume of the
/// imported model WITHOUT changing any heights (the model brush must have
/// an imported model — the UI Import Model button or the MCP import_model).
/// Every covered column gets the layers [height .. height +
/// norm*modelHeight] following the model's heightmap; height < 0 paints
/// from 0. paintInterior false paints only the top slab of `depth` layers.
/// The model scale comes from the current brush UI settings.
duk_ret_t apiPaintColorModel(duk_context* ctx)
{
    ScriptEnv* env = getEnv(ctx);
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, 0, x, y)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "first argument must be a position {x, y}");
    }
    if (!env->brush->hasModelBrushData()) {
        return duk_error(ctx, DUK_ERR_ERROR,
                         "no model imported (use the Import Model button or "
                         "the MCP import_model first)");
    }
    const std::uint32_t rgb =
        (static_cast<std::uint32_t>(duk_to_int(ctx, 1) & 0xFF) << 16) |
        (static_cast<std::uint32_t>(duk_to_int(ctx, 2) & 0xFF) << 8) |
        static_cast<std::uint32_t>(duk_to_int(ctx, 3) & 0xFF);
    const int height = BrushTool::clampPaintHeight(duk_to_int(ctx, 4));
    const int modelHeight = std::clamp(duk_to_int(ctx, 5), 0,
                                       MAX_VX_HEIGHT_WHOLE);
    const bool paintInterior = duk_to_boolean(ctx, 6);
    const int depth = BrushTool::clampPaintDepth(duk_to_int(ctx, 7));

    BrushParams op = *env->params;
    op.mode = BrushMode::Dam;
    op.effect = BrushEffect::SetColor;
    op.useImageMask = false;
    op.shape3d = Shape3DKind::None;
    op.modelBrush = true;
    op.colorRgb = rgb;
    op.paintHeight = height;
    op.modelHeight = modelHeight;
    op.paintInterior3d = paintInterior;
    op.paintDepth = depth;
    // The model raster is resampled inside applyStamp's rectangle path.
    env->brush->applyStamp(x, y, op);
    return 0;
}

/// paintTransparentAt(pos, height, depth, radius, square) — paint the
/// internal voxel layers [height-depth+1 .. height] as TRANSPARENT (empty)
/// voxels: no color, no terraforming participation, units and line of sight
/// pass through. A transparent voxel can never be the topmost one — on the
/// top it is destroyed immediately and the terrain height recalculates to
/// the nearest solid voxel; raising terrain through a transparent voxel
/// replaces it with a solid one. height = -1 paints the depth layers at the
/// column top (which recedes accordingly). Dam cells only.
duk_ret_t apiPaintTransparentAt(duk_context* ctx)
{
    ScriptEnv* env = getEnv(ctx);
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, 0, x, y)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "first argument must be a position {x, y}");
    }
    const int height = BrushTool::clampPaintHeight(duk_to_int(ctx, 1));
    const int depth = BrushTool::clampPaintDepth(duk_to_int(ctx, 2));
    const int radius = BrushTool::clampRadius(duk_to_int(ctx, 3));
    if (radius < 1) {
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "radius must be a number >= 1");
    }
    const bool square = duk_to_boolean(ctx, 4);

    BrushParams op = *env->params;
    BrushMask mask;
    if (op.useImageMask && env->brush->hasImageMask()) {
        mask = env->brush->imageMask();
        mask.translate(x, y);
    } else {
        mask = square ? BrushMasks::filledSquare(radius)
                      : BrushMasks::filledCircle(radius);
        mask.translate(x, y);
    }
    op.mode = BrushMode::Dam;
    op.effect = BrushEffect::SetColor;
    op.useImageMask = false;
    op.colorRgb = 0;
    op.paintHeight = height;
    op.paintDepth = depth;
    op.paintTransparent = true;
    env->brush->applyMask(mask, op);
    return 0;
}

/// paintDecal(pos, height, depth) — stamp the loaded image decal (the
/// ImageDecal brush image, loaded with the UI Image Decal button or the
/// MCP load_decal_image) centred at pos. height/depth select the internal
/// voxel layers exactly like paintColorAt: [height-depth+1 .. height] below
/// the surface; height = -1 (or at/above the surface) paints the pixel
/// colors onto the column tops (the default decal behaviour). Dam cells
/// only; transparent pixels never apply.
duk_ret_t apiPaintDecal(duk_context* ctx)
{
    ScriptEnv* env = getEnv(ctx);
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, 0, x, y)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "first argument must be a position {x, y}");
    }
    if (env->brush->decalImage().empty()) {
        return duk_error(ctx, DUK_ERR_ERROR,
                         "no decal image loaded (use the Image Decal button "
                         "or the MCP load_decal_image first)");
    }
    const int height = BrushTool::clampPaintHeight(duk_to_int(ctx, 1));
    const int depth = BrushTool::clampPaintDepth(duk_to_int(ctx, 2));

    BrushParams op = *env->params;
    op.mode = BrushMode::Dam;
    op.effect = BrushEffect::ImageDecal;
    op.useImageMask = false;
    op.shape3d = Shape3DKind::None;
    op.modelBrush = false;
    op.paintHeight = height;
    op.paintDepth = depth;
    // The decal rectangle is built inside applyStamp's decal path.
    env->brush->applyStamp(x, y, op);
    return 0;
}

/// Shared tail of applyBlur/applySmooth: the sculpting brushes own their
/// stamp geometry, so the radius sits at index 1 (unlike the height ops,
/// where index 1 carries the height). The stamp is Dam-mode only, exactly
/// like the editor sculpting toolbar; the effect fraction is the explicit
/// `effect` argument (0..1, clamped like the Sculpting spinbox — the UI
/// value is NOT used), and the Gaussian blur additionally takes its kernel
/// radius — clamped into [1, stampRadius - 1] the same way clampedParams()
/// does for the toolbar brush.
/// Stack layouts:
///   applySmooth(pos, radius, effect, square)
///   applyBlur(pos, radius, blurRadius, effect, square)
duk_ret_t applySculptOp(duk_context* ctx, BrushEffect effect, int blurRadius)
{
    ScriptEnv* env = getEnv(ctx);
    const bool isBlur = effect == BrushEffect::GaussianBlur;
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, 0, x, y)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "first argument must be a position {x, y}");
    }
    int radius = duk_to_int(ctx, 1);
    if (radius < 1) {
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "radius must be a number >= 1, got %d", radius);
    }
    radius = BrushTool::clampRadius(radius);
    // The effect argument: index 2 for applySmooth, index 3 for applyBlur
    // (the blurRadius argument shifts it).
    const int effectIndex = isBlur ? 3 : 2;
    if (isBlur && blurRadius < 1) {
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "blurRadius must be a number >= 1, got %d",
                         blurRadius);
    }
    const double requestedEffect = duk_to_number(ctx, effectIndex);
    if (!std::isfinite(requestedEffect)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "effect must be a finite number 0..1");
    }
    const double effectFraction =
        BrushTool::clampSculptEffect(requestedEffect);
    const bool square = duk_to_boolean(ctx, effectIndex + 1);

    BrushMask mask = square ? BrushMasks::filledSquare(radius)
                            : BrushMasks::filledCircle(radius);
    mask.translate(x, y);

    BrushParams op = *env->params;
    op.mode = BrushMode::Dam; // sculpting owns the dam layer
    op.effect = effect;
    op.useImageMask = false;
    op.sculptEffect = effectFraction;
    if (isBlur) {
        op.blurRadius = BrushTool::clampBlurRadius(blurRadius, radius);
    }
    env->brush->applyMask(mask, op);
    return 0;
}

duk_ret_t apiApplyBlur(duk_context* ctx)
{
    return applySculptOp(ctx, BrushEffect::GaussianBlur, duk_to_int(ctx, 2));
}

duk_ret_t apiApplySmooth(duk_context* ctx)
{
    // applySmooth is the TRICUBIC sculpting brush (bicubic Catmull-Rom
    // interpolation of a coarse lattice over the pre-edit heights) — the
    // same brush as the Sculpting toolbar's "Tricubic". The
    // average-height brush (BrushEffect::Smooth, "force the ground into
    // the average plane") is NOT what this binding promises; see
    // BrushTool.h for the effect semantics.
    return applySculptOp(ctx, BrushEffect::Tricubic, 0);
}

/// applyHeightFn(pos, radius, fn, square) — the two-pass height-function
/// tool. `fn(x, y, h)` receives a covered cell's map coordinates and its
/// PRE-EDIT dam surface height (voxels, fractional 1/32 precision, 0 where
/// no dam) and returns the cell's new ABSOLUTE height. EVERY covered cell
/// is evaluated against the original surface before ANY write, so adjacent
/// cells / radius-1 stamps cannot add up on each other the way overlapping
/// raise/lower stamps do — the seamless way to shape an area by a function
/// of position and the original height. A throw (or a non-numeric /
/// non-finite return) aborts the whole op with nothing written; when no
/// cell would change the call is a quiet no-op. Dam layer, like the other
/// script height ops.
duk_ret_t apiApplyHeightFn(duk_context* ctx)
{
    ScriptEnv* env = getEnv(ctx);
    int x = 0;
    int y = 0;
    if (!readPosition(ctx, 0, x, y)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "first argument must be a position {x, y}");
    }
    int radius = duk_to_int(ctx, 1);
    if (radius < 1) {
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "radius must be a number >= 1, got %d", radius);
    }
    radius = BrushTool::clampRadius(radius);
    if (!duk_is_callable(ctx, 2)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "third argument must be a function fn(x, y, h) "
                         "returning the new absolute height");
    }
    const bool square = duk_to_boolean(ctx, 3);

    BrushMask mask = square ? BrushMasks::filledSquare(radius)
                            : BrushMasks::filledCircle(radius);
    mask.translate(x, y);

    // The callable stays at stack index 2 for the whole evaluation (the
    // wrapper's pushes/pops are balanced and applyHeightFunction never
    // touches the duktape stack); each invocation dup()s it. Errors are
    // captured as text — raising a duktape error across the C++ pass-1
    // frames would longjmp over live std::vector locals.
    std::string fnError;
    auto heightFn = [&](int cx, int cy, double h, double& outTarget) {
        if (!fnError.empty()) {
            return false; // already failed — stop the loop
        }
        duk_dup(ctx, 2);
        duk_push_int(ctx, cx);
        duk_push_int(ctx, cy);
        duk_push_number(ctx, h);
        if (duk_pcall(ctx, 3) != DUK_EXEC_SUCCESS) {
            fnError = describeError(ctx);
            duk_pop(ctx);
            return false;
        }
        if (!duk_is_number(ctx, -1)) {
            fnError = "fn must return a finite number (the new height)";
            duk_pop(ctx);
            return false;
        }
        const double result = duk_get_number(ctx, -1);
        duk_pop(ctx);
        if (!std::isfinite(result)) {
            fnError = "fn returned a non-finite height";
            return false;
        }
        outTarget = result;
        return true;
    };

    if (!env->brush->applyHeightFunction(mask, heightFn)) {
        if (!fnError.empty()) {
            return duk_error(ctx, DUK_ERR_ERROR, "%s", fnError.c_str());
        }
        return 0; // nothing would change / outside the map: a quiet no-op
    }
    return 0;
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

/// Shared tail of setGeoPalette/setDamPalette: (index, {r, g, b}) with the
/// index and the colour components range-checked. The edit goes through
/// EditorSession::setPaletteRgb + applyPalettes, exactly like the palette
/// panel — the render tables are rebuilt; the UI hook then repaints the
/// viewport and the palette widgets.
duk_ret_t apiSetPalette(duk_context* ctx, EditorSession::PaletteLayer layer)
{
    const int index = duk_to_int(ctx, 0);
    if (index < 0 || index > 255) {
        return duk_error(ctx, DUK_ERR_RANGE_ERROR,
                         "palette index must be 0..255, got %d", index);
    }
    if (!duk_is_object(ctx, 1)) {
        return duk_error(ctx, DUK_ERR_TYPE_ERROR,
                         "second argument must be a colour {r, g, b}");
    }
    auto colorComponent = [&](const char* name) {
        duk_get_prop_string(ctx, 1, name);
        const int value = std::clamp(duk_to_int(ctx, -1), 0, 255);
        duk_pop(ctx);
        return value;
    };
    const int r = colorComponent("r");
    const int g = colorComponent("g");
    const int b = colorComponent("b");
    getEnv(ctx)->session->setPaletteRgb(
        layer, index,
        (static_cast<unsigned int>(r) << 16) |
            (static_cast<unsigned int>(g) << 8) |
            static_cast<unsigned int>(b));
    getEnv(ctx)->session->applyPalettes();
    if (g_paletteHook) {
        g_paletteHook();
    }
    return 0;
}

duk_ret_t apiSetGeoPalette(duk_context* ctx)
{
    return apiSetPalette(ctx, EditorSession::PaletteLayer::Geo);
}

duk_ret_t apiSetDamPalette(duk_context* ctx)
{
    return apiSetPalette(ctx, EditorSession::PaletteLayer::Dam);
}

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
/// fires the heap's MapEditorScriptTimeout — see executeScript().

/// Registers the whole editor API (shared by the scripted brush and the
/// Run Script window).
void registerApiBindings(duk_context* ctx)
{
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
    registerBinding(ctx, "paintColorAt", apiPaintColorAt, 8);
    registerBinding(ctx, "paintColorShape", apiPaintColorShape, 10);
    registerBinding(ctx, "paintColorModel", apiPaintColorModel, 8);
    registerBinding(ctx, "paintDecal", apiPaintDecal, 3);
    registerBinding(ctx, "paintTransparentAt", apiPaintTransparentAt, 5);
    registerBinding(ctx, "applyBlur", apiApplyBlur, 5);
    registerBinding(ctx, "applySmooth", apiApplySmooth, 4);
    registerBinding(ctx, "applyHeightFn", apiApplyHeightFn, 4);
    registerBinding(ctx, "getDamHeight", apiGetDamHeight, 1);
    registerBinding(ctx, "getGeoColor", apiGetGeoColor, 1);
    registerBinding(ctx, "getDamColor", apiGetDamColor, 1);
    registerBinding(ctx, "setGeoPalette", apiSetGeoPalette, 2);
    registerBinding(ctx, "setDamPalette", apiSetDamPalette, 2);
    registerBinding(ctx, "showInfoMessageBox", apiShowInfoMessageBox, 1);
    registerBinding(ctx, "showErrorMessageBox", apiShowErrorMessageBox, 1);
}

/// Describes the error object on the stack top WITH its script location:
/// "Name: message (brush.js:LINE)" when duktape recorded fileName/lineNumber
/// (tracebacks), falling back to the plain safe string. Used for both the
/// syntax errors (compile) and the runtime errors (pcall) so the user sees
/// WHERE the script failed.
std::string describeError(duk_context* ctx)
{
    if (!duk_is_error(ctx, -1)) {
        return duk_safe_to_string(ctx, -1);
    }
    duk_dup(ctx, -1); // [err err]
    duk_get_prop_string(ctx, -1, "name");
    const std::string name = duk_safe_to_string(ctx, -1);
    duk_pop(ctx);
    duk_get_prop_string(ctx, -1, "message");
    const std::string message = duk_safe_to_string(ctx, -1);
    duk_pop(ctx);
    duk_get_prop_string(ctx, -1, "fileName");
    const char* file = duk_is_string(ctx, -1) ? duk_get_string(ctx, -1) : "";
    const std::string fileName = file ? file : "";
    duk_pop(ctx);
    duk_get_prop_string(ctx, -1, "lineNumber");
    const int line = duk_is_number(ctx, -1) ? duk_get_int(ctx, -1) : 0;
    duk_pop_2(ctx); // []
    if (!fileName.empty() && line > 0) {
        return name + ": " + message + " (" + fileName + ":" +
               std::to_string(line) + ")";
    }
    if (!name.empty() && !message.empty()) {
        return name + ": " + message;
    }
    return name.empty() ? message : name;
}

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
        errorOut = describeError(ctx);
        duk_pop(ctx);
        return false;
    }
    return true;
}

/// Creates the duktape heap, exposes the editor API and runs `script` once
/// with (cx, cy) as the current position. No undo grouping here — the
/// callers (runScriptedBrush/runEditorScript) own that.
bool executeScript(EditorSession& session, BrushTool& brush,
                   const BrushParams& params, int cx, int cy,
                   const std::string& script, std::string& errorOut,
                   int timeoutMs)
{
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

        registerApiBindings(ctx);

        if (compileScript(ctx, script, errorOut)) {
            if (duk_pcall(ctx, 0) == DUK_EXEC_SUCCESS) {
                ok = true;
            } else if (timeout.hit) {
                errorOut = "the script exceeded the " +
                           std::to_string(timeoutMs) +
                           " ms time limit and was aborted";
            } else {
                errorOut = describeError(ctx);
            }
        }
        duk_destroy_heap(ctx);
    }
    return ok;
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
    const bool ok = executeScript(session, brush, params, cx, cy, params.script,
                                  errorOut, timeoutMs);
    // Merge whatever was recorded (possibly nothing on a syntax error)
    // into the single undo entry.
    session.endUndoGroup();
    return ok;
}

bool runEditorScript(EditorSession& session, BrushTool& brush,
                     const BrushParams& params, int cx, int cy,
                     const std::string& script, std::string& errorOut,
                     int timeoutMs)
{
    if (script.empty()) {
        errorOut = "the script is empty";
        return false;
    }
    if (!session.isWorldLoaded()) {
        errorOut = "no world loaded";
        return false;
    }
    // One Run = one undo entry, exactly like a scripted-brush stamp.
    session.beginUndoGroup();
    const bool ok = executeScript(session, brush, params, cx, cy, script,
                                  errorOut, timeoutMs);
    session.endUndoGroup();
    return ok;
}

void setScriptedBrushMessageHook(void (*hook)(bool isError,
                                              const std::string& text))
{
    g_messageBoxHook = hook;
}

void setScriptPaletteHook(void (*hook)())
{
    g_paletteHook = hook;
}

} // namespace MapEditor

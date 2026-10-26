// ============================================================================
// MapEditorRender — editor sokol render device implementation.
//
// Subclasses the game's cSokolRender (Source/Render/sokol, compiled unmodified
// in the MapEditorRender target) and overrides only what differs when the
// "window" is a QOpenGLWidget instead of an SDL window:
//
//   Init   — no SDL GL context creation (Qt owns the context); sokol is set
//            up on whatever context is current, all other steps identical.
//   Done   — the parent skips sg_shutdown when sdl_window is null (ours is),
//            so the sg_shutdown() call moves here.
//   EndScene— retargets the inherited private `swapchain` member at the
//            widget's framebuffer object before the parent renders the pass.
//   Flush  — parent asserts without sdl_window and SDL-swaps it; here we only
//            sg_commit() and recycle the pooled resources.
//   ChangeSize — same as parent minus the SDL vsync call (Qt controls swap).
//
// cSokolRender keeps the swapchain/pool/command machinery private. The
// subclass only touches those members from this single translation unit,
// where SokolRender.h is parsed with `private` temporarily defined as
// `public` (push_macro/pop_macro keeps the hack confined to that include).
// This is deliberately chosen over copying the whole device implementation
// into the editor — see docs/architecture.md. Nothing outside this file may
// include the hacked include path.
// ============================================================================

#include <cstddef>
#include <cstdint>

// Render stack context (does NOT include SokolRender.h — its chain must parse
// with normal access before the shim below).
#include "StdAfxRD.h"
#include "DrawBuffer.h"

#include <SDL_video.h>

#include "SokolIncludes.h"
#include "SokolTypes.h"

// --- confined access shim: parse SokolRender.h with private => public -------
#pragma push_macro("private")
#undef private
#define private public
#include "SokolRender.h"
#pragma pop_macro("private")
// ----------------------------------------------------------------------------

#include "SokolRenderPipeline.h"
#include "SokolResources.h"

#include "RenderTracker.h"
#include "RenderMT.h"
#include "xerrhand.h"

#include "EditorTrace.h"
using MapEditor::editorTrace;

namespace {

const char* kBackendName = "OpenGL Core (editor)";

class EditorSokolRender : public cSokolRender {
public:
    int Init(int xScr, int yScr, int mode, SDL_Window* wnd, int RefreshRateInHz) override;
    bool ChangeSize(int xScr, int yScr, int mode) override;
    int Done() override;
    int EndScene() override;
    int Flush(bool wnd) override;

    /// Widget layer calls this once per frame, before EndScene, to point the
    /// swapchain pass at the QOpenGLWidget framebuffer. Sizes are in device
    /// pixels (widget size * devicePixelRatio), matching ScreenSize.
    void SetTargetFramebuffer(uint32_t fbo, int width, int height) {
        targetFramebuffer_ = fbo;
        targetWidth_ = width;
        targetHeight_ = height;
    }

private:
    uint32_t targetFramebuffer_ = 0;
    int targetWidth_ = 0;
    int targetHeight_ = 0;
};

int EditorSokolRender::Init(int xScr, int yScr, int mode, SDL_Window* wnd, int) {
    RenderSubmitEvent(RenderEvent::INIT, "Sokol (editor) start");

    // Base init: gb_RenderDevice, ScreenSize, TexLibrary. No SDL window ever
    // — Qt owns the GL context, sokol runs on the current context.
    int res = cInterfaceRenderDevice::Init(xScr, yScr, mode, nullptr, 0);
    if (res != 0) {
        return res;
    }

    ClearPooledResources(0);
    ClearAllCommands();
    ClearPipelines();

    //Init some state
    activePipelineType = PIPELINE_TYPE_DEFAULT;
    activePipelineMode.cull = CameraCullMode = CULL_CCW;

    //Prepare sokol gfx desc (same capacities the game uses)
    sg_desc desc = {};
    desc.pipeline_pool_size = PERIMETER_SOKOL_PIPELINES_MAX;
    desc.shader_pool_size = 64;
    desc.buffer_pool_size = 1024 * 32;
    desc.uniform_buffer_size = 1024 * 1024 * 32;
    desc.image_pool_size = 1024 * 4;
    desc.logger.func = slog_func;

    //Setup swapchain and fill color; the actual GL framebuffer is supplied
    //per frame via SetTargetFramebuffer()
    swapchain = {};
    swapchain.width = ScreenSize.x;
    swapchain.height = ScreenSize.y;
    swapchain.sample_count = 1;
    fill_color = sg_color { 0.0f, 0.0f, 0.0f, 1.0f };
#ifdef PERIMETER_SOKOL_GL
    swapchain.gl.framebuffer = 0;
#endif

    sg_setup(&desc);
    if (!sg_isvalid()) {
        ErrH.Abort("sg_setup failed for the map editor GPU viewport", XERR_CRITICAL, 0);
        return -1;
    }
    printf("Map editor GPU viewport: sokol %s\n", kBackendName);

    const simgui_desc_t simgui_desc = {};
    simgui_setup(&simgui_desc);
    debugUIEnabled = false;

    //Create sampler (copy of the game init; needed by texture/material paths)
    sg_sampler_desc sampler_desc = {};
    sampler_desc.label = "SamplerLinear";
    sampler_desc.wrap_u = SG_WRAP_REPEAT;
    sampler_desc.wrap_v = SG_WRAP_REPEAT;
    sampler_desc.min_lod = 0.0f;
    sampler_desc.max_lod = 0.0f;    // for max_lod, zero-initialized means "FLT_MAX"
    sampler_desc.min_filter = SG_FILTER_LINEAR;
    sampler_desc.mag_filter = SG_FILTER_LINEAR;
    sampler_desc.mipmap_filter = SG_FILTER_LINEAR;
    sampler = sg_make_sampler(sampler_desc);

    sg_sampler_desc shadow_sampler_desc = {};
    shadow_sampler_desc.min_filter = SG_FILTER_LINEAR;
    shadow_sampler_desc.mag_filter = SG_FILTER_LINEAR;
    shadow_sampler_desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    shadow_sampler_desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    shadow_sampler_desc.compare = SG_COMPAREFUNC_LESS;
    shadow_sampler = sg_make_sampler(&shadow_sampler_desc);

    //Create empty texture
    sg_image_desc* imgdesc = new sg_image_desc();
    imgdesc->label = nullptr;
    imgdesc->width = imgdesc->height = 64;
    imgdesc->pixel_format = SG_PIXELFORMAT_RGBA8;
    imgdesc->num_mipmaps = 1;
    imgdesc->usage = SG_USAGE_IMMUTABLE;
    size_t pixel_len = sokol_pixelformat_bytesize(imgdesc->pixel_format);
    size_t buf_len = imgdesc->height * imgdesc->width * pixel_len;
    uint8_t* buf = new uint8_t[buf_len];
    memset(buf, 0xFF, buf_len);
    imgdesc->data.subimage[0][0] = { buf, buf_len };
    emptyTexture = new SokolTexture2D(imgdesc);
    emptyTexture->label = "EmptySlotTexture";
    PrepareSokolTexture(emptyTexture);

    for (int i = 0; i < GetMaxTextureSlots(); ++i) {
        activeTextureTransform[i] = Mat4f::ID;
        SetTextureImage(i, nullptr);
    }

    RenderSubmitEvent(RenderEvent::INIT, "Sokol (editor) done");
    return UpdateRenderMode();
}

bool EditorSokolRender::ChangeSize(int xScr, int yScr, int mode) {
    int mode_mask = RENDERDEVICE_MODE_WINDOW | RENDERDEVICE_MODE_VSYNC;
    bool need_resize = xScr != ScreenSize.x || yScr != ScreenSize.y;
    if (!need_resize && (RenderMode & mode_mask) == mode) {
        return true; //Nothing to do
    }
    ScreenSize.x = xScr;
    ScreenSize.y = yScr;
    RenderMode &= ~mode_mask;
    RenderMode |= mode;
    //Update swapchain — no SDL vsync call: Qt controls the swap
    swapchain.width = ScreenSize.x;
    swapchain.height = ScreenSize.y;
    return UpdateRenderMode() == 0;
}

int EditorSokolRender::Done() {
    RenderSubmitEvent(RenderEvent::DONE, "Sokol (editor) start");
    // Parent cleans everything except sg_shutdown (skipped because sdl_window
    // is null in the editor) — do the sokol shutdown here.
    editorTrace("EditorSokolRender::Done: calling cSokolRender::Done");
    int ret = cSokolRender::Done();
    editorTrace("EditorSokolRender::Done: cSokolRender::Done returned %d", ret);
    if (sg_isvalid()) {
        editorTrace("EditorSokolRender::Done: calling sg_shutdown");
        RenderSubmitEvent(RenderEvent::DONE, "Sokol (editor) shutdown");
        sg_shutdown();
        editorTrace("EditorSokolRender::Done: sg_shutdown returned");
    }
    return ret;
}

int EditorSokolRender::EndScene() {
    if (!ActiveScene) {
        xassert(0);
        return 1;
    }
    if (targetFramebuffer_ != 0) {
        //Point the swapchain pass at the widget's FBO (device pixels). Zero
        //means no surface this frame — parent EndScene would render into
        //framebuffer 0, which is not a valid target in Qt, so bail out.
        swapchain.width = targetWidth_;
        swapchain.height = targetHeight_;
#ifdef PERIMETER_SOKOL_GL
        swapchain.gl.framebuffer = targetFramebuffer_;
#endif
    }
    return cSokolRender::EndScene();
}

int EditorSokolRender::Flush(bool wnd) {
    MT_IS_GRAPH();
    RenderSubmitEvent(RenderEvent::FLUSH_SCENE);
    (void)wnd;
    if (ActiveScene) {
        EndScene();
    }

    //Commit the sokol frame; Qt composites the widget FBO itself, no swap.
    sg_commit();

#ifdef PERIMETER_DEBUG
    const uint32_t pooled_life = 10000;
#else
    const uint32_t pooled_life = 32000;
#endif
    ClearPooledResources(pooled_life);
    ClearAllCommands();

    xassert(!activeDrawBuffer || !activeDrawBuffer->written_vertices);

    return 0;
}

} // namespace

namespace MapEditor {

cInterfaceRenderDevice* CreateEditorSokolDevice() {
    return new EditorSokolRender();
}

void setDeviceTarget(cInterfaceRenderDevice* device,
                     unsigned fbo, unsigned windowWidth, unsigned windowHeight)
{
    // EditorSokolRender is created only through the factory above, so the
    // downcast is always valid.
    auto* ed = static_cast<EditorSokolRender*>(device);
    if (ed) {
        ed->SetTargetFramebuffer(fbo,
                                 static_cast<int>(windowWidth),
                                 static_cast<int>(windowHeight));
    }
}

void resetDeviceStateCache(cInterfaceRenderDevice* device) {
    if (device) {
        sg_reset_state_cache();
    }
}

}

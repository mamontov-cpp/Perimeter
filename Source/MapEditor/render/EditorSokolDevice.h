// ============================================================================
// MapEditorRender — editor sokol render device factory.
//
// The GPU viewport renders the game's real terrain pipeline (Source/Render/
// tilemap) into a QOpenGLWidget framebuffer. That needs a
// cInterfaceRenderDevice implementation the editor controls:
//
//   * the sokol backend must be SOKOL_GLCORE (the MapEditorRender target is
//     compiled for OpenGL 4.1 core, mirroring the game's Linux configuration),
//   * the SDL window/context lifecycle of cSokolRender::Init does not exist —
//     Qt owns the GL context, sokol must run on the widget's context and
//     target the widget's framebuffer object (QOpenGLWidget renders into an
//     FBO, not the default 0),
//   * cSokolRender::Flush asserts without an SDL window and swaps it — the
//     editor must not swap; Qt composites the widget FBO itself.
//
// EditorSokolRender subclasses the game's cSokolRender (compiled unmodified
// in MapEditorRender) and overrides only the lifecycle/surface bits. The
// scene-graph terrain rendering (BeginScene / scene->Draw / EndScene /
// Flush) and all buffer/texture pooling is inherited verbatim.
//
// See docs/architecture.md ("Map editor GPU viewport") for the linkage
// decision and the confined private-access shim used in the .cpp.
// ============================================================================

#pragma once

class cInterfaceRenderDevice;

namespace MapEditor {

/// Reachability guard: this header must stay includeable from Qt TUs without
/// pulling the sokol headers in — it only exposes the factory.
cInterfaceRenderDevice* CreateEditorSokolDevice();

/// Point an editor sokol device (as returned above) at the framebuffer the
/// next frame should render into: the editor device ignores the SDL window
/// plumbing of the game device and binds `swapchain` to `fbo` each frame.
/// windowWidth/windowHeight are the target size in device pixels
/// (widget size * devicePixelRatio), matching what the camera paths read
/// back through the device.
void setDeviceTarget(cInterfaceRenderDevice* device,
                     unsigned fbo, unsigned windowWidth, unsigned windowHeight);

/// Drop the sokol GL state cache. QPainter and the texture blitter mutate
/// the shared GL state between frames behind sokol's back; call this once
/// per frame before BeginScene so sokol re-applies its state instead of
/// applying only deltas over foreign state.
void resetDeviceStateCache(cInterfaceRenderDevice* device);

}

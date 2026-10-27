// ============================================================================
// MapEditorRender — TerrainGpuView implementation.
//
// Builds and drives the game scene stack for the GPU oblique viewport. See
// TerrainGpuView.h for the ownership/architecture notes.
// ============================================================================

#include "TerrainGpuView.h"

#include "EditorSokolDevice.h"

// Game render stack (compiled unmodified in MapEditorRender). NOTE:
// StdAfxRD.h has no include guard — it must be parsed exactly once per TU.
// It comes via EditorTerraInterface.h; do not include it again here or in
// any other header of this directory.
#include "EditorTerraInterface.h"

#include "Scene.h"
#include "VisGeneric.h"
#include "../tilemap/TileMap.h"

// World state (vMap / hZeroPlast / H_SIZE ...)
#include "terra.h"

#include "ViewportRenderer.h"

#include "EditorTrace.h"

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

inline double degToRad(double deg) { return deg * kPi / 180.0; }

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Clamp a sample coordinate to map range (SGetAlt offsets are unclamped and
/// an unclamped ofs would read out of the VxDBuf/VxGBuf allocation).
inline void clampToMap(int& x, int& y) {
    x = max(0, min(vMap.H_SIZE - 1, x));
    y = max(0, min(vMap.V_SIZE - 1, y));
}

/// Terrain height in world units at (possibly out of range) cell coords —
/// same semantics as StandartTerraInterface::GetZf with clamped input.
inline float sampleTerrainZ(double x, double y) {
    int xi = static_cast<int>(xm::floor(x));
    int yi = static_cast<int>(xm::floor(y));
    clampToMap(xi, yi);
    int ofs = xi + vMap.H_SIZE * yi;
    return vMap.SGetAlt(ofs) / static_cast<float>(1 << VX_FRACTION);
}

} // namespace

namespace MapEditor {

TerrainGpuView::~TerrainGpuView() {
    editorTrace("TerrainGpuView: dtor start (device=%p vis=%p scene=%p camera=%p tilemap=%p terra=%p)",
                static_cast<void*>(device_), static_cast<void*>(visGeneric_),
                static_cast<void*>(scene_), static_cast<void*>(camera_),
                static_cast<void*>(tileMap_), static_cast<void*>(terra_));
    shutdown();
    editorTrace("TerrainGpuView: dtor done");
}

void TerrainGpuView::setColorSource(ViewportRenderer* colors) {
    if (colors_ == colors) return;
    colors_ = colors;
    if (device_ && tileMap_) {
        // The tilemap baked colors from the old source — rebuild.
        reloadWorld();
    }
}

float TerrainGpuView::zeroplastZ() const {
    return static_cast<float>(vMap.hZeroPlast);
}

bool TerrainGpuView::init(int width, int height) {
    editorTrace("TerrainGpuView: init(%d,%d) start", width, height);
    shutdown();

    if (!colors_ || !colors_->isBound()) {
        editorTrace("TerrainGpuView: init — no bound colors, bail");
        return false;
    }

    device_ = CreateEditorSokolDevice();
    if (!device_) {
        return false;
    }
    int res = device_->Init(width, height, RENDERDEVICE_MODE_WINDOW, nullptr, 0);
    editorTrace("TerrainGpuView: device Init res=%d", res);
    if (res != 0) {
        // dtor runs Done() which is safe right after a failed Init
        RELEASE(device_);
        return false;
    }

    visGeneric_ = CreateIVisGeneric();
    if (!visGeneric_) {
        editorTrace("TerrainGpuView: CreateIVisGeneric failed");
        shutdown();
        return false;
    }
    visGeneric_->SetData(device_);
    visGeneric_->SetShadowType(SHADOW_NONE, 0);

    if (!buildScene()) {
        editorTrace("TerrainGpuView: buildScene failed");
        shutdown();
        return false;
    }

    applyCamera();
    editorTrace("TerrainGpuView: init OK");
    return true;
}

bool TerrainGpuView::buildScene() {
    if (!device_ || !visGeneric_ || !colors_ || !colors_->isBound()) {
        return false;
    }

    scene_ = visGeneric_->CreateScene();
    if (!scene_) {
        return false;
    }
    // Must happen BEFORE CreateMap (asserts TileMap==nullptr there)
    scene_->DisableTileMapVisibleTest();

    terra_ = new EditorTerraInterface(colors_);
    camera_ = scene_->CreateCamera();
    camera_->SetAttr(ATTRCAMERA_PERSPECTIVE);

    // CreateMap stores Size, creates the cTileMap and SetBuffer()s it — which
    // allocates tiles, calls device CreateTilemap() and queues a full-map
    // UpdateMap rect consumed by the first scene->Draw.
    tileMap_ = scene_->CreateMap(terra_, 0);
    if (!tileMap_) {
        destroyScene();
        return false;
    }
    return true;
}

void TerrainGpuView::destroyScene() {
    editorTrace("TerrainGpuView::destroyScene start");
    // Order matters: the tilemap feeds through the camera/scene and holds the
    // device reference (dtor calls gb_RenderDevice->DeleteTilemap), the
    // camera/scene own per-frame node lists. All while the device is alive.
    //
    // cTileMap::terra is a borrowed pointer (ctor does NOT IncRef), but its
    // dtor calls RELEASE(terra) — so the tilemap effectively owns the one
    // reference to terra_.  After RELEASE(tileMap_) the terra_ pointer is
    // already dangling; releasing it again would be a double-free.
    // We must only release terra_ ourselves when tileMap_ was never created.
    const bool terraOwnedByTilemap = (tileMap_ != nullptr);
    editorTrace("TerrainGpuView::destroyScene releasing tileMap_ (terra owned by tilemap: %d)",
                terraOwnedByTilemap ? 1 : 0);
    RELEASE(tileMap_);
    if (!terraOwnedByTilemap) {
        editorTrace("TerrainGpuView::destroyScene releasing terra_ (tileMap was null)");
        RELEASE(terra_);
    } else {
        // tileMap_->~cTileMap() already called RELEASE(terra) on the shared
        // pointer; terra_ is now dangling — null it to prevent double-free
        // if destroyScene() is ever called again before a rebuild.
        terra_ = nullptr;
    }
    editorTrace("TerrainGpuView::destroyScene releasing camera_");
    RELEASE(camera_);
    editorTrace("TerrainGpuView::destroyScene releasing scene_");
    RELEASE(scene_);
    editorTrace("TerrainGpuView::destroyScene done");
}

void TerrainGpuView::shutdown() {
    editorTrace("TerrainGpuView::shutdown start");
    destroyScene();
    editorTrace("TerrainGpuView::shutdown releasing visGeneric_");
    RELEASE(visGeneric_);
    editorTrace("TerrainGpuView::shutdown releasing device_");
    // dtor chain runs Done() (parent cleanup + sg_shutdown for our null window)
    RELEASE(device_);
    editorTrace("TerrainGpuView::shutdown done");
}

void TerrainGpuView::reloadWorld() {
    if (!device_) {
        return;
    }
    destroyScene();
    buildScene();
    applyCamera();
}

void TerrainGpuView::updateCells(int x0, int y0, int x1, int y1) {
    if (!tileMap_) return;
    tileMap_->UpdateMap(Vect2i(x0, y0), Vect2i(x1, y1));
}

void TerrainGpuView::applyCamera() {
    if (!camera_ || !device_) return;

    const CameraState& c = cameraState_;
    // Depression below the horizontal plane; the editor never uses a
    // degenerate angle (nadir lean=90 handled by clamping to >90).
    const double leanRad = degToRad(clampd(c.leanDeg, 90.5, 179.0));
    const double turnRad = degToRad(c.turnAngleDeg);
    const double theta = leanRad - kPi / 2.0;      // polar angle from +Z

    // Camera height above the zeroplast plane (world units); combined with
    // the focal length below it yields the ground scale — the widget maps
    // heightCells = focusC + cameraDZ so nadir cells-per-widget-px is
    // (focusC + cameraDZ) / focusC, exactly the CPU view's formula.
    const double H = c.heightCells;
    const double r = H / xm::cos(theta);

    const double zZero = static_cast<double>(vMap.hZeroPlast);
    const Vect3f lookAt(static_cast<float>(c.centerX),
                        static_cast<float>(c.centerY),
                        static_cast<float>(zZero));

    // Game matrix construction (terCameraType::update,
    // Source/Game/CameraManager.cpp) — copied verbatim with our spherical
    // parameters: psi = PI/2 + turn, theta = lean - PI/2, r = H/cos(theta).
    const float psi = static_cast<float>(kPi / 2.0 + turnRad);
    Vect3f position;
    position.setSpherical(psi, static_cast<float>(theta), static_cast<float>(r));
    position += lookAt;

    MatXf matrix = MatXf::ID;
    matrix.rot() = Mat3f(static_cast<float>(theta), X_AXIS)
                 * Mat3f(static_cast<float>(kPi / 2.0 - psi), Z_AXIS);
    matrix *= MatXf(Mat3f::ID, -position);

    // CameraManager::SetCameraPosition flips (Source/Game/CameraManager.cpp):
    // ml flips the view z axis, mr the x axis of the right transform.
    MatXf ml = MatXf::ID;
    MatXf mr = MatXf::ID;
    ml.rot()[2][2] = -1;
    mr.rot()[1][1] = -1;
    MatXf cameraMatrix = mr * ml * matrix;
    camera_->SetPosition(cameraMatrix);   // calls Update()

    // Frustum — same values the game uses (terCameraType::SetFrustumGame),
    // except the focal length comes from the editor camera: focalDevPx is
    // the focal length in device pixels, the clip-space focus is
    // focalDevPx / screen width. Near plane follows the camera height so
    // close-up zooms stay visible.
    Vect2f center(0.5f, 0.5f);
    sRectangle4f clip(-0.5f, -0.5f, 0.5f, 0.5f);
    float focusClip = static_cast<float>(c.focalDevPx / device_->GetSizeX());
    Vect2f focus(focusClip, focusClip);
    Vect2f zplane(clampd(H * 0.1, 1.0, 30.0), 1e5f);
    camera_->SetFrustum(&center, &clip, &focus, &zplane);

    // Push the recomputed matrices into the device. cSokolRender::SetDrawNode
    // early-outs when the DrawNode POINTER is unchanged, and it is the only
    // caller of SetDrawTransform → SetVPMatrix — so with a single editor
    // camera the view-projection would be frozen at its first-frame value
    // (in the game the pointer alternates between the main and the
    // reflection/shadow cameras every frame, which hides this). Feed the
    // transform explicitly; it also refreshes viewport/clip from the camera.
    device_->SetDrawTransform(camera_);
}

void TerrainGpuView::renderFrame(uint32_t framebuffer, int width, int height) {
    if (!device_ || !tileMap_ || framebuffer == 0 || width <= 0 || height <= 0) {
        return;
    }

    // sokol does not own the GL context here — QPainter and the texture
    // blitter of the previous frame mutated shared state, so drop sokol's
    // state cache and make it re-apply everything this frame.
    resetDeviceStateCache(device_);

    if (width != device_->GetSizeX() || height != device_->GetSizeY()) {
        device_->ChangeSize(width, height, RENDERDEVICE_MODE_WINDOW);
    }

    // Aim the sokol swapchain pass at the widget's FBO before EndScene.
    setDeviceTarget(device_, framebuffer,
                    static_cast<unsigned>(width), static_cast<unsigned>(height));

    applyCamera();

    // Game frame flow (Source/Game/GameShell.cpp) minus the logic side:
    device_->Fill(0, 0, 0, 255);
    device_->BeginScene();
    scene_->Draw(camera_);   // PreDrawScene + TileMap->PreDraw + DrawScene
    device_->EndScene();     // renders the pass into the FBO
    device_->Flush(false);   // sg_commit, no swap (Qt composites)
}

bool TerrainGpuView::pickCell(double sxPx, double syPx, int& cellX, int& cellY) const {
    if (!camera_ || !device_) return false;
    if (sxPx < 0 || syPx < 0
        || sxPx >= device_->GetSizeX() || syPx >= device_->GetSizeY()) {
        return false;
    }

    // Screen (device px, y down) → ray input domain: [-0.5, 0.5] centered at
    // the screen center, y up (see cCamera::ConvertorCameraToWorld).
    Vect2f pe(static_cast<float>(sxPx / device_->GetSizeX() - 0.5),
              static_cast<float>(1.0 - 2.0 * syPx / device_->GetSizeY()));
    Vect3f pos, dir;
    camera_->GetWorldRay(pe, pos, dir);
    if (dir.z >= -1e-6) {
        return false; // looking away from the ground
    }

    const double zZero = static_cast<double>(vMap.hZeroPlast);
    // March to a bit past the point where the ray crosses the zeroplast
    // plane (terrain never rises much above it in the editor).
    double tMax = (zZero - pos.z) / dir.z * 1.05 + 32.0;
    tMax = clampd(tMax, 32.0, 60000.0);
    const double step = max(0.5, tMax / 2048.0);

    double tBelow = -1.0;
    double t = 0.0;
    while (t <= tMax) {
        Vect3f p = pos + dir * static_cast<float>(t);
        if (p.z <= sampleTerrainZ(p.x, p.y)) {
            tBelow = t;
            break;
        }
        t += step;
    }
    if (tBelow < 0.0) {
        return false;
    }

    // Bisect between the last above-terrain t and tBelow.
    double tLo = max(0.0, tBelow - step), tHi = tBelow;
    for (int i = 0; i < 12; ++i) {
        double tm = 0.5 * (tLo + tHi);
        Vect3f p = pos + dir * static_cast<float>(tm);
        if (p.z <= sampleTerrainZ(p.x, p.y)) tHi = tm; else tLo = tm;
    }
    Vect3f hit = pos + dir * static_cast<float>(tHi);

    int cx = static_cast<int>(xm::floor(hit.x));
    int cy = static_cast<int>(xm::floor(hit.y));
    if (cx < 0 || cy < 0 || cx >= vMap.H_SIZE || cy >= vMap.V_SIZE) {
        return false;
    }
    cellX = cx;
    cellY = cy;
    return true;
}

bool TerrainGpuView::projectToScreen(double cellX, double cellY, double z,
                                     double& sxPx, double& syPx) const {
    if (!camera_) return false;
    Vect3f pw(static_cast<float>(cellX), static_cast<float>(cellY),
              static_cast<float>(z));
    Vect3f pv, pe;
    camera_->ConvertorWorldToViewPort(&pw, &pv, &pe);
    if (pv.z <= 0) {
        return false; // behind the camera
    }
    sxPx = pe.x;
    syPx = pe.y;
    return true;
}

} // namespace MapEditor

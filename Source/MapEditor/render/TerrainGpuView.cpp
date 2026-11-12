// ============================================================================
// MapEditorRender — TerrainGpuView implementation.
//
// Builds and drives the game scene stack for the GPU oblique viewport. See
// TerrainGpuView.h for the ownership/architecture notes.
// ============================================================================

#include "TerrainGpuView.h"

#include "EditorMath.h"

#include "EditorSokolDevice.h"

// Game render stack (compiled unmodified in MapEditorRender). NOTE:
// StdAfxRD.h has no include guard — it must be parsed exactly once per TU.
// It comes via EditorTerraInterface.h; do not include it again here or in
// any other header of this directory.
#include "EditorTerraInterface.h"

#include "Scene.h"
#include "VisGeneric.h"
#include "ObjNode.h"
#include "UnkLight.h"
#include "../inc/VisGenericDefine.h"
#include "../tilemap/TileMap.h"

#include "UnitModelCatalog.h"

// World state (vMap / hZeroPlast / H_SIZE ...)
#include "terra.h"

#include "ViewportRenderer.h"

#include "EditorTrace.h"

#include <cmath>

namespace {

// kPi lives in EditorMath.h (shared math constants).

inline double degToRad(double deg) { return deg * MapEditor::kPi / 180.0; }

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// The game camera matrix construction (terCameraType::update +
/// CameraManager::SetCameraPosition flips, Source/Game/CameraManager.cpp) —
/// one place for the matrix + flip recipe shared by the editor camera, the
/// game camera branch and the icon baker.
inline MatXf buildCameraMatrix(const Vect3f& position, float theta, float psi)
{
    MatXf matrix = MatXf::ID;
    matrix.rot() = Mat3f(theta, X_AXIS)
                 * Mat3f(static_cast<float>(MapEditor::kPi / 2.0) - psi, Z_AXIS);
    matrix *= MatXf(Mat3f::ID, -position);
    // ml flips the view z axis, mr the x axis of the right transform.
    MatXf ml = MatXf::ID;
    MatXf mr = MatXf::ID;
    ml.rot()[2][2] = -1;
    mr.rot()[1][1] = -1;
    return mr * ml * matrix;
}

/// Clamp a sample coordinate to map range (SGetAlt offsets are unclamped and
/// an unclamped ofs would read out of the VxDBuf/VxGBuf allocation).
inline void clampToMap(int& x, int& y) {
    x = max(0, min(vMap.H_SIZE - 1, x));
    y = max(0, min(vMap.V_SIZE - 1, y));
}

/// Terrain height in world units at (possibly out of range) cell coords —
/// same semantics as StandartTerraInterface::GetZf with clamped input.
/// Height-slice mode: clipped at the slice plane like the rendered mesh, so
/// picking and the brush contour land on the visible (clipped) surface.
inline float sampleTerrainZ(double x, double y) {
    int xi = static_cast<int>(xm::floor(x));
    int yi = static_cast<int>(xm::floor(y));
    clampToMap(xi, yi);
    int ofs = xi + vMap.H_SIZE * yi;
    float z = vMap.SGetAlt(ofs) / static_cast<float>(1 << VX_FRACTION);
    const int slice = vMap.viewSliceHeight;
    if (slice >= 0 && z > static_cast<float>(slice)) {
        z = static_cast<float>(slice);
    }
    return z;
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

float TerrainGpuView::nearPlaneZ() const {
    if (!camera_) return 1.0f;
    return camera_->GetZPlane().x;
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
    // Models reference effect libraries through "effect: <lib> <name>" node
    // names (cObjLibrary::ParseNodeEffect) — without the path VisError
    // aborts on the first such model (e.g. filth_spot.m3d -> steam.effect).
    // Same paths the game sets (HTManager::initGraphics, Runtime.cpp).
    visGeneric_->SetEffectLibraryPath("RESOURCE\\FX",
                                      "RESOURCE\\FX\\TEXTURES");

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

    // Placed-building models ride the same scene->Draw.
    buildingModels_.init(scene_);
    return true;
}

void TerrainGpuView::destroyScene() {
    editorTrace("TerrainGpuView::destroyScene start");
    // Building model instances and the icon stack reference the scene and
    // device — tear them down first.
    buildingModels_.shutdown();
    destroyIconScene();
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
    if (c.gameMode) {
        applyGameCamera();
        return;
    }

    // Depression below the horizontal plane; the editor never uses a
    // degenerate angle (nadir lean=90 handled by clamping to >90).
    const double leanRad = degToRad(clampd(c.leanDeg, 90.5, 179.0));
    const double turnRad = degToRad(c.turnAngleDeg);
    const double theta = leanRad - MapEditor::kPi / 2.0;      // polar angle from +Z

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
    const float psi = static_cast<float>(MapEditor::kPi / 2.0 + turnRad);
    Vect3f position;
    position.setSpherical(psi, static_cast<float>(theta), static_cast<float>(r));
    position += lookAt;

    const MatXf cameraMatrix =
        buildCameraMatrix(position, static_cast<float>(theta), psi);
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

void TerrainGpuView::applyGameCamera() {
    // The main game's camera model, replicated from terCameraType
    // (Source/Game/CameraManager.cpp): update() builds the position from the
    // spherical CameraCoordinate, SetFrustumGame() sets the frustum.
    const CameraState& c = cameraState_;

    const double psi = degToRad(c.psiDeg);
    // 0 = nadir (the game's default at battle start); clamp just below the
    // horizon so the projection stays finite even if a caller pushes further
    // than the game's restricted tilt limit (PI/2.85).
    const double theta = degToRad(clampd(c.thetaDeg, 0.0, 89.0));

    // terCameraType::update(): the aspect factor stretches the radius so a
    // 4:3 window and a 16:9 window show the same vertical ground span.
    const double aspectScale =
        static_cast<double>(device_->GetSizeX()) /
        static_cast<double>(device_->GetSizeY()) / (4.0 / 3.0);
    const double r = c.distance * aspectScale;

    const double zZero = static_cast<double>(vMap.hZeroPlast);
    const Vect3f lookAt(static_cast<float>(c.centerX),
                        static_cast<float>(c.centerY),
                        static_cast<float>(zZero));
    Vect3f position;
    position.setSpherical(static_cast<float>(psi), static_cast<float>(theta),
                          static_cast<float>(r));
    position += lookAt;
    // terCameraType::update() clamps the camera z into
    // [CAMERA_MIN_HEIGHT, CAMERA_MAX_HEIGHT] (restricted game, the shipped
    // Perimeter.ini sets CameraRestriction=1).
    position.z = clampd(position.z, 200.0, 10000.0);

    // The game matrix construction — same as the editor branch, but with the
    // game's psi/theta angles.
    const MatXf cameraMatrix =
        buildCameraMatrix(position, static_cast<float>(theta), psi);
    camera_->SetPosition(cameraMatrix);   // calls Update()

    // terCameraType::SetFrustumGame verbatim: focus = the game's
    // HardwareCameraFocus script option (the FOV), zplane (30, 1e5).
    Vect2f center(0.5f, 0.5f);
    sRectangle4f clip(-0.5f, -0.5f, 0.5f, 0.5f);
    const float focusClip = static_cast<float>(c.focusClip);
    Vect2f focus(focusClip, focusClip);
    Vect2f zplane(30.0f, 1e5f);
    camera_->SetFrustum(&center, &clip, &focus, &zplane);

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

    // Switch the sokol sampler to nearest-neighbor for chunky mode (no
    // bilinear smoothing) or linear for smooth — matching the CPU path.
    // Must happen before BeginScene so the tilemap pipeline binds the
    // correct sampler.
    MapEditor::setDeviceTextureFilter(device_, nearestFiltering_);

    // Game frame flow (Source/Game/GameShell.cpp) minus the logic side:
    device_->Fill(0, 0, 0, 255);
    device_->BeginScene();
    scene_->Draw(camera_);   // PreDrawScene + TileMap->PreDraw + DrawScene
    device_->EndScene();     // renders the pass into the FBO
    device_->Flush(false);   // sg_commit, no swap (Qt composites)
}

void TerrainGpuView::syncBuildingModels(const std::vector<BuildingModelPose>& poses) {
    if (!scene_) {
        return;
    }
    buildingModels_.sync(poses, zeroplastZ());
}

void TerrainGpuView::setBuildingModelPreview(bool valid, const BuildingModelPose& pose) {
    if (!scene_) {
        return;
    }
    buildingModels_.setPreview(valid, pose, zeroplastZ());
}

void TerrainGpuView::destroyIconScene() {
    if (iconCamera_) {
        iconCamera_->Release();
        iconCamera_ = nullptr;
    }
    if (iconLight_) {
        iconLight_->Release();
        iconLight_ = nullptr;
    }
    if (iconScene_) {
        iconScene_->Release();
        iconScene_ = nullptr;
    }
}

bool TerrainGpuView::renderModelIcon(uint32_t framebuffer, int width, int height,
                                     int attributeId) {
    if (!device_ || !visGeneric_ || framebuffer == 0 || width <= 0 || height <= 0) {
        return false;
    }
    const UnitModelInfo* info = unitModelInfo(attributeId);
    if (!info) {
        return false;
    }

    // Icon scene stack — same recipe as the viewport scene but standalone so
    // baking never touches the terrain state.
    if (!iconScene_) {
        iconScene_ = visGeneric_->CreateScene();
        if (!iconScene_) {
            return false;
        }
        iconCamera_ = iconScene_->CreateCamera();
        iconCamera_->SetAttr(ATTRCAMERA_PERSPECTIVE);
        iconLight_ = iconScene_->CreateLight(ATTRLIGHT_DIRECTION);
        if (iconLight_) {
            iconLight_->SetPosition(MatXf(Mat3f::ID, Vect3f(0.f, 0.f, 0.f)));
            sColor4f ambient(0.3f, 0.3f, 0.32f, 1.f);
            sColor4f diffuse(1.f, 1.f, 1.f, 1.f);
            iconLight_->SetColor(&ambient, &diffuse);
            Vect3f dir(-234.f, 0.f, -180.f);
            dir.normalize();
            iconLight_->SetDirection(dir);
        }
    }

    cObjectNodeRoot* node = iconScene_->CreateObject(
        info->modelFile, "Resource\\Models\\Main\\EXODUS\\");
    if (!node) {
        editorTrace("TerrainGpuView: icon model load failed (%s)", info->modelFile);
        return false;
    }

    // Scale and center the bbox at the origin (game scale formula — see
    // BuildingModels::poseInstance).
    sBox6f bound;
    node->GetBoundBox(bound);
    const float nativeRadius = bound.GetRadius();
    float scale = static_cast<float>(info->boundScale);
    if (info->boundRadius > 0.0 && nativeRadius > 1e-6f) {
        scale = static_cast<float>(info->boundRadius) / nativeRadius;
    }
    node->SetScale(Vect3f(scale, scale, scale));
    const Vect3f center = (bound.min + bound.max) * 0.5f * scale;
    const float radius = nativeRadius * scale;
    node->SetPosition(MatXf(Mat3f::ID, -center));
    node->Update();

    // Camera: spherical position (BGScene recipe, Source/UserInterface/
    // BGScene.cpp), pulled back far enough to fit the bounds with margin.
    // Frustum focus f gives a visible height of dist/f at the target plane.
    const float theta = static_cast<float>(50.0 * kDegToRad);  // from +Z (nadir)
    const float psi = static_cast<float>(-35.0 * kDegToRad);   // azimuth
    const float dist = radius * 3.4f + 1e-3f;
    Vect3f position;
    position.setSpherical(psi, theta, dist);

    iconCamera_->SetPosition(buildCameraMatrix(position, theta, psi));

    const float focus = dist / (radius * 2.6f + 1e-3f);
    Vect2f camCenter(0.5f, 0.5f);
    sRectangle4f clip(-0.5f, -0.5f, 0.5f, 0.5f);
    Vect2f focusV(focus, focus);
    Vect2f zplane(dist - radius * 2.f > 1.f ? dist - radius * 2.f : 1.f,
                  dist + radius * 4.f);
    iconCamera_->SetFrustum(&camCenter, &clip, &focusV, &zplane);
    device_->SetDrawTransform(iconCamera_);

    // Same frame flow as renderFrame, into the caller's FBO, transparent
    // background. The next renderFrame retargets the device at the widget
    // FBO automatically.
    resetDeviceStateCache(device_);
    if (width != device_->GetSizeX() || height != device_->GetSizeY()) {
        device_->ChangeSize(width, height, RENDERDEVICE_MODE_WINDOW);
    }
    setDeviceTarget(device_, framebuffer, static_cast<unsigned>(width),
                    static_cast<unsigned>(height));
    device_->Fill(0, 0, 0, 0);
    device_->BeginScene();
    iconScene_->Draw(iconCamera_);
    device_->EndScene();
    device_->Flush(false);

    node->Release();
    return true;
}

bool TerrainGpuView::pickCell(double sxPx, double syPx, int& cellX, int& cellY) const {
    if (!camera_ || !device_) return false;
    if (sxPx < 0 || syPx < 0
        || sxPx >= device_->GetSizeX() || syPx >= device_->GetSizeY()) {
        return false;
    }

    // Screen (device px, y down) → ray input domain: pe in [-0.5, 0.5]
    // centered at the screen center. The game's ConvertorCameraToWorld
    // expects pe in screen coordinates (y=0 at top, y=1 at bottom) and
    // negates y internally to convert to view-space y-up. We pass
    // pe.y = syPx/deviceY - 0.5 so the game's internal negation produces
    // the correct view-space direction without double-inversion.
    Vect2f pe(static_cast<float>(sxPx / device_->GetSizeX() - 0.5),
              static_cast<float>(syPx / device_->GetSizeY() - 0.5));
    Vect3f pos, dir;
    camera_->GetWorldRay(pe, pos, dir);
    // dir.z is the ray's vertical component in world space. A value near 0
    // means the ray points almost horizontally. The previous -1e-6 threshold
    // was too tight: at extreme lean angles the ray to terrain near the
    // horizon can have dir.z ≈ -0.001, which would be rejected. Also
    // increase tMax so the ray reaches terrain at the far edge of the map
    // where the GPU camera's wider FOV still sees ground but the ray is
    // very shallow.
    if (dir.z >= -1e-3) {
        return false; // looking away from the ground
    }

    // The march must continue PAST the zeroplast plane down to the lowest
    // possible terrain. Heights are stored unsigned (SGetAlt), so the floor
    // is 0; geo-only cells (no dam) can sit well below the zeroplast plane,
    // and a march that stops at zeroplast height never reaches them — the
    // ray stays above the geo surface, pickCell returns false and the brush
    // cursor disappears over geo-only ground. March to the z=0 plane instead.
    const double zFloor = 0.0;
    double tMax = (zFloor - pos.z) / dir.z * 1.02 + 64.0;
    tMax = clampd(tMax, 32.0, 200000.0);
    const double step = max(0.25, tMax / 4096.0);

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

bool TerrainGpuView::groundFootprint(double planeZ, double outX[4], double outY[4]) const {
    if (!camera_ || !device_) return false;
    const double sizeX = device_->GetSizeX();
    const double sizeY = device_->GetSizeY();
    if (sizeX < 2.0 || sizeY < 2.0) return false;

    // A ray that would hit the plane further than this is clamped: near the
    // horizon the ground distance explodes to infinity, and an unbounded
    // corner makes the minimap polygon useless (it is clipped to the map
    // anyway). The map diagonal is a sane visual bound.
    const double maxDist = std::hypot(static_cast<double>(vMap.H_SIZE),
                                      static_cast<double>(vMap.V_SIZE));

    // Screen corners in device pixels (y down), in screen order.
    const double sx[4] = {0.0, sizeX, sizeX, 0.0};
    const double sy[4] = {0.0, 0.0, sizeY, sizeY};

    for (int i = 0; i < 4; ++i) {
        // Same input domain as pickCell: pe in [-0.5, 0.5] of the screen.
        Vect2f pe(static_cast<float>(sx[i] / sizeX - 0.5),
                  static_cast<float>(sy[i] / sizeY - 0.5));
        Vect3f pos, dir;
        camera_->GetWorldRay(pe, pos, dir);

        const double hlen = std::hypot(static_cast<double>(dir.x),
                                      static_cast<double>(dir.y));
        double t = -1.0;
        if (dir.z < -1e-6 && pos.z > planeZ) {
            t = (planeZ - pos.z) / dir.z; // > 0: the ray descends to the plane
        }
        if (!(t > 0.0)) {
            // Parallel to / rising from the plane — take a far point instead.
            t = hlen > 1e-6 ? maxDist / hlen : 0.0;
        }
        if (hlen > 1e-6 && t * hlen > maxDist) {
            t = maxDist / hlen;
        }
        const Vect3f p = pos + dir * static_cast<float>(t);
        outX[i] = p.x;
        outY[i] = p.y;
    }
    return true;
}

int TerrainGpuView::projectGroundContour(const double* cellXY, int count,
                                         double* outSx, double* outSy) const {
    if (!camera_ || !cellXY || count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const double x = cellXY[2 * i + 0];
        const double y = cellXY[2 * i + 1];
        // Clamped like sampleTerrainZ: a contour a bit outside the map still
        // projects (the stamp wraps at the seams anyway).
        const double z = sampleTerrainZ(x, y);
        Vect3f pw(static_cast<float>(x), static_cast<float>(y),
                  static_cast<float>(z));
        Vect3f pv, pe;
        camera_->ConvertorWorldToViewPort(&pw, &pv, &pe);
        if (pv.z <= 0) {
            continue; // behind the camera
        }
        outSx[n] = pe.x;
        outSy[n] = pe.y;
        ++n;
    }
    return n;
}

bool TerrainGpuView::projectToScreenZ(double cellX, double cellY, double z,
                                      double& camZ, double& sxPx, double& syPx) const {
    if (!camera_) return false;
    Vect3f pw(static_cast<float>(cellX), static_cast<float>(cellY),
              static_cast<float>(z));
    Vect3f pv, pe;
    camera_->ConvertorWorldToViewPort(&pw, &pv, &pe);
    camZ = pv.z;
    sxPx = pe.x;
    syPx = pe.y;
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

// ============================================================================
// MapEditorRender — BuildingModels.
//
// Renders the pre-placed battle-map buildings as real game models (M3D) in
// the GPU viewport's scene. The heavy lifting is the game's own stack,
// compiled unmodified into MapEditorRender (cObjLibrary::LoadM3D via
// cScene::CreateObject — the same path the game uses through createObject):
// attaching a cObjectNodeRoot to the editor scene makes the existing
// scene_->Draw(camera_) render it with textures and lighting.
//
// Pose replication (terBuilding::setPose / terUnitReal::setRealModel,
// Source/Units/IronBuilding.cpp / RealUnit.cpp):
//   scale  = BoundRadius > 0 ? BoundRadius / nativeBoundRadius : BoundScale
//   z      = hZeroPlast - boxMin().z        (boxMin = scaled model bounds)
//   rot    = yaw around +Z (the spg "orientaion" quaternion is yaw-only for
//            buildings; the UI carries it as angleDeg)
// Texture path is the game's default faction path (GetBelligerentTexturePath
// with the default belligerent -> EXODUS); pre-placed buildings belong to
// nobody, and the game re-skins nothing for them at load.
//
// All calls happen on the Qt UI thread with the GL context current
// (paintGL), which matches the render stack's single-threaded contract.
// ============================================================================

#pragma once

#include <vector>

class cScene;
class cObjectNodeRoot;
class cUnkLight;

namespace MapEditor {

/// One placed building as the render layer needs it (plain data — this
/// header is included from Qt UI code, keep it game-header-free).
struct BuildingModelPose
{
    int attributeId = -1;   ///< terUnitAttributeID (UnitModelCatalog row).
    float x = 0.f;          ///< Map cell == world unit (see TerrainGpuView).
    float y = 0.f;
    float angleDeg = 0.f;   ///< Yaw around +Z, degrees.
};

class BuildingModels
{
public:
    BuildingModels() = default;
    ~BuildingModels();
    BuildingModels(const BuildingModels&) = delete;
    BuildingModels& operator=(const BuildingModels&) = delete;

    /// Attach to a scene and create the directional light (the game's
    /// Runtime.cpp recipe, with a small ambient so back faces stay readable
    /// in the editor). Called once after the scene exists.
    void init(cScene* scene);
    /// Release every instance and the light (scene still alive).
    void shutdown();

    bool isActive() const { return scene_ != nullptr; }
    int instanceCount() const { return static_cast<int>(nodes_.size()); }

    /// Reconcile the scene instances with the pose list (index-aligned):
    /// grows/shrinks the pool and re-poses every node. `zeroPlastZ` is the
    /// zeroplast plane height in world units (vMap.hZeroPlast).
    void sync(const std::vector<BuildingModelPose>& poses, float zeroPlastZ);

    /// Placement-preview instance; valid=false hides it (ATTRUNKOBJ_IGNORE).
    void setPreview(bool valid, const BuildingModelPose& pose, float zeroPlastZ);

    /// Whether a model instance exists for pose index (the UI falls back to
    /// the ghost disc when false — e.g. game folder without the M3D).
    bool hasInstance(int index) const { return index >= 0 && index < static_cast<int>(nodes_.size()) && nodes_[index] != nullptr; }
    bool hasPreviewInstance() const { return previewNode_ != nullptr; }

private:
    cObjectNodeRoot* createInstance(int attributeId);
    void poseInstance(cObjectNodeRoot* node, int attributeId,
                      const BuildingModelPose& pose, float zeroPlastZ);

    cScene* scene_ = nullptr;                    ///< not owned
    cUnkLight* light_ = nullptr;                 ///< owned (scene-attached)
    std::vector<cObjectNodeRoot*> nodes_;        ///< owned, scene-attached
    std::vector<int> nodeAttributeIds_;          ///< parallel to nodes_
    cObjectNodeRoot* previewNode_ = nullptr;     ///< owned, scene-attached
    int previewAttributeId_ = -1;
};

} // namespace MapEditor

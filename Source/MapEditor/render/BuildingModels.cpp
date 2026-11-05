// ============================================================================
// MapEditorRender — BuildingModels implementation. See the header for the
// game-side references this replicates.
// ============================================================================

#include "BuildingModels.h"

// Game render stack (compiled unmodified in MapEditorRender). NOTE:
// StdAfxRD.h has no include guard — it must be parsed exactly once per TU.
// It comes via EditorTerraInterface.h; do not include it again here or in
// any other header of this directory.
#include "EditorTerraInterface.h"

#include "Scene.h"
#include "ObjNode.h"
#include "UnkLight.h"

#include "../inc/VisGenericDefine.h" // ATTRLIGHT_DIRECTION, ATTRUNKOBJ_*

#include "UnitModelCatalog.h"
#include "EditorMath.h"
#include "EditorTrace.h"

#include <cmath>

namespace MapEditor {

namespace {

/// Game default faction texture path (GetBelligerentTexturePath,
/// Source/Units/UnitAttribute.cpp — the default belligerent maps to EXODUS).
const char* kFactionTexturePath = "Resource\\Models\\Main\\EXODUS\\";

/// Scale for an attribute: BoundRadius > 0 scales the native bound radius
/// to it, otherwise BoundScale (GeometryAttribute::initGeometryAttribute).
float modelScale(const UnitModelInfo* info, float nativeBoundRadius)
{
    if (info->boundRadius > 0.0 && nativeBoundRadius > 1e-6f) {
        return static_cast<float>(info->boundRadius) / nativeBoundRadius;
    }
    return static_cast<float>(info->boundScale);
}

} // namespace

BuildingModels::~BuildingModels()
{
    // Instances are released in shutdown(); the scene releases any that are
    // still attached when it dies, so a missing shutdown is not a leak crash.
    shutdown();
}

void BuildingModels::init(cScene* scene)
{
    if (!scene || scene_) {
        return;
    }
    scene_ = scene;

    // The game's scene light (HTManager::initGraphics, Source/Game/Runtime.cpp)
    // with a raised ambient: the game fills ambient from the world's light
    // environment elsewhere; a pitch-black back side would be unusable here.
    light_ = scene_->CreateLight(ATTRLIGHT_DIRECTION);
    if (light_) {
        light_->SetPosition(MatXf(Mat3f::ID, Vect3f(0.f, 0.f, 0.f)));
        sColor4f ambient(0.3f, 0.3f, 0.32f, 1.f);
        sColor4f diffuse(1.f, 1.f, 1.f, 1.f);
        light_->SetColor(&ambient, &diffuse);
        Vect3f dir(-234.f, 0.f, -180.f);
        dir.normalize();
        light_->SetDirection(dir);
    }
}

void BuildingModels::shutdown()
{
    for (cObjectNodeRoot* node : nodes_) {
        if (node) {
            node->Release();
        }
    }
    nodes_.clear();
    nodeKeys_.clear();

    if (previewNode_) {
        previewNode_->Release();
        previewNode_ = nullptr;
        previewKey_.clear();
    }

    if (light_) {
        light_->Release();
        light_ = nullptr;
    }
    scene_ = nullptr;
}

cObjectNodeRoot* BuildingModels::createInstance(const BuildingModelPose& pose)
{
    if (!scene_) {
        return nullptr;
    }
    if (pose.modelFile) {
        // Nature object: the model comes from the save entry; textures
        // resolve relative to the model's own directory.
        return scene_->CreateObject(pose.modelFile, nullptr);
    }
    const UnitModelInfo* info = unitModelInfo(pose.attributeId);
    if (!info) {
        return nullptr;
    }
    cObjectNodeRoot* node = scene_->CreateObject(info->modelFile, kFactionTexturePath);
    if (!node) {
        editorTrace("BuildingModels: model load failed for attribute %d (%s)",
                    pose.attributeId, info->modelFile);
        return nullptr;
    }
    return node;
}

void BuildingModels::poseInstance(cObjectNodeRoot* node, int attributeId,
                                  const BuildingModelPose& pose,
                                  float zeroPlastZ)
{
    // GlobalBound is computed at load (cObjLibrary::LoadM3D -> CalcBorder):
    // the unscaled local-space bounds of the whole hierarchy.
    sBox6f bound;
    node->GetBoundBox(bound);

    if (pose.modelFile) {
        // Nature object: radius IS the footprint — scale the model so its
        // native bound radius equals it; z is stored verbatim (the game
        // does not re-snap nature objects on load).
        const float nativeRadius = bound.GetRadius();
        const float scale =
            nativeRadius > 1e-6f ? pose.radius / nativeRadius : 1.f;
        node->SetScale(Vect3f(scale, scale, scale));
        Mat3f rot(static_cast<float>(pose.angleDeg * kDegToRad), Z_AXIS);
        node->SetPosition(
            MatXf(rot, Vect3f(pose.x, pose.y, pose.z)));
        node->Update();
        return;
    }

    const UnitModelInfo* info = unitModelInfo(attributeId);
    if (!info) {
        return;
    }
    const float scale = modelScale(info, bound.GetRadius());

    node->SetScale(Vect3f(scale, scale, scale));

    // terBuilding::setPose: z snaps so the scaled box bottom sits on the
    // zeroplast plane (x/y stay the saved map position; 1 cell == 1 world
    // unit in this engine, see TerrainGpuView).
    const float z = zeroPlastZ - bound.min.z * scale;

    Mat3f rot(static_cast<float>(pose.angleDeg * kDegToRad), Z_AXIS);
    node->SetPosition(MatXf(rot, Vect3f(pose.x, pose.y, z)));
    node->Update();
}

void BuildingModels::sync(const std::vector<BuildingModelPose>& poses, float zeroPlastZ)
{
    if (!scene_) {
        return;
    }

    // Shrink the pool.
    while (static_cast<int>(nodes_.size()) > static_cast<int>(poses.size())) {
        if (nodes_.back()) {
            nodes_.back()->Release();
        }
        nodes_.pop_back();
        nodeKeys_.pop_back();
    }

    // Grow the pool.
    while (static_cast<int>(nodes_.size()) < static_cast<int>(poses.size())) {
        const int index = static_cast<int>(nodes_.size());
        nodes_.push_back(createInstance(poses[index]));
        nodeKeys_.push_back(instanceKey(poses[index]));
    }

    // Identity change on an existing slot (attribute id for catalog
    // buildings, model file for nature objects): recreate the instance.
    for (int i = 0; i < static_cast<int>(poses.size()); ++i) {
        if (nodeKeys_[i] != instanceKey(poses[i])) {
            if (nodes_[i]) {
                nodes_[i]->Release();
            }
            nodes_[i] = createInstance(poses[i]);
            nodeKeys_[i] = instanceKey(poses[i]);
        }
    }

    // Re-pose everything (SetPosition is cheap; sync runs on change only).
    for (int i = 0; i < static_cast<int>(poses.size()); ++i) {
        if (nodes_[i]) {
            poseInstance(nodes_[i], poses[i].attributeId, poses[i], zeroPlastZ);
        }
    }
}

void BuildingModels::setPreview(bool valid, const BuildingModelPose& pose, float zeroPlastZ)
{
    if (!scene_) {
        return;
    }

    if (!valid) {
        if (previewNode_) {
            previewNode_->SetAttr(ATTRUNKOBJ_IGNORE);
        }
        return;
    }

    if (previewKey_ != instanceKey(pose)) {
        if (previewNode_) {
            previewNode_->Release();
            previewNode_ = nullptr;
        }
        previewNode_ = createInstance(pose);
        previewKey_ = instanceKey(pose);
    }
    if (!previewNode_) {
        return;
    }
    previewNode_->ClearAttr(ATTRUNKOBJ_IGNORE);
    poseInstance(previewNode_, pose.attributeId, pose, zeroPlastZ);
}

} // namespace MapEditor

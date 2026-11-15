#ifndef _RAW_MESH_OBJ_H_
#define _RAW_MESH_OBJ_H_

/// A scene object rendering a raw triangle soup (position + per-vertex
/// diffuse, untextured). Used by the transparent-voxel cavity overlay
/// (VoxCavityOverlay) and potentially other debug/annotation geometry.
///
/// Thread safety: setTriangles may be called from any thread (the game
/// fires tilemap update callbacks on the logic thread); Draw copies the
/// buffer under a lock.

#include "UnkObj.h"
#include "VertexFormat.h"

#include <memory>
#include <mutex>
#include <vector>

class DrawBuffer;

class cRawMeshObj : public cUnkObj
{
public:
	cRawMeshObj();
	~cRawMeshObj() override;

	/// Replace the whole geometry (triangle list, 3 vertices per triangle).
	void setTriangles(const std::vector<sVertexXYZDT1>& triangles);
	/// Geometry is shown only while non-empty and not ATTRUNKOBJ_IGNORE.
	void clear();

	void PreDraw(cCamera* camera) override;
	void Draw(cCamera* camera) override;

private:
	// Double buffering: setTriangles publishes an immutable snapshot; Draw
	// takes a cheap shared_ptr copy and uploads from it — no per-frame
	// deep copy of a potentially huge mesh (the game renders every frame
	// even when the overlay is off-screen: no frustum culling for scene
	// nodes).
	std::mutex mutex_;
	std::shared_ptr<const std::vector<sVertexXYZDT1>> triangles_;
	// Fixed pool of owned STATIC draw buffers (one per chunk), see Draw():
	// uploaded once per mesh change, submitted per frame.
	std::vector<std::unique_ptr<DrawBuffer>> buffers_;
	std::shared_ptr<const std::vector<sVertexXYZDT1>> uploaded_;
	// Copy of the uploaded mesh for per-chunk diffing on re-upload.
	std::vector<sVertexXYZDT1> uploadedCopy_;
};

#endif // _RAW_MESH_OBJ_H_

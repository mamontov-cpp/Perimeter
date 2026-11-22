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
	// Vertices per Draw() chunk: bounds the 16-bit index space and sets
	// the frustum-culling granularity. AABBs (setTriangles) and the draw
	// submission MUST use the same value or the culling misaligns.
	// A multiple of 6 (the mesher's quad fan size): chunks must not cut a
	// triangle, and the sokol submit asserts indices % 3 == 0.
	static constexpr size_t kChunkVerts = 60000;

	cRawMeshObj();
	~cRawMeshObj() override;

	/// Replace the whole geometry (triangle list, 3 vertices per triangle).
	void setTriangles(const std::vector<sVertexXYZDT2>& triangles);
	/// Geometry is shown only while non-empty and not ATTRUNKOBJ_IGNORE.
	void clear();

	void PreDraw(cCamera* camera) override;
	void Draw(cCamera* camera) override;

protected:
	/// Optional second texture for the draw (slot 1). Queried FRESH each
	/// Draw — do NOT store a backend-owned pointer in Texture[1] for
	/// this: cUnkObj::SetTexture would RELEASE the previous value on the
	/// next call, and backends (D3D9) free/replace the light map render
	/// target at will — a stored raw pointer becomes dangling (the
	/// EXTDEMO-in-game crash). Default null (the shader's empty slot
	/// samples white, tex*color = color).
	virtual cTexture* drawLightMap() { return nullptr; }

private:
	// Double buffering: setTriangles publishes an immutable snapshot; Draw
	// takes a cheap shared_ptr copy and uploads from it — no per-frame
	// deep copy of a potentially huge mesh (the game renders every frame
	// even when the overlay is off-screen: no frustum culling for scene
	// nodes).
	std::mutex mutex_;
	std::shared_ptr<const std::vector<sVertexXYZDT2>> triangles_;
	// Fixed pool of owned STATIC draw buffers (one per chunk), see Draw():
	// uploaded once per mesh change, submitted per frame.
	std::vector<std::unique_ptr<DrawBuffer>> buffers_;
	std::shared_ptr<const std::vector<sVertexXYZDT2>> uploaded_;
	// Copy of the uploaded mesh for per-chunk diffing on re-upload.
	std::vector<sVertexXYZDT2> uploadedCopy_;
	// Per-chunk bounding boxes, parallel to the 60000-vertex chunks Draw()
	// submits: the mesh spans the whole map, so frustum-culling off-screen
	// chunks skips most of the GPU vertex load per frame.
	std::vector<Vect3f> chunkMins_;
	std::vector<Vect3f> chunkMaxs_;
};

#endif // _RAW_MESH_OBJ_H_

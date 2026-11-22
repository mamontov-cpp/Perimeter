#include "StdAfxRD.h"

#include "RawMeshObj.h"

#include "DrawBuffer.h"

#include "../../Terra/VoxPerf.h"

#include <algorithm>
#include <cstring>
#include <numeric>

cRawMeshObj::cRawMeshObj()
    : cUnkObj(KIND_NULL)
{
}

cRawMeshObj::~cRawMeshObj() = default;

void cRawMeshObj::setTriangles(const std::vector<sVertexXYZDT2>& triangles)
{
	std::lock_guard<std::mutex> lock(mutex_);
	triangles_ = std::make_shared<const std::vector<sVertexXYZDT2>>(triangles);
	// Bounding boxes per Draw() chunk (kChunk vertices each) for the
	// frustum culling in Draw().
	const size_t kChunk = kChunkVerts;
	const size_t chunks = (triangles.size() + kChunk - 1) / kChunk;
	chunkMins_.resize(chunks);
	chunkMaxs_.resize(chunks);
	for (size_t c = 0; c < chunks; ++c) {
		const size_t beg = c * kChunk;
		const size_t end = std::min(beg + kChunk, triangles.size());
		Vect3f mn(1e30f, 1e30f, 1e30f);
		Vect3f mx(-1e30f, -1e30f, -1e30f);
		for (size_t i = beg; i < end; ++i) {
			const sVertexXYZDT2& v = triangles[i];
			mn.x = std::min(mn.x, v.x);
			mn.y = std::min(mn.y, v.y);
			mn.z = std::min(mn.z, v.z);
			mx.x = std::max(mx.x, v.x);
			mx.y = std::max(mx.y, v.y);
			mx.z = std::max(mx.z, v.z);
		}
		chunkMins_[c] = mn;
		chunkMaxs_[c] = mx;
	}
}

void cRawMeshObj::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	triangles_.reset();
}

void cRawMeshObj::PreDraw(cCamera* camera)
{
	if (GetAttribute(ATTRUNKOBJ_IGNORE) != 0) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!triangles_ || triangles_->empty()) {
			return;
		}
	}
	camera->Attach(SCENENODE_OBJECT, this);
}

void cRawMeshObj::Draw(cCamera* camera)
{
	const double tDraw0 = voxperf::nowMs();
	std::shared_ptr<const std::vector<sVertexXYZDT2>> local;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		local = triangles_;
	}
	if (!local || local->empty()) {
		return;
	}
	const std::vector<sVertexXYZDT2>& mesh = *local;

	// Texture slot 1 carries the terrain light map when the owner
	// provides it (cavity overlay) — mesh_color_tex2 then computes
	// bake*lightmap*color like the terrain. Queried per draw, never
	// stored (see drawLightMap).
	gb_RenderDevice->SetNoMaterial(ALPHA_NONE, 0, GetTexture(), drawLightMap());
	gb_RenderDevice->SetWorldMatXf(GetGlobalMatrix());
	// The cavity mesh is single-wound (quad() emits one fan) - faces must
	// show from both sides, so culling goes off for this draw.
	const uint32_t cullBackup = gb_RenderDevice->GetRenderState(RS_CULLMODE);
	gb_RenderDevice->SetRenderState(RS_CULLMODE, CULL_NONE);

	// The mesh is chunked under the 16-bit index cap and drawn through a
	// FIXED POOL of owned STATIC DrawBuffers. Static = uploaded to the GPU
	// ONCE per mesh change; per frame we only submit draw commands against
	// the existing GPU data. The previous dynamic re-uploaded the whole
	// mesh every frame (~20 ms for a big cavity map — a constant per-frame
	// tax that grew with the mesh for the entire session).
	const size_t kChunk = kChunkVerts;
	const size_t chunks = (mesh.size() + kChunk - 1) / kChunk;
	if (buffers_.size() != chunks) {
		buffers_.clear();
		buffers_.reserve(chunks);
		for (size_t i = 0; i < chunks; ++i) {
			std::unique_ptr<DrawBuffer> db(new DrawBuffer());
			db->Create(kChunk, false, kChunk, false,
			           sVertexXYZDT2::fmt, PT_TRIANGLES);
			buffers_.push_back(std::move(db));
		}
		uploaded_ = nullptr; // force re-upload into the fresh buffers
	}
	if (uploaded_ != local) {
		// Mesh content changed (new snapshot). Re-upload ONLY the chunks
		// whose bytes differ — a local terraform edit touches 1–2 chunks
		// of a multi-chunk mesh, and uploading everything cost ~50 ms per
		// edit on a big cavity map.
		size_t uploadedChunks =
			(uploadedCopy_.size() + kChunk - 1) / kChunk;
		if (uploadedChunks != chunks) {
			uploadedCopy_.clear(); // force full upload
		}
		for (size_t c = 0; c < chunks; ++c) {
			const size_t base = c * kChunk;
			const size_t n = std::min(kChunk, mesh.size() - base);
			const bool same =
				c < uploadedChunks &&
				uploadedCopy_.size() >= base + n &&
				std::memcmp(uploadedCopy_.data() + base, mesh.data() + base,
				            n * sizeof(sVertexXYZDT2)) == 0;
			if (same) {
				continue;
			}
			// IMMUTABLE buffers cannot be re-uploaded (the device's
			// PrepareSokolBuffer expects to see them exactly once) —
			// recreate the chunk's buffer from scratch instead. Rare:
			// only chunks whose bytes changed.
			buffers_[c].reset();
			buffers_[c] = std::make_unique<DrawBuffer>();
			DrawBuffer* db = buffers_[c].get();
			db->Create(kChunk, false, kChunk, false,
			           sVertexXYZDT2::fmt, PT_TRIANGLES);
			sVertexXYZDT2* v = nullptr;
			indices_t* ip = nullptr;
			db->Lock(n, n, v, ip, false);
			memcpy(v, mesh.data() + base, n * sizeof(sVertexXYZDT2));
			for (size_t i = 0; i < n; ++i) {
				ip[i] = static_cast<indices_t>(i);
			}
			// Lock(..., increment_written=false) does NOT advance the
			// written counters — the engine's own pattern sets them
			// manually, otherwise the buffer counts as empty and stays
			// active across EndScene (assert) while drawing nothing.
			db->lock_written_vertices = n;
			db->lock_written_indices = n;
			db->Unlock();
		}
		uploaded_ = local;
		if (uploadedCopy_.size() != mesh.size()) {
			uploadedCopy_.resize(mesh.size());
		}
		// Зеркалим ТОЛЬКО поменявшиеся чанки (полный assign — 17 MB копия
		// на каждый паблиш, при локальном терраформе меняются 1-2 чанка).
		for (size_t c = 0; c < chunks; ++c) {
			const size_t base = c * kChunk;
			const size_t n = std::min(kChunk, mesh.size() - base);
			if (uploadedChunks == chunks &&
			    std::memcmp(uploadedCopy_.data() + base, mesh.data() + base,
			                n * sizeof(sVertexXYZDT2)) == 0) {
				continue;
			}
			std::memcpy(uploadedCopy_.data() + base, mesh.data() + base,
			            n * sizeof(sVertexXYZDT2));
		}
	}
	// Some passes draw with no camera (shadow/reflection helpers) - those
	// get the full mesh, never a null-camera crash.
	const bool cull = camera != nullptr;
	for (size_t c = 0; c < chunks; ++c) {
		if (cull && c < chunkMins_.size()) {
			// The mesh spans the whole map; skip chunks outside the
			// current camera frustum - the GPU vertex load dominated
			// the frame (~15 ms for ~500k verts every frame).
			const Vect3f& mn = chunkMins_[c];
			const Vect3f& mx = chunkMaxs_[c];
			const Vect3f center = (mn + mx) * 0.5f;
			const float radius = (mx - mn).norm() * 0.5f;
			if (camera->TestVisible(center, radius) == VISIBLE_OUTSIDE) {
				continue;
			}
		}
		buffers_[c]->Draw();
	}
	gb_RenderDevice->SetRenderState(RS_CULLMODE, cullBackup);
	const double drawMs = voxperf::nowMs() - tDraw0;
	if (drawMs > 4.0) {
		voxperf::log("RawMeshObj::Draw: %.2f ms verts=%zu chunks=%zu%s",
		             drawMs, mesh.size(), chunks,
		             uploaded_ == local ? "" : " (upload)");
	}
}

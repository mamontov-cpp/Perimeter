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

void cRawMeshObj::setTriangles(const std::vector<sVertexXYZDT1>& triangles)
{
	std::lock_guard<std::mutex> lock(mutex_);
	triangles_ = std::make_shared<const std::vector<sVertexXYZDT1>>(triangles);
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
	std::shared_ptr<const std::vector<sVertexXYZDT1>> local;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		local = triangles_;
	}
	if (!local || local->empty()) {
		return;
	}
	const std::vector<sVertexXYZDT1>& mesh = *local;

	gb_RenderDevice->SetNoMaterial(ALPHA_NONE, 0, GetTexture());
	gb_RenderDevice->SetWorldMatXf(GetGlobalMatrix());

	// The mesh is chunked under the 16-bit index cap and drawn through a
	// FIXED POOL of owned STATIC DrawBuffers. Static = uploaded to the GPU
	// ONCE per mesh change; per frame we only submit draw commands against
	// the existing GPU data. The previous dynamic re-uploaded the whole
	// mesh every frame (~20 ms for a big cavity map — a constant per-frame
	// tax that grew with the mesh for the entire session).
	const size_t kChunk = 60000;
	const size_t chunks = (mesh.size() + kChunk - 1) / kChunk;
	if (buffers_.size() != chunks) {
		buffers_.clear();
		buffers_.reserve(chunks);
		for (size_t i = 0; i < chunks; ++i) {
			std::unique_ptr<DrawBuffer> db(new DrawBuffer());
			db->Create(kChunk, false, kChunk, false,
			           sVertexXYZDT1::fmt, PT_TRIANGLES);
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
				            n * sizeof(sVertexXYZDT1)) == 0;
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
			           sVertexXYZDT1::fmt, PT_TRIANGLES);
			sVertexXYZDT1* v = nullptr;
			indices_t* ip = nullptr;
			db->Lock(n, n, v, ip, false);
			memcpy(v, mesh.data() + base, n * sizeof(sVertexXYZDT1));
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
		uploadedCopy_.assign(mesh.begin(), mesh.end());
	}
	for (size_t c = 0; c < chunks; ++c) {
		buffers_[c]->Draw();
	}
	const double drawMs = voxperf::nowMs() - tDraw0;
	if (drawMs > 4.0) {
		voxperf::log("RawMeshObj::Draw: %.2f ms verts=%zu chunks=%zu%s",
		             drawMs, mesh.size(), chunks,
		             uploaded_ == local ? "" : " (upload)");
	}
}

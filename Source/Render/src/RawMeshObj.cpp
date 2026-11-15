#include "StdAfxRD.h"

#include "RawMeshObj.h"

#include "DrawBuffer.h"

#include "../../Terra/VoxPerf.h"

#include <algorithm>
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

	// The mesh is chunked under the 16-bit index cap. The chunks are drawn
	// through a FIXED POOL of owned DrawBuffers — NOT the device's
	// size-keyed drawbuffer cache: a fresh cache entry allocates a new GL
	// buffer and the cache never shrinks, so drawing with a different size
	// per mesh state (the chunk remainder drifts as the mesh grows) leaked
	// GPU buffers until the whole game halted. Owned buffers: one per
	// chunk slot, constant size, reused every frame; each owns its sg
	// buffer, so chunks never overwrite each other's submitted data.
	const size_t kChunk = 60000;
	const size_t chunks = (mesh.size() + kChunk - 1) / kChunk;
	if (buffers_.size() != chunks) {
		buffers_.clear();
		buffers_.reserve(chunks);
		for (size_t i = 0; i < chunks; ++i) {
			std::unique_ptr<DrawBuffer> db(new DrawBuffer());
			db->Create(kChunk, true, kChunk, true,
			           sVertexXYZDT1::fmt, PT_TRIANGLES);
			buffers_.push_back(std::move(db));
		}
	}
	for (size_t c = 0; c < chunks; ++c) {
		const size_t base = c * kChunk;
		const size_t n = std::min(kChunk, mesh.size() - base);
		DrawBuffer* db = buffers_[c].get();
		sVertexXYZDT1* v = nullptr;
		indices_t* ip = nullptr;
		db->Lock(n, n, v, ip, false);
		memcpy(v, mesh.data() + base, n * sizeof(sVertexXYZDT1));
		for (size_t i = 0; i < n; ++i) {
			ip[i] = static_cast<indices_t>(i);
		}
		// Lock(..., increment_written=false) does NOT advance the written
		// counters — the engine's own pattern (SokolRenderState.cpp) sets
		// them manually, otherwise the buffer counts as empty and stays
		// active across EndScene (assert) while drawing nothing.
		db->lock_written_vertices = n;
		db->lock_written_indices = n;
		db->Unlock();
		db->Draw();
	}
	const double drawMs = voxperf::nowMs() - tDraw0;
	if (drawMs > 4.0) {
		voxperf::log("RawMeshObj::Draw: %.2f ms verts=%zu chunks=%zu", drawMs, mesh.size(), chunks);
	}
}

#include "StdAfxRD.h"

#include "RawMeshObj.h"

#include "DrawBuffer.h"

#include "../../Terra/VoxPerf.h"

#include <algorithm>
#include <cstring>

namespace {

std::shared_ptr<const cRawMeshObj::Snapshot> makeSnapshot(
    std::vector<sVertexXYZDT2> triangles)
{
	auto snapshot = std::make_shared<cRawMeshObj::Snapshot>();
	snapshot->triangles = std::move(triangles);
	snapshot->mins = Vect3f(1e30f, 1e30f, 1e30f);
	snapshot->maxs = Vect3f(-1e30f, -1e30f, -1e30f);
	for (const sVertexXYZDT2& v : snapshot->triangles) {
		snapshot->mins.x = std::min(snapshot->mins.x, v.x);
		snapshot->mins.y = std::min(snapshot->mins.y, v.y);
		snapshot->mins.z = std::min(snapshot->mins.z, v.z);
		snapshot->maxs.x = std::max(snapshot->maxs.x, v.x);
		snapshot->maxs.y = std::max(snapshot->maxs.y, v.y);
		snapshot->maxs.z = std::max(snapshot->maxs.z, v.z);
	}
	return snapshot;
}

bool tileInRect(cRawMeshObj::TileKey key, const Vect2i& lo,
                const Vect2i& hi)
{
	const int tx = static_cast<std::uint32_t>(key);
	const int ty = static_cast<std::uint32_t>(key >> 32);
	return tx >= lo.x / cRawMeshObj::kTileSize &&
	       tx <= hi.x / cRawMeshObj::kTileSize &&
	       ty >= lo.y / cRawMeshObj::kTileSize &&
	       ty <= hi.y / cRawMeshObj::kTileSize;
}

} // namespace

cRawMeshObj::cRawMeshObj() : cUnkObj(KIND_NULL) {}
cRawMeshObj::~cRawMeshObj() = default;

void cRawMeshObj::setTiles(TileTriangles tiles, const TileTextures& textures,
                           const Vect2i* dirtyLo, const Vect2i* dirtyHi)
{
	const bool partial = dirtyLo && dirtyHi;
	if (partial) {
		for (auto& kv : tiles) {
			const bool empty = kv.second.empty();
			auto snapshot = empty
			                    ? std::shared_ptr<const Snapshot>()
			                    : makeSnapshot(std::move(kv.second));
			const auto texIt = textures.find(kv.first);
			cTexture* texture = texIt != textures.end() ? texIt->second : nullptr;
			std::lock_guard<std::mutex> lock(mutex_);
			if (empty) {
				tiles_.erase(kv.first);
				continue;
			}
			const auto oldIt = tiles_.find(kv.first);
			if (oldIt != tiles_.end() && oldIt->second.snapshot &&
			    oldIt->second.snapshot->triangles.size() ==
			        snapshot->triangles.size() &&
			    (snapshot->triangles.empty() ||
			     std::memcmp(oldIt->second.snapshot->triangles.data(),
			                 snapshot->triangles.data(),
			                 snapshot->triangles.size() * sizeof(sVertexXYZDT2)) == 0)) {
				snapshot = oldIt->second.snapshot;
			}
			tiles_[kv.first] = TileState{std::move(snapshot), texture};
		}
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto it = tiles_.begin(); it != tiles_.end();) {
			if (tileInRect(it->first, *dirtyLo, *dirtyHi) &&
			    tiles.find(it->first) == tiles.end()) {
				it = tiles_.erase(it);
			} else {
				++it;
			}
		}
		return;
	}
	TileStates old;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		old = tiles_;
	}
	TileStates next;
	for (auto& kv : tiles) {
		if (kv.second.empty()) {
			if (partial) {
				next.erase(kv.first);
			}
			continue;
		}
		auto snapshot = makeSnapshot(std::move(kv.second));
		const auto oldIt = old.find(kv.first);
		if (oldIt != old.end() && oldIt->second.snapshot &&
		    oldIt->second.snapshot->triangles.size() == snapshot->triangles.size() &&
		    (snapshot->triangles.empty() ||
		     std::memcmp(oldIt->second.snapshot->triangles.data(),
		                 snapshot->triangles.data(),
		                 snapshot->triangles.size() * sizeof(sVertexXYZDT2)) == 0)) {
			snapshot = oldIt->second.snapshot;
		}
		cTexture* texture = nullptr;
		const auto texIt = textures.find(kv.first);
		if (texIt != textures.end()) {
			texture = texIt->second;
		}
		next[kv.first] = TileState{std::move(snapshot), texture};
	}
	if (partial) {
		for (const auto& kv : old) {
			if (tileInRect(kv.first, *dirtyLo, *dirtyHi) &&
			    tiles.find(kv.first) == tiles.end()) {
				next.erase(kv.first);
			}
		}
	}
	std::lock_guard<std::mutex> lock(mutex_);
	tiles_.swap(next);
}

void cRawMeshObj::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	tiles_.clear();
}

void cRawMeshObj::PreDraw(cCamera* camera)
{
	if (GetAttribute(ATTRUNKOBJ_IGNORE) != 0) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (tiles_.empty()) {
			return;
		}
	}
	camera->Attach(SCENENODE_OBJECT, this);
}

void cRawMeshObj::Draw(cCamera* camera)
{
	const double tDraw0 = voxperf::nowMs();
	TileStates local;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		local = tiles_;
	}
	if (local.empty()) {
		return;
	}

	gb_RenderDevice->SetWorldMatXf(GetGlobalMatrix());
	const uint32_t cullBackup = gb_RenderDevice->GetRenderState(RS_CULLMODE);
	gb_RenderDevice->SetRenderState(RS_CULLMODE, CULL_NONE);
	size_t visibleTiles = 0;
	for (const auto& kv : local) {
		const TileState& state = kv.second;
		const Snapshot& snapshot = *state.snapshot;
		if (camera) {
			const Vect3f center = (snapshot.mins + snapshot.maxs) * 0.5f;
			const float radius = (snapshot.maxs - snapshot.mins).norm() * 0.5f;
			if (camera->TestVisible(center, radius) == VISIBLE_OUTSIDE) {
				continue;
			}
		}
		++visibleTiles;
		RenderTile& render = renderTiles_[kv.first];
		if (render.uploaded != state.snapshot) {
			render.buffers.clear();
			const size_t quads = snapshot.triangles.size() / 6;
			const size_t chunks = (quads + kChunkQuads - 1) / kChunkQuads;
			render.buffers.reserve(chunks);
			for (size_t c = 0; c < chunks; ++c) {
				const size_t q0 = c * kChunkQuads;
				const size_t q1 = std::min(quads, q0 + kChunkQuads);
				const size_t vertexCount = (q1 - q0) * 4;
				const size_t indexCount = (q1 - q0) * 6;
				auto db = std::make_unique<DrawBuffer>();
				db->Create(kChunkQuads * 4, false, kChunkQuads * 6, false,
				           sVertexXYZDT2::fmt, PT_TRIANGLES);
				sVertexXYZDT2* vertices = nullptr;
				indices_t* indices = nullptr;
				db->Lock(vertexCount, indexCount, vertices, indices, false);
				for (size_t q = q0; q < q1; ++q) {
					const sVertexXYZDT2* src = snapshot.triangles.data() + q * 6;
					const size_t v = (q - q0) * 4;
					const size_t i = (q - q0) * 6;
					vertices[v] = src[0];
					vertices[v + 1] = src[1];
					vertices[v + 2] = src[2];
					vertices[v + 3] = src[5];
					indices[i] = static_cast<indices_t>(v);
					indices[i + 1] = static_cast<indices_t>(v + 1);
					indices[i + 2] = static_cast<indices_t>(v + 2);
					indices[i + 3] = static_cast<indices_t>(v);
					indices[i + 4] = static_cast<indices_t>(v + 2);
					indices[i + 5] = static_cast<indices_t>(v + 3);
				}
				db->lock_written_vertices = vertexCount;
				db->lock_written_indices = indexCount;
				db->Unlock();
				render.buffers.push_back(std::move(db));
			}
			render.uploaded = state.snapshot;
		}
		gb_RenderDevice->SetNoMaterial(
		    ALPHA_NONE, 0, state.texture ? state.texture : GetTexture(),
		    drawLightMap());
		for (auto& buffer : render.buffers) {
			buffer->Draw();
		}
	}
	for (auto it = renderTiles_.begin(); it != renderTiles_.end();) {
		if (local.find(it->first) == local.end()) {
			it = renderTiles_.erase(it);
		} else {
			++it;
		}
	}
	gb_RenderDevice->SetRenderState(RS_CULLMODE, cullBackup);
	const double drawMs = voxperf::nowMs() - tDraw0;
	if (drawMs > 4.0) {
		voxperf::log("RawMeshObj::Draw: %.2f ms tiles=%zu/%zu", drawMs,
		             visibleTiles, local.size());
	}
}

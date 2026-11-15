// ============================================================================
// VoxCavityOverlay implementation. See VoxCavityOverlay.h.
// ============================================================================

#include "StdAfxRD.h"

#include "VoxCavityOverlay.h"

#include "../src/RawMeshObj.h"
#include "TileMap.h"

#include "../src/Scene.h"

#include "terra.h"
#include "VMAP.h"

#include "VoxPerf.h"

#include <algorithm>
#include <cstring>

namespace {

/// The overlay's scene node: drains the pending update rects (once per
/// frame at most) before the base class decides visibility.
class CavityOverlayNode : public cRawMeshObj
{
public:
	explicit CavityOverlayNode(VoxCavityOverlay* owner) : owner_(owner) {}

	void PreDraw(cCamera* camera) override
	{
		owner_->drainPending();
		cRawMeshObj::PreDraw(camera);
	}

private:
	VoxCavityOverlay* owner_;
};

} // namespace

VoxCavityOverlay::~VoxCavityOverlay()
{
	shutdown();
}

void VoxCavityOverlay::init(cScene* scene, cTileMap* tilemap)
{
	shutdown();
	scene_ = scene;
	tilemap_ = tilemap;
	if (!scene_ || !tilemap_) {
		return;
	}
	node_ = new CavityOverlayNode(this);
	scene_->AttachObj(node_);
	tilemap_->RegisterUpdateMap(&VoxCavityOverlay::onUpdateMap, this);
	voxperf::log("overlay init: full rebuild START");
	rebuildAll();
	voxperf::log("overlay init: full rebuild END (chunks=%zu)", chunks_.size());
	lastSeenVersion_ = vMap.terraVersion + vMap.voxLayers.version;
}

void VoxCavityOverlay::shutdown()
{
	if (node_) {
		if (tilemap_) {
			tilemap_->UnRegisterUpdateMap(&VoxCavityOverlay::onUpdateMap,
			                              this);
		}
		node_->Release();
		node_ = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		chunks_.clear();
		capRows_.clear();
		dirty_ = false;
	}
	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		pending_.clear();
	}
	scene_ = nullptr;
	tilemap_ = nullptr;
}

void VoxCavityOverlay::onUpdateMap(const Vect2i& pos1, const Vect2i& pos2,
                                   void* data)
{
	VoxCavityOverlay* self = static_cast<VoxCavityOverlay*>(data);
	if (!self->node_) {
		return;
	}
	// Queue + time-coalesced synchronous drain: game terraforming fires
	// many updates per second and rebuilding on each one re-triangulates
	// the world. The callback runs on the caller's thread (the game frame
	// loop quant — the same thread that writes the terrain), so draining
	// here is race-free; drainPending() (render thread, once per frame)
	// only flushes leftovers after a burst ends.
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(self->pendingMutex_);
		// Expand by one cell: side faces depend on the neighbors.
		Vect2i lo(std::max(0, pos1.x - 1), std::max(0, pos1.y - 1));
		Vect2i hi(std::min(static_cast<int>(vMap.H_SIZE) - 1, pos2.x + 1),
		          std::min(static_cast<int>(vMap.V_SIZE) - 1, pos2.y + 1));
		self->pending_.emplace_back(lo, hi);
		const bool due =
			(now - self->lastRebuild_) > std::chrono::milliseconds(150);
		if (!due) {
			return;
		}
	}
	self->drainPending();
}

void VoxCavityOverlay::drainPending()
{
	std::vector<std::pair<Vect2i, Vect2i>> rects;
	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		if (pending_.empty()) {
			return;
		}
		rects.swap(pending_);
	}
	// Nothing the mesh depends on (heights, layers) changed since the last
	// drain — the queued rects are pure color/vision updates (moving
	// units, zeroplast re-marking). Drop them wholesale.
	const std::uint64_t seen = vMap.terraVersion + vMap.voxLayers.version;
	if (seen == lastSeenVersion_) {
		static unsigned skippedVision = 0;
		skippedVision += static_cast<unsigned>(rects.size());
		if (skippedVision >= 1000) {
			voxperf::log("drainPending: %u vision-only rects dropped",
			             skippedVision);
			skippedVision = 0;
		}
		return;
	}
	lastSeenVersion_ = seen;
	lastRebuild_ = std::chrono::steady_clock::now();
	const double tDrain = voxperf::nowMs();
	// Merge overlapping/nearby rects: a burst of unit updates typically
	// covers the same neighborhood repeatedly — rebuilding it once per
	// drain instead of once per rect.
	std::sort(rects.begin(), rects.end(),
	          [](const std::pair<Vect2i, Vect2i>& a,
	             const std::pair<Vect2i, Vect2i>& b) {
		          return a.first.x != b.first.x ? a.first.x < b.first.x
		                                        : a.first.y < b.first.y;
	          });
	std::vector<std::pair<Vect2i, Vect2i>> merged;
	const int kMargin = 2; // expanded rects already touch neighbors
	for (const auto& r : rects) {
		if (!merged.empty()) {
			auto& last = merged.back();
			if (!(r.first.x > last.second.x + kMargin ||
			      last.first.x > r.second.x + kMargin ||
			      r.first.y > last.second.y + kMargin ||
			      last.first.y > r.second.y + kMargin)) {
				last.first.x = std::min(last.first.x, r.first.x);
				last.first.y = std::min(last.first.y, r.first.y);
				last.second.x = std::max(last.second.x, r.second.x);
				last.second.y = std::max(last.second.y, r.second.y);
				continue;
			}
		}
		merged.push_back(r);
	}
	rects.swap(merged);
	// Union-bbox degenerate case: a genuinely map-wide change (world
	// load) rebuilds everything at once. Anything smaller drains rect by
	// rect — the cheap reject skips the vast majority of game updates
	// (moving units etc. touch no cavity), and a flood of small rects
	// never degenerates into a multi-second full rebuild.
	int x0 = rects.front().first.x, y0 = rects.front().first.y;
	int x1 = rects.front().second.x, y1 = rects.front().second.y;
	for (const auto& r : rects) {
		x0 = std::min(x0, r.first.x);
		y0 = std::min(y0, r.first.y);
		x1 = std::max(x1, r.second.x);
		y1 = std::max(y1, r.second.y);
	}
	const long long area =
	    static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1);
	const long long mapArea =
	    static_cast<long long>(vMap.H_SIZE) * vMap.V_SIZE;
	if (area > mapArea / 4) {
		voxperf::log("drainPending: %zu rects unioned to FULL map rebuild",
		             rects.size());
		rebuildAll();
		return;
	}
	for (const auto& r : rects) {
		rebuildRect(r.first.x, r.first.y, r.second.x, r.second.y);
	}
	voxperf::log("drainPending: %zu rects drained in %.2f ms", rects.size(),
	             voxperf::nowMs() - tDrain);
}

void VoxCavityOverlay::rebuildAll()
{
	if (static_cast<int>(vMap.H_SIZE) <= 0 ||
	    static_cast<int>(vMap.V_SIZE) <= 0) {
		return;
	}
	rebuildRect(0, 0, static_cast<int>(vMap.H_SIZE) - 1,
	            static_cast<int>(vMap.V_SIZE) - 1);
}

void VoxCavityOverlay::rebuildRect(int x0, int y0, int x1, int y1)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const int mapW = static_cast<int>(vMap.H_SIZE);
	const std::uint32_t offBeg =
		static_cast<std::uint32_t>(y0) * mapW + static_cast<std::uint32_t>(x0);
	const std::uint32_t offEnd =
		static_cast<std::uint32_t>(y1) * mapW + static_cast<std::uint32_t>(x1);

	// Terraform in the game fires MANY UpdateMap callbacks per second
	// (per unit, per op, radius-sized rects) — most of them nowhere near
	// any cavity. Rebuilding on each would re-touch the whole mesh ("the
	// whole world remeshes" slowdown). Cheap reject: when the rect touches
	// neither a live cavity column nor an existing chunk, there is nothing
	// to do — two binary searches, no allocations.
	{
		bool chunkInRect = false;
		const auto wit = std::lower_bound(
			chunks_.begin(), chunks_.end(), offBeg,
			[](const Chunk& c, std::uint32_t off) { return c.off < off; });
		for (auto it = wit; it != chunks_.end() && it->off <= offEnd; ++it) {
			const int ccx = static_cast<int>(it->off % mapW);
			if (ccx >= x0 && ccx <= x1) {
				chunkInRect = true;
				break;
			}
		}
		const bool capRowInRect =
			!capRows_.empty() && y0 <= capRows_.rbegin()->first &&
			y1 >= capRows_.begin()->first;
		const auto colRange0 = vMap.voxLayers.columnIndexRange(offBeg, offEnd);
		if (colRange0.first == colRange0.second && !chunkInRect &&
		    !capRowInRect) {
			static unsigned skipped = 0;
			if (++skipped % 1000 == 0) {
				voxperf::log("rebuildRect: %u no-op updates skipped", skipped);
			}
			return;
		}
	}
	voxperf::log("rebuildRect: REBUILD (%d,%d)-(%d,%d) chunksBefore=%zu", x0, y0, x1, y1, chunks_.size());
	const double tReb = voxperf::nowMs();

	// Segmented rebuild: erase only the chunks inside the rect, regenerate
	// only the live cavity columns of the rect (re-inserted in offset
	// order). The flat render buffer is republished only when something
	// actually changed — untouched regions are never copied.
	bool changed = false;
	std::vector<Chunk> next;
	next.reserve(chunks_.size());
	const auto winLo = std::lower_bound(
		chunks_.begin(), chunks_.end(), offBeg,
		[](const Chunk& c, std::uint32_t off) { return c.off < off; });
	const auto winHi = std::upper_bound(winLo, chunks_.end(), offEnd,
	                                    [](std::uint32_t off, const Chunk& c) {
		                                    return off < c.off;
	                                    });
	next.insert(next.end(), std::make_move_iterator(chunks_.begin()),
	            std::make_move_iterator(winLo));
	for (auto it = winLo; it != winHi; ++it) {
		const bool inRect =
			static_cast<int>(it->off % mapW) >= x0 &&
			static_cast<int>(it->off % mapW) <= x1;
		if (inRect) {
			changed = true; // dropped — re-added below if still a cavity
			continue;
		}
		next.push_back(std::move(*it));
	}
	next.insert(next.end(), std::make_move_iterator(winHi),
	            std::make_move_iterator(chunks_.end()));
	const auto colRange = vMap.voxLayers.columnIndexRange(offBeg, offEnd);
	for (int i = colRange.first; i < colRange.second; ++i) {
		const std::uint32_t off = vMap.voxLayers.columnOffset(i);
		// Skip columns outside the rect's x span on the intermediate
		// rows (the flat offset window over-includes them).
		const int cx = static_cast<int>(off % mapW);
		if (cx < x0 || cx > x1) {
			continue;
		}
		const int cy = static_cast<int>(off / mapW);
		Chunk ch;
		ch.off = off;
		voxCavityAppendColumn(vMap, cx, cy, ch.verts);
		if (!ch.verts.empty()) {
			// Insert keeping the offset order.
			auto pos = std::lower_bound(
				next.begin(), next.end(), off,
				[](const Chunk& c, std::uint32_t o) { return c.off < o; });
			next.insert(pos, std::move(ch));
			changed = true;
		}
	}
	// Caps: regenerate every row the rect spans (full row width — runs
	// merge beyond the rect).
	for (int y = y0; y <= y1; ++y) {
		auto oldRow = capRows_.find(y);
		const bool hadRow = oldRow != capRows_.end();
		if (hadRow) {
			capRows_.erase(oldRow);
			changed = true;
		}
		std::vector<VoxCavityVertex> caps;
		voxCavityAppendRowCaps(vMap, y, 0,
		                       static_cast<int>(vMap.H_SIZE) - 1, caps);
		if (!caps.empty()) {
			capRows_.emplace(y, std::move(caps));
			changed = true;
		}
	}
	if (!changed) {
		return;
	}
	chunks_.swap(next);
	dirty_ = true;
	voxperf::log("rebuildRect: rebuild took %.2f ms, chunksAfter=%zu", voxperf::nowMs() - tReb, chunks_.size());
	if (node_) {
		// Flatten once per actual change (not per update). VoxCavityVertex
		// mirrors sVertexXYZDT1 byte-for-byte — bulk memcpy per chunk,
		// no per-vertex conversion.
		static_assert(sizeof(VoxCavityVertex) == sizeof(sVertexXYZDT1),
		              "VoxCavityVertex must mirror sVertexXYZDT1");
		std::vector<sVertexXYZDT1> flat;
		size_t total = 0;
		for (const Chunk& ch : chunks_) {
			total += ch.verts.size();
		}
		for (const auto& row : capRows_) {
			total += row.second.size();
		}
		flat.resize(total);
		const double tFlat = voxperf::nowMs();
		sVertexXYZDT1* dst = flat.data();
		for (const Chunk& ch : chunks_) {
			memcpy(dst, ch.verts.data(),
			       ch.verts.size() * sizeof(sVertexXYZDT1));
			dst += ch.verts.size();
		}
		for (const auto& row : capRows_) {
			memcpy(dst, row.second.data(),
			       row.second.size() * sizeof(sVertexXYZDT1));
			dst += row.second.size();
		}
		voxperf::log("publish: flatten %.2f ms verts=%zu", voxperf::nowMs() - tFlat, flat.size());
		node_->setTriangles(flat);
	}
}

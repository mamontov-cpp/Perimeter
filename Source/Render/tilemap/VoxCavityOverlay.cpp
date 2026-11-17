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
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>

namespace {

/// The overlay's scene node: drains the pending update rects (once per
/// frame at most) before the base class decides visibility.
/// FNV-1a over every terrain input voxCavityAppendColumn reads for the
/// column (own + XCYCL 4-neighborhood): surface height, light byte,
/// default color and the layer ranges (the differ walks them for colors
/// and transparency). Same inputs -> same mesh, so a matching hash lets
/// the rebuild skip the ~35-50 us mesher walk entirely.
std::uint64_t columnInputHash(int x, int y)
{
	const int mapW = static_cast<int>(vMap.H_SIZE);
	const int mapH = static_cast<int>(vMap.V_SIZE);
	std::uint64_t h = 1469598103934665603ULL;
	auto mix = [&h](std::uint64_t v) {
		h ^= v;
		h *= 1099511628211ULL;
	};
	for (int d = -1; d < 4; ++d) {
		const int nx = d < 0 ? x : vMap.XCYCL(x + (d == 0 ? 1 : d == 1 ? -1 : 0));
		const int ny = d < 0 ? y : vMap.YCYCL(y + (d == 2 ? 1 : d == 3 ? -1 : 0));
		if (nx < 0 || ny < 0 || nx >= mapW || ny >= mapH) {
			continue;
		}
		const int off = ny * mapW + nx;
		const int surf = vMap.VxDBuf[off] != 0 ? vMap.VxDBuf[off]
		                                       : vMap.VxGBuf[off];
		mix(static_cast<std::uint64_t>(surf));
		mix(static_cast<std::uint64_t>(vMap.RnrBuf[off] & 0x7F));
		mix(static_cast<std::uint64_t>(vMap.defaultColumnRgb(off)));
		const int idx = vMap.voxLayers.findColumn(
		    static_cast<std::uint32_t>(off));
		int count = 0;
		const VoxLayerRange* ranges =
		    idx >= 0 ? vMap.voxLayers.columnRanges(idx, &count) : nullptr;
		mix(static_cast<std::uint64_t>(count));
		for (int i = 0; i < count; ++i) {
			mix(static_cast<std::uint64_t>(
			    (ranges[i].z0 << 16) ^ (ranges[i].z1 << 4) ^
			    (ranges[i].flags << 28) ^ ranges[i].r ^ (ranges[i].g << 8) ^
			    (static_cast<std::uint32_t>(ranges[i].b) << 16)));
		}
	}
	return h;
}

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
	if (std::getenv("VOX_NO_OVERLAY")) {
		node_->SetAttr(ATTRUNKOBJ_IGNORE); // A/B diagnosis
	}
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
	voxperf::log(
	    "drain: terraVersion +%llu (last off %d), layersVersion +%llu (last "
	    "off %u)",
	    static_cast<unsigned long long>(vMap.terraVersion - lastTerraVersion_),
	    vMap.terraVersionLastOff,
	    static_cast<unsigned long long>(vMap.voxLayers.version -
	                                   lastLayersVersion_),
	    vMap.voxLayers.lastMutationOff);
	lastTerraVersion_ = vMap.terraVersion;
	lastLayersVersion_ = vMap.voxLayers.version;
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
	// Drain rect by rect — the near-merge above already collapses a burst
	// covering one neighborhood, and a genuinely map-wide change (world
	// load) arrives as a single map-wide rect, which rebuilds fully on its
	// own. NEVER union distant rects into a full-map rebuild: two far-
	// apart edits used to trigger a multi-second whole-world rebuild.
	for (const auto& r : rects) {
		rebuildRect(r.first.x, r.first.y, r.second.x, r.second.y);
	}
	voxperf::log("drainPending: %zu rects drained in %.2f ms", rects.size(),
	             voxperf::nowMs() - tDrain);
}

void VoxCavityOverlay::rebuildAll()
{
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

	// Segmented rebuild: regenerate only the live cavity columns of the
	// rect and diff against the previous chunks - the flat render buffer
	// is republished ONLY when bytes actually changed. Game terraforming
	// re-fires the same neighborhood constantly; regenerating identical
	// chunks used to flip `changed` unconditionally and republish +
	// re-upload the whole mesh every time.
	bool changed = false;
	std::vector<Chunk> old;
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
	old.reserve(std::distance(winLo, winHi));
	for (auto it = winLo; it != winHi; ++it) {
		const bool inRect =
			static_cast<int>(it->off % mapW) >= x0 &&
			static_cast<int>(it->off % mapW) <= x1;
		if (inRect) {
			old.push_back(std::move(*it)); // candidate for re-add
		} else {
			next.push_back(std::move(*it));
		}
	}
	next.insert(next.end(), std::make_move_iterator(winHi),
	            std::make_move_iterator(chunks_.end()));
	// Rebuild the cavity columns in the rect AND their 4-neighbors: a
	// solid column bordering a void renders that void's walls from the
	// solid side (boolean-cut/cliff faces), so it needs a chunk too.
	std::vector<std::uint32_t> colOffs;
	const auto colRange = vMap.voxLayers.columnIndexRange(offBeg, offEnd);
	const int mapH = static_cast<int>(vMap.V_SIZE);
	for (int i = colRange.first; i < colRange.second; ++i) {
		const std::uint32_t off = vMap.voxLayers.columnOffset(i);
		// Skip columns outside the rect's x span on the intermediate
		// rows (the flat offset window over-includes them).
		const int cx = static_cast<int>(off % mapW);
		if (cx < x0 || cx > x1) {
			continue;
		}
		const int cy = static_cast<int>(off / mapW);
		colOffs.push_back(off);
		const int dxs[4] = {1, -1, 0, 0};
		const int dys[4] = {0, 0, 1, -1};
		for (int d = 0; d < 4; ++d) {
			const int nx2 = cx + dxs[d];
			const int ny2 = cy + dys[d];
			if (nx2 < 0 || ny2 < 0 || nx2 >= mapW || ny2 >= mapH) {
				continue;
			}
			const std::uint32_t noff =
				static_cast<std::uint32_t>(ny2) * mapW + nx2;
			if (!vMap.voxLayers.columnHasTransparent(noff)) {
				colOffs.push_back(noff); // solid neighbor — maybe faces
			}
		}
	}
	std::sort(colOffs.begin(), colOffs.end());
	colOffs.erase(std::unique(colOffs.begin(), colOffs.end()),
	              colOffs.end());
	// Regenerate each candidate column into a sorted side list; reuse the
	// previous chunk byte-for-byte when nothing changed (same heights/
	// ranges/colors -> same mesh), so an unchanged neighborhood never
	// republishes. The two sorted lists are MERGED at the end - inserting
	// each chunk into the 60k-entry vector directly is O(N) memmove per
	// chunk, which turned dense terraform zones into multi-second drains.
	std::vector<Chunk> additions;
	additions.reserve(colOffs.size());
	for (const std::uint32_t off : colOffs) {
		const int cx = static_cast<int>(off % mapW);
		const int cy = static_cast<int>(off / mapW);
		auto oldIt = std::lower_bound(
			old.begin(), old.end(), off,
			[](const Chunk& c, std::uint32_t o) { return c.off < o; });
		const bool hadOld = oldIt != old.end() && oldIt->off == off;
		const std::uint64_t hash = columnInputHash(cx, cy);
		if (hadOld && oldIt->hash == hash) {
			// Inputs unchanged - keep the chunk, skip the mesher.
			additions.push_back(std::move(*oldIt));
			old.erase(oldIt);
			continue;
		}
		Chunk ch;
		ch.off = off;
		ch.hash = hash;
		voxCavityAppendColumn(vMap, cx, cy, ch.verts);
		if (ch.verts.empty()) {
			if (hadOld) {
				old.erase(oldIt); // no longer generates - dropped
				changed = true;
			}
			continue;
		}
		if (hadOld && oldIt->verts == ch.verts) {
			old.erase(oldIt); // identical - no change
		} else {
			if (hadOld) {
				old.erase(oldIt);
			}
			changed = true;
		}
		additions.push_back(std::move(ch));
	}
	if (!old.empty()) {
		// Chunks inside the rect that were not regenerated at all (the
		// column is no longer cavity-adjacent) left the mesh.
		changed = true;
	}
	if (!additions.empty()) {
		std::vector<Chunk> merged;
		merged.reserve(next.size() + additions.size());
		std::merge(std::make_move_iterator(next.begin()),
		           std::make_move_iterator(next.end()),
		           std::make_move_iterator(additions.begin()),
		           std::make_move_iterator(additions.end()),
		           std::back_inserter(merged),
		           [](const Chunk& a, const Chunk& b) { return a.off < b.off; });
		next.swap(merged);
	}
	// Caps: regenerate the rows the rect spans (full row width -
	// runs merge beyond the rect). Per-row cheap reject: a row with no
	// cavity column in the rect span and no existing caps is skipped
	// with one binary search; during game terraforming most rows in a
	// rect have no cavities at all (a full 4096-wide row walk per row
	// dominated the rebuild time).
	const int capX0 = std::max(0, x0 - 1);
	const int capX1 = std::min(mapW - 1, x1 + 1);
	for (int y = y0; y <= y1; ++y) {
		auto oldRow = capRows_.find(y);
		const bool hadRow = oldRow != capRows_.end();
		const std::uint32_t rowBeg =
		    static_cast<std::uint32_t>(y) * mapW +
		    static_cast<std::uint32_t>(capX0);
		const std::uint32_t rowEnd =
		    static_cast<std::uint32_t>(y) * mapW +
		    static_cast<std::uint32_t>(capX1);
		const auto rowRange =
		    vMap.voxLayers.columnIndexRange(rowBeg, rowEnd);
		if (rowRange.first == rowRange.second && !hadRow) {
			continue;
		}
		std::vector<VoxCavityVertex> caps;
		voxCavityAppendRowCaps(vMap, y, 0,
		                       static_cast<int>(vMap.H_SIZE) - 1, caps);
		const bool same = hadRow && oldRow->second == caps;
		if (hadRow) {
			capRows_.erase(oldRow);
		}
		if (!same) {
			changed = true;
		}
		if (!caps.empty()) {
			capRows_.emplace(y, std::move(caps));
		}
	}
	// ALWAYS install `next`: the window chunks were MOVED into old/next,
	// so an unchanged rebuild that returned early here would leave chunks_
	// full of moved-from (emptied) entries - the stored mesh was silently
	// gutted rect by rect and the next real change published the wreck
	// (425k verts collapsing to the last rebuild's additions only).
	chunks_.swap(next);
	if (!changed) {
		return;
	}
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

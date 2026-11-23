// ============================================================================
// VoxCavityOverlay implementation. See VoxCavityOverlay.h.
// ============================================================================

#include "StdAfxRD.h"

#include "VoxCavityOverlay.h"

#include "../src/RawMeshObj.h"
#include "TileMap.h"

#include "../src/Scene.h"

#ifndef VOX_OVERLAY_HEADLESS
#include "../inc/IRenderDevice.h"
#include "../src/Texture.h"
#endif

#include "terra.h"
#include "VMAP.h"

#include "VoxPerf.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <algorithm>

namespace {

constexpr int kCapTileCells = 256;
constexpr int kCapTextureSize = 512;

std::uint64_t capTileKey(int tx, int ty)
{
	return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ty)) << 32) |
	       static_cast<std::uint32_t>(tx);
}

std::uint64_t capTileForGeometryKey(cRawMeshObj::TileKey key)
{
	const int tx = static_cast<std::uint32_t>(key);
	const int ty = static_cast<std::uint32_t>(key >> 32);
	return capTileKey(tx * cRawMeshObj::kTileSize / kCapTileCells,
	                  ty * cRawMeshObj::kTileSize / kCapTileCells);
}

/// The overlay's scene node: drains the pending update rects (once per
/// frame at most) before the base class decides visibility.
/// FNV-1a over every terrain input voxCavityAppendColumn reads for the
/// column (own + XCYCL 4-neighborhood): surface height, light byte,
/// default color and the layer ranges (the differ walks them for colors
/// and transparency). Same inputs -> same mesh, so a matching hash lets
/// the rebuild skip the ~35-50 us mesher walk entirely.
std::uint64_t columnInputHash(int x, int y)
{
	std::shared_lock<std::shared_mutex> lock(vMap.voxLayers.accessMtx);
	const int mapW = static_cast<int>(vMap.H_SIZE);
	const int mapH = static_cast<int>(vMap.V_SIZE);
	std::uint64_t h = 1469598103934665603ULL;
	auto mix = [&h](std::uint64_t v) {
		h ^= v;
		h *= 1099511628211ULL;
	};
	const int off = y * mapW + x;
	const int surf = vMap.VxDBuf[off] != 0 ? vMap.VxDBuf[off]
	                                       : vMap.VxGBuf[off];
	mix(static_cast<std::uint64_t>(surf));
	mix(static_cast<std::uint64_t>(vMap.RnrBuf[off] & 0x7F));
	mix(static_cast<std::uint64_t>(vMap.defaultColumnRgb(off)));
	const int idx = vMap.voxLayers.findColumnImpl(static_cast<std::uint32_t>(off));
	if (idx >= 0) {
		const uint32_t b = vMap.voxLayers.begin_[static_cast<size_t>(idx)];
		const uint32_t e = vMap.voxLayers.end_[static_cast<size_t>(idx)];
		mix(e - b);
		for (uint32_t i = b; i < e; ++i) {
			const VoxLayerRange& r = vMap.voxLayers.ranges_[i];
			const std::uint64_t packed =
			    static_cast<std::uint64_t>(r.z0) |
			    (static_cast<std::uint64_t>(r.z1) << 16) |
			    (static_cast<std::uint64_t>(r.r) << 32) |
			    (static_cast<std::uint64_t>(r.g) << 40) |
			    (static_cast<std::uint64_t>(r.b) << 48) |
			    (static_cast<std::uint64_t>(r.flags) << 56);
			mix(packed);
		}
	} else {
		mix(0);
	}
	for (int d = 0; d < 4; ++d) {
		const int nx = vMap.XCYCL(x + (d == 0 ? 1 : d == 1 ? -1 : 0));
		const int ny = vMap.YCYCL(y + (d == 2 ? 1 : d == 3 ? -1 : 0));
		if (nx < 0 || ny < 0 || nx >= mapW || ny >= mapH) {
			continue;
		}
		const int noff = ny * mapW + nx;
		int display = -1;
		const int dam = vMap.VxDBuf[noff];
		if (dam > 0) {
			const int ni = vMap.voxLayers.findColumnImpl(
			    static_cast<std::uint32_t>(noff));
			if (ni >= 0) {
				const uint32_t b = vMap.voxLayers.begin_[static_cast<size_t>(ni)];
				const uint32_t e = vMap.voxLayers.end_[static_cast<size_t>(ni)];
				for (uint32_t i = b; i < e; ++i) {
					const VoxLayerRange& r = vMap.voxLayers.ranges_[i];
					if ((r.flags & VOX_LAYER_TRANSPARENT) && r.z0 < dam &&
					    (display < 0 || r.z0 < display)) {
						display = r.z0;
					}
				}
			}
		}
		const int neighborSurf = dam != 0 ? dam : vMap.VxGBuf[noff];
		mix(static_cast<std::uint64_t>(display >= 0 ? display : neighborSurf));
	}
	return h;
}

/// Число уровней mip-цепочки текстуры maxDim x maxDim.
int capTexMipLevelsFor(std::uint32_t maxDim)
{
	std::uint32_t m = maxDim;
	int levels = 1;
	while (m > 1) {
		m >>= 1;
		++levels;
	}
	return levels;
}

/// Побайтовое усреднение 2x2 RGBA (для CPU-генерации мипов бейка).
/// Нулевые (пустые) тексели ИСКЛЮЧАЮТСЯ из среднего: бейки разреженные
/// (пустота = 0x00000000), и усреднение с чёрным давало бы тёмную кайму
/// на мип-уровнях у террейна её нет — его текстуры заполнены целиком.
/// Все четыре пустые -> 0 (копим пустоту вглубь мипов).
std::uint32_t capTexAvg2x2(std::uint32_t a, std::uint32_t b, std::uint32_t c,
                           std::uint32_t d)
{
	std::uint32_t sum[4] = {0, 0, 0, 0};
	std::uint32_t cnt = 0;
	const std::uint32_t v[4] = {a, b, c, d};
	for (int k = 0; k < 4; ++k) {
		if (v[k] == 0) {
			continue;
		}
		++cnt;
		for (int sh = 0; sh < 32; sh += 8) {
			sum[sh / 8] += (v[k] >> sh) & 0xFFu;
		}
	}
	if (cnt == 0) {
		return 0;
	}
	std::uint32_t r = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		r |= ((sum[sh / 8] / cnt) << sh);
	}
	return r;
}

void capTexDilate(std::vector<std::uint32_t>& tex, std::uint32_t w,
                  std::uint32_t h, std::vector<std::uint32_t>& scratch)
{
	scratch = tex;
	const std::vector<std::uint32_t>& src = scratch;
	for (std::uint32_t y = 0; y < h; ++y) {
		for (std::uint32_t x = 0; x < w; ++x) {
			const size_t at = static_cast<size_t>(y) * w + x;
			if (src[at] != 0) {
				continue;
			}
			std::uint32_t sums[4] = {0, 0, 0, 0};
			unsigned count = 0;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					if ((dx == 0 && dy == 0) ||
					    (dx < 0 && x == 0) || (dy < 0 && y == 0) ||
					    (dx > 0 && x + 1 == w) ||
					    (dy > 0 && y + 1 == h)) {
						continue;
					}
					const std::uint32_t color =
					    src[static_cast<size_t>(static_cast<int>(y) + dy) * w +
					        static_cast<int>(x) + dx];
					if (color == 0) {
						continue;
					}
					++count;
					for (int shift = 0; shift < 32; shift += 8) {
						sums[shift / 8] += (color >> shift) & 0xFFu;
					}
				}
			}
			if (count == 0) {
				continue;
			}
			std::uint32_t color = 0;
			for (int shift = 0; shift < 32; shift += 8) {
				color |= (sums[shift / 8] / count) << shift;
			}
			tex[at] = color;
		}
	}
}

VoxCavityVertex interpolateWallVertex(const VoxCavityVertex& a,
                                      const VoxCavityVertex& b, float t)
{
	auto lerp = [t](float x, float y) { return x + (y - x) * t; };
	VoxCavityVertex out;
	out.x = lerp(a.x, b.x);
	out.y = lerp(a.y, b.y);
	out.z = lerp(a.z, b.z);
	std::uint32_t color = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const float av = static_cast<float>((a.diffuse >> shift) & 0xFFu);
		const float bv = static_cast<float>((b.diffuse >> shift) & 0xFFu);
		color |= static_cast<std::uint32_t>(lerp(av, bv) + 0.5f) << shift;
	}
	out.diffuse = color;
	out.u = lerp(a.u, b.u);
	out.v = lerp(a.v, b.v);
	out.u2 = lerp(a.u2, b.u2);
	out.v2 = lerp(a.v2, b.v2);
	return out;
}

void clipWallQuad(VoxCavityVertex* quad, int axis, float cut,
                  bool clipRight)
{
	auto coordinate = [axis](const VoxCavityVertex& v) {
		return axis == 0 ? v.x : v.y;
	};
	const float lo = coordinate(quad[0]);
	const float hi = coordinate(quad[1]);
	if (hi == lo) {
		return;
	}
	const float t = (cut - lo) / (hi - lo);
	const VoxCavityVertex a = quad[0];
	const VoxCavityVertex b = quad[1];
	const VoxCavityVertex c = quad[2];
	const VoxCavityVertex d = quad[5];
	if (clipRight) {
		quad[1] = interpolateWallVertex(a, b, t);
		quad[2] = interpolateWallVertex(d, c, t);
		quad[4] = quad[2];
	} else {
		quad[0] = interpolateWallVertex(a, b, t);
		quad[3] = quad[0];
		quad[5] = interpolateWallVertex(d, c, t);
	}
}

void appendSpatialQuad(cRawMeshObj::TileTriangles& tiles,
                       const VoxCavityVertex* quad, bool capSurface = false,
                       const Vect2i* dirtyLo = nullptr,
                       const Vect2i* dirtyHi = nullptr)
{
	if (dirtyLo && dirtyHi) {
		float x0 = quad[0].x, x1 = x0, y0 = quad[0].y, y1 = y0;
		for (int k : {1, 2, 5}) {
			x0 = std::min(x0, quad[k].x);
			x1 = std::max(x1, quad[k].x);
			y0 = std::min(y0, quad[k].y);
			y1 = std::max(y1, quad[k].y);
		}
		if (x1 < dirtyLo->x || x0 >= dirtyHi->x + 1 ||
		    y1 < dirtyLo->y || y0 >= dirtyHi->y + 1) {
			return;
		}
	}
	auto coordinate = [quad](int axis) {
		const float a = axis == 0 ? quad[0].x : quad[0].y;
		const float b = axis == 0 ? quad[1].x : quad[1].y;
		return std::pair<float, float>(a, b);
	};
	for (int axis = 0; axis < 2; ++axis) {
		const auto bounds = coordinate(axis);
		if (bounds.first >= bounds.second) {
			continue;
		}
		const int tile0 = static_cast<int>(std::floor(
		    bounds.first / cRawMeshObj::kTileSize));
		const int tile1 = static_cast<int>(std::floor(
		    (bounds.second - 1e-4f) / cRawMeshObj::kTileSize));
		if (tile0 != tile1 &&
		    static_cast<float>((tile0 + 1) * cRawMeshObj::kTileSize) <
		        bounds.second) {
			const float cut = static_cast<float>(
			    (tile0 + 1) * cRawMeshObj::kTileSize);
			VoxCavityVertex lo[6];
			VoxCavityVertex hi[6];
			std::copy_n(quad, 6, lo);
			std::copy_n(quad, 6, hi);
			clipWallQuad(lo, axis, cut, true);
			clipWallQuad(hi, axis, cut, false);
			appendSpatialQuad(tiles, lo, capSurface, dirtyLo, dirtyHi);
			appendSpatialQuad(tiles, hi, capSurface, dirtyLo, dirtyHi);
			return;
		}
	}
	float x0 = quad[0].x, x1 = x0, y0 = quad[0].y, y1 = y0;
	for (int k : {1, 2, 5}) {
		x0 = std::min(x0, quad[k].x);
		x1 = std::max(x1, quad[k].x);
		y0 = std::min(y0, quad[k].y);
		y1 = std::max(y1, quad[k].y);
	}
	const int tx = static_cast<int>(std::floor(
	    (x0 + x1) * 0.5f / cRawMeshObj::kTileSize));
	const int ty = static_cast<int>(std::floor(
	    (y0 + y1) * 0.5f / cRawMeshObj::kTileSize));
	if (dirtyLo && dirtyHi &&
	    (tx < dirtyLo->x / cRawMeshObj::kTileSize ||
	     tx > dirtyHi->x / cRawMeshObj::kTileSize ||
	     ty < dirtyLo->y / cRawMeshObj::kTileSize ||
	     ty > dirtyHi->y / cRawMeshObj::kTileSize)) {
		return;
	}
	const cRawMeshObj::TileKey key =
	    (static_cast<cRawMeshObj::TileKey>(static_cast<std::uint32_t>(ty)) << 32) |
	    static_cast<std::uint32_t>(tx);
	VoxCavityVertex adjusted[6];
	const VoxCavityVertex* source = quad;
	if (capSurface) {
		std::copy_n(quad, 6, adjusted);
		const int originX = static_cast<int>(std::floor(
		    (x0 + x1) * 0.5f / kCapTileCells)) * kCapTileCells;
		const int originY = static_cast<int>(std::floor(
		    (y0 + y1) * 0.5f / kCapTileCells)) * kCapTileCells;
		for (VoxCavityVertex& v : adjusted) {
			v.u = (v.x - static_cast<float>(originX) + 1.0f) /
			      static_cast<float>(kCapTextureSize);
			v.v = (v.y - static_cast<float>(originY) + 1.0f) /
			      static_cast<float>(kCapTextureSize);
		}
		source = adjusted;
	}
	auto& out = tiles[key];
	const size_t at = out.size();
	out.resize(at + 6);
	static_assert(sizeof(VoxCavityVertex) == sizeof(sVertexXYZDT2));
	std::memcpy(out.data() + at, source, 6 * sizeof(VoxCavityVertex));
}

#ifndef VOX_OVERLAY_HEADLESS
class CavityOverlayNode : public cRawMeshObj
{
public:
	CavityOverlayNode(VoxCavityOverlay* owner, bool drains)
	    : owner_(owner), drains_(drains) {}

	void PreDraw(cCamera* camera) override
	{
		if (drains_) {
			owner_->drainPending();
		}
		cRawMeshObj::PreDraw(camera);
	}

protected:
	// Лайтмап террейна СВЕЖИЙ на каждый Draw — в Texture[1] его хранить
	// нельзя: cUnkObj::SetTexture делает RELEASE старого значения, а
	// бэкенд (D3D9) владеет рендер-таргетом и освобождает его между
	// кадрами — RELEASE по висячему указателю (крах EXTDEMO в игре).
	// Только бэкенды, где лайтмапа мультипликативна (sokol, белая база):
	// у D3D9 это топ-даун проекция террейна для шейдеров теней — умножение
	// на неё темнит меш (острова тёмные в игре, яркие в редакторе).
	cTexture* drawLightMap() override
	{
		if (!gb_RenderDevice || !gb_RenderDevice->IsLightMapMultiplicative()) {
			return nullptr;
		}
		return gb_RenderDevice->GetLightMap();
	}

private:
	VoxCavityOverlay* owner_;
	bool drains_;
};
#endif // VOX_OVERLAY_HEADLESS

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
	worker_ = std::thread(&VoxCavityOverlay::workerLoop, this);
#ifndef VOX_OVERLAY_HEADLESS
	chunksNode_ = new CavityOverlayNode(this, true);
	capsNode_ = new CavityOverlayNode(this, false);
	rowWallsNode_ = new CavityOverlayNode(this, false);
	colWallsNode_ = new CavityOverlayNode(this, false);
#endif
	if (std::getenv("VOX_NO_OVERLAY")) {
		chunksNode_->SetAttr(ATTRUNKOBJ_IGNORE); // A/B diagnosis
		capsNode_->SetAttr(ATTRUNKOBJ_IGNORE);
		rowWallsNode_->SetAttr(ATTRUNKOBJ_IGNORE);
		colWallsNode_->SetAttr(ATTRUNKOBJ_IGNORE);
	}
#ifndef VOX_OVERLAY_HEADLESS
	scene_->AttachObj(chunksNode_);
	scene_->AttachObj(capsNode_);
	scene_->AttachObj(rowWallsNode_);
	scene_->AttachObj(colWallsNode_);
	tilemap_->RegisterUpdateMap(&VoxCavityOverlay::onUpdateMap, this);
#endif
	// Бейк цветов кепок: текстура РОВНО W×H (движок требует степени
	// двойки — cTexture иначе округлит размер и залогирует «Bad width»).
	capTexW_ = static_cast<std::uint32_t>(vMap.H_SIZE);
	capTexH_ = static_cast<std::uint32_t>(vMap.V_SIZE);
	capTex_.assign(static_cast<size_t>(capTexW_) * capTexH_, 0u);
#ifndef VOX_OVERLAY_HEADLESS
	if (gb_RenderDevice) {
		// GPU cap textures are allocated lazily during publish.
		//
		//
		// (порча кучи c0000374 при закрытии редактора/выходе из скирмиша).
	}
#endif
	voxperf::log("overlay init: full rebuild START");
	rebuildAll();
	voxperf::log("overlay init: full rebuild END (chunks=%zu)", chunks_.size());
	lastSeenVersion_ = vMap.terraVersion + vMap.voxLayers.version +
	                   vMap.surVersion;
}

void VoxCavityOverlay::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(workerMutex_);
		workerStop_ = true;
		workerWorkPending_ = false;
		workerRects_.clear();
	}
	workerCv_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
	{
		std::lock_guard<std::mutex> lock(workerMutex_);
		workerBusy_ = false;
		workerStop_ = false;
	}
#ifndef VOX_OVERLAY_HEADLESS
	auto releaseTex = [](cTexture*& t) {
		if (t) {
			if (gb_RenderDevice) {
				gb_RenderDevice->DeleteTexture(t);
			}
			t->Release();
			t = nullptr;
		}
	};
	if (tilemap_) {
		tilemap_->UnRegisterUpdateMap(&VoxCavityOverlay::onUpdateMap,
		                              this);
	}
	auto releaseNode = [](cRawMeshObj*& n) {
		if (n) {
			n->Release();
			n = nullptr;
		}
	};
	releaseNode(chunksNode_);
	releaseNode(capsNode_);
	releaseNode(rowWallsNode_);
	releaseNode(colWallsNode_);
	for (auto& kv : capTiles_) {
		releaseTex(kv.second.texture);
	}
#endif
	{
		std::lock_guard<std::mutex> lock(mutex_);
		chunks_.clear();
		capRows_.clear();
		wallRows_.clear();
		wallCols_.clear();
		capTex_.clear();
		capTiles_.clear();
		capTexW_ = 0;
		capTexH_ = 0;
		capTexDirty_ = false;
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
	if (!self->chunksNode_) {
		return;
	}
	// Только постановка в очередь: сама перестройка — на графическом
	// потоке (CavityOverlayNode::PreDraw -> drainPending). Раньше drain
	// исполнялся ещё и здесь, синхронно на потоке логики (колбэк дёргается
	// из renderQuant вселенной) — секундные rebuildRect вставали на
	// логическом тике, и игра замедлялась в разы (см. voxperf.log:
	// "rebuild took 2767 ms" между квантами логики).
	std::lock_guard<std::mutex> lock(self->pendingMutex_);
	// Expand by one cell: side faces depend on the neighbors.
	Vect2i lo(std::max(0, pos1.x - 1), std::max(0, pos1.y - 1));
	Vect2i hi(std::min(static_cast<int>(vMap.H_SIZE) - 1, pos2.x + 1),
	          std::min(static_cast<int>(vMap.V_SIZE) - 1, pos2.y + 1));
	self->pending_.emplace_back(lo, hi);
}

void VoxCavityOverlay::drainPending()
{
	// Троттлинг окном: при непрерывном терраформинге очередь пополняется
	// каждый квант — без окна drainPending (раз в кадр на рендере)
	// перестраивал бы меш каждый кадр.
	if (std::chrono::steady_clock::now() - lastRebuild_ <
	    std::chrono::milliseconds(300)) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(workerMutex_);
		if (workerBusy_) {
			return;
		}
	}
	std::vector<std::pair<Vect2i, Vect2i>> rects;
	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		if (pending_.empty()) {
			return;
		}
		rects.swap(pending_);
	}
	// Nothing the mesh depends on (heights, layers, surface decals)
	// changed since the last drain — the queued rects are pure color/vision
	// updates (moving units, zeroplast re-marking). Drop them wholesale.
	const std::uint64_t seen = vMap.terraVersion + vMap.voxLayers.version +
	                           vMap.surVersion;
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
	// renderQuant шлёт обновления ПОЛОСАМИ шириной в грязные строки
	// (x-диапазон × 1–11 строк) — один бур камаза даёт ~40 ректов с
	// вертикальными зазорами ~4–6 клеток. С kMargin=2 полосы НЕ сливались,
	// и каждая полоса (fixed cost: движение чанков на всю ширину строк,
	// реген кепок/стен по строкам/колонкам ~47 мс) перестраивалась
	// отдельно: бурст из 68 полос = 3.2 с rebuild'а (см. voxperf.log
	// "68 rects drained in 3210 ms"). Margin 12 схлопывает бурст в ОДИН
	// блок (~240×170) — один rebuild вместо десятков; далёкие правки
	// (>12 клеток) по-прежнему не объединяются (multi-second whole-world
	// rebuild за один Union далёких ректов — прежняя ловушка).
	const int kMargin = 12;
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
	{
		std::lock_guard<std::mutex> lock(workerMutex_);
		workerRects_ = std::move(rects);
		workerWorkPending_ = true;
		workerBusy_ = true;
	}
	workerCv_.notify_one();
}

void VoxCavityOverlay::workerLoop()
{
	std::unique_lock<std::mutex> lock(workerMutex_);
	while (!workerStop_) {
		workerCv_.wait(lock, [this] {
			return workerStop_ || workerWorkPending_;
		});
		if (workerStop_) {
			break;
		}
		std::vector<std::pair<Vect2i, Vect2i>> rects =
		    std::move(workerRects_);
		workerRects_.clear();
		workerWorkPending_ = false;
		lock.unlock();
		const double tDrain = voxperf::nowMs();
		for (const auto& r : rects) {
			rebuildRect(r.first.x, r.first.y, r.second.x, r.second.y);
		}
		voxperf::log("drainPending: %zu rects drained in %.2f ms",
		             rects.size(), voxperf::nowMs() - tDrain);
		lock.lock();
		workerBusy_ = false;
	}
}

void VoxCavityOverlay::rebuildAll(int bandsRequested)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ensureCapTexLocked();
	std::fill(capTex_.begin(), capTex_.end(), 0u);
	capTexDirty_ = true;
	const int mapW = static_cast<int>(vMap.H_SIZE);
	const int mapH = static_cast<int>(vMap.V_SIZE);
	const unsigned hc = std::thread::hardware_concurrency();
	const int bands = bandsRequested > 0
	    ? bandsRequested
	    : std::min(16, std::max(1, hc ? static_cast<int>(hc) : 1));
	voxperf::log("rebuildAll: full rebuild START bands=%d", bands);
	const double tAll = voxperf::nowMs();

	// ФАЗА 1 (один поток): ГЛОБАЛЬНЫЙ список кандидатов — все кавити-
	// колонки мира + их твёрдые 4-соседи (владельцы стен обрывов к
	// полостям). Раньше каждая полоса собирала СВОЙ список с кольцом ±1:
	// на границах полос мерж серий разрезался (меш зависел от числа ядер!)
	// и терялись клетки-«спонсоры» с другой стороны границы (дырки в
	// стенах на ряд ровно по границе полосы).
	const std::uint32_t offLast =
	    static_cast<std::uint32_t>(mapH) * mapW - 1;
	const auto allRange = vMap.voxLayers.columnIndexRange(0, offLast);
	std::vector<std::uint32_t> colOffs;
	colOffs.reserve(
	    static_cast<size_t>(allRange.second - allRange.first) * 2);
	for (int i = allRange.first; i < allRange.second; ++i) {
		const std::uint32_t off = vMap.voxLayers.columnOffset(i);
		colOffs.push_back(off);
		const int cx = static_cast<int>(off % mapW);
		const int cy = static_cast<int>(off / mapW);
		const int dxs[4] = {1, -1, 0, 0};
		const int dys[4] = {0, 0, 1, -1};
		for (int d = 0; d < 4; ++d) {
			const int nx = cx + dxs[d];
			const int ny = cy + dys[d];
			if (nx < 0 || ny < 0 || nx >= mapW || ny >= mapH) {
				continue;
			}
			const std::uint32_t noff =
			    static_cast<std::uint32_t>(ny) * mapW + nx;
			if (!vMap.voxLayers.columnHasTransparent(noff)) {
				colOffs.push_back(noff);
			}
		}
	}
	std::sort(colOffs.begin(), colOffs.end());
	colOffs.erase(std::unique(colOffs.begin(), colOffs.end()),
	              colOffs.end());
	std::vector<std::vector<std::uint32_t>> colBuckets(
	    static_cast<size_t>(mapW));
	for (const std::uint32_t off : colOffs) {
		colBuckets[static_cast<size_t>(off % mapW)].push_back(off);
	}

	// ФАЗА 2 (полосы): чанки — непересекающийся срез общего списка;
	// кепки и +y стены — свой диапазон СТРОК; +x стены — свой диапазон
	// КОЛОНОК с полным охватом по y (иначе серии разрезались бы на границах
	// полос). Каждая строка/колонка обрабатывается ровно один раз и с
	// ПОЛНЫМ списком кандидатов — выход идентичен однополосному прогону
	// при любом числе полос (меш не зависит от числа ядер).
	std::vector<RectMeshes> outs(bands);
	{
		std::vector<std::thread> threads;
		threads.reserve(bands);
		for (int b = 0; b < bands; ++b) {
			threads.emplace_back(
			    [this, &colOffs, &colBuckets, &outs, b, bands, mapW, mapH] {
				RectMeshes& o = outs[b];
				// Чанки: непересекающийся срез общего (уже уникального)
				// списка — дедуп при мерже больше не нужен.
				const size_t lo =
				    static_cast<size_t>(b) * colOffs.size() / bands;
				const size_t hi =
				    static_cast<size_t>(b + 1) * colOffs.size() / bands;
				o.chunks.reserve(hi - lo);
				for (size_t k = lo; k < hi; ++k) {
					const std::uint32_t off = colOffs[k];
					if (!vMap.voxLayers.columnHasTransparent(off)) {
						continue;
					}
					Chunk ch;
					ch.off = off;
					const int cx = static_cast<int>(off % mapW);
					const int cy = static_cast<int>(off / mapW);
					ch.hash = columnInputHash(cx, cy);
					voxCavityAppendColumn(vMap, cx, cy, ch.verts,
					                      /*emitSides=*/false);
					if (!ch.verts.empty()) {
						o.chunks.push_back(std::move(ch));
					}
				}
				// Строки [y0..y1]: кепки и +y стены. Список отсортирован
				// по off (row-major) — клетки каждой строки идут непрерывным
				// куском.
				const int y0 = b * mapH / bands;
				const int y1 = (b + 1) * mapH / bands - 1;
				const std::uint32_t rowBeg =
				    static_cast<std::uint32_t>(y0) * mapW;
				const std::uint32_t rowEnd =
				    static_cast<std::uint32_t>(y1) * mapW + (mapW - 1);
				const auto rowLo = std::lower_bound(
				    colOffs.begin(), colOffs.end(), rowBeg);
				const auto rowHi = std::upper_bound(
				    colOffs.begin(), colOffs.end(), rowEnd);
				std::vector<std::uint32_t> rowCells;
				std::vector<std::uint32_t> bakeRow;
				std::vector<std::uint32_t> emptyRow;
				for (auto it = rowLo; it != rowHi;) {
					const int cy = static_cast<int>(*it / mapW);
					auto jt = it;
					while (jt != rowHi &&
					       static_cast<int>(*jt / mapW) == cy) {
						++jt;
					}
					rowCells.assign(it, jt);
					it = jt;
					const auto cavRange = vMap.voxLayers.columnIndexRange(
					    static_cast<std::uint32_t>(cy) * mapW,
					    static_cast<std::uint32_t>(cy) * mapW +
					        (mapW - 1));
					if (cavRange.first != cavRange.second) {
						std::vector<VoxCavityVertex> caps;
						voxCavityAppendRowCaps(vMap, cy, 0, mapW - 1, caps,
						                       &bakeRow);
						bakeCapRowLocked(cy, bakeRow);
						if (!caps.empty()) {
							o.capRows.emplace(cy, std::move(caps));
						}
					} else {
						bakeCapRowLocked(cy, emptyRow);
					}
					if (!rowCells.empty()) {
						std::vector<VoxCavityVertex> w;
						voxCavityAppendRowWallY(vMap, cy, rowCells.data(),
						                        rowCells.size(), w);
						if (!w.empty()) {
							o.wallRows.emplace(cy, std::move(w));
						}
					}
				}
				// Колонки [x0..x1]: +x стены по полной высоте (серии идут
				// вдоль y — разрезание по строкам полос рвало бы мерж).
				const int x0 = b * mapW / bands;
				const int x1 = (b + 1) * mapW / bands - 1;
				for (int x = x0; x <= x1; ++x) {
					const std::vector<std::uint32_t>& colCells =
					    colBuckets[static_cast<size_t>(x)];
					if (colCells.empty()) {
						continue;
					}
					std::vector<VoxCavityVertex> w;
					voxCavityAppendColWallX(vMap, x, colCells.data(),
					                        colCells.size(), w);
					if (!w.empty()) {
						o.wallCols.emplace(x, std::move(w));
					}
				}
			});
		}
		for (auto& t : threads) {
			t.join();
		}
	}
	chunks_.clear();
	capRows_.clear();
	wallRows_.clear();
	wallCols_.clear();
	for (int b = 0; b < bands; ++b) {
		RectMeshes& o = outs[b];
		for (Chunk& chunk : o.chunks) {
			chunks_.emplace(chunk.off, std::move(chunk));
		}
		for (auto& kv : o.capRows) {
			capRows_.emplace(kv.first, std::move(kv.second));
		}
		for (auto& kv : o.wallRows) {
			wallRows_.emplace(kv.first, std::move(kv.second));
		}
		for (auto& kv : o.wallCols) {
			wallCols_.emplace(kv.first, std::move(kv.second));
		}
	}
	dirty_ = true;
	voxperf::log("rebuildAll: full rebuild END (chunks=%zu) took %.2f ms",
	             chunks_.size(), voxperf::nowMs() - tAll);
	publishLocked();
}

void VoxCavityOverlay::rebuildRect(int x0, int y0, int x1, int y1)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ensureCapTexLocked();
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
		const auto wit = chunks_.lower_bound(offBeg);
		for (auto it = wit; it != chunks_.end() && it->first <= offEnd; ++it) {
			const int ccx = static_cast<int>(it->first % mapW);
			if (ccx >= x0 && ccx <= x1) {
				chunkInRect = true;
				break;
			}
		}
		const auto capIt = capRows_.lower_bound(y0);
		const bool capRowInRect =
		    capIt != capRows_.end() && capIt->first <= y1;
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
	auto winLo = chunks_.lower_bound(offBeg);
	const auto winHi = chunks_.upper_bound(offEnd);
	for (auto it = winLo; it != winHi;) {
		const bool inRect =
			static_cast<int>(it->first % mapW) >= x0 &&
			static_cast<int>(it->first % mapW) <= x1;
		if (inRect) {
			old.push_back(std::move(it->second));
			it = chunks_.erase(it);
		} else {
			++it;
		}
	}
	// Candidates include solid neighbors for the separate wall passes; only
	// genuinely transparent columns need slab/underside chunks.
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
	// Список old и кандидаты ОБА отсортированы по off — идём двумя
	// курсорами (merge-walk). Раньше каждый consume делал old.erase() —
	// memmove хвоста вектора на каждую колонку, O(N²) на весь проход:
	// с накопленными чанками (110k+) перестройка вырастала до секунд и
	// всё сильнее деградировала по ходу сессии (см. voxperf.log:
	// "rebuild took 480-1700 ms", chunksAfter растёт от сессии).
	std::vector<Chunk> additions;
	additions.reserve(colOffs.size());
	size_t oldPos = 0;
	for (const std::uint32_t off : colOffs) {
		const int cx = static_cast<int>(off % mapW);
		const int cy = static_cast<int>(off / mapW);
		while (oldPos < old.size() && old[oldPos].off < off) {
			++oldPos; // вне списка кандидатов — chunk покидает меш
		}
		const bool hadOld = oldPos < old.size() && old[oldPos].off == off;
		if (!vMap.voxLayers.columnHasTransparent(off)) {
			if (hadOld) {
				changed = true;
				++oldPos;
			}
			continue;
		}
		const std::uint64_t hash = columnInputHash(cx, cy);
		if (hadOld && old[oldPos].hash == hash) {
			// Inputs unchanged - keep the chunk, skip the mesher.
			additions.push_back(std::move(old[oldPos]));
			++oldPos;
			continue;
		}
		Chunk ch;
		ch.off = off;
		ch.hash = hash;
		voxCavityAppendColumn(vMap, cx, cy, ch.verts, /*emitSides=*/false);
		if (hadOld) {
			if (ch.verts.empty() || old[oldPos].verts != ch.verts) {
				changed = true;
			}
			++oldPos; // consumed (moved-from)
		}
		if (ch.verts.empty()) {
			continue;
		}
		if (!hadOld) {
			changed = true;
		}
		additions.push_back(std::move(ch));
	}
	if (oldPos < old.size()) {
		// Chunks inside the rect that were not regenerated at all (the
		// column is no longer cavity-adjacent) left the mesh.
		changed = true;
	}
	for (Chunk& chunk : additions) {
		chunks_.emplace(chunk.off, std::move(chunk));
	}
	// Caps: regenerate the rows the rect spans (full row width -
	// runs merge beyond the rect). Per-row cheap reject: a row with no
	// cavity column in the rect span and no existing caps is skipped
	// with one binary search; during game terraforming most rows in a
	// rect have no cavities at all (a full 4096-wide row walk per row
	// dominated the rebuild time).
	const int capX0 = std::max(0, x0 - 1);
	const int capX1 = std::min(mapW - 1, x1 + 1);
	std::vector<std::uint32_t> bakeScratch_;
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
		voxCavityAppendRowCaps(vMap, y, capX0, capX1, caps, &bakeScratch_);
		bakeCapRowLocked(y, bakeScratch_, capX0);
		bool same = false;
		if (hadRow) {
			const std::vector<VoxCavityVertex> oldCopy = oldRow->second;
			const float cutLo = static_cast<float>(capX0);
			const float cutHi = static_cast<float>(capX1 + 1);
			std::vector<VoxCavityVertex> keptLo, keptHi;
			for (size_t i = 0; i + 6 <= oldCopy.size(); i += 6) {
				float q0 = oldCopy[i].x, q1 = oldCopy[i].x;
				for (int k = 1; k < 6; ++k) {
					q0 = std::min(q0, oldCopy[i + k].x);
					q1 = std::max(q1, oldCopy[i + k].x);
				}
				if (q1 <= cutLo) {
					keptLo.insert(keptLo.end(), oldCopy.begin() + i,
						oldCopy.begin() + i + 6);
					continue;
				}
				if (q0 >= cutHi) {
					keptHi.insert(keptHi.end(), oldCopy.begin() + i,
						oldCopy.begin() + i + 6);
					continue;
				}
				if (q0 < cutLo) {
					keptLo.insert(keptLo.end(), oldCopy.begin() + i,
						oldCopy.begin() + i + 6);
					clipWallQuad(keptLo.data() + keptLo.size() - 6, 0,
					             cutLo, true);
				}
				if (q1 > cutHi) {
					keptHi.insert(keptHi.end(), oldCopy.begin() + i,
						oldCopy.begin() + i + 6);
					clipWallQuad(keptHi.data() + keptHi.size() - 6, 0,
					             cutHi, false);
				}
			}
			std::vector<VoxCavityVertex> merged;
			merged.reserve(oldCopy.size() + caps.size());
			merged.insert(merged.end(), keptLo.begin(), keptLo.end());
			merged.insert(merged.end(), std::make_move_iterator(caps.begin()),
			              std::make_move_iterator(caps.end()));
			merged.insert(merged.end(), keptHi.begin(), keptHi.end());
			caps = std::move(merged);
			same = oldCopy == caps;
			capRows_.erase(oldRow);
		}
		if (!same) {
			changed = true;
		}
		if (!caps.empty()) {
			capRows_.emplace(y, std::move(caps));
		}
	}
	// Walls: merged side faces, stored per row (+y owners) and per column
	// (+x owners) like the caps. Candidate cells (cavity columns + solid
	// neighbours, colOffs) are bucketed by owner row/column; a row/column
	// with no candidates and no stored entry is skipped.
	{
		const int rowY0 = std::max(0, y0 - 1);
		const int colX0 = std::max(0, x0 - 1);
		std::vector<std::vector<std::uint32_t>> rowCells(
		    static_cast<size_t>(y1 - rowY0 + 1));
		std::vector<std::vector<std::uint32_t>> colCells(
		    static_cast<size_t>(x1 - colX0 + 1));
		for (const std::uint32_t off : colOffs) {
			const int cx = static_cast<int>(off % mapW);
			const int cy = static_cast<int>(off / mapW);
			if (cy >= rowY0 && cy <= y1) {
				rowCells[static_cast<size_t>(cy - rowY0)].push_back(off);
			}
			if (cx >= colX0 && cx <= x1) {
				colCells[static_cast<size_t>(cx - colX0)].push_back(off);
			}
		}
		for (int y = rowY0; y <= y1; ++y) {
			auto oldRow = wallRows_.find(y);
			const bool hadRow = oldRow != wallRows_.end();
			const auto& cells = rowCells[static_cast<size_t>(y - rowY0)];
			if (cells.empty() && !hadRow) {
				continue;
			}
			std::vector<VoxCavityVertex> w;
			if (!cells.empty()) {
				voxCavityAppendRowWallY(vMap, y, cells.data(), cells.size(),
				                        w);
			}
			// Как у колонок ниже: запись общая на ВСЮ ширину строки, а
			// кандидаты — только рект с кольцом ±1. Без сохранения квадов
			// вне x-полосы регенерация от локальных клеток стирала стены
			// строки за пределами ректа — одна выровненная колонка на
			// краю острова пробивала прозрачную полосу через всю сторону
			// (в том числе в нетронутых колонках). Сохраняем КВАДАМИ
			// (6 вершин) — сортировка по вершинам рвёт треугольные
			// веера; порядок и так корректен: сохранённые квады кончаются
			// левее полосы, новые начинаются внутри неё.
			bool same = false;
			if (hadRow) {
				const std::vector<VoxCavityVertex> oldCopy = oldRow->second;
				const int keepLeft = x0 - 1;   // квады с x1 <= keepLeft
				const int keepRight = x1 + 2;  // квады с x0 >= keepRight
				// Три части в порядке возрастания x: сохранённые левее полосы,
				// новые (полоса ректа), сохранённые правее. КВАДАМИ (6 вершин):
				// сорт по вершинам рвёт треугольные веера.
				std::vector<VoxCavityVertex> keptLo, keptHi;
				// Насквозь проходящие серии рвём ФРАГМЕНТАМИ по границам
				// полосы — иначе хвосты за пределами ректа терялись
				// (дырки в покрытии стены). КВАДАМИ (6 вершин).
				// Границы — по ФАКТИЧЕСКОМУ охвату кандидатов:
				// колонка, собранная только из горизонтальных
				// соседей, не имеет вертикального кольца ±1.
				float cutLo = 1e9f, cutHi = -1e9f;
				for (const std::uint32_t coff : cells) {
					const float cx = static_cast<float>(coff % mapW);
					cutLo = std::min(cutLo, cx);
					cutHi = std::max(cutHi, cx + 1.0f);
				}
				if (cells.empty()) { cutLo = 1e9f; cutHi = -1e9f; }
				for (size_t i = 0; i + 6 <= oldCopy.size(); i += 6) {
					float q0 = oldCopy[i].x, q1 = oldCopy[i].x;
					for (int k = 1; k < 6; ++k) {
						q0 = std::min(q0, oldCopy[i + k].x);
						q1 = std::max(q1, oldCopy[i + k].x);
					}
					if (q1 <= cutLo) {
						keptLo.insert(keptLo.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						continue;
					}
					if (q0 >= cutHi) {
						keptHi.insert(keptHi.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						continue;
					}
					if (q0 < cutLo) {
						keptLo.insert(keptLo.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						clipWallQuad(keptLo.data() + keptLo.size() - 6, 0,
						             cutLo, true);
					}
					if (q1 > cutHi) {
						keptHi.insert(keptHi.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						clipWallQuad(keptHi.data() + keptHi.size() - 6, 0,
						             cutHi, false);
					}
				}
				std::vector<VoxCavityVertex> merged;
				merged.reserve(oldCopy.size() + w.size());
				merged.insert(merged.end(), keptLo.begin(), keptLo.end());
				merged.insert(merged.end(), std::make_move_iterator(w.begin()),
					std::make_move_iterator(w.end()));
				merged.insert(merged.end(), keptHi.begin(), keptHi.end());
				w = std::move(merged);
				same = oldCopy == w;
				wallRows_.erase(oldRow);
			}
			if (!same) {
				changed = true;
			}
			if (!w.empty()) {
				wallRows_.emplace(y, std::move(w));
			}
		}
		for (int x = colX0; x <= x1; ++x) {
			auto oldCol = wallCols_.find(x);
			const bool hadCol = oldCol != wallCols_.end();
			const std::vector<std::uint32_t>& cells =
			    colCells[static_cast<size_t>(x - colX0)];
			if (cells.empty() && !hadCol) {
				continue;
			}
			std::vector<VoxCavityVertex> w;
			if (!cells.empty()) {
				voxCavityAppendColWallX(vMap, x, cells.data(), cells.size(),
				                        w);
			}
			bool same = false;
			if (hadCol) {
				// Новый проход покрывает y в [y0-1..y1+1] (кольцо
				// кандидатов) — СТАРЫЕ квады вне этого диапазона
				// сохраняем: запись общая для всей высоты колонки,
				// замена целиком теряла стены за пределами ректа
				// (дырки после правки рядом с полостью).
				const std::vector<VoxCavityVertex> oldCopy = oldCol->second;
				const int keepBelow = y0 - 1; // квады с y1 <= keepBelow
				const int keepAbove = y1 + 2; // квады с y0 >= keepAbove
				// Сохраняем КВАДАМИ (6 вершин), сохранённые — ДО новых:
				// они кончаются ниже полосы, новые начинаются внутри неё,
				// так что порядок вееров корректен без сортировки. Сорт по
				// вершинам тут рвал треугольные веера (квады собирались
				// из вершин соседних квадов).
				// Три части в порядке возрастания y: сохранённые ниже полосы,
				// новые (полоса ректа), сохранённые выше — иначе «выше» оказывались
				// раньше новых и порядок ломался. КВАДАМИ (6 вершин): сорт по
				// вершинам рвёт треугольные веера.
				std::vector<VoxCavityVertex> keptLo, keptHi;
				// Насквозь проходящие серии рвём ФРАГМЕНТАМИ по границам
				// полосы — иначе хвосты за пределами ректа терялись
				// (дырки в покрытии стены). КВАДАМИ (6 вершин).
				// Границы — по фактическому охвату кандидатов
				// (см. строки выше).
				float cutLo = 1e9f, cutHi = -1e9f;
				for (const std::uint32_t coff : cells) {
					const float cy = static_cast<float>(coff / mapW);
					cutLo = std::min(cutLo, cy);
					cutHi = std::max(cutHi, cy + 1.0f);
				}
				if (cells.empty()) { cutLo = 1e9f; cutHi = -1e9f; }
				for (size_t i = 0; i + 6 <= oldCopy.size(); i += 6) {
					float q0 = oldCopy[i].y, q1 = oldCopy[i].y;
					for (int k = 1; k < 6; ++k) {
						q0 = std::min(q0, oldCopy[i + k].y);
						q1 = std::max(q1, oldCopy[i + k].y);
					}
					if (q1 <= cutLo) {
						keptLo.insert(keptLo.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						continue;
					}
					if (q0 >= cutHi) {
						keptHi.insert(keptHi.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						continue;
					}
					if (q0 < cutLo) {
						keptLo.insert(keptLo.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						clipWallQuad(keptLo.data() + keptLo.size() - 6, 1,
						             cutLo, true);
					}
					if (q1 > cutHi) {
						keptHi.insert(keptHi.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						clipWallQuad(keptHi.data() + keptHi.size() - 6, 1,
						             cutHi, false);
					}
				}
				std::vector<VoxCavityVertex> merged;
				merged.reserve(oldCopy.size() + w.size());
				merged.insert(merged.end(), keptLo.begin(), keptLo.end());
				merged.insert(merged.end(), std::make_move_iterator(w.begin()),
					std::make_move_iterator(w.end()));
				merged.insert(merged.end(), keptHi.begin(), keptHi.end());
				w = std::move(merged);
				same = oldCopy == w;
				wallCols_.erase(oldCol);
			}
			if (!same) {
				changed = true;
			}
			if (!w.empty()) {
				wallCols_.emplace(x, std::move(w));
			}
		}
	}
	if (!changed && !capTexDirty_) {
		return;
	}
	dirty_ = true;
	voxperf::log("rebuildRect: rebuild took %.2f ms, chunksAfter=%zu", voxperf::nowMs() - tReb, chunks_.size());
	const Vect2i dirtyLo(x0, y0);
	const Vect2i dirtyHi(x1, y1);
	publishLocked(&dirtyLo, &dirtyHi);
}

void VoxCavityOverlay::ensureCapTexLocked()
{
	if (capTex_.empty()) {
		capTexW_ = static_cast<std::uint32_t>(vMap.H_SIZE);
		capTexH_ = static_cast<std::uint32_t>(vMap.V_SIZE);
		capTex_.assign(static_cast<size_t>(capTexW_) * capTexH_, 0u);
	}
}

void VoxCavityOverlay::markCapTileDirty(int tx, int ty)
{
	if (tx < 0 || ty < 0 ||
	    tx >= (static_cast<int>(capTexW_) + kCapTileCells - 1) / kCapTileCells ||
	    ty >= (static_cast<int>(capTexH_) + kCapTileCells - 1) / kCapTileCells) {
		return;
	}
	capTiles_[capTileKey(tx, ty)].dirty = true;
}

void VoxCavityOverlay::bakeCapRowLocked(
    int y, const std::vector<std::uint32_t>& cellColors, int x0)
{
	const size_t w = capTexW_;
	std::uint32_t* row = capTex_.data() + static_cast<size_t>(y) * w;
	const size_t cells = cellColors.empty()
	                         ? w - static_cast<size_t>(x0)
	                         : std::min(cellColors.size(),
	                                    w - static_cast<size_t>(x0));
	bool changed = false;
	for (size_t i = 0; i < cells; ++i) {
		const std::uint32_t color =
		    i < cellColors.size() ? cellColors[i] : 0u;
		const int x = x0 + static_cast<int>(i);
		if (row[x] != color) {
			changed = true;
			const int tx = x / kCapTileCells;
			const int ty = y / kCapTileCells;
			const int lx = x % kCapTileCells;
			const int ly = y % kCapTileCells;
			markCapTileDirty(tx, ty);
			if (lx == 0) markCapTileDirty(tx - 1, ty);
			if (lx == kCapTileCells - 1) markCapTileDirty(tx + 1, ty);
			if (ly == 0) {
				markCapTileDirty(tx, ty - 1);
				if (lx == 0) markCapTileDirty(tx - 1, ty - 1);
				if (lx == kCapTileCells - 1) markCapTileDirty(tx + 1, ty - 1);
			}
			if (ly == kCapTileCells - 1) {
				markCapTileDirty(tx, ty + 1);
				if (lx == 0) markCapTileDirty(tx - 1, ty + 1);
				if (lx == kCapTileCells - 1) markCapTileDirty(tx + 1, ty + 1);
			}
		}
	}
	if (!changed) {
		return;
	}
	for (size_t i = 0; i < cells; ++i) {
		row[x0 + static_cast<size_t>(i)] =
		    i < cellColors.size() ? cellColors[i] : 0u;
	}
	capTexDirty_ = true;
}

void VoxCavityOverlay::syncCapTileLocked(std::uint64_t key, CapGpuTile& tile)
{
#ifndef VOX_OVERLAY_HEADLESS
	if (!tile.dirty || !tile.texture || !gb_RenderDevice) {
		return;
	}
	cTexture* tex = tile.texture;
	const double t0 = voxperf::nowMs();
	const bool swapRB = gb_RenderDevice->SwapRBOnTextureUpload();
	const int originX = static_cast<int>(static_cast<std::uint32_t>(key)) *
	                    kCapTileCells;
	const int originY = static_cast<int>(static_cast<std::uint32_t>(key >> 32)) *
	                    kCapTileCells;
	tile.upload.resize(static_cast<size_t>(kCapTextureSize) * kCapTextureSize);
	for (int y = 0; y < kCapTextureSize; ++y) {
		const int ly = std::clamp(y - 1, -1, kCapTileCells);
		const int sy = originY + ly;
		for (int x = 0; x < kCapTextureSize; ++x) {
			const int lx = std::clamp(x - 1, -1, kCapTileCells);
			const int sx = originX + lx;
			std::uint32_t color = 0;
			if (sx >= 0 && sy >= 0 && sx < static_cast<int>(capTexW_) &&
			    sy < static_cast<int>(capTexH_)) {
				color = capTex_[static_cast<size_t>(sy) * capTexW_ + sx];
			}
			tile.upload[static_cast<size_t>(y) * kCapTextureSize + x] =
			    swapRB ? ((color & 0xFF00FF00u) | ((color >> 16) & 0xFFu) |
			              ((color & 0xFFu) << 16))
			           : color;
		}
	}
	capTexDilate(tile.upload, kCapTextureSize, kCapTextureSize,
	             tile.dilateScratch);
	int pitch = 0;
	if (void* dst = gb_RenderDevice->LockTexture(tex, pitch)) {
		const int need = kCapTextureSize * 4;
		if (pitch == need) {
			std::memcpy(dst, tile.upload.data(), tile.upload.size() * 4);
		} else {
			for (int y = 0; y < kCapTextureSize; ++y) {
				std::memcpy(static_cast<char*>(dst) + y * pitch,
				            tile.upload.data() +
				                static_cast<size_t>(y) * kCapTextureSize,
				            need);
			}
		}
		gb_RenderDevice->UnlockTexture(tex);
	}
	const std::vector<std::uint32_t>* prev = &tile.upload;
	std::uint32_t pw = kCapTextureSize, ph = kCapTextureSize;
	for (int level = 1; level < tex->GetNumberMipMap(); ++level) {
		const std::uint32_t w = std::max<std::uint32_t>(1, pw >> 1);
		const std::uint32_t h = std::max<std::uint32_t>(1, ph >> 1);
		std::vector<std::uint32_t>& cur = level & 1 ? tile.mipA : tile.mipB;
		cur.resize(static_cast<size_t>(w) * h);
		for (std::uint32_t y = 0; y < h; ++y) {
			const std::uint32_t sy0 = std::min(y * 2, ph - 1);
			const std::uint32_t sy1 = std::min(y * 2 + 1, ph - 1);
			for (std::uint32_t x = 0; x < w; ++x) {
				const std::uint32_t sx0 = std::min(x * 2, pw - 1);
				const std::uint32_t sx1 = std::min(x * 2 + 1, pw - 1);
				cur[static_cast<size_t>(y) * w + x] = capTexAvg2x2(
				    (*prev)[static_cast<size_t>(sy0) * pw + sx0],
				    (*prev)[static_cast<size_t>(sy0) * pw + sx1],
				    (*prev)[static_cast<size_t>(sy1) * pw + sx0],
				    (*prev)[static_cast<size_t>(sy1) * pw + sx1]);
			}
		}
		capTexDilate(cur, w, h, tile.dilateScratch);
		gb_RenderDevice->SetTextureMipData(tex, level, cur.data(),
		                                   static_cast<int>(cur.size() * 4));
		prev = &cur;
		pw = w;
		ph = h;
	}
	tile.dirty = false;
	voxperf::log("overlay cap tile sync: %.2f ms", voxperf::nowMs() - t0);
#else
	(void)key;
	(void)tile;
#endif
}

void VoxCavityOverlay::syncCapTextureLocked()
{
	for (auto& kv : capTiles_) {
		if (kv.second.texture) {
			syncCapTileLocked(kv.first, kv.second);
		}
	}
	capTexDirty_ = false;
}

size_t VoxCavityOverlay::debugSpatialQuadVertices(
    const VoxCavityVertex* quad, bool capSurface)
{
	cRawMeshObj::TileTriangles tiles;
	appendSpatialQuad(tiles, quad, capSurface);
	size_t total = 0;
	for (const auto& kv : tiles) {
		total += kv.second.size();
	}
	return total;
}

void VoxCavityOverlay::debugDumpMesh(std::vector<VoxCavityVertex>& out) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	size_t total = 0;
	for (const auto& kv : chunks_) {
		total += kv.second.verts.size();
	}
	for (const auto& row : capRows_) {
		total += row.second.size();
	}
	for (const auto& row : wallRows_) {
		total += row.second.size();
	}
	for (const auto& col : wallCols_) {
		total += col.second.size();
	}
	out.reserve(out.size() + total);
	for (const auto& kv : chunks_) {
		out.insert(out.end(), kv.second.verts.begin(), kv.second.verts.end());
	}
	for (const auto& row : capRows_) {
		out.insert(out.end(), row.second.begin(), row.second.end());
	}
	for (const auto& row : wallRows_) {
		out.insert(out.end(), row.second.begin(), row.second.end());
	}
	for (const auto& col : wallCols_) {
		out.insert(out.end(), col.second.begin(), col.second.end());
	}
}

struct VoxCavityOverlay::SpatialTiles
{
	cRawMeshObj::TileTriangles chunks;
	cRawMeshObj::TileTriangles caps;
	cRawMeshObj::TileTriangles rowWalls;
	cRawMeshObj::TileTriangles colWalls;
};

void VoxCavityOverlay::buildSpatialTilesLocked(const Vect2i* dirtyLo,
                                               const Vect2i* dirtyHi,
                                               SpatialTiles& out) const
{
	// setTiles replaces whole tiles. Rebuild their complete contents, not just
	// quads touching the small edit rect, or untouched geometry in the same
	// 128x128 tile disappears from the render node.
	Vect2i tileLo, tileHi;
	if (dirtyLo && dirtyHi) {
		tileLo = Vect2i(
		    dirtyLo->x / cRawMeshObj::kTileSize * cRawMeshObj::kTileSize,
		    dirtyLo->y / cRawMeshObj::kTileSize * cRawMeshObj::kTileSize);
		tileHi = Vect2i(
		    (dirtyHi->x / cRawMeshObj::kTileSize + 1) *
		        cRawMeshObj::kTileSize - 1,
		    (dirtyHi->y / cRawMeshObj::kTileSize + 1) *
		        cRawMeshObj::kTileSize - 1);
		dirtyLo = &tileLo;
		dirtyHi = &tileHi;
	}
	auto tile = [dirtyLo, dirtyHi](
	                const std::vector<const std::vector<VoxCavityVertex>*>& parts,
	                bool capSurface = false) {
		cRawMeshObj::TileTriangles tiles;
		for (const auto* part : parts) {
			for (size_t i = 0; i + 6 <= part->size(); i += 6) {
				appendSpatialQuad(tiles, part->data() + i, capSurface, dirtyLo,
				                  dirtyHi);
			}
		}
		return tiles;
	};
	std::vector<const std::vector<VoxCavityVertex>*> capParts;
	std::vector<const std::vector<VoxCavityVertex>*> chunkParts;
	std::vector<const std::vector<VoxCavityVertex>*> rowWallParts;
	std::vector<const std::vector<VoxCavityVertex>*> colWallParts;
	const auto outside = [dirtyLo, dirtyHi](int x, int y) {
		return dirtyLo && dirtyHi &&
		       (x < dirtyLo->x || x > dirtyHi->x || y < dirtyLo->y ||
		        y > dirtyHi->y);
	};
	const int mapW = static_cast<int>(vMap.H_SIZE);
	for (const auto& kv : chunks_) {
		if (outside(static_cast<int>(kv.first % mapW),
		            static_cast<int>(kv.first / mapW))) {
			continue;
		}
		chunkParts.push_back(&kv.second.verts);
	}
	for (const auto& row : capRows_) {
		if (dirtyLo && dirtyHi &&
		    (row.first < dirtyLo->y || row.first > dirtyHi->y)) {
			continue;
		}
		capParts.push_back(&row.second);
	}
	for (const auto& row : wallRows_) {
		if (dirtyLo && dirtyHi &&
		    (row.first < dirtyLo->y - 1 || row.first > dirtyHi->y)) {
			continue;
		}
		rowWallParts.push_back(&row.second);
	}
	for (const auto& col : wallCols_) {
		if (dirtyLo && dirtyHi &&
		    (col.first < dirtyLo->x - 1 || col.first > dirtyHi->x)) {
			continue;
		}
		colWallParts.push_back(&col.second);
	}
	out.caps = tile(capParts, true);
	out.chunks = tile(chunkParts);
	out.rowWalls = tile(rowWallParts);
	out.colWalls = tile(colWallParts);
}

void VoxCavityOverlay::debugDumpTileMesh(int x0, int y0, int x1, int y1,
                                         bool partial,
                                         std::vector<VoxCavityVertex>& out) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Vect2i lo(x0, y0);
	const Vect2i hi(x1, y1);
	SpatialTiles tiles;
	buildSpatialTilesLocked(partial ? &lo : nullptr, partial ? &hi : nullptr,
	                        tiles);
	auto append = [&out, x0, y0, x1, y1](
	                  const cRawMeshObj::TileTriangles& part) {
		for (const auto& kv : part) {
			const int tx = static_cast<std::uint32_t>(kv.first);
			const int ty = static_cast<std::uint32_t>(kv.first >> 32);
			if (tx < x0 / cRawMeshObj::kTileSize ||
			    tx > x1 / cRawMeshObj::kTileSize ||
			    ty < y0 / cRawMeshObj::kTileSize ||
			    ty > y1 / cRawMeshObj::kTileSize) {
				continue;
			}
			const size_t at = out.size();
			out.resize(at + kv.second.size());
			static_assert(sizeof(VoxCavityVertex) == sizeof(sVertexXYZDT2));
			std::memcpy(out.data() + at, kv.second.data(),
			            kv.second.size() * sizeof(VoxCavityVertex));
		}
	};
	append(tiles.chunks);
	append(tiles.caps);
	append(tiles.rowWalls);
	append(tiles.colWalls);
}

void VoxCavityOverlay::publishLocked(const Vect2i* dirtyLo,
                                     const Vect2i* dirtyHi)
{
	if (!chunksNode_) {
		return;
	}
	const double tFlat = voxperf::nowMs();
	SpatialTiles spatial;
	buildSpatialTilesLocked(dirtyLo, dirtyHi, spatial);
	auto& chunksFlat = spatial.chunks;
	auto& capsFlat = spatial.caps;
	auto& rowWallsFlat = spatial.rowWalls;
	auto& colWallsFlat = spatial.colWalls;
	voxperf::log("publish: spatial partition %.2f ms tiles chunks=%zu caps=%zu "
	             "rowWalls=%zu colWalls=%zu", voxperf::nowMs() - tFlat,
	             chunksFlat.size(), capsFlat.size(), rowWallsFlat.size(),
	             colWallsFlat.size());
	cRawMeshObj::TileTextures capTextures;
#ifndef VOX_OVERLAY_HEADLESS
	for (const auto& kv : capsFlat) {
		const std::uint64_t key = capTileForGeometryKey(kv.first);
		CapGpuTile& cap = capTiles_[key];
		if (!cap.texture && gb_RenderDevice) {
			cap.texture = new cTexture;
			cap.texture->SetNumberMipMap(capTexMipLevelsFor(kCapTextureSize));
			cap.texture->New(1);
			cap.texture->SetWidth(kCapTextureSize);
			cap.texture->SetHeight(kCapTextureSize);
			cap.texture->SetTimePerFrame(0);
			cap.texture->label = "VoxCavityCapTile";
			gb_RenderDevice->CreateTexture(cap.texture, nullptr);
			cap.dirty = true;
		}
		capTextures[kv.first] = cap.texture;
	}
#endif
	syncCapTextureLocked();
#ifndef VOX_OVERLAY_HEADLESS
	chunksNode_->setTiles(std::move(chunksFlat), {}, dirtyLo, dirtyHi);
	capsNode_->setTiles(std::move(capsFlat), capTextures, dirtyLo, dirtyHi);
	rowWallsNode_->setTiles(std::move(rowWallsFlat), {}, dirtyLo, dirtyHi);
	colWallsNode_->setTiles(std::move(colWallsFlat), {}, dirtyLo, dirtyHi);
#endif
}

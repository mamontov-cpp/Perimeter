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
#include <cstdlib>
#include <cstring>
#include <iterator>
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
	// Сырые указатели columnRanges ниже живут вне вызова — лок на всё
	// тело: логический поток игры может параллельно править слои
	// (solidify при терраформинге).
	std::lock_guard<std::recursive_mutex> lock(vMap.voxLayers.accessMtx);
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

int capTexMipLevels()
{
	return capTexMipLevelsFor(
	    std::max(static_cast<std::uint32_t>(vMap.H_SIZE),
	             static_cast<std::uint32_t>(vMap.V_SIZE)));
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

#ifndef VOX_OVERLAY_HEADLESS
class CavityOverlayNode : public cRawMeshObj
{
public:
	explicit CavityOverlayNode(VoxCavityOverlay* owner) : owner_(owner) {}

	void PreDraw(cCamera* camera) override
	{
		owner_->drainPending();
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
#ifndef VOX_OVERLAY_HEADLESS
	chunksNode_ = new CavityOverlayNode(this);
	capsNode_ = new CavityOverlayNode(this);
	rowWallsNode_ = new CavityOverlayNode(this);
	colWallsNode_ = new CavityOverlayNode(this);
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
		auto makeTex = [](const char* label, int w, int h, int levels) {
			cTexture* t = new cTexture;
			t->SetNumberMipMap(levels);
			t->New(1);
			t->SetWidth(w);
			t->SetHeight(h);
			t->SetTimePerFrame(0);
			t->label = label;
			gb_RenderDevice->CreateTexture(t, nullptr);
			return t;
		};
		const int mapW = static_cast<int>(vMap.H_SIZE);
		const int mapH = static_cast<int>(vMap.V_SIZE);
		const int wallLevels = capTexMipLevelsFor(
		    std::max(mapW, VOX_WALL_TEX_ZCAP));
		capTexture_ = makeTex("VoxCavityCaps", mapW, mapH,
		                      capTexMipLevels());
		rowWallTexture_ = makeTex("VoxCavityRowWalls", mapW,
		                          VOX_WALL_TEX_ZCAP, wallLevels);
		colWallTexture_ = makeTex("VoxCavityColWalls", mapH,
		                          VOX_WALL_TEX_ZCAP, wallLevels);
		// Текстуру несёт только соответствующая нода; нода плит рисует
		// без неё (пустой слот шейдера сэмплит белую emptyTexture).
		// Слоту ноды нужна СВОЯ ссылка: cUnkObj::~cUnkObj делает RELEASE
		// каждого Texture[i], а SetTexture IncRef НЕ делает — без своей
		// ссылки деструктор ноды роняет счётчик удалённой текстуры
		// (порча кучи c0000374 при закрытии редактора/выходе из скирмиша).
		capsNode_->SetTexture(0, capTexture_);
		capTexture_->IncRef();
		rowWallsNode_->SetTexture(0, rowWallTexture_);
		rowWallTexture_->IncRef();
		colWallsNode_->SetTexture(0, colWallTexture_);
		colWallTexture_->IncRef();
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
	// A5: фоновый rebuild дожидаем — не трогать состояние из-под него.
	if (worker_.joinable()) {
		worker_.join();
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
	releaseTex(capTexture_);
	releaseTex(rowWallTexture_);
	releaseTex(colWallTexture_);
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
#endif
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
	// A5: rebuild — на фоновом потоке. Кадр не ждёт: rebuildRect берёт
	// overlay-мьютекс на всё своё время, но поток рендера его НЕ берёт
	// (нода публикует/копирует снимок под своим собственным коротким
	// локом). Предыдущий воркер обычно завершён (троттлинг 300 мс против
	// rebuild ~50–300 мс); join страхует от наложения rebuild'ов.
	if (worker_.joinable()) {
		worker_.join();
	}
	worker_ = std::thread(
	    [this](std::vector<std::pair<Vect2i, Vect2i>> rs) {
		    const double tDrain = voxperf::nowMs();
		    // Drain rect by rect — the near-merge above already collapses
		    // a burst covering one neighborhood, and a genuinely map-wide
		    // change (world load) arrives as a single map-wide rect, which
		    // rebuilds fully on its own. NEVER union distant rects into a
		    // full-map rebuild: two far-apart edits used to trigger a
		    // multi-second whole-world rebuild.
		    for (const auto& r : rs) {
			    rebuildRect(r.first.x, r.first.y, r.second.x, r.second.y);
		    }
		    voxperf::log("drainPending: %zu rects drained in %.2f ms",
		                 rs.size(), voxperf::nowMs() - tDrain);
	    },
	    std::move(rects));
}

void VoxCavityOverlay::rebuildAll(int bandsRequested)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ensureCapTexLocked();
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
			    [this, &colOffs, &outs, b, bands, mapW, mapH] {
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
						std::vector<VoxWallRun> runs;
						voxCavityAppendRowWallY(vMap, cy, rowCells.data(),
						                        rowCells.size(), w, &runs);
						if (!w.empty()) {
							o.wallRows.emplace(cy, std::move(w));
						}
						o.wallRowRuns.emplace(cy, std::move(runs));
					}
				}
				// Колонки [x0..x1]: +x стены по полной высоте (серии идут
				// вдоль y — разрезание по строкам полос рвало бы мерж).
				const int x0 = b * mapW / bands;
				const int x1 = (b + 1) * mapW / bands - 1;
				std::vector<std::uint32_t> colCells;
				for (int x = x0; x <= x1; ++x) {
					colCells.clear();
					for (const std::uint32_t off : colOffs) {
						if (static_cast<int>(off % mapW) == x) {
							colCells.push_back(off);
						}
					}
					if (colCells.empty()) {
						continue;
					}
					std::vector<VoxCavityVertex> w;
					std::vector<VoxWallRun> runs;
					voxCavityAppendColWallX(vMap, x, colCells.data(),
					                        colCells.size(), w, &runs);
					if (!w.empty()) {
						o.wallCols.emplace(x, std::move(w));
					}
					o.wallColRuns.emplace(x, std::move(runs));
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
	wallRowRuns_.clear();
	wallColRuns_.clear();
	for (int b = 0; b < bands; ++b) {
		RectMeshes& o = outs[b];
		chunks_.insert(chunks_.end(),
		               std::make_move_iterator(o.chunks.begin()),
		               std::make_move_iterator(o.chunks.end()));
		for (auto& kv : o.capRows) {
			capRows_.emplace(kv.first, std::move(kv.second));
		}
		for (auto& kv : o.wallRows) {
			wallRows_.emplace(kv.first, std::move(kv.second));
		}
		for (auto& kv : o.wallCols) {
			wallCols_.emplace(kv.first, std::move(kv.second));
		}
		for (auto& kv : o.wallRowRuns) {
			wallRowRuns_[kv.first] = std::move(kv.second);
		}
		for (auto& kv : o.wallColRuns) {
			wallColRuns_[kv.first] = std::move(kv.second);
		}
	}
	// Чанки собраны из непересекающихся срезов — сортировка под бинарные
	// поиски rebuildRect (unique не нужен: список кандидатов был уникален).
	std::sort(chunks_.begin(), chunks_.end(),
	          [](const Chunk& a, const Chunk& b) { return a.off < b.off; });
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
		voxCavityAppendRowCaps(vMap, y, 0,
		                       static_cast<int>(vMap.H_SIZE) - 1, caps,
		                       &bakeScratch_);
		bakeCapRowLocked(y, bakeScratch_);
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
	// Walls: merged side faces, stored per row (+y owners) and per column
	// (+x owners) like the caps. Candidate cells (cavity columns + solid
	// neighbours, colOffs) are bucketed by owner row/column; a row/column
	// with no candidates and no stored entry is skipped.
	{
		std::vector<std::vector<std::uint32_t>> rowCells(
		    static_cast<size_t>(y1) + 2);
		// B7: индексированный вектор вместо std::map (аллокация узла на
		// колонку в каждом drain).
		std::vector<std::vector<std::uint32_t>> colCells(
		    static_cast<size_t>(x1) + 2);
		for (const std::uint32_t off : colOffs) {
			const int cx = static_cast<int>(off % mapW);
			const int cy = static_cast<int>(off / mapW);
			if (cy >= y0 - 1 && cy <= y1) {
				rowCells[static_cast<size_t>(cy)].push_back(off);
			}
			if (cx >= x0 - 1 && cx <= x1) {
				colCells[static_cast<size_t>(cx)].push_back(off);
			}
		}
		for (int y = std::max(0, y0 - 1); y <= y1; ++y) {
			auto oldRow = wallRows_.find(y);
			const bool hadRow = oldRow != wallRows_.end();
			const auto& cells = rowCells[static_cast<size_t>(y)];
			if (cells.empty() && !hadRow) {
				continue;
			}
			std::vector<VoxCavityVertex> w;
			std::vector<VoxWallRun> runs;
			if (!cells.empty()) {
				voxCavityAppendRowWallY(vMap, y, cells.data(), cells.size(),
				                        w, &runs);
			}
			wallRowRuns_[y] = std::move(runs);
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
						for (size_t k = keptLo.size() - 6; k < keptLo.size(); ++k) {
							if (keptLo[k].x == q1) keptLo[k].x = cutLo;
						}
					}
					if (q1 > cutHi) {
						keptHi.insert(keptHi.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						for (size_t k = keptHi.size() - 6; k < keptHi.size(); ++k) {
							if (keptHi[k].x == q0) keptHi[k].x = cutHi;
						}
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
		for (int x = std::max(0, x0 - 1); x <= x1; ++x) {
			auto oldCol = wallCols_.find(x);
			const bool hadCol = oldCol != wallCols_.end();
			const std::vector<std::uint32_t>& cells =
			    colCells[static_cast<size_t>(x)];
			if (cells.empty() && !hadCol) {
				continue;
			}
			std::vector<VoxCavityVertex> w;
			std::vector<VoxWallRun> runs;
			if (!cells.empty()) {
				voxCavityAppendColWallX(vMap, x, cells.data(), cells.size(),
				                        w, &runs);
			}
			wallColRuns_[x] = std::move(runs);
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
						for (size_t k = keptLo.size() - 6; k < keptLo.size(); ++k) {
							if (keptLo[k].y == q1) keptLo[k].y = cutLo;
						}
					}
					if (q1 > cutHi) {
						keptHi.insert(keptHi.end(), oldCopy.begin() + i,
							oldCopy.begin() + i + 6);
						for (size_t k = keptHi.size() - 6; k < keptHi.size(); ++k) {
							if (keptHi[k].y == q0) keptHi[k].y = cutHi;
						}
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
	publishLocked();
}

void VoxCavityOverlay::ensureCapTexLocked()
{
	if (capTex_.empty()) {
		capTexW_ = static_cast<std::uint32_t>(vMap.H_SIZE);
		capTexH_ = static_cast<std::uint32_t>(vMap.V_SIZE);
		capTex_.assign(static_cast<size_t>(capTexW_) * capTexH_, 0u);
	}
	const size_t rowTexels =
	    static_cast<size_t>(vMap.H_SIZE) * VOX_WALL_TEX_ZCAP;
	const size_t colTexels =
	    static_cast<size_t>(vMap.V_SIZE) * VOX_WALL_TEX_ZCAP;
	if (rowWallTex_.size() != rowTexels) {
		rowWallTex_.assign(rowTexels, 0u);
	}
	if (colWallTex_.size() != colTexels) {
		colWallTex_.assign(colTexels, 0u);
	}
}

void VoxCavityOverlay::bakeCapRowLocked(
    int y, const std::vector<std::uint32_t>& cellColors)
{
	const size_t w = capTexW_;
	std::uint32_t* row = capTex_.data() + static_cast<size_t>(y) * w;
	const size_t cells = std::min(cellColors.size(), w);
	for (size_t x = 0; x < cells; ++x) {
		row[x] = cellColors[x];
	}
	for (size_t x = cells; x < w; ++x) {
		row[x] = 0u;
	}
	capTexDirty_ = true;
}

void VoxCavityOverlay::syncTextureLocked(
    cTexture* tex, const std::vector<std::uint32_t>& bake, bool& dirtyFlag)
{
#ifndef VOX_OVERLAY_HEADLESS
	if (!tex || !dirtyFlag || !gb_RenderDevice) {
		dirtyFlag = tex != nullptr && dirtyFlag;
		return;
	}
	dirtyFlag = false;
	const std::uint32_t texW = static_cast<std::uint32_t>(tex->GetWidth());
	const std::uint32_t texH = static_cast<std::uint32_t>(tex->GetHeight());
	const double t0 = voxperf::nowMs();
	// Порядок байт — бэкенд-специфичен: sokol (RGBA8) сэмплит byte0
	// как R и требует свопа R/B; D3D9 (A8R8G8B8) читает byte0 как B —
	// канонический 0xAARRGGBB ложится туда без свопа (своп для D3D как
	// раз перепутывает цвета). См. IRenderDevice::SwapRBOnTextureUpload.
	const bool swapRB = gb_RenderDevice->SwapRBOnTextureUpload();
	std::vector<std::uint32_t> upload(bake.size());
	for (size_t i = 0; i < bake.size(); ++i) {
		const std::uint32_t c = bake[i];
		upload[i] = swapRB ? ((c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) |
		                      ((c & 0xFFu) << 16))
		                   : c;
	}
	// Уровень 0 — через Lock/Unlock (загрузка случится на рендер-потоке
	// при бинде dirty-текстуры); мипы — через SetTextureMipData (тот же
	// флэш). CPU-фильтрация цепочки поглощается воркером.
	int pitch = 0;
	if (void* dst = gb_RenderDevice->LockTexture(tex, pitch)) {
		const int need = static_cast<int>(texW * 4);
		if (pitch == need) {
			std::memcpy(dst, upload.data(), upload.size() * 4);
		} else {
			for (std::uint32_t y = 0; y < texH; ++y) {
				std::memcpy(
				    static_cast<char*>(dst) + y * pitch,
				    upload.data() + static_cast<size_t>(y) * texW, need);
			}
		}
		gb_RenderDevice->UnlockTexture(tex);
	}
	std::vector<std::uint32_t> prev = std::move(upload);
	std::uint32_t pw = texW, ph = texH;
	for (int level = 1; level < tex->GetNumberMipMap(); ++level) {
		const std::uint32_t w = std::max<std::uint32_t>(1, pw >> 1);
		const std::uint32_t h = std::max<std::uint32_t>(1, ph >> 1);
		std::vector<std::uint32_t> cur(static_cast<size_t>(w) * h);
		for (std::uint32_t y = 0; y < h; ++y) {
			const std::uint32_t sy0 = std::min(y * 2, ph - 1);
			const std::uint32_t sy1 = std::min(y * 2 + 1, ph - 1);
			for (std::uint32_t x = 0; x < w; ++x) {
				const std::uint32_t sx0 = std::min(x * 2, pw - 1);
				const std::uint32_t sx1 = std::min(x * 2 + 1, pw - 1);
				cur[static_cast<size_t>(y) * w + x] = capTexAvg2x2(
				    prev[static_cast<size_t>(sy0) * pw + sx0],
				    prev[static_cast<size_t>(sy0) * pw + sx1],
				    prev[static_cast<size_t>(sy1) * pw + sx0],
				    prev[static_cast<size_t>(sy1) * pw + sx1]);
			}
		}
		gb_RenderDevice->SetTextureMipData(tex, level, cur.data(),
		                                   static_cast<int>(cur.size() * 4));
		prev.swap(cur);
		pw = w;
		ph = h;
	}
	voxperf::log("overlay texture sync: %.2f ms levels=%d",
	             voxperf::nowMs() - t0, tex->GetNumberMipMap());
#endif
}

void VoxCavityOverlay::syncCapTextureLocked()
{
	syncTextureLocked(capTexture_, capTex_, capTexDirty_);
}

void VoxCavityOverlay::bakeWallTexturesLocked()
{
	ensureCapTexLocked();
	const std::uint32_t w = static_cast<std::uint32_t>(vMap.H_SIZE);
	const std::uint32_t cap = VOX_WALL_TEX_ZCAP;
	// Полный перепёк: порядок std::map детерминирован; при коллизиях
	// серий в одном текселе (разные плоскости, один (x,z)) побеждает
	// последняя — у одноцветных стен (типично) коллизий нет.
	std::fill(rowWallTex_.begin(), rowWallTex_.end(), 0u);
	for (const auto& kv : wallRowRuns_) {
		for (const VoxWallRun& r : kv.second) {
			const std::uint32_t x1 =
			    std::min(static_cast<std::uint32_t>(r.along1), w);
			const std::uint32_t z1 =
			    std::min(static_cast<std::uint32_t>(r.z1), cap);
			for (std::uint32_t z = static_cast<std::uint32_t>(r.z0);
			     z < z1; ++z) {
				std::uint32_t* row =
				    rowWallTex_.data() + static_cast<size_t>(z) * w;
				for (std::uint32_t x = static_cast<std::uint32_t>(r.along0);
				     x < x1; ++x) {
					row[x] = r.color;
				}
			}
		}
	}
	rowWallTexDirty_ = true;
	std::fill(colWallTex_.begin(), colWallTex_.end(), 0u);
	const std::uint32_t h = static_cast<std::uint32_t>(vMap.V_SIZE);
	for (const auto& kv : wallColRuns_) {
		for (const VoxWallRun& r : kv.second) {
			const std::uint32_t y1 =
			    std::min(static_cast<std::uint32_t>(r.along1), h);
			const std::uint32_t z1 =
			    std::min(static_cast<std::uint32_t>(r.z1), cap);
			for (std::uint32_t z = static_cast<std::uint32_t>(r.z0);
			     z < z1; ++z) {
				std::uint32_t* row =
				    colWallTex_.data() + static_cast<size_t>(z) * h;
				for (std::uint32_t y = static_cast<std::uint32_t>(r.along0);
				     y < y1; ++y) {
					row[y] = r.color;
				}
			}
		}
	}
	colWallTexDirty_ = true;
}

void VoxCavityOverlay::debugDumpMesh(std::vector<VoxCavityVertex>& out) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	size_t total = 0;
	for (const Chunk& ch : chunks_) {
		total += ch.verts.size();
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
	for (const Chunk& ch : chunks_) {
		out.insert(out.end(), ch.verts.begin(), ch.verts.end());
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

void VoxCavityOverlay::publishLocked()
{
	if (!chunksNode_) {
		return;
	}
	// Flatten once per actual change (not per update). VoxCavityVertex
	// mirrors sVertexXYZDT2 byte-for-byte — bulk memcpy per chunk,
	// no per-vertex conversion.
	static_assert(sizeof(VoxCavityVertex) == sizeof(sVertexXYZDT2),
	              "VoxCavityVertex must mirror sVertexXYZDT2");
	// Четыре ноды по текстурному режиму (см. заголовок класса). Порядок
	// внутри каждой — как раньше (chunks, caps, wallRows, wallCols).
	auto flatten = [](const std::vector<const std::vector<VoxCavityVertex>*>& parts) {
		size_t total = 0;
		for (const auto* p : parts) total += p->size();
		std::vector<sVertexXYZDT2> flat(total);
		sVertexXYZDT2* dst = flat.data();
		for (const auto* p : parts) {
			if (!p->empty()) {
				memcpy(dst, p->data(), p->size() * sizeof(sVertexXYZDT2));
				dst += p->size();
			}
		}
		return flat;
	};
	std::vector<const std::vector<VoxCavityVertex>*> capParts;
	std::vector<const std::vector<VoxCavityVertex>*> chunkParts;
	std::vector<const std::vector<VoxCavityVertex>*> rowWallParts;
	std::vector<const std::vector<VoxCavityVertex>*> colWallParts;
	for (const Chunk& ch : chunks_) {
		chunkParts.push_back(&ch.verts);
	}
	for (const auto& row : capRows_) {
		capParts.push_back(&row.second);
	}
	for (const auto& row : wallRows_) {
		rowWallParts.push_back(&row.second);
	}
	for (const auto& col : wallCols_) {
		colWallParts.push_back(&col.second);
	}
	const double tFlat = voxperf::nowMs();
	std::vector<sVertexXYZDT2> capsFlat = flatten(capParts);
	std::vector<sVertexXYZDT2> chunksFlat = flatten(chunkParts);
	std::vector<sVertexXYZDT2> rowWallsFlat = flatten(rowWallParts);
	std::vector<sVertexXYZDT2> colWallsFlat = flatten(colWallParts);
	voxperf::log("publish: flatten %.2f ms chunks=%zu caps=%zu "
	             "rowWalls=%zu colWalls=%zu",
	             voxperf::nowMs() - tFlat, chunksFlat.size(), capsFlat.size(),
	             rowWallsFlat.size(), colWallsFlat.size());
	syncCapTextureLocked();
	bakeWallTexturesLocked();
	syncTextureLocked(rowWallTexture_, rowWallTex_, rowWallTexDirty_);
	syncTextureLocked(colWallTexture_, colWallTex_, colWallTexDirty_);
#ifndef VOX_OVERLAY_HEADLESS
	chunksNode_->setTriangles(chunksFlat);
	capsNode_->setTriangles(capsFlat);
	rowWallsNode_->setTriangles(rowWallsFlat);
	colWallsNode_->setTriangles(colWallsFlat);
#endif
}

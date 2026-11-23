// ============================================================================
// VoxLayers.cpp — see VoxLayers.h.
// ============================================================================

#include "stdafxTr.h"

#include "VoxLayers.h"

#include <algorithm>

bool VoxColLayers::lookup(uint32_t cellOffset, int height, uint32_t* rgb) const
{
	uint8_t flags = 0;
	return lookup(cellOffset, height, rgb, &flags);
}

bool VoxColLayers::lookup(uint32_t cellOffset, int height, uint32_t* rgb,
                          uint8_t* flags) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	*flags = 0;
	if (height < 0 || height > 0xFFFF) {
		return false;
	}
	const int idx = findColumnImpl(cellOffset);
	if (idx < 0) {
		return false;
	}
	const uint32_t b = begin_[static_cast<size_t>(idx)];
	const uint32_t e = end_[static_cast<size_t>(idx)];
	const uint16_t h = static_cast<uint16_t>(height);
	for (uint32_t i = b; i < e; ++i) {
		const VoxLayerRange& r = ranges_[i];
		if (h < r.z0) {
			break;
		}
		if (h <= r.z1) {
			*flags = r.flags;
			*rgb = (static_cast<uint32_t>(r.r) << 16) |
			       (static_cast<uint32_t>(r.g) << 8) | r.b;
			return true;
		}
	}
	return false;
}

bool VoxColLayers::isTransparent(uint32_t cellOffset, int height) const
{
	uint32_t rgb = 0;
	uint8_t flags = 0;
	return lookup(cellOffset, height, &rgb, &flags) &&
	       (flags & VOX_LAYER_TRANSPARENT) != 0;
}

int VoxColLayers::topSolidVoxel(uint32_t cellOffset, int height) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	if (height <= 0) {
		return -1;
	}
	const int idx = findColumnImpl(cellOffset);
	if (idx < 0) {
		return height - 1; // слоёв нет — порода сплошная
	}
	const uint32_t b = begin_[static_cast<size_t>(idx)];
	const uint32_t e = end_[static_cast<size_t>(idx)];
	const VoxLayerRange* r = ranges_.data() + b;
	const int count = static_cast<int>(e - b);
	int h = height - 1;
	for (int i = count; i-- > 0;) {
		if (r[i].z0 > h) {
			continue; // диапазон выше текущего запроса
		}
		if (r[i].z1 < h) {
			return h; // зазор — безымянный сплошной воксель
		}
		if (r[i].flags & VOX_LAYER_TRANSPARENT) {
			h = static_cast<int>(r[i].z0) - 1; // проваливаемся сквозь пустоту
			if (h < 0) {
				return -1;
			}
			continue;
		}
		return h; // h внутри сплошного диапазона
	}
	return h; // ниже всех диапазонов — сплошная порода
}

bool VoxColLayers::lookupSolidBelow(uint32_t cellOffset, int height,
                                    uint32_t* rgb) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	if (height < 0 || height > 0xFFFF) {
		return false;
	}
	const int colIdx = findColumnImpl(cellOffset);
	if (colIdx < 0) {
		return false;
	}
	// Обратный проход от верхнего диапазона с z1 <= height: первый
	// непрозрачный — самый высокий сплошной воксель не выше запроса.
	const uint32_t b = begin_[static_cast<size_t>(colIdx)];
	const uint32_t e = end_[static_cast<size_t>(colIdx)];
	const uint16_t h = static_cast<uint16_t>(height);
	for (uint32_t i = e; i-- > b;) {
		const VoxLayerRange& r = ranges_[i];
		if (r.z1 > h) {
			continue; // выше запроса
		}
		if (!(r.flags & VOX_LAYER_TRANSPARENT)) {
			*rgb = (static_cast<uint32_t>(r.r) << 16) |
			       (static_cast<uint32_t>(r.g) << 8) | r.b;
			return true;
		}
	}
	return false;
}

bool VoxColLayers::querySurfaceColor(uint32_t cellOffset, int rawSurface,
                                     int damHeight, int viewSliceHeight,
                                     uint32_t* rgb, bool* cutVoid) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	*rgb = 0;
	*cutVoid = false;
	if (rawSurface < 0) {
		return false;
	}
	const int idx = findColumnImpl(cellOffset);
	if (idx < 0) {
		return false;
	}
	const uint32_t b = begin_[static_cast<size_t>(idx)];
	const uint32_t e = end_[static_cast<size_t>(idx)];
	int surface = rawSurface;
	if (damHeight > 0) {
		int display = -1;
		for (uint32_t i = b; i < e; ++i) {
			const VoxLayerRange& r = ranges_[i];
			if ((r.flags & VOX_LAYER_TRANSPARENT) && r.z0 < damHeight &&
			    (display < 0 || r.z0 < display)) {
				display = r.z0;
			}
		}
		if (display >= 0 && display < surface) {
			surface = display;
		}
	}
	const int h = viewSliceHeight >= 0 && viewSliceHeight < surface
	                  ? viewSliceHeight
	                  : surface;
	if (h < 0 || h > 0xFFFF) {
		return false;
	}
	const VoxLayerRange* hit = nullptr;
	for (uint32_t i = b; i < e; ++i) {
		const VoxLayerRange& r = ranges_[i];
		if (h < r.z0) {
			break;
		}
		if (h <= r.z1) {
			hit = &r;
			break;
		}
	}
	if (!hit) {
		return false;
	}
	if (!(hit->flags & VOX_LAYER_TRANSPARENT)) {
		*rgb = (static_cast<uint32_t>(hit->r) << 16) |
		       (static_cast<uint32_t>(hit->g) << 8) | hit->b;
		return true;
	}
	for (uint32_t i = e; i-- > b;) {
		const VoxLayerRange& r = ranges_[i];
		if (r.z1 > h) {
			continue;
		}
		if (!(r.flags & VOX_LAYER_TRANSPARENT)) {
			*rgb = (static_cast<uint32_t>(r.r) << 16) |
			       (static_cast<uint32_t>(r.g) << 8) | r.b;
			return true;
		}
	}
	*cutVoid = true;
	return false;
}

int VoxColLayers::findColumnImpl(uint32_t cellOffset) const
{
	// Caller holds a shared or unique lock; writers keep blockBegin_ current.
	const size_t blk = static_cast<size_t>(cellOffset) >> kBlockShift;
	if (blk >= blockCount_.size() || blockCount_[blk] == 0) {
		return -1;
	}
	int lo = static_cast<int>(blockBegin_[blk]);
	int hi = static_cast<int>(blockBegin_[blk + 1]);
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (col_[static_cast<size_t>(mid)] < cellOffset) {
			lo = mid + 1;
		} else if (col_[static_cast<size_t>(mid)] > cellOffset) {
			hi = mid;
		} else {
			return mid;
		}
	}
	return -1;
}

// Верхний сплошной воксель строго ниже height по уже снятому списку
// диапазонов (та же математика, что в topSolidVoxel, но без локов и
// поиска — normalize уже держит снимок).
static int topSolidFromRanges(const VoxLayerRange* r, int count, int height)
{
	if (height <= 0) {
		return -1;
	}
	if (count == 0) {
		return height - 1; // слоёв нет — порода сплошная
	}
	int h = height - 1;
	for (int i = count; i-- > 0;) {
		if (r[i].z0 > h) {
			continue; // диапазон выше текущего запроса
		}
		if (r[i].z1 < h) {
			return h; // зазор — безымянный сплошной воксель
		}
		if (r[i].flags & VOX_LAYER_TRANSPARENT) {
			h = static_cast<int>(r[i].z0) - 1; // проваливаемся сквозь пустоту
			if (h < 0) {
				return -1;
			}
			continue;
		}
		return h; // h внутри сплошного диапазона
	}
	return h; // ниже всех диапазонов — сплошная порода
}

bool VoxColLayers::capInfo(int colIdx, int vxd, int* displayH,
                           std::uint32_t* topRgb, std::uint8_t* topFlags,
                           std::uint32_t* topInherit,
                           std::uint32_t inheritFallback, bool* topFound) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	*displayH = -1;
	*topRgb = 0;
	*topFlags = 0;
	if (topInherit) {
		*topInherit = inheritFallback;
	}
	if (topFound) {
		*topFound = false;
	}
	if (colIdx < 0 || colIdx >= static_cast<int>(col_.size())) {
		return false;
	}
	const uint32_t b = begin_[static_cast<size_t>(colIdx)];
	const uint32_t e = end_[static_cast<size_t>(colIdx)];
	if (vxd > 0) {
		int display = -1;
		for (uint32_t k = b; k < e; ++k) {
			const VoxLayerRange& r = ranges_[k];
			if ((r.flags & VOX_LAYER_TRANSPARENT) && r.z0 < vxd &&
			    (display < 0 || r.z0 < display)) {
				display = r.z0;
			}
		}
		*displayH = display < 0 ? -1
		                        : (display <= vxd ? display : vxd);
		const int h = vxd - 1;
		for (uint32_t k = b; k < e; ++k) {
			const VoxLayerRange& r = ranges_[k];
			if (h < r.z0) {
				break;
			}
			if (h <= r.z1) {
				*topFlags = r.flags;
				*topRgb = (static_cast<std::uint32_t>(r.r) << 16) |
				          (static_cast<std::uint32_t>(r.g) << 8) | r.b;
				if (topFound) {
					*topFound = true;
				}
				break;
			}
		}
		if (topInherit) {
			for (uint32_t k = e; k > b; --k) {
				const VoxLayerRange& r = ranges_[k - 1];
				if (static_cast<int>(r.z1) >= h ||
				    (r.flags & VOX_LAYER_TRANSPARENT) != 0) {
					continue;
				}
				// Покраска пола полости (по срезу) — поверхность, не
				// тело острова: на кепку не наследуется (репорт:
				// краска по нижнему параллелепипеду перекрашивала и
				// верхний).
				if ((r.flags & VOX_LAYER_OVER_VOID) != 0) {
					continue;
				}
				*topInherit = (static_cast<std::uint32_t>(r.r) << 16) |
				              (static_cast<std::uint32_t>(r.g) << 8) | r.b;
				break;
			}
		}
	}
	return true;
}

int VoxColLayers::normalize(uint32_t cellOffset, int oldHeight, int newHeight,
                            std::uint32_t rgb, bool prune, bool* prunedOut)
{
	std::unique_lock<std::shared_mutex> lock(accessMtx);
	if (prunedOut) {
		*prunedOut = false;
	}
	const int idx = findColumnImpl(cellOffset);
	if (idx < 0) {
		return newHeight;
	}
	const uint32_t b = begin_[static_cast<size_t>(idx)];
	const uint32_t e = end_[static_cast<size_t>(idx)];
	const int count = static_cast<int>(e - b);
	// C3 — быстрый выход O(1): прозрачных нет (схлоп не сработает,
	// заливать нечего) и prune нечего резать (последний диапазон
	// заканчивается ниже новой высоты). Горячий путь «ровная колонка
	// с краской под собой» при каждом кванте камаза.
	if (colHasTransparent_[static_cast<size_t>(idx)] == 0 &&
	    (!prune || count == 0 ||
	     static_cast<int>(ranges_[e - 1].z1) < newHeight)) {
		return newHeight;
	}
	// Рабочая копия диапазонов: стек при коротком списке (обычно 1–3),
	// куча при длинном. Результат пишется через setColumn один раз —
	// слот, флаги и version (C1) фиксирует он.
	VoxLayerRange work[32];
	std::vector<VoxLayerRange> workHeap;
	const VoxLayerRange* cur = ranges_.data() + b;
	int n = count;
	bool modified = false;
	// Заливка прозрачных вокселей ниже нового верха сплошным цветом
	// колонки (правило solidify): на подъёме — всегда; на СПУСКЕ —
	// только если верхний воксель новой высоты оказался бы прозрачным:
	// без заливки инвариант «верх непрозрачен» схлопывал бы колонку к
	// сплошному под полостью — ОСТРОВ терялся одним шагом при копании
	// сквозь него (репорт: «columns instantly disappearing», данные
	// рваные). С заливкой остров тает плавно, пустота под новой
	// поверхностью заделывается.
	// upTo — новая высотка поверхности. dropRemainder (СПУСК): остаток
	// прозрачного диапазона, начинающийся на/выше upTo, — съеденный
	// материал, выбрасывается (иначе 1-воксельный «хвост» на старой
	// поверхности вечно держал колонку cavity и оверлей квадами).
	auto fillBelow = [&](int upTo, bool dropRemainder) {
		const uint8_t rr = static_cast<uint8_t>((rgb >> 16) & 0xFF);
		const uint8_t gg = static_cast<uint8_t>((rgb >> 8) & 0xFF);
		const uint8_t bb = static_cast<uint8_t>(rgb & 0xFF);
		VoxLayerRange* dst = work;
		// Буфер на n+1 слот достаточен: диапазоны колонки непересекающиеся
		// (paint поддерживает инвариант), поэтому upTo пересекает МАКСИМУМ
		// один прозрачный диапазон — хвост-двойник бывает только один.
		// Берём 2n запасом: при нарушенном инварианте (баг/рассинхрон
		// загрузки) переполнение work[32] на стеке портило бы соседние
		// объекты, включая внутренние указатели workHeap, — порча кучи.
		const size_t need = static_cast<size_t>(n) * 2;
		if (need > 32) {
			workHeap.resize(need);
			dst = workHeap.data();
		}
		int m = 0;
		for (int i = 0; i < n; ++i) {
			const VoxLayerRange& o = cur[i];
			if (!(o.flags & VOX_LAYER_TRANSPARENT) ||
			    o.z0 > upTo - 1) {
				dst[m++] = o;
				continue;
			}
			dst[m++] = VoxLayerRange{
			    o.z0, static_cast<uint16_t>(std::min(static_cast<int>(o.z1), upTo - 1)),
			    rr, gg, bb, 0};
			if (o.z1 > upTo - 1 && !dropRemainder) {
				dst[m++] = VoxLayerRange{
				    static_cast<uint16_t>(upTo), o.z1,
				    o.r, o.g, o.b,
				    static_cast<uint8_t>(VOX_LAYER_TRANSPARENT)};
			}
		}
		cur = dst;
		n = m;
		modified = true;
	};
	if (newHeight > oldHeight) {
		// Подъём: ЛЮБОЙ прозрачный воксель ниже нового верха заливается.
		for (int i = 0; i < count; ++i) {
			if ((cur[i].flags & VOX_LAYER_TRANSPARENT) &&
			    cur[i].z0 < newHeight) {
				fillBelow(newHeight, false);
				break;
			}
		}
	} else if (newHeight < oldHeight) {
		// СПУСК (копание): заливаем ТОЛЬКО если верх на новой высоте
		// прозрачный — иначе копание сквозь остров схлопнуло бы его
		// целиком (репорт: «columns instantly disappearing», рваные
		// данные). При oldHeight == newHeight (снятие прозрачного топа
		// краской) схлоп СОХРАНЯЕТСЯ — краской пустоты по верху колонку
		// разъедает намеренно.
		const int solid = topSolidFromRanges(cur, n, newHeight);
		if (solid != newHeight - 1) {
			bool any = false;
			for (int i = 0; i < n; ++i) {
				if ((cur[i].flags & VOX_LAYER_TRANSPARENT) &&
				    cur[i].z0 < newHeight) {
					any = true;
					break;
				}
			}
			if (any) {
				fillBelow(newHeight, true);
			}
		}
	}
	// Инвариант «верхний воксель непрозрачный»: высота схлопывается к
	// ближайшему сплошному под новым верхом (после заливки выше не
	// срабатывает — осталось страховкой на экзотические расклады).
	const int solid = topSolidFromRanges(cur, n, newHeight);
	const int h = (solid < 0) ? 0 : solid + 1;
	int kept = n;
	if (prune) {
		// Диапазоны, целиком над итоговой высотой, вырезаются.
		kept = 0;
		while (kept < n && cur[kept].z0 < h) {
			++kept;
		}
		if (kept != n) {
			modified = true;
			if (prunedOut) {
				*prunedOut = true;
			}
		}
	}
	if (modified) {
		setColumnImpl(cellOffset, cur, kept);
	}
	return h;
}

int VoxColLayers::findColumn(uint32_t cellOffset) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	const size_t blk = static_cast<size_t>(cellOffset) >> kBlockShift;
	if (blk >= blockCount_.size() || blockCount_[blk] == 0) {
		return -1;
	}
	int lo = static_cast<int>(blockBegin_[blk]);
	int hi = static_cast<int>(blockBegin_[blk + 1]);
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		if (col_[static_cast<size_t>(mid)] < cellOffset) {
			lo = mid + 1;
		} else if (col_[static_cast<size_t>(mid)] > cellOffset) {
			hi = mid;
		} else {
			return mid;
		}
	}
	return -1;
}

void VoxColLayers::rebuildBlockBegin()
{
	if (col_.empty()) {
		blockBegin_.clear();
		return;
	}
	const size_t blocks =
	    (static_cast<size_t>(col_.back()) >> kBlockShift) + 1;
	blockBegin_.assign(blocks + 1, 0);
	size_t idx = 0;
	for (size_t b = 0; b < blocks; ++b) {
		const uint32_t beg = static_cast<uint32_t>(b) << kBlockShift;
		while (idx < col_.size() && col_[idx] < beg) {
			++idx;
		}
		blockBegin_[b] = static_cast<uint32_t>(idx);
	}
	blockBegin_[blocks] = static_cast<uint32_t>(col_.size());
}

const VoxLayerRange* VoxColLayers::columnRanges(int colIdx, int* count) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	if (colIdx < 0 || colIdx >= static_cast<int>(col_.size())) {
		*count = 0;
		return nullptr;
	}
	const uint32_t b = begin_[static_cast<size_t>(colIdx)];
	const uint32_t e = end_[static_cast<size_t>(colIdx)];
	*count = static_cast<int>(e - b);
	return ranges_.data() + b;
}

bool VoxColLayers::sameColor(const VoxLayerRange& a, uint8_t r, uint8_t g,
                             uint8_t b, bool transparent)
{
	const uint8_t want = transparent ? VOX_LAYER_TRANSPARENT : 0;
	return a.r == r && a.g == g && a.b == b &&
	       (a.flags & VOX_LAYER_TRANSPARENT) == want;
}

void VoxColLayers::setColumn(uint32_t cellOffset, const VoxLayerRange* ranges,
                             int count)
{
	std::unique_lock<std::shared_mutex> lock(accessMtx);
	setColumnImpl(cellOffset, ranges, count);
}

void VoxColLayers::setColumnImpl(uint32_t cellOffset,
                                 const VoxLayerRange* ranges, int count)
{
	if (count < 0) {
		count = 0;
	}
	// Мутации O(диапазоны колонки), не O(пул): слот колонки задаётся парой
	// begin_/end_ (границы развязаны — в пуле есть дырки), удалённая
	// колонка остаётся ТОМБСТОУНОМ (begin_==end_) — без memmove col_.
	// Дырки и томбстоуны упаковывает лениво compact() (перед сохранением).
	// C1: version бампается ТОЛЬКО при реальном изменении содержимого —
	// идентичная перезапись (той же формы, равные байты) его не трогает,
	// иначе оверлей полостей получает ложный сигнал и rebuild'ит rect
	// впустую (drainPending не может отбросить такое обновление).
	bool bumpVersion = false;
	const int idx = findColumnImpl(cellOffset);
	if (idx >= 0) {
		const uint32_t b = begin_[static_cast<size_t>(idx)];
		const uint32_t e = end_[static_cast<size_t>(idx)];
		liveRanges_ -= e - b;
		if (count == 0) {
			if (e > b) {
				// live -> tombstone: живых слоёв больше нет — игровой гейт
				// hasLiveColumn перестаёт звать normalize для этой колонки.
				const size_t blk =
				    static_cast<size_t>(cellOffset) >> kBlockShift;
				if (blk < blockLive_.size() && blockLive_[blk] > 0) {
					--blockLive_[blk];
				}
				++tombstones_;
				bumpVersion = true;
			}
			end_[static_cast<size_t>(idx)] = b;
			colHasTransparent_[static_cast<size_t>(idx)] = 0;
			if (bumpVersion) {
				++version;
				lastMutationOff = cellOffset;
			}
			return;
		}
		liveRanges_ += count;
		if (e == b) {
			// Воскрешение томбстоуна.
			const size_t blk =
			    static_cast<size_t>(cellOffset) >> kBlockShift;
			growBlockIndex(cellOffset);
			++blockLive_[blk];
			if (tombstones_ > 0) {
				--tombstones_;
			}
			bumpVersion = true;
		}
		uint8_t hasTransparent = 0;
		for (int i = 0; i < count; ++i) {
			if (ranges[i].flags & VOX_LAYER_TRANSPARENT) {
				hasTransparent = 1;
				break;
			}
		}
		if (static_cast<uint32_t>(count) == e - b) {
			// Same shape: перезапись на месте (обычная перекраска). C1:
			// идентичные байты — ничего не трогаем вообще.
			if (std::memcmp(ranges, ranges_.data() + b,
			                count * sizeof(VoxLayerRange)) != 0) {
				std::copy(ranges, ranges + count, ranges_.begin() + b);
				colHasTransparent_[static_cast<size_t>(idx)] = hasTransparent;
				bumpVersion = true;
			}
		} else if (static_cast<uint32_t>(count) < e - b) {
			// Ужать на месте; хвост старого слота — дырка в пуле.
			std::copy(ranges, ranges + count, ranges_.begin() + b);
			end_[static_cast<size_t>(idx)] = b + count;
			colHasTransparent_[static_cast<size_t>(idx)] = hasTransparent;
			bumpVersion = true;
		} else {
			// Рост: новый слот в конец пула (amortized), старый — дырка.
			const uint32_t nb = static_cast<uint32_t>(ranges_.size());
			ranges_.insert(ranges_.end(), ranges, ranges + count);
			begin_[static_cast<size_t>(idx)] = nb;
			end_[static_cast<size_t>(idx)] = nb + count;
			colHasTransparent_[static_cast<size_t>(idx)] = hasTransparent;
			bumpVersion = true;
		}
		if (bumpVersion) {
			++version;
			lastMutationOff = cellOffset;
		}
		return;
	}
	if (count == 0) {
		return;
	}
	// Вставка новой колонки: слот — в конец пула (не в середину, без
	// memmove пула; порядок пула != порядок col_ — допустимо, границы
	// колонок заданы begin_/end_).
	const uint32_t nb = static_cast<uint32_t>(ranges_.size());
	ranges_.insert(ranges_.end(), ranges, ranges + count);
	size_t pos = 0;
	{
		size_t lo = 0, hi = col_.size();
		while (lo < hi) { // upper bound: первый col_ > cellOffset
			const size_t mid = (lo + hi) / 2;
			if (col_[mid] <= cellOffset) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		pos = lo;
	}
	uint8_t hasTransparent = 0;
	for (int i = 0; i < count; ++i) {
		if (ranges[i].flags & VOX_LAYER_TRANSPARENT) {
			hasTransparent = 1;
			break;
		}
	}
	col_.insert(col_.begin() + static_cast<ptrdiff_t>(pos), cellOffset);
	begin_.insert(begin_.begin() + static_cast<ptrdiff_t>(pos), nb);
	end_.insert(end_.begin() + static_cast<ptrdiff_t>(pos),
	            nb + static_cast<uint32_t>(count));
	colHasTransparent_.insert(
	    colHasTransparent_.begin() + static_cast<ptrdiff_t>(pos),
	    hasTransparent);
	liveRanges_ += count;
	growBlockIndex(cellOffset);
	++blockCount_[static_cast<size_t>(cellOffset) >> kBlockShift];
	++blockLive_[static_cast<size_t>(cellOffset) >> kBlockShift];
	const size_t block = static_cast<size_t>(cellOffset) >> kBlockShift;
	if (blockBegin_.empty()) {
		rebuildBlockBegin();
	} else {
		if (block >= blockBegin_.size() - 1) {
			blockBegin_.resize(block + 2, static_cast<std::uint32_t>(pos));
		}
		for (size_t b = 0; b + 1 < blockBegin_.size(); ++b) {
			if ((b > block && blockBegin_[b] >= pos) ||
			    (b == block && blockBegin_[b] > pos)) {
				++blockBegin_[b];
			}
		}
		blockBegin_.back() = static_cast<std::uint32_t>(col_.size());
	}
	++version;
	lastMutationOff = cellOffset;
}

void VoxColLayers::compact()
{
	std::unique_lock<std::shared_mutex> lock(accessMtx);
	std::vector<uint32_t> ncol;
	std::vector<uint32_t> nbegin;
	std::vector<uint32_t> nend;
	std::vector<VoxLayerRange> nranges;
	std::vector<uint8_t> nflags;
	ncol.reserve(col_.size());
	nflags.reserve(col_.size());
	nbegin.reserve(col_.size());
	nend.reserve(col_.size());
	nranges.reserve(static_cast<size_t>(liveRanges_));
	for (size_t i = 0; i < col_.size(); ++i) {
		const uint32_t c =
		    end_[i] - begin_[i];
		if (!c) {
			continue; // tombstone
		}
		nbegin.push_back(static_cast<uint32_t>(nranges.size()));
		nranges.insert(nranges.end(), ranges_.begin() + begin_[i],
		               ranges_.begin() + end_[i]);
		nend.push_back(static_cast<uint32_t>(nranges.size()));
		ncol.push_back(col_[i]);
		uint8_t t = 0;
		for (uint32_t k = begin_[i]; k < end_[i]; ++k) {
			if (ranges_[k].flags & VOX_LAYER_TRANSPARENT) {
				t = 1;
				break;
			}
		}
		nflags.push_back(t);
	}
	col_.swap(ncol);
	begin_.swap(nbegin);
	end_.swap(nend);
	colHasTransparent_.swap(nflags);
	ranges_.swap(nranges);
	tombstones_ = 0;
	buildBlockIndexImpl();
	++version;
}

void VoxColLayers::paint(uint32_t cellOffset, int z0, int z1,
                         uint8_t r, uint8_t g, uint8_t b, bool transparent)
{
	std::unique_lock<std::shared_mutex> lock(accessMtx);
	if (z1 < z0 || z0 < 0) {
		return;
	}
	if (z1 > 0xFFFF) {
		z1 = 0xFFFF;
	}
	const uint16_t nz0 = static_cast<uint16_t>(z0);
	const uint16_t nz1 = static_cast<uint16_t>(z1);
	const uint8_t flags = transparent ? VOX_LAYER_TRANSPARENT : 0;

	// Вычитаем [nz0..nz1] из существующих диапазонов колонки.
	const int idx = findColumnImpl(cellOffset);
	std::vector<VoxLayerRange> kept;
	bool coveredVoid = false;
	if (idx >= 0) {
		const uint32_t b = begin_[static_cast<size_t>(idx)];
		const uint32_t e = end_[static_cast<size_t>(idx)];
		const int count = static_cast<int>(e - b);
		const VoxLayerRange* old = ranges_.data() + b;
		for (int i = 0; i < count; ++i) {
			const VoxLayerRange& o = old[i];
			if (o.z1 < nz0 || o.z0 > nz1) {
				kept.push_back(o); // не пересекается
				continue;
			}
			if (((o.flags & VOX_LAYER_TRANSPARENT) != 0 ||
			     (o.flags & VOX_LAYER_OVER_VOID) != 0) &&
			    z1 >= o.z0 && z0 <= o.z1) {
				// краска легла на пустоту или на уже замаркированный
				// «пустотный» диапазон — маркер наследуется (иначе
				// перекраска пола теряла бы его)
				coveredVoid = true;
			}
			if (o.z0 < nz0) { // левый остаток
				VoxLayerRange left = o;
				left.z1 = nz0 - 1;
				kept.push_back(left);
			}
			if (o.z1 > nz1) { // правый остаток
				VoxLayerRange right = o;
				right.z0 = nz1 + 1;
				kept.push_back(right);
			}
		}
	}

	// Вставка с слиянием смежных одноцветных (и того же типа прозрачности
	// и того же маркера OVER_VOID — иначе маркер потерялся бы в мерже).
	VoxLayerRange ins{nz0, nz1, r, g, b, flags};
	if (coveredVoid) {
		ins.flags = static_cast<uint8_t>(ins.flags | VOX_LAYER_OVER_VOID);
	}
	std::vector<VoxLayerRange> merged;
	size_t i = 0;
	while (i < kept.size() && kept[i].z1 < ins.z0) {
		merged.push_back(kept[i]);
		++i;
	}
	if (i < kept.size() && kept[i].z0 == ins.z0 &&
	    sameColor(kept[i], r, g, b, transparent) &&
	    (kept[i].flags & VOX_LAYER_OVER_VOID) ==
	        (ins.flags & VOX_LAYER_OVER_VOID)) {
		ins.z1 = kept[i].z1; // сливаемся с одноцветным справа
		++i;
	}
	merged.push_back(ins);
	for (; i < kept.size(); ++i) {
		if (kept[i].z0 == ins.z1 + 1 &&
		    sameColor(kept[i], r, g, b, transparent) &&
		    (kept[i].flags & VOX_LAYER_OVER_VOID) ==
		        (ins.flags & VOX_LAYER_OVER_VOID)) {
			merged.back().z1 = kept[i].z1; // одноцветный сосед справа
		} else {
			merged.push_back(kept[i]);
		}
	}

	setColumnImpl(cellOffset, merged.data(), static_cast<int>(merged.size()));
}

void VoxColLayers::solidify(uint32_t cellOffset, int z0, int z1,
                            uint8_t r, uint8_t g, uint8_t b)
{
	std::unique_lock<std::shared_mutex> lock(accessMtx);
	if (z1 < z0 || z0 < 0) {
		return;
	}
	if (z1 > 0xFFFF) {
		z1 = 0xFFFF;
	}
	const int idx = findColumnImpl(cellOffset);
	if (idx < 0) {
		return;
	}
	const uint32_t rangeBegin = begin_[static_cast<size_t>(idx)];
	const uint32_t rangeEnd = end_[static_cast<size_t>(idx)];
	const int count = static_cast<int>(rangeEnd - rangeBegin);
	const VoxLayerRange* old = ranges_.data() + rangeBegin;
	// C1: нет пересечений с прозрачными — setColumn не нужен (иначе
	// бампается version и оверлей rebuild'ит rect впустую).
	bool any = false;
	for (int i = 0; i < count; ++i) {
		if ((old[i].flags & VOX_LAYER_TRANSPARENT) &&
		    old[i].z1 >= z0 && old[i].z0 <= z1) {
			any = true;
			break;
		}
	}
	if (!any) {
		return;
	}
	// Новый список: прозрачные пересечения [z0..z1] заменяются сплошным
	// диапазоном с цветом по умолчанию; непрозрачные не трогаем.
	std::vector<VoxLayerRange> next;
	next.reserve(static_cast<size_t>(count) + 2);
	for (int i = 0; i < count; ++i) {
		const VoxLayerRange& o = old[i];
		if (!(o.flags & VOX_LAYER_TRANSPARENT) || o.z1 < z0 || o.z0 > z1) {
			next.push_back(o);
			continue;
		}
		if (o.z0 < z0) { // прозрачный левый остаток
			VoxLayerRange left = o;
			left.z1 = static_cast<uint16_t>(z0 - 1);
			next.push_back(left);
		}
		VoxLayerRange mid{
			static_cast<uint16_t>(std::max(static_cast<int>(o.z0), z0)),
			static_cast<uint16_t>(std::min(static_cast<int>(o.z1), z1)),
			r, g, b, 0};
		next.push_back(mid);
		if (o.z1 > z1) { // прозрачный правый остаток
			VoxLayerRange right = o;
			right.z0 = static_cast<uint16_t>(z1 + 1);
			next.push_back(right);
		}
	}
	setColumnImpl(cellOffset, next.data(), static_cast<int>(next.size()));
}

void VoxColLayers::pruneAbove(uint32_t cellOffset, int height)
{
	std::unique_lock<std::shared_mutex> lock(accessMtx);
	const int idx = findColumnImpl(cellOffset);
	if (idx < 0) {
		return;
	}
	const uint32_t b = begin_[static_cast<size_t>(idx)];
	const uint32_t e = end_[static_cast<size_t>(idx)];
	const VoxLayerRange* r = ranges_.data() + b;
	const int count = static_cast<int>(e - b);
	// Диапазоны отсортированы по z0: всё от первого с z0 >= height — над
	// поверхностью.
	int kept = 0;
	while (kept < count && r[kept].z0 < height) {
		++kept;
	}
	if (kept == count) {
		return;
	}
	setColumnImpl(cellOffset, r, kept);
}

void VoxColLayers::clear()
{
	std::unique_lock<std::shared_mutex> lock(accessMtx);
	col_.clear();
	begin_.clear();
	end_.clear();
	ranges_.clear();
	colHasTransparent_.clear();
	blockCount_.clear();
	blockLive_.clear();
	blockBegin_.clear();
	liveRanges_ = 0;
	tombstones_ = 0;
}

void VoxColLayers::buildBlockIndex()
{
	std::unique_lock<std::shared_mutex> lock(accessMtx);
	buildBlockIndexImpl();
}

void VoxColLayers::buildBlockIndexImpl()
{
	blockCount_.clear();
	blockLive_.clear();
	blockBegin_.clear();
	if (col_.empty()) {
		return;
	}
	const size_t blocks =
		(static_cast<size_t>(col_.back()) >> kBlockShift) + 1;
	blockCount_.assign(blocks, 0u);
	blockLive_.assign(blocks, 0u);
	blockBegin_.assign(blocks + 1, 0u);
	size_t idx = 0;
	for (size_t b = 0; b < blocks; ++b) {
		const uint32_t beg = static_cast<uint32_t>(b) << kBlockShift;
		while (idx < col_.size() && col_[idx] < beg) {
			++idx;
		}
		blockBegin_[b] = static_cast<uint32_t>(idx);
	}
	blockBegin_[blocks] = static_cast<uint32_t>(col_.size());
	for (size_t i = 0; i < col_.size(); ++i) {
		const size_t blk = static_cast<size_t>(col_[i]) >> kBlockShift;
		++blockCount_[blk];
		if (end_[i] > begin_[i]) {
			++blockLive_[blk];
		}
	}
}

void VoxColLayers::growBlockIndex(uint32_t cellOffset)
{
	const size_t b = static_cast<size_t>(cellOffset) >> kBlockShift;
	if (b >= blockCount_.size()) {
		blockCount_.resize(b + 1, 0u);
	}
	if (b >= blockLive_.size()) {
		blockLive_.resize(b + 1, 0u);
	}
}

std::pair<int, int> VoxColLayers::columnIndexRange(uint32_t offBeg,
                                                   uint32_t offEnd) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	if (col_.empty() || offEnd < offBeg) {
		return {0, 0};
	}
	const size_t blocks = blockBegin_.size() - 1;
	const size_t first = static_cast<size_t>(offBeg) >> kBlockShift;
	if (first >= blocks) {
		const int end = static_cast<int>(col_.size());
		return {end, end};
	}
	const size_t last = std::min(
	    static_cast<size_t>(offEnd) >> kBlockShift, blocks - 1);
	const auto begin = col_.begin() + blockBegin_[first];
	const auto end = col_.begin() + blockBegin_[last + 1];
	const auto lo = std::lower_bound(begin, end, offBeg);
	const auto hi = std::upper_bound(lo, end, offEnd);
	return {static_cast<int>(lo - col_.begin()),
	        static_cast<int>(hi - col_.begin())};
}

bool VoxColLayers::columnHasTransparent(uint32_t cellOffset) const
{
	std::shared_lock<std::shared_mutex> lock(accessMtx);
	const int idx = findColumnImpl(cellOffset);
	return idx >= 0 && colHasTransparent_[static_cast<size_t>(idx)] != 0;
}

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
	*flags = 0;
	if (height < 0 || height > 0xFFFF) {
		return false;
	}
	// Бинарный поиск колонки.
	int lo = 0;
	int hi = static_cast<int>(col_.size()) - 1;
	while (lo <= hi) {
		const int mid = (lo + hi) / 2;
		if (col_[static_cast<size_t>(mid)] < cellOffset) {
			lo = mid + 1;
		} else if (col_[static_cast<size_t>(mid)] > cellOffset) {
			hi = mid - 1;
		} else {
			const uint32_t b = begin_[static_cast<size_t>(mid)];
			const uint32_t e =
				mid + 1 < static_cast<int>(begin_.size())
					? begin_[static_cast<size_t>(mid) + 1]
					: static_cast<uint32_t>(ranges_.size());
			const uint16_t h = static_cast<uint16_t>(height);
			// Диапазонов в колонке обычно единицы — линейный проход; при
			// больших списках выигрыш бинарного поиска нивелируется кэшем.
			for (uint32_t i = b; i < e; ++i) {
				const VoxLayerRange& r = ranges_[i];
				if (h < r.z0) {
					break; // диапазоны отсортированы по z0
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

bool VoxColLayers::lookupSolidBelow(uint32_t cellOffset, int height,
                                    uint32_t* rgb) const
{
	if (height < 0 || height > 0xFFFF) {
		return false;
	}
	const int colIdx = findColumn(cellOffset);
	if (colIdx < 0) {
		return false;
	}
	// Обратный проход от верхнего диапазона с z1 <= height: первый
	// непрозрачный — самый высокий сплошной воксель не выше запроса.
	const uint32_t b = begin_[static_cast<size_t>(colIdx)];
	const uint32_t e =
		colIdx + 1 < static_cast<int>(begin_.size())
			? begin_[static_cast<size_t>(colIdx) + 1]
			: static_cast<uint32_t>(ranges_.size());
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

int VoxColLayers::findColumn(uint32_t cellOffset) const
{
	int lo = 0;
	int hi = static_cast<int>(col_.size()) - 1;
	while (lo <= hi) {
		const int mid = (lo + hi) / 2;
		if (col_[static_cast<size_t>(mid)] < cellOffset) {
			lo = mid + 1;
		} else if (col_[static_cast<size_t>(mid)] > cellOffset) {
			hi = mid - 1;
		} else {
			return mid;
		}
	}
	return -1;
}

const VoxLayerRange* VoxColLayers::columnRanges(int colIdx, int* count) const
{
	if (colIdx < 0 || colIdx >= static_cast<int>(col_.size())) {
		*count = 0;
		return nullptr;
	}
	const uint32_t b = begin_[static_cast<size_t>(colIdx)];
	const uint32_t e =
		colIdx + 1 < static_cast<int>(begin_.size())
			? begin_[static_cast<size_t>(colIdx) + 1]
			: static_cast<uint32_t>(ranges_.size());
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
	if (count < 0) {
		count = 0;
	}
	// In-place splice. The previous version rebuilt all three vectors for
	// every call — O(n) allocations + full copies per painted column, which
	// made a stamp over k cells O(k·n) and visibly slow. All element types
	// are PODs, so insert/erase are plain memmoves; the only O(n) part is
	// the begin_ shift (4-byte ints), unavoidable with a flat pool.
	const int idx = findColumn(cellOffset);
	if (idx >= 0) {
		const uint32_t b = begin_[static_cast<size_t>(idx)];
		const uint32_t e =
			idx + 1 < static_cast<int>(begin_.size())
				? begin_[static_cast<size_t>(idx) + 1]
				: static_cast<uint32_t>(ranges_.size());
		const uint32_t oldCount = e - b;
		if (static_cast<uint32_t>(count) == oldCount) {
			// Same shape: copy the ranges in place (the common repaint).
			std::copy(ranges, ranges + count, ranges_.begin() + b);
			return;
		}
		ranges_.erase(ranges_.begin() + b, ranges_.begin() + e);
		if (count > 0) {
			ranges_.insert(ranges_.begin() + b, ranges, ranges + count);
		}
		const int delta = count - static_cast<int>(oldCount);
		for (size_t j = static_cast<size_t>(idx) + 1; j < begin_.size(); ++j) {
			begin_[j] = static_cast<uint32_t>(static_cast<int>(begin_[j]) +
			                                 delta);
		}
		if (count == 0) {
			col_.erase(col_.begin() + idx);
			begin_.erase(begin_.begin() + idx);
		}
		return;
	}
	if (count == 0) {
		return;
	}
	// Вставка новой колонки с сохранением сортировки: пул сращивается
	// в позиции poolPos — между диапазонами колонок pos-1 и pos.
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
	const uint32_t poolPos =
		pos < begin_.size() ? begin_[pos] : static_cast<uint32_t>(ranges_.size());
	col_.insert(col_.begin() + static_cast<ptrdiff_t>(pos), cellOffset);
	begin_.insert(begin_.begin() + static_cast<ptrdiff_t>(pos), poolPos);
	ranges_.insert(ranges_.begin() + poolPos, ranges, ranges + count);
	for (size_t j = pos + 1; j < begin_.size(); ++j) {
		begin_[j] = static_cast<uint32_t>(static_cast<int>(begin_[j]) + count);
	}
}

void VoxColLayers::paint(uint32_t cellOffset, int z0, int z1,
                         uint8_t r, uint8_t g, uint8_t b, bool transparent)
{
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
	const int idx = findColumn(cellOffset);
	std::vector<VoxLayerRange> kept;
	if (idx >= 0) {
		int count = 0;
		const VoxLayerRange* old = columnRanges(idx, &count);
		for (int i = 0; i < count; ++i) {
			const VoxLayerRange& o = old[i];
			if (o.z1 < nz0 || o.z0 > nz1) {
				kept.push_back(o); // не пересекается
				continue;
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

	// Вставка с слиянием смежных одноцветных (и того же типа прозрачности).
	VoxLayerRange ins{nz0, nz1, r, g, b, flags};
	std::vector<VoxLayerRange> merged;
	size_t i = 0;
	while (i < kept.size() && kept[i].z1 < ins.z0) {
		merged.push_back(kept[i]);
		++i;
	}
	if (i < kept.size() && kept[i].z0 == ins.z0 &&
	    sameColor(kept[i], r, g, b, transparent)) {
		ins.z1 = kept[i].z1; // сливаемся с одноцветным справа
		++i;
	}
	merged.push_back(ins);
	for (; i < kept.size(); ++i) {
		if (kept[i].z0 == ins.z1 + 1 &&
		    sameColor(kept[i], r, g, b, transparent)) {
			merged.back().z1 = kept[i].z1; // одноцветный сосед справа
		} else {
			merged.push_back(kept[i]);
		}
	}

	setColumn(cellOffset, merged.data(), static_cast<int>(merged.size()));
}

void VoxColLayers::solidify(uint32_t cellOffset, int z0, int z1,
                            uint8_t r, uint8_t g, uint8_t b)
{
	if (z1 < z0 || z0 < 0) {
		return;
	}
	if (z1 > 0xFFFF) {
		z1 = 0xFFFF;
	}
	const int idx = findColumn(cellOffset);
	if (idx < 0) {
		return;
	}
	int count = 0;
	const VoxLayerRange* old = columnRanges(idx, &count);
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
	setColumn(cellOffset, next.data(), static_cast<int>(next.size()));
}

void VoxColLayers::clear()
{
	col_.clear();
	begin_.clear();
	ranges_.clear();
}

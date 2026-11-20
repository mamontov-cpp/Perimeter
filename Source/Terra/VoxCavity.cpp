// ============================================================================
// VoxCavity.cpp — see VoxCavity.h.
// ============================================================================

#include "stdafxTr.h"

#include "VoxCavity.h"

#include "VMAP.h"

#include <algorithm>
#include <vector>

namespace {

// Face shading factors — the cavity interior must read as 3D.
const float kTopShade = 1.0f;
const float kSideShade = 0.75f;
const float kBottomShade = 0.55f;
// Side-plane epsilon (fraction of a cell) against the heightfield cliff
// triangles at plateau boundaries. Must exceed the depth-buffer precision
// at gameplay distances (near 30 / far 1e5 gives ~2 cm at 3k units) —
// 0.01 z-fought the cliffs into blurry streaks.
const float kSideEps = 0.1f;

struct Segment
{
	int z0;
	int z1;
};

// One constant-color run of solid voxels [z0..z1].
struct SolidRun
{
	int z0;
	int z1;
	std::uint32_t color;
};

// One constant-color wall run of the column's side face [z0..z1), color
// already shaded with the owner column's light.
struct SideRun
{
	int z0;
	int z1;
	std::uint32_t color;

	bool operator==(const SideRun& o) const
	{
		return z0 == o.z0 && z1 == o.z1 && color == o.color;
	}
	bool operator!=(const SideRun& o) const { return !(*this == o); }
};

// Копия диапазонов колонки под коротким локом: мешер НЕ держит accessMtx
// на весь проход (иначе логический поток игры, правящий слои при
// терраформинге, встаёт на сотни миллисекунд за время перестройки).
struct RangeCopy
{
	static const int kStack = 32;
	VoxLayerRange stack[kStack];
	std::vector<VoxLayerRange> heap;
	const VoxLayerRange* data = stack;
	int count = 0;
};

void copyRanges(vrtMap& m, std::uint32_t off, RangeCopy& out)
{
	std::lock_guard<std::recursive_mutex> lock(m.voxLayers.accessMtx);
	const int idx = m.voxLayers.findColumn(off);
	if (idx < 0) {
		out.count = 0;
		return;
	}
	int count = 0;
	const VoxLayerRange* r = m.voxLayers.columnRanges(idx, &count);
	if (count <= RangeCopy::kStack) {
		std::copy_n(r, count, out.stack);
		out.data = out.stack;
	} else {
		out.heap.assign(r, r + count);
		out.data = out.heap.data();
	}
	out.count = count;
}

// Solid segments of the dam column [0..H) split by transparent ranges —
// color-agnostic maximal spans of solid voxel material.
void segmentsOfRanges(const VoxLayerRange* ranges, int count, int height,
                      std::vector<Segment>& out)
{
	int z = 0;
	int ti = 0;
	while (z < height) {
		while (ti < count &&
		       (!(ranges[ti].flags & VOX_LAYER_TRANSPARENT) ||
		        ranges[ti].z1 < z)) {
			++ti;
		}
		if (ti < count && ranges[ti].z0 <= z) {
			z = static_cast<int>(ranges[ti].z1) + 1; // прозрачный пропуск
			continue;
		}
		const int s0 = z;
		if (ti < count) {
			z = ranges[ti].z0; // до следующего (прозрачного) диапазона
		} else {
			z = height;
		}
		out.push_back({s0, z - 1});
	}
}

// Constant-color solid runs of the column [0..height): the solid segments
// split at layer-color boundaries; gaps and un-painted voxels take the
// column default color. O(count) — runs, not voxels.
void runsOfRanges(const VoxLayerRange* ranges, int count, int height,
                  std::uint32_t fallback, std::vector<SolidRun>& out)
{
	std::vector<Segment> spans;
	segmentsOfRanges(ranges, count, height, spans);
	for (const Segment& s : spans) {
		int c = s.z0;
		int ri = 0;
		while (ri < count && static_cast<int>(ranges[ri].z1) < c) {
			++ri;
		}
		while (c <= s.z1) {
			if (ri < count && static_cast<int>(ranges[ri].z0) <= c &&
			    !(ranges[ri].flags & VOX_LAYER_TRANSPARENT)) {
				const int e = std::min(static_cast<int>(ranges[ri].z1), s.z1);
				const std::uint32_t col =
				    (static_cast<std::uint32_t>(ranges[ri].r) << 16) |
				    (static_cast<std::uint32_t>(ranges[ri].g) << 8) |
				    ranges[ri].b;
				if (!out.empty() && out.back().z1 + 1 == c &&
				    out.back().color == col) {
					out.back().z1 = e;
				} else {
					out.push_back({c, e, col});
				}
				c = e + 1;
				++ri;
				continue;
			}
			int e = s.z1;
			if (ri < count && static_cast<int>(ranges[ri].z0) > c) {
				e = std::min(static_cast<int>(ranges[ri].z0) - 1, s.z1);
			}
			if (!out.empty() && out.back().z1 + 1 == c &&
			    out.back().color == fallback) {
				out.back().z1 = e;
			} else {
				out.push_back({c, e, fallback});
			}
			c = e + 1;
		}
	}
}

std::uint32_t shade(std::uint32_t rgb, float factor, unsigned light)
{
	unsigned r = static_cast<unsigned>((rgb >> 16) & 0xFF) * light / 127u;
	unsigned g = static_cast<unsigned>((rgb >> 8) & 0xFF) * light / 127u;
	unsigned b = static_cast<unsigned>(rgb & 0xFF) * light / 127u;
	r = static_cast<unsigned>(static_cast<float>(r) * factor);
	g = static_cast<unsigned>(static_cast<float>(g) * factor);
	b = static_cast<unsigned>(static_cast<float>(b) * factor);
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/// One quad, single winding only. The overlay draws with culling disabled
/// (cRawMeshObj::Draw), so the reversed fan would be pure duplication -
/// halving the vertex count halves both the upload and the GPU vertex load.
void quad(std::vector<VoxCavityVertex>& out,
          const VoxCavityVertex& a, const VoxCavityVertex& b,
          const VoxCavityVertex& c, const VoxCavityVertex& d)
{
	out.push_back(a);
	out.push_back(b);
	out.push_back(c);
	out.push_back(a);
	out.push_back(c);
	out.push_back(d);
}

} // namespace

namespace {

/// Height the neighbor RENDERS at (display height for cavity columns).
int neighborDisplayHeight(vrtMap& m, int cx, int cy)
{
	cx = m.XCYCL(cx);
	cy = m.YCYCL(cy);
	const int noff = cy * static_cast<int>(m.H_SIZE) + cx;
	const int d = m.cavityDisplayHeight(noff);
	if (d >= 0) {
		return d;
	}
	return m.VxDBuf[noff] != 0 ? m.VxDBuf[noff] : m.VxGBuf[noff];
}

/// True when any side face of this column borders on emptiness — i.e. the
/// cavity (or the column's island) is visible from outside. Cheap display-
/// height test: a neighbor rendering below this column's top leaves a gap.
bool columnExposed(vrtMap& m, int x, int y, int height)
{
	return neighborDisplayHeight(m, x + 1, y) < height ||
	       neighborDisplayHeight(m, x - 1, y) < height ||
	       neighborDisplayHeight(m, x, y + 1) < height ||
	       neighborDisplayHeight(m, x, y - 1) < height;
}

} // namespace

/// One side quad (both windings) for the voxel run [z0..z1) on direction
/// 0:+x 1:-x 2:+y 3:-y, pushed a small epsilon into the empty neighbor cell.
void emitSide(std::vector<VoxCavityVertex>& out, int dir, int x, int y,
              int z0, int z1, std::uint32_t col)
{
	const float zf = static_cast<float>(z0);
	const float zf1 = static_cast<float>(z1);
	VoxCavityVertex a, b, c, d;
	if (dir == 0) {
		const float xe = static_cast<float>(x + 1) + kSideEps;
		a = {xe, static_cast<float>(y), zf, col};
		b = {xe, static_cast<float>(y + 1), zf, col};
		c = {xe, static_cast<float>(y + 1), zf1, col};
		d = {xe, static_cast<float>(y), zf1, col};
	} else if (dir == 1) {
		const float xw = static_cast<float>(x) - kSideEps;
		a = {xw, static_cast<float>(y), zf, col};
		b = {xw, static_cast<float>(y + 1), zf, col};
		c = {xw, static_cast<float>(y + 1), zf1, col};
		d = {xw, static_cast<float>(y), zf1, col};
	} else if (dir == 2) {
		const float ye = static_cast<float>(y + 1) + kSideEps;
		a = {static_cast<float>(x), ye, zf, col};
		b = {static_cast<float>(x + 1), ye, zf, col};
		c = {static_cast<float>(x + 1), ye, zf1, col};
		d = {static_cast<float>(x), ye, zf1, col};
	} else {
		const float ys = static_cast<float>(y) - kSideEps;
		a = {static_cast<float>(x), ys, zf, col};
		b = {static_cast<float>(x + 1), ys, zf, col};
		c = {static_cast<float>(x + 1), ys, zf1, col};
		d = {static_cast<float>(x), ys, zf1, col};
	}
	quad(out, a, b, c, d);
}

/// Boundary plane between this column and the +x/+y neighbor: the runs
/// where EXACTLY ONE side is solid at that voxel (a boolean cut shows
/// the solid material's faces bordering the void, whichever column owns
/// the solid). Event-driven over the two color-run lists — O(runs), not
/// O(column height). Colors come out SHADED with the owner column's
/// light — merging coplanar runs compares shaded values, so different
/// light never merges. Returns false when the column has no dam (no
/// walls possible).
bool sideRunsFor(vrtMap& m, int x, int y, bool dirY,
                 std::vector<SideRun>& out)
{
	out.clear();
	const int mapW = static_cast<int>(m.H_SIZE);
	const int off = y * mapW + x;
	const int height = m.VxDBuf[off];
	if (height <= 0) {
		return false;
	}
	const int noff =
	    dirY ? (m.YCYCL(y + 1) * mapW + x) : (y * mapW + m.XCYCL(x + 1));
	const int nh =
		m.VxDBuf[noff] != 0 ? m.VxDBuf[noff] : m.VxGBuf[noff];
	const unsigned light = m.RnrBuf[off] & 0x7F;
	RangeCopy myCopy;
	copyRanges(m, static_cast<std::uint32_t>(off), myCopy);
	RangeCopy nCopy;
	copyRanges(m, static_cast<std::uint32_t>(noff), nCopy);
	std::vector<SolidRun> mine;
	mine.reserve(8);
	runsOfRanges(myCopy.data, myCopy.count, height, m.defaultColumnRgb(off),
	             mine);
	std::vector<SolidRun> neighbor;
	neighbor.reserve(8);
	runsOfRanges(nCopy.data, nCopy.count, nh, m.defaultColumnRgb(noff),
	             neighbor);
	// События — все границы ранов обеих колонок + walkMax.
	const int walkMax = height > nh ? height : nh;
	std::vector<int> bounds;
	bounds.reserve(2 + 2 * (mine.size() + neighbor.size()) + 2);
	bounds.push_back(0);
	bounds.push_back(walkMax);
	for (const SolidRun& r : mine) {
		bounds.push_back(r.z0);
		if (r.z1 + 1 < walkMax) bounds.push_back(r.z1 + 1);
	}
	for (const SolidRun& r : neighbor) {
		bounds.push_back(r.z0);
		if (r.z1 + 1 < walkMax) bounds.push_back(r.z1 + 1);
	}
	std::sort(bounds.begin(), bounds.end());
	bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
	auto solidAt = [](const std::vector<SolidRun>& runs, int z,
	                  std::uint32_t* col) {
		for (const SolidRun& r : runs) {
			if (z < r.z0) break;
			if (z <= r.z1) {
				*col = r.color;
				return true;
			}
		}
		return false;
	};
	for (size_t k = 0; k + 1 < bounds.size(); ++k) {
		const int z0 = bounds[k];
		const int z1 = bounds[k + 1];
		if (z0 >= walkMax || z0 >= z1) {
			continue;
		}
		std::uint32_t colA = 0;
		std::uint32_t colB = 0;
		const bool aSolid = solidAt(mine, z0, &colA);
		const bool bSolid = solidAt(neighbor, z0, &colB);
		if (aSolid == bSolid) {
			continue; // void==void или solid==solid — грань не нужна
		}
		out.push_back(
		    {z0, z1, shade(aSolid ? colA : colB, kSideShade, light)});
	}
	return true;
}

namespace {

// Merged wall walker core: iterates a sorted list of candidate cell
// offsets on one line (row y for dirY, column x otherwise), computes
// each cell's side runs and coalesces runs of CONSECUTIVE cells with
// identical run lists (same spans, same shaded colors) into ONE long
// quad. A cell without walls (or a gap between candidates) breaks the
// run. Это схлопывает стены полостей с ~100 квадов на колонку до
// единиц квадов на серию — меш оверлея падает на порядки (иначе
// ~700k вершин рисуются КАЖДЫЙ КАДР, см. voxperf.log RawMeshObj::Draw
// 20+ ms). Кандидаты — кавити-колонки ректа И их 4-соседи: владельцем
// стены может быть и «просто solid» клетка (обрыв к полости), в col_
// её нет, поэтому список передаётся явно.
template <bool dirY>
void appendMergedWalls(vrtMap& m, int line, const std::uint32_t* offs,
                       size_t n, std::vector<VoxCavityVertex>& out)
{
	const int mapW = static_cast<int>(m.H_SIZE);
	std::vector<SideRun> accRuns;
	std::vector<SideRun> cur;
	bool accActive = false;
	int accA = 0;
	int accB = 0;
	auto flush = [&]() {
		if (!accActive) {
			return;
		}
		for (const SideRun& r : accRuns) {
			const float zf = static_cast<float>(r.z0);
			const float zf1 = static_cast<float>(r.z1);
			VoxCavityVertex a, b, c, d;
			if (dirY) {
				const float ye =
				    static_cast<float>(line + 1) + kSideEps;
				a = {static_cast<float>(accA), ye, zf, r.color};
				b = {static_cast<float>(accB), ye, zf, r.color};
				c = {static_cast<float>(accB), ye, zf1, r.color};
				d = {static_cast<float>(accA), ye, zf1, r.color};
			} else {
				const float xe =
				    static_cast<float>(line + 1) + kSideEps;
				a = {xe, static_cast<float>(accA), zf, r.color};
				b = {xe, static_cast<float>(accB), zf, r.color};
				c = {xe, static_cast<float>(accB), zf1, r.color};
				d = {xe, static_cast<float>(accA), zf1, r.color};
			}
			quad(out, a, b, c, d);
		}
		accActive = false;
	};
	int prev = -10;
	for (size_t i = 0; i < n; ++i) {
		const std::uint32_t off = offs[i];
		const int t = dirY ? static_cast<int>(off % mapW)
		                   : static_cast<int>(off / mapW);
		const int x = dirY ? t : line;
		const int y = dirY ? line : t;
		if (!sideRunsFor(m, x, y, dirY, cur)) {
			flush(); // дамбы нет — стен нет, серия рвётся
			prev = -10;
			continue;
		}
		if (accActive && t == prev + 1 && cur == accRuns) {
			accB = t + 1;
			prev = t;
			continue;
		}
		flush();
		if (!cur.empty()) {
			accActive = true;
			accA = t;
			accB = t + 1;
			accRuns = cur;
		}
		prev = t;
	}
	flush();
}

} // namespace

void voxCavityAppendRowWallY(vrtMap& m, int y, const std::uint32_t* offs,
                             size_t n, std::vector<VoxCavityVertex>& out)
{
	appendMergedWalls<true>(m, y, offs, n, out);
}

void voxCavityAppendColWallX(vrtMap& m, int x, const std::uint32_t* offs,
                             size_t n, std::vector<VoxCavityVertex>& out)
{
	appendMergedWalls<false>(m, x, offs, n, out);
}

void voxCavityAppendRowCaps(vrtMap& m, int y, int x0, int x1,
                            std::vector<VoxCavityVertex>& out)
{
	const int mapW = static_cast<int>(m.H_SIZE);
	const std::uint32_t rowBeg =
	    static_cast<std::uint32_t>(y) * mapW + static_cast<std::uint32_t>(x0);
	const std::uint32_t rowEnd =
	    static_cast<std::uint32_t>(y) * mapW + static_cast<std::uint32_t>(x1);
	const auto range = m.voxLayers.columnIndexRange(rowBeg, rowEnd);
	int runStart = -1;
	int runEndXCached = -1;
	int runH = -1;
	std::uint32_t runRgb = 0;
	auto flush = [&](int runEndX) {
		if (runStart < 0) {
			return;
		}
		const int off = y * mapW + runStart;
		const unsigned light = m.RnrBuf[off] & 0x7F;
		const std::uint32_t col = shade(runRgb, kTopShade, light);
		const float z = static_cast<float>(runH);
		VoxCavityVertex a, b, c, d;
		a = {static_cast<float>(runStart), static_cast<float>(y), z, col};
		b = {static_cast<float>(runEndX + 1), static_cast<float>(y), z, col};
		c = {static_cast<float>(runEndX + 1), static_cast<float>(y + 1), z, col};
		d = {static_cast<float>(runStart), static_cast<float>(y + 1), z, col};
		quad(out, a, b, c, d);
		runStart = -1;
	};
	for (int i = range.first; i < range.second; ++i) {
		const std::uint32_t off = m.voxLayers.columnOffset(i);
		const int cx = static_cast<int>(off % mapW);
		if (cx < x0 || cx > x1) {
			continue;
		}
		const int h = m.VxDBuf[off];
		if (h <= 0) {
			flush(cx - 1);
			continue;
		}
		// Only REAL cavities get a cap. A column with mere color layers
		// (no transparent ranges) renders its top through the heightfield
		// at the same z — emitting a cap there z-fights the surface into
		// striped circles in the exact shape of the depth paint. Likewise
		// a column whose display height already equals the top.
		const int capDisp = m.cavityDisplayHeight(off);
		if (capDisp < 0 || capDisp >= h) {
			flush(cx - 1);
			continue;
		}
		// Top voxel color: the solid layer at h-1, else the default.
		const std::uint32_t rgb = [&]() {
			std::uint32_t rgb2 = 0;
			uint8_t flags = 0;
			if (m.voxLayers.lookup(off, h - 1, &rgb2, &flags) &&
			    !(flags & VOX_LAYER_TRANSPARENT)) {
				return rgb2;
			}
			return m.defaultColumnRgb(static_cast<int>(off));
		}();
		if (runStart >= 0 && cx == runEndXCached + 1 && h == runH &&
		    rgb == runRgb) {
			runEndXCached = cx;
			continue;
		}
		flush(runEndXCached);
		runStart = cx;
		runEndXCached = cx;
		runH = h;
		runRgb = rgb;
	}
	flush(runEndXCached);
}

void voxCavityAppendColumn(vrtMap& m, int x, int y,
                           std::vector<VoxCavityVertex>& out, bool emitSides)
{
	const int mapW = static_cast<int>(m.H_SIZE);
	const int off = y * mapW + x;
	const int height = m.VxDBuf[off];
	if (height <= 0) {
		return; // no dam — transparent ranges live in the dam span only
	}

	// Снимаем диапазоны своей колонки и соседей (+x/+y) под коротким
	// локом и дальше работаем с копиями: лок НЕ держим на весь проход
	// мешера (иначе логический поток игры, правящий слои при
	// терраформинге, встаёт на время перестройки — см. voxperf.log).
	RangeCopy myCopy;
	copyRanges(m, static_cast<std::uint32_t>(off), myCopy);
	bool hasTransparent = false;
	for (int i = 0; i < myCopy.count; ++i) {
		if (myCopy.data[i].flags & VOX_LAYER_TRANSPARENT) {
			hasTransparent = true;
			break;
		}
	}

	std::vector<Segment> segments;
	segmentsOfRanges(myCopy.data, myCopy.count, height, segments);
	if (!hasTransparent) {
		// No transparent ranges: a SOLID column — it gets a chunk when it
		// borders a void (the overlay enumerates cavity neighbors too):
		// its faces against emptied neighbors render the boolean-cut /
		// cliff walls from the solid side. One solid segment.
		segments.push_back({0, height - 1});
	}
	// The heightfield renders the base surface at the display height
	// (cavityDisplayHeight) — the overlay must NOT re-emit that floor
	// (z-fighting), but MUST cap the island top: the heightfield no longer
	// spans the cavity columns at their full height.
	const int display = m.cavityDisplayHeight(off);
	const unsigned light = m.RnrBuf[off] & 0x7F;
	const std::uint32_t fallback = m.defaultColumnRgb(off);

	// The island TOP cap must cover every cavity column (it is the surface
	// you see from above). The UNDERSIDE is only visible through the side
	// gap — deep-interior columns (nothing exposed in the 4-neighborhood)
	// skip it, which keeps huge cavities from flooding the GPU: a single
	// stamp over a r=120 circle was ~3M vertices before this.
	// Undersides only where THIS column borders on emptiness (ring 0): a
	// wider ring kept ~every column "exposed" on maps with many scattered
	// cavities (undersides dominated the mesh at hundreds of thousands of
	// vertices).
	const bool exposed = columnExposed(m, x, y, height);

	auto colorAt = [&](int z) {
		for (int i = 0; i < myCopy.count; ++i) {
			if (z < myCopy.data[i].z0) break;
			if (z <= myCopy.data[i].z1 &&
			    !(myCopy.data[i].flags & VOX_LAYER_TRANSPARENT)) {
				return (static_cast<std::uint32_t>(myCopy.data[i].r) << 16) |
				       (static_cast<std::uint32_t>(myCopy.data[i].g) << 8) |
				       myCopy.data[i].b;
			}
		}
		return fallback;
	};
	auto transparentAt = [&](int z) {
		for (int i = 0; i < myCopy.count; ++i) {
			if (z < myCopy.data[i].z0) break;
			if (z <= myCopy.data[i].z1) {
				return (myCopy.data[i].flags & VOX_LAYER_TRANSPARENT) != 0;
			}
		}
		return false;
	};

	for (const Segment& s : segments) {
		// Top face: emitted for every EXCEPT the display-height floor
		// (the heightfield renders the cavity floor) — but NOT for the
		// topmost segment: the island CAP is a flat surface spanning all
		// cavity columns of the same height and is MERGED PER ROW by
		// voxCavityAppendRowCaps (a per-column cap made the mesh ~1M
		// vertices — 13 ms of upload per frame).
		const bool isDisplayFloor =
			display >= 0 && s.z1 + 1 == display;
		const bool isTopmost = s.z1 + 1 == height;
		if (!isDisplayFloor && !isTopmost) {
			const float z = static_cast<float>(s.z1 + 1);
			const std::uint32_t col = shade(
				colorAt(s.z1), kTopShade, light);
			VoxCavityVertex a, b, c, d;
			a = {static_cast<float>(x), static_cast<float>(y), z, col};
			b = {static_cast<float>(x + 1), static_cast<float>(y), z, col};
			c = {static_cast<float>(x + 1), static_cast<float>(y + 1), z, col};
			d = {static_cast<float>(x), static_cast<float>(y + 1), z, col};
			quad(out, a, b, c, d);
		}
		// Bottom face (a cavity below the slab) — only where the cavity
		// can be seen (this column or a neighbor borders on emptiness).
		if (exposed && s.z0 > 0 && transparentAt(s.z0 - 1)) {
			const float z = static_cast<float>(s.z0);
			const std::uint32_t col = shade(colorAt(s.z0), kBottomShade, light);
			VoxCavityVertex a, b, c, d;
			a = {static_cast<float>(x), static_cast<float>(y), z, col};
			b = {static_cast<float>(x + 1), static_cast<float>(y), z, col};
			c = {static_cast<float>(x + 1), static_cast<float>(y + 1), z, col};
			d = {static_cast<float>(x), static_cast<float>(y + 1), z, col};
			quad(out, a, d, c, b); // south-facing underside
		}
	}
	// Side faces on the +x/+y boundary planes — the SYMMETRIC VOXEL DIFFER
	// (event-driven over constant-color solid runs). Овнерство: каждая
	// грань рисуется один раз — восточной/северной колонкой (+x/+y walk).
	// Оверлей ПО УМОЛЧАНИЮ пропускает стены здесь и мержит их поперёк
	// колонок отдельно (voxCavityAppendRowWallY/AppendColWallX) — иначе
	// меш ~700k вершин рисуется каждый кадр.
	if (emitSides) {
		std::vector<SideRun> runs;
		for (int dir = 0; dir < 4; ++dir) {
			if (dir == 1 || dir == 3) {
				continue; // the west/south neighbor draws this plane
			}
			sideRunsFor(m, x, y, dir == 2, runs);
			for (const SideRun& r : runs) {
				emitSide(out, dir, x, y, r.z0, r.z1, r.color);
			}
		}
	}
}

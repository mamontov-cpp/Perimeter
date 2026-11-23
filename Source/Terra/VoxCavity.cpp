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
// 0.01 z-fought the cliffs into blurry streaks, 0.1 тоже дало полосы на
// скользящих углах (репорт: полосатая крышка цилиндра-острова EXTDEMO
// при далёкой камере) — 0.3 перекрывает запас точности буфера глубины.
const float kSideEps = 0.3f;
const int kMaxSmoothHeightDelta = 10;

bool canSmoothHeightPair(int a, int b)
{
	const int delta = a - b;
	return delta >= -kMaxSmoothHeightDelta && delta <= kMaxSmoothHeightDelta;
}

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
	std::shared_lock<std::shared_mutex> lock(m.voxLayers.accessMtx);
	const int idx = m.voxLayers.findColumnImpl(off);
	if (idx < 0) {
		out.count = 0;
		return;
	}
	const uint32_t b = m.voxLayers.begin_[static_cast<size_t>(idx)];
	const uint32_t e = m.voxLayers.end_[static_cast<size_t>(idx)];
	const int count = static_cast<int>(e - b);
	const VoxLayerRange* r = m.voxLayers.ranges_.data() + b;
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

// Цвет ближайшего СПЛОШНОГО окрашенного диапазона строго ниже z.
// «Безымянная» порода (зазоры между диапазонами) наследует краску под
// собой, а не палитру материала поверхности колонки: SurBuf исторически
// «рваный» (покраски, следы инструментов), и крышки/стены/нижние грани
// полостей из него рендерились полосами поверх ровного тела острова
// (полосатая крышка цилиндра EXTDEMO — слои под ней едино жёлтые). Ниже
// ничего окрашенного нет — классический fallback (DamPal[SurBuf]).
std::uint32_t nearestSolidColorBelow(const VoxLayerRange* ranges, int count,
                                     int z, std::uint32_t fallback)
{
	for (int i = count - 1; i >= 0; --i) {
		if (static_cast<int>(ranges[i].z1) >= z ||
		    (ranges[i].flags & VOX_LAYER_TRANSPARENT)) {
			continue;
		}
		return (static_cast<std::uint32_t>(ranges[i].r) << 16) |
		       (static_cast<std::uint32_t>(ranges[i].g) << 8) |
		       static_cast<std::uint32_t>(ranges[i].b);
	}
	return fallback;
}

// Constant-color solid runs of the column [0..height): the solid segments
// split at layer-color boundaries; gaps and un-painted voxels take the
// column default color. O(count) — runs, not voxels.
void runsOfRanges(const VoxLayerRange* ranges, int count, int height,
                  std::uint32_t fallback, std::vector<SolidRun>& out)
{
	// B3: буфер потока вместо локального вектора на каждый вызов.
	static thread_local std::vector<Segment> spansBuf;
	spansBuf.clear();
	std::vector<Segment>& spans = spansBuf;
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
			const std::uint32_t col =
			    nearestSolidColorBelow(ranges, count, c, fallback);
			if (!out.empty() && out.back().z1 + 1 == c &&
			    out.back().color == col) {
				out.back().z1 = e;
			} else {
				out.push_back({c, e, col});
			}
			c = e + 1;
		}
	}
}

// Затенение — ТОЖДЕСТВЕННО террейну (vrtMap::trueColor32): k = 0.05 +
// light*(1.45/127) с клампом — линейный scale давал кепкам/стенам оверлея
// цвета на ~30% темнее, чем у террейна на тех же данных (репорт: тёмный
// верхний цилиндр при ярком «полу» той же колонки). Множители граней
// (верх/сторона/низ) вкладываются в light.
std::uint32_t shade(std::uint32_t rgb, float factor, unsigned light)
{
	const unsigned eff =
	    std::min(127u, static_cast<unsigned>(light * factor));
	return 0xFF000000u |
	       vrtMap::trueColor32(0xFF000000u | rgb, eff);
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

// Текстура кепок РОВНО W×H (движок требует степени двойки — cTexture
// округляет вверх с «Bad width» иначе), клетка (x,y) — тексель (x,y), UV
// — по углам клеток: u = x/W лежит МЕЖДУ текселями, билинейная выборка
// сглаживает соседние клетки как у террейна. Стены/плиты рисуются
// отдельной нодой БЕЗ текстуры (мesh_color_tex1 с пустым слотом сэмплит
// белую emptyTexture — tex*color = color), их UV не используется.
float voxCavityCapU(const vrtMap& m, int x)
{
	return static_cast<float>(x) / static_cast<float>(m.H_SIZE);
}

float voxCavityCapV(const vrtMap& m, int y)
{
	return static_cast<float>(y) / static_cast<float>(m.V_SIZE);
}

void voxCavityWhiteUv(const vrtMap& m, float& u, float& v)
{
	u = 0.5f / static_cast<float>(m.H_SIZE);
	v = 0.5f / static_cast<float>(m.V_SIZE);
}

void voxCavityWorldUv(const vrtMap& m, float x, float y, float& u, float& v)
{
	u = x / static_cast<float>(m.H_SIZE);
	v = y / static_cast<float>(m.V_SIZE);
}

namespace {

// uv2 (мировые UV для лайтмапа террейна) из позиции вершины.
void setWorldUv(const vrtMap& m, VoxCavityVertex& vtx)
{
	voxCavityWorldUv(m, vtx.x, vtx.y, vtx.u2, vtx.v2);
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

// Continuous display surface at a heightfield grid vertex. Cavity columns use
// their upper cap; ordinary columns use the same height as TerraInterface.
int continuousSurfaceHeight(vrtMap& m, int off)
{
	const int dam = m.VxDBuf[off];
	const int display = m.cavityDisplayHeight(off);
	if (display >= 0) {
		return display < dam ? dam : display;
	}
	return dam != 0 ? dam : m.VxGBuf[off];
}

int continuousSurfaceHeight(vrtMap& m, int cx, int cy)
{
	cx = m.XCYCL(cx);
	cy = m.YCYCL(cy);
	return continuousSurfaceHeight(
	    m, cy * static_cast<int>(m.H_SIZE) + cx);
}

int capCornerHeight(vrtMap& m, int cellHeight, int cx, int cy)
{
	const int corner = continuousSurfaceHeight(m, cx, cy);
	return canSmoothHeightPair(cellHeight, corner) ? corner : cellHeight;
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
void emitSide(vrtMap& m, std::vector<VoxCavityVertex>& out, int dir, int x,
              int y, int z0, int z1, std::uint32_t col)
{
	const float zf = static_cast<float>(z0);
	const float zf1 = static_cast<float>(z1);
	float wu, wv;
	voxCavityWhiteUv(m, wu, wv);
	VoxCavityVertex a, b, c, d;
	if (dir == 0) {
		const float xe = static_cast<float>(x + 1) + kSideEps;
		a = {xe, static_cast<float>(y), zf, col, wu, wv};
		b = {xe, static_cast<float>(y + 1), zf, col, wu, wv};
		c = {xe, static_cast<float>(y + 1), zf1, col, wu, wv};
		d = {xe, static_cast<float>(y), zf1, col, wu, wv};
	} else if (dir == 1) {
		const float xw = static_cast<float>(x) - kSideEps;
		a = {xw, static_cast<float>(y), zf, col, wu, wv};
		b = {xw, static_cast<float>(y + 1), zf, col, wu, wv};
		c = {xw, static_cast<float>(y + 1), zf1, col, wu, wv};
		d = {xw, static_cast<float>(y), zf1, col, wu, wv};
	} else if (dir == 2) {
		const float ye = static_cast<float>(y + 1) + kSideEps;
		a = {static_cast<float>(x), ye, zf, col, wu, wv};
		b = {static_cast<float>(x + 1), ye, zf, col, wu, wv};
		c = {static_cast<float>(x + 1), ye, zf1, col, wu, wv};
		d = {static_cast<float>(x), ye, zf1, col, wu, wv};
	} else {
		const float ys = static_cast<float>(y) - kSideEps;
		a = {static_cast<float>(x), ys, zf, col, wu, wv};
		b = {static_cast<float>(x + 1), ys, zf, col, wu, wv};
		c = {static_cast<float>(x + 1), ys, zf1, col, wu, wv};
		d = {static_cast<float>(x), ys, zf1, col, wu, wv};
	}
	setWorldUv(m, a);
	setWorldUv(m, b);
	setWorldUv(m, c);
	setWorldUv(m, d);
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
	RangeCopy myCopy;
	copyRanges(m, static_cast<std::uint32_t>(off), myCopy);
	RangeCopy nCopy;
	copyRanges(m, static_cast<std::uint32_t>(noff), nCopy);
	// B3: буферы потока, не аллокации на каждую клетку (вокер зовёт
	// sideRunsFor на каждого кандидата — десятки тысяч раз на drain).
	static thread_local std::vector<SolidRun> mineBuf;
	static thread_local std::vector<SolidRun> neighborBuf;
	mineBuf.clear();
	neighborBuf.clear();
	std::vector<SolidRun>& mine = mineBuf;
	std::vector<SolidRun>& neighbor = neighborBuf;
	runsOfRanges(myCopy.data, myCopy.count, height, m.defaultColumnRgb(off),
	             mine);
	runsOfRanges(nCopy.data, nCopy.count, nh, m.defaultColumnRgb(noff),
	             neighbor);
	const int walkMax = height > nh ? height : nh;
	size_t ai = 0;
	size_t bi = 0;
	int z = 0;
	while (z < walkMax && (ai < mine.size() || bi < neighbor.size())) {
		const bool aSolid = ai < mine.size() && mine[ai].z0 <= z &&
		                    z <= mine[ai].z1;
		const bool bSolid = bi < neighbor.size() && neighbor[bi].z0 <= z &&
		                    z <= neighbor[bi].z1;
		int z1 = walkMax;
		if (ai < mine.size()) {
			z1 = std::min(z1, aSolid ? mine[ai].z1 + 1 : mine[ai].z0);
		}
		if (bi < neighbor.size()) {
			z1 = std::min(z1, bSolid ? neighbor[bi].z1 + 1 : neighbor[bi].z0);
		}
		if (aSolid != bSolid && z < z1) {
			out.push_back({z, z1, aSolid ? mine[ai].color : neighbor[bi].color});
		}
		if (aSolid && z1 == mine[ai].z1 + 1) {
			++ai;
		}
		if (bSolid && z1 == neighbor[bi].z1 + 1) {
			++bi;
		}
		z = z1;
	}
	return true;
}

namespace {

// Merged wall walker core: iterates a sorted list of candidate cell
// offsets on one line (row y for dirY, column x otherwise), computes
// each cell's side runs and coalesces runs of CONSECUTIVE cells with
// identical run lists (same spans, same unshaded colors) and light into
// ONE long quad. Shared corner colors smooth material and light changes.
// A cell without walls (or a gap between candidates) breaks the run.
// Это схлопывает стены полостей с ~100 квадов на колонку до
// единиц квадов на серию — меш оверлея падает на порядки (иначе
// ~700k вершин рисуются КАЖДЫЙ КАДР, см. voxperf.log RawMeshObj::Draw
// 20+ ms). Кандидаты — кавити-колонки ректа И их 4-соседи: владельцем
// стены может быть и «просто solid» клетка (обрыв к полости), в col_
// её нет, поэтому список передаётся явно.
// Свет колонки — в условии мержа наравне с ранами: рельефное затенение
// краевой клетки (склон у края плато) иначе красит ВСЮ серию своим
// светом — полосы на стенах/крышках (тот же дефект, что у кепок).
template <bool dirY>
void appendMergedWalls(vrtMap& m, int line, const std::uint32_t* offs,
                       size_t n, std::vector<VoxCavityVertex>& out)
{
	const int mapW = static_cast<int>(m.H_SIZE);
	const int lineSize =
	    dirY ? mapW : static_cast<int>(m.V_SIZE);
	struct SampledWall
	{
		int along = 0;
		unsigned light = 0;
		int physicalTop = 0;
		int smoothTop = 0;
		std::vector<SideRun> runs;
	};
	std::vector<int> sampleAlong;
	sampleAlong.reserve(n * 3);
	for (size_t i = 0; i < n; ++i) {
		const int t = dirY ? static_cast<int>(offs[i] % mapW)
		                   : static_cast<int>(offs[i] / mapW);
		for (int d = -1; d <= 1; ++d) {
			if (t + d >= 0 && t + d < lineSize) {
				sampleAlong.push_back(t + d);
			}
		}
	}
	std::sort(sampleAlong.begin(), sampleAlong.end());
	sampleAlong.erase(std::unique(sampleAlong.begin(), sampleAlong.end()),
	                  sampleAlong.end());
	std::vector<SampledWall> samples(sampleAlong.size());
	for (size_t i = 0; i < sampleAlong.size(); ++i) {
		SampledWall& sample = samples[i];
		sample.along = sampleAlong[i];
		const int x = dirY ? sample.along : line;
		const int y = dirY ? line : sample.along;
		const int off = y * mapW + x;
		sample.light = m.RnrBuf[off] & 0x7F;
		const int nx = dirY ? x : m.XCYCL(x + 1);
		const int ny = dirY ? m.YCYCL(y + 1) : y;
		const int noff = ny * mapW + nx;
		const int nh = m.VxDBuf[noff] != 0 ? m.VxDBuf[noff] : m.VxGBuf[noff];
		sample.physicalTop = std::max<int>(m.VxDBuf[off], nh);
		const int surface = continuousSurfaceHeight(m, off);
		const int neighborSurface = continuousSurfaceHeight(m, noff);
		sample.smoothTop = canSmoothHeightPair(surface, neighborSurface)
		    ? std::min(surface, neighborSurface)
		    : sample.physicalTop;
		sideRunsFor(m, x, y, dirY, sample.runs);
	}
	auto sampledAt = [&samples](int along) -> const SampledWall* {
		const auto it = std::lower_bound(
		    samples.begin(), samples.end(), along,
		    [](const SampledWall& sample, int value) {
			    return sample.along < value;
		    });
		return it != samples.end() && it->along == along ? &*it : nullptr;
	};
	auto sampleColor = [&](int along, int z, std::uint32_t* color) {
		const SampledWall* sample = sampledAt(along);
		if (!sample) {
			return false;
		}
		for (const SideRun& run : sample->runs) {
			if (z < run.z0) {
				break;
			}
			if (z < run.z1) {
				*color = shade(run.color, kSideShade, sample->light);
				return true;
			}
		}
		return false;
	};
	auto cornerColor = [&](int along, int z, std::uint32_t fallback) {
		std::uint32_t sums[4] = {0, 0, 0, 0};
		int count = 0;
		for (int da = -1; da <= 0; ++da) {
			for (int dz = -1; dz <= 0; ++dz) {
				std::uint32_t color = 0;
				if (!sampleColor(along + da, z + dz, &color)) {
					continue;
				}
				++count;
				for (int shift = 0; shift < 32; shift += 8) {
					sums[shift / 8] += (color >> shift) & 0xFFu;
				}
			}
		}
		if (count == 0) {
			return fallback;
		}
		std::uint32_t color = 0;
		for (int shift = 0; shift < 32; shift += 8) {
			color |= (sums[shift / 8] / static_cast<unsigned>(count)) << shift;
		}
		return color;
	};

	std::vector<SideRun> accRuns;
	bool accActive = false;
	int accA = 0;
	int accB = 0;
	unsigned accLight = 0;
	int accSmoothTop = 0;
	bool accClipTop = false;
	float whiteU = 0.f;
	float whiteV = 0.f;
	voxCavityWhiteUv(m, whiteU, whiteV);
	auto flush = [&]() {
		if (!accActive) {
			return;
		}
		for (const SideRun& run : accRuns) {
			const std::uint32_t fallback =
			    shade(run.color, kSideShade, accLight);
			const std::uint32_t c00 =
			    cornerColor(accA, run.z0, fallback);
			const std::uint32_t c10 =
			    cornerColor(accB, run.z0, fallback);
			const std::uint32_t c11 =
			    cornerColor(accB, run.z1, fallback);
			const std::uint32_t c01 =
			    cornerColor(accA, run.z1, fallback);
			const float z0 = static_cast<float>(run.z0);
			const float z1 = static_cast<float>(run.z1);
			float z1A = z1, z1B = z1;
			const SampledWall* first = sampledAt(accA);
			const SampledWall* last = sampledAt(accB);
			if (accClipTop && first && run.z1 == first->physicalTop) {
				z1A = std::max(z0, std::min(z1, static_cast<float>(first->smoothTop)));
				if (last) {
					z1B = std::max(z0, std::min(z1, static_cast<float>(last->smoothTop)));
				}
			}
			if (z1A <= z0 && z1B <= z0) {
				continue;
			}
			VoxCavityVertex a, b, c, d;
			if (dirY) {
				const float plane =
				    static_cast<float>(line + 1) + kSideEps;
				a = {static_cast<float>(accA), plane, z0, c00, whiteU, whiteV};
				b = {static_cast<float>(accB), plane, z0, c10, whiteU, whiteV};
				c = {static_cast<float>(accB), plane, z1B, c11, whiteU, whiteV};
				d = {static_cast<float>(accA), plane, z1A, c01, whiteU, whiteV};
			} else {
				const float plane =
				    static_cast<float>(line + 1) + kSideEps;
				a = {plane, static_cast<float>(accA), z0, c00, whiteU, whiteV};
				b = {plane, static_cast<float>(accB), z0, c10, whiteU, whiteV};
				c = {plane, static_cast<float>(accB), z1B, c11, whiteU, whiteV};
				d = {plane, static_cast<float>(accA), z1A, c01, whiteU, whiteV};
			}
			setWorldUv(m, a);
			setWorldUv(m, b);
			setWorldUv(m, c);
			setWorldUv(m, d);
			quad(out, a, b, c, d);
		}
		accActive = false;
	};
	int prev = -10;
	for (size_t i = 0; i < n; ++i) {
		const std::uint32_t off = offs[i];
		const int t = dirY ? static_cast<int>(off % mapW)
		                   : static_cast<int>(off / mapW);
		const SampledWall* sample = sampledAt(t);
		if (!sample || sample->runs.empty()) {
			flush();
			prev = t;
			continue;
		}
		bool smoothCompatible = true;
		if (accActive && accClipTop) {
			const SampledWall* next = sampledAt(t + 1);
			smoothCompatible = sample->smoothTop == accSmoothTop && next &&
			                   next->smoothTop == accSmoothTop;
		}
		if (accActive && t == prev + 1 && sample->runs == accRuns &&
		    sample->light == accLight && smoothCompatible) {
			accB = t + 1;
			prev = t;
			continue;
		}
		flush();
		accActive = true;
		accA = t;
		accB = t + 1;
		accLight = sample->light;
		accSmoothTop = sample->smoothTop;
		accClipTop = !sample->runs.empty() &&
		             sample->runs.back().z1 == sample->physicalTop &&
		             sample->smoothTop < sample->physicalTop;
		accRuns = sample->runs;
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
                            std::vector<VoxCavityVertex>& out,
                            std::vector<std::uint32_t>* cellColors)
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
	int runNextH = -1;
	// Кепки рисуются БЕЛЫМИ с UV в топ-даун текстуру цветов: оверлей
	// печёт mipmapped-текстуру (тот же затенённый цвет, что раньше жил в
	// diffuse) и получает трилинейную фильтрацию как террейн. Затенённый
	// цвет дублируется в cellColors (источник пикселей той текстуры).
	const float vA = voxCavityCapV(m, y);
	const float vB = voxCavityCapV(m, y + 1);
	auto flush = [&](int runEndX) {
		if (runStart < 0) {
			return;
		}
		const float zA = static_cast<float>(runH);
		const float zB = static_cast<float>(
		    capCornerHeight(m, runH, runEndX + 1, y));
		const float zC = static_cast<float>(
		    capCornerHeight(m, runH, runEndX + 1, y + 1));
		const float zD = static_cast<float>(runNextH);
		const float uA = voxCavityCapU(m, runStart);
		const float uB = voxCavityCapU(m, runEndX + 1);
		VoxCavityVertex a, b, c, d;
		a = {static_cast<float>(runStart), static_cast<float>(y), zA,
		     0xFFFFFFFFu, uA, vA};
		b = {static_cast<float>(runEndX + 1), static_cast<float>(y), zB,
		     0xFFFFFFFFu, uB, vA};
		c = {static_cast<float>(runEndX + 1), static_cast<float>(y + 1), zC,
		     0xFFFFFFFFu, uB, vB};
		d = {static_cast<float>(runStart), static_cast<float>(y + 1), zD,
		     0xFFFFFFFFu, uA, vB};
		setWorldUv(m, a);
		setWorldUv(m, b);
		setWorldUv(m, c);
		setWorldUv(m, d);
		quad(out, a, b, c, d);
		runStart = -1;
	};
	if (cellColors) {
		cellColors->assign(static_cast<size_t>(x1 - x0 + 1), 0u);
	}
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
		// B5: display height и цвет верха берём под одним локом по уже
		// известному индексу колонки — без повторных findColumn в
		// cavityDisplayHeight/lookup.
		int capDisp = -1;
		std::uint32_t rgb2 = 0;
		std::uint8_t flags = 0;
		std::uint32_t capInherit = 0;
		bool topFound = false;
		m.voxLayers.capInfo(i, h, &capDisp, &rgb2, &flags, &capInherit,
		                    m.defaultColumnRgb(static_cast<int>(off)),
		                    &topFound);
		if (capDisp < 0 || capDisp >= h) {
			flush(cx - 1);
			continue;
		}
		// Цвет кепки по приоритету: сплошной диапазон, накрывший верхний
		// воксель; поверхностная декаль SurColBuf (покраска «наверх» — она
		// обязана красить и крышку cavity-острова, иначе краска с топа
		// пропадает: слои её не трогают, хайтфилд рисует срез 253);
		// наследие тела под верхом; в крайнем случае — палитра поверхности
		// (capInherit с fallback'ом по умолчанию — «рваный» SurBuf больше
		// не достаётся до сюда, пока под верхом есть окрашенное тело).
		std::uint32_t rgb;
		if (topFound && !(flags & VOX_LAYER_TRANSPARENT)) {
			rgb = rgb2;
		} else if (m.SurColBuf != nullptr &&
		           (m.SurColBuf[off] & 0xFF000000u) != 0) {
			rgb = m.SurColBuf[off] & 0x00FFFFFFu;
		} else {
			rgb = capInherit;
		}
		if (cellColors) {
			const unsigned cellLight = m.RnrBuf[off] & 0x7F;
			(*cellColors)[static_cast<size_t>(cx - x0)] =
			    shade(rgb, kTopShade, cellLight);
		}
		const int nextH = capCornerHeight(m, h, cx, y + 1);
		const int rightH = capCornerHeight(m, h, cx + 1, y);
		const int rightNextH = capCornerHeight(m, h, cx + 1, y + 1);
		if (runStart >= 0 && cx == runEndXCached + 1 && h == runH &&
		    nextH == runNextH && rightH == runH && rightNextH == runNextH) {
			runEndXCached = cx;
			continue;
		}
		flush(runEndXCached);
		runStart = cx;
		runEndXCached = cx;
		runH = h;
		runNextH = nextH;
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
	if (!emitSides && !hasTransparent) {
		return;
	}

	std::vector<Segment> segments;
	segmentsOfRanges(myCopy.data, myCopy.count, height, segments);
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
		// «Безымянная» порода наследует краску тела под собой — иначе
		// грани плит внутри полости выходят полосатыми от «рваного» SurBuf.
		return nearestSolidColorBelow(myCopy.data, myCopy.count, z,
		                              fallback);
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
		// topmost segment: the island CAP is emitted per row by
		// voxCavityAppendRowCaps with interpolated corner heights (a
		// per-column cap made the mesh ~1M vertices — 13 ms of upload per
		// frame).
		const bool isDisplayFloor =
			display >= 0 && s.z1 + 1 == display;
		const bool isTopmost = s.z1 + 1 == height;
		if (!isDisplayFloor && !isTopmost) {
			const float z = static_cast<float>(s.z1 + 1);
			const std::uint32_t col = shade(
				colorAt(s.z1), kTopShade, light);
			float wu, wv;
			voxCavityWhiteUv(m, wu, wv);
			VoxCavityVertex a, b, c, d;
			a = {static_cast<float>(x), static_cast<float>(y), z, col, wu, wv};
			b = {static_cast<float>(x + 1), static_cast<float>(y), z, col, wu, wv};
			c = {static_cast<float>(x + 1), static_cast<float>(y + 1), z, col, wu, wv};
			d = {static_cast<float>(x), static_cast<float>(y + 1), z, col, wu, wv};
			setWorldUv(m, a);
			setWorldUv(m, b);
			setWorldUv(m, c);
			setWorldUv(m, d);
			quad(out, a, b, c, d);
		}
		// Bottom face (a cavity below the slab) — only where the cavity
		// can be seen (this column or a neighbor borders on emptiness).
		if (exposed && s.z0 > 0 && transparentAt(s.z0 - 1)) {
			const float z = static_cast<float>(s.z0);
			const std::uint32_t col = shade(colorAt(s.z0), kBottomShade, light);
			float wu, wv;
			voxCavityWhiteUv(m, wu, wv);
			VoxCavityVertex a, b, c, d;
			a = {static_cast<float>(x), static_cast<float>(y), z, col, wu, wv};
			b = {static_cast<float>(x + 1), static_cast<float>(y), z, col, wu, wv};
			c = {static_cast<float>(x + 1), static_cast<float>(y + 1), z, col, wu, wv};
			d = {static_cast<float>(x), static_cast<float>(y + 1), z, col, wu, wv};
			setWorldUv(m, a);
			setWorldUv(m, b);
			setWorldUv(m, c);
			setWorldUv(m, d);
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
				emitSide(m, out, dir, x, y, r.z0, r.z1,
				         shade(r.color, kSideShade, light));
			}
		}
	}
}

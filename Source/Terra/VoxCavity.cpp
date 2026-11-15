// ============================================================================
// VoxCavity.cpp — see VoxCavity.h.
// ============================================================================

#include "stdafxTr.h"

#include "VoxCavity.h"

#include "VMAP.h"

namespace {

// Face shading factors — the cavity interior must read as 3D.
const float kTopShade = 1.0f;
const float kSideShade = 0.75f;
const float kBottomShade = 0.55f;
// Side-plane epsilon (fraction of a cell) against the heightfield cliff
// triangles at plateau boundaries.
const float kSideEps = 0.01f;

struct Segment
{
	int z0;
	int z1;
};

/// Solid segments of the dam column [0..H) split by transparent ranges.
/// Returns false when the column has no transparent ranges at all.
bool solidSegments(vrtMap& m, uint32_t off, int height,
                   std::vector<Segment>& out)
{
	const int idx = m.voxLayers.findColumn(off);
	if (idx < 0) {
		return false;
	}
	int count = 0;
	const VoxLayerRange* ranges = m.voxLayers.columnRanges(idx, &count);
	std::vector<std::pair<int, int>> transparent;
	for (int i = 0; i < count; ++i) {
		if (ranges[i].flags & VOX_LAYER_TRANSPARENT) {
			const int t0 = ranges[i].z0;
			const int t1 = static_cast<int>(ranges[i].z1);
			if (t0 < height) {
				transparent.emplace_back(
					t0, t1 < height - 1 ? t1 : height - 1);
			}
		}
	}
	if (transparent.empty()) {
		return false;
	}
	int z = 0;
	for (size_t i = 0; i < transparent.size(); ++i) {
		if (transparent[i].first > z) {
			out.push_back({z, transparent[i].first - 1});
		}
		z = transparent[i].second + 1;
	}
	if (z < height) {
		out.push_back({z, height - 1});
	}
	return true;
}

/// Raw RGB of the solid voxel z: its layer color, else the column default.
std::uint32_t voxelRgb(vrtMap& m, uint32_t off, int z,
                       std::uint32_t fallback)
{
	std::uint32_t rgb = 0;
	uint8_t flags = 0;
	if (m.voxLayers.lookup(off, z, &rgb, &flags) &&
	    !(flags & VOX_LAYER_TRANSPARENT)) {
		return rgb;
	}
	return fallback;
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

/// One quad in both windings. First triangle fan (a,b,c)+(a,c,d), then the
/// reversed fan so the face is visible from both sides.
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
	out.push_back(a);
	out.push_back(c);
	out.push_back(b);
	out.push_back(a);
	out.push_back(d);
	out.push_back(c);
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

/// Linear-walk color of the voxel z (hint advances monotonically): the
/// layer range containing z, else the column fallback. Transparent ranges
/// yield the fallback too (they have no render color).
std::uint32_t voxelRgbHinted(const VoxLayerRange* ranges, int count, int* hint,
                             int z, std::uint32_t fallback)
{
	if (!ranges) {
		return fallback;
	}
	int i = *hint;
	while (i < count && ranges[i].z1 < z) {
		++i;
	}
	*hint = i;
	if (i < count && ranges[i].z0 <= z &&
	    !(ranges[i].flags & VOX_LAYER_TRANSPARENT)) {
		return (static_cast<std::uint32_t>(ranges[i].r) << 16) |
		       (static_cast<std::uint32_t>(ranges[i].g) << 8) |
		       ranges[i].b;
	}
	return fallback;
}

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

/// Island caps for one map row, merged: consecutive cavity columns in
/// [x0..x1] whose top sits at the SAME height with the SAME top-voxel color
/// become ONE quad (the cap is a flat surface — per-column quads multiplied
/// the mesh by the cavity area and dominated the whole frame budget).
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
                           std::vector<VoxCavityVertex>& out)
{
	const int mapW = static_cast<int>(m.H_SIZE);
	const int off = y * mapW + x;
	const int height = m.VxDBuf[off];
	if (height <= 0) {
		return; // no dam — transparent ranges live in the dam span only
	}
	std::vector<Segment> segments;
	if (!solidSegments(m, static_cast<uint32_t>(off), height, segments)) {
		return;
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
				voxelRgb(m, off, s.z1, fallback), kTopShade, light);
			VoxCavityVertex a, b, c, d;
			a = {static_cast<float>(x), static_cast<float>(y), z, col};
			b = {static_cast<float>(x + 1), static_cast<float>(y), z, col};
			c = {static_cast<float>(x + 1), static_cast<float>(y + 1), z, col};
			d = {static_cast<float>(x), static_cast<float>(y + 1), z, col};
			quad(out, a, b, c, d);
		}
		// Bottom face (a cavity below the slab) — only where the cavity
		// can be seen (this column or a neighbor borders on emptiness).
		if (exposed && s.z0 > 0 &&
		    m.voxLayers.isTransparent(static_cast<uint32_t>(off), s.z0 - 1)) {
			const float z = static_cast<float>(s.z0);
			const std::uint32_t col = shade(voxelRgb(m, off, s.z0, fallback),
			                                kBottomShade, light);
			VoxCavityVertex a, b, c, d;
			a = {static_cast<float>(x), static_cast<float>(y), z, col};
			b = {static_cast<float>(x + 1), static_cast<float>(y), z, col};
			c = {static_cast<float>(x + 1), static_cast<float>(y + 1), z, col};
			d = {static_cast<float>(x), static_cast<float>(y + 1), z, col};
			quad(out, a, d, c, b); // reversed winding family, both emitted
		}
		// Side faces on ALL FOUR directions, computed by SPAN SUBTRACTION:
		// each neighbor's solid spans are built once (its height minus its
		// transparent ranges), the visible runs are our segment minus those
		// spans, merged by color. Per-voxel neighbor probing (a binary
		// search per voxel per direction) made the full-map rebuild of a
		// big cavity billions of comparisons — a hang at load.
		const int npx = m.XCYCL(x + 1);
		const int nmx = m.XCYCL(x - 1);
		const int npy = m.YCYCL(y + 1);
		const int nmy = m.YCYCL(y - 1);
		const int noffE = y * mapW + npx;
		const int noffW = y * mapW + nmx;
		const int noffN = npy * mapW + x;
		const int noffS = nmy * mapW + x;
		const bool westNeighborCavity =
			m.voxLayers.columnHasTransparent(static_cast<uint32_t>(noffW));
		const bool southNeighborCavity =
			m.voxLayers.columnHasTransparent(static_cast<uint32_t>(noffS));
		const VoxLayerRange* myRanges = nullptr;
		int myRangeCount = 0;
		{
			const int myIdx = m.voxLayers.findColumn(
				static_cast<uint32_t>(off));
			if (myIdx >= 0) {
				myRanges = m.voxLayers.columnRanges(myIdx, &myRangeCount);
			}
		}
		for (int dir = 0; dir < 4; ++dir) {
			// Ownership: a boundary plane between TWO cavity columns is
			// drawn once — by the east/north column; against a non-cavity
			// neighbor (lower terrain, no overlay chunks) this column
			// draws its own west/south faces.
			if (dir == 1 && westNeighborCavity) {
				continue;
			}
			if (dir == 3 && southNeighborCavity) {
				continue;
			}
			const int noff = dir == 0 ? noffE : dir == 1 ? noffW
			                    : dir == 2 ? noffN : noffS;
			const int nh =
				m.VxDBuf[noff] != 0 ? m.VxDBuf[noff] : m.VxGBuf[noff];
			// The neighbor is EMPTY exactly in its transparent ranges and
			// above its top — those (clamped to our segment) are the only
			// heights where a side face is needed. (The previous walk
			// emitted the neighbor's SOLID stretches instead and skipped
			// the empty ones — every column emitted full side spans and
			// the mesh ballooned to millions of vertices.)
			const VoxLayerRange* ntr = nullptr;
			int ntrCount = 0;
			{
				const int nidx = m.voxLayers.findColumn(
					static_cast<uint32_t>(noff));
				if (nidx >= 0) {
					ntr = m.voxLayers.columnRanges(nidx, &ntrCount);
				}
			}
			int ci = 0; // linear color cursor into myRanges
			auto emitMerged = [&](int zBeg, int zEnd) {
				// Emit [zBeg..zEnd] split at voxel color changes.
				int z = zBeg;
				while (z <= zEnd) {
					const std::uint32_t rgb0 =
						voxelRgbHinted(myRanges, myRangeCount, &ci, z,
						               fallback);
					int ze = z;
					while (ze + 1 <= zEnd &&
					       voxelRgbHinted(myRanges, myRangeCount, &ci,
					                      ze + 1, fallback) == rgb0) {
						++ze;
					}
					emitSide(out, dir, x, y, z, ze + 1,
					         shade(rgb0, kSideShade, light));
					z = ze + 1;
				}
			};
			for (int q = 0; q < ntrCount; ++q) {
				if (!(ntr[q].flags & VOX_LAYER_TRANSPARENT)) {
					continue;
				}
				const int b = ntr[q].z0 > s.z0 ? ntr[q].z0 : s.z0;
				const int e = ntr[q].z1 < s.z1 ? ntr[q].z1 : s.z1;
				if (b <= e) {
					emitMerged(b, e);
				}
			}
			if (nh <= s.z1) {
				emitMerged(nh > s.z0 ? nh : s.z0, s.z1);
			}
		}
	}
}

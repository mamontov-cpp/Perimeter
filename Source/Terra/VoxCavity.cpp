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
// triangles at plateau boundaries. Must exceed the depth-buffer precision
// at gameplay distances (near 30 / far 1e5 gives ~2 cm at 3k units) —
// 0.01 z-fought the cliffs into blurry streaks.
const float kSideEps = 0.1f;

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
			quad(out, a, d, c, b); // south-facing underside
		}
	}
		const int npx = m.XCYCL(x + 1);
		const int npy = m.XCYCL(y + 1);
		const int noffE = y * mapW + npx;
		const int noffN = npy * mapW + x;
		// Our column's ranges (colors + the voids) for the differ.
		const VoxLayerRange* myRanges = nullptr;
		int myRangeCount = 0;
		{
			const int myIdx0 = m.voxLayers.findColumn(
				static_cast<uint32_t>(off));
			if (myIdx0 >= 0) {
				myRanges =
				    m.voxLayers.columnRanges(myIdx0, &myRangeCount);
			}
		}
		// Side faces on ALL FOUR directions — the SYMMETRIC VOXEL DIFFER:
		// the boundary plane emits wherever EXACTLY ONE side is solid
		// (a boolean cut must show the solid material's faces bordering
		// the void, whichever column owns the solid — the previous rules
		// only drew the cavity column's own faces, so a cut's interior
		// walls facing solid rock never rendered). Heights are walked once
		// per direction with run merging: no per-voxel probing, no
		// allocations. Ownership: EVERY plane is drawn once — by the
		// east/north column's +x/+y walk (which runs unconditionally; a
		// solid neighbor of a cavity is enumerated as a cavity neighbor).
		const VoxLayerRange* ctr = myRanges;
		const int ctrCount = myRangeCount;
		for (int dir = 0; dir < 4; ++dir) {
			if (dir == 1 || dir == 3) {
				continue; // the west/south neighbor draws this plane (+x/+y rule)
			}
			const int noff = dir == 0 ? noffE : noffN;
			const int nh =
				m.VxDBuf[noff] != 0 ? m.VxDBuf[noff] : m.VxGBuf[noff];
			const VoxLayerRange* ntr = nullptr;
			int ntrCount = 0;
			{
				const int nidx = m.voxLayers.findColumn(
					static_cast<uint32_t>(noff));
				if (nidx >= 0) {
					ntr = m.voxLayers.columnRanges(nidx, &ntrCount);
				}
			}
			int ci = 0;  // color cursor into myRanges
			int ni = 0;  // cursor into the neighbor's ranges
			int ti = 0;  // cursor into our transparent ranges
			int tj = 0;  // cursor into the neighbor's transparent ranges
			auto nTransparentAt = [&](int z) {
				while (tj < ntrCount &&
				       (!(ntr[tj].flags & VOX_LAYER_TRANSPARENT) ||
				        ntr[tj].z1 < z)) {
					++tj;
				}
				return tj < ntrCount && ntr[tj].z0 <= z;
			};
			auto neighborRgb = [&](int z) {
				while (ni < ntrCount && ntr[ni].z1 < z) {
					++ni;
				}
				if (ni < ntrCount && ntr[ni].z0 <= z &&
				    !(ntr[ni].flags & VOX_LAYER_TRANSPARENT)) {
					return (static_cast<std::uint32_t>(ntr[ni].r) << 16) |
					       (static_cast<std::uint32_t>(ntr[ni].g) << 8) |
					       ntr[ni].b;
				}
				return m.defaultColumnRgb(noff);
			};
			// Walk the plane up to the TALLER of the two columns: a
			// short column next to a tall one must emit the stretch of
			// wall above its own top (bounded by `height` alone, the
			// plane was walked only to the shorter side's height and
			// the band above stayed a see-through hole). Above both
			// tops the differ is void==void — nothing is emitted, so
			// no z-fighting with the heightfield cliffs.
			const int walkMax = height > nh ? height : nh;
			int z = 0;
			while (z < walkMax) {
				while (ti < ctrCount &&
				       (!(ctr[ti].flags & VOX_LAYER_TRANSPARENT) ||
				        ctr[ti].z1 < z)) {
					++ti;
				}
				const bool cSolid =
				    z < height && !(ti < ctrCount && ctr[ti].z0 <= z);
				const bool nSolid = z < nh && !nTransparentAt(z);
				if (cSolid == nSolid) {
					++z;
					continue;
				}
				const bool cSide = cSolid;
				const std::uint32_t rgb0 =
				    cSide ? voxelRgbHinted(myRanges, myRangeCount, &ci,
				                           z, fallback)
				          : neighborRgb(z);
				int ze = z;
				while (ze + 1 < walkMax) {
					const int zn = ze + 1;
					while (ti < ctrCount &&
					       (!(ctr[ti].flags & VOX_LAYER_TRANSPARENT) ||
					        ctr[ti].z1 < zn)) {
						++ti;
					}
					const bool cSolidN =
					    zn < height &&
					    !(ti < ctrCount && ctr[ti].z0 <= zn);
					const bool nSolidN =
					    zn < nh && !nTransparentAt(zn);
					if (cSolidN == nSolidN || cSolidN != cSolid) {
						break;
					}
					const std::uint32_t rgbN =
					    cSolidN ? voxelRgbHinted(myRanges, myRangeCount,
					                             &ci, zn, fallback)
					            : neighborRgb(zn);
					if (rgbN != rgb0) {
						break;
					}
					ze = zn;
				}

				emitSide(out, dir, x, y, z, ze + 1,
				         shade(rgb0, kSideShade, light));
				z = ze + 1;
			}
		}
}
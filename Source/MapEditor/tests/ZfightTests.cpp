// ============================================================================
// Cavity-overlay z-fight analysis (EXTDEMO cylinder island, editor report:
// camera-angle-dependent stripes on the island top — "silver triangles").
//
// Composes the overlay mesh for the cylinder neighbourhood exactly like
// VoxCavityOverlay::meshRectFull does (public producers: per-column chunks
// with emitSides=false, row caps, merged row/column walls) and fails on
// ANY pair of coplanar, 2D-overlapping quads — the z-fighting signature.
// Runs standalone on EXTDEMO (order 130).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BrushTool.h"
#include "EditorSession.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_  1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"
#include "VoxCavity.h"

#include "VoxCavityOverlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Quad {
    float x0, y0, x1, y1, z0, z1;
    int plane; // 0 = horizontal (z const), 1 = x const, 2 = y const
};

bool isCapQuad(const VoxCavityVertex* q)
{
    const float w = static_cast<float>(vMap.H_SIZE);
    const float h = static_cast<float>(vMap.V_SIZE);
    for (int k : {0, 1, 2, 5}) {
        if (q[k].diffuse != 0xFFFFFFFFu || q[k].u != q[k].x / w ||
            q[k].v != q[k].y / h) {
            return false;
        }
    }
    return true;
}

using CapCellCoverage =
    std::map<std::pair<int, int>, std::array<int, 4>>;

CapCellCoverage capCellCoverage(const std::vector<VoxCavityVertex>& mesh,
                                int* duplicates = nullptr)
{
    CapCellCoverage out;
    if (duplicates) *duplicates = 0;
    auto scaled = [](float z) { return static_cast<int>(std::lround(z * 256.f)); };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
        const VoxCavityVertex* q = mesh.data() + i;
        if (!isCapQuad(q)) continue;
        const float left = q[0].x, right = q[1].x;
        if (right <= left) continue;
        const int y = static_cast<int>(std::lround(q[0].y));
        const int first = static_cast<int>(std::ceil(left - 1e-4f));
        const int last = static_cast<int>(std::floor(right + 1e-4f));
        for (int x = first; x < last; ++x) {
            const float t0 = (x - left) / (right - left);
            const float t1 = (x + 1 - left) / (right - left);
            const std::array<int, 4> corners = {
                scaled(lerp(q[0].z, q[1].z, t0)),
                scaled(lerp(q[0].z, q[1].z, t1)),
                scaled(lerp(q[5].z, q[2].z, t1)),
                scaled(lerp(q[5].z, q[2].z, t0))};
            if (!out.emplace(std::make_pair(x, y), corners).second && duplicates) {
                ++*duplicates;
            }
        }
    }
    return out;
}

void composeRect(int x0, int y0, int x1, int y1,
                 std::vector<VoxCavityVertex>& out)
{
    const int mapW = vMap.H_SIZE;
    const std::uint32_t offBeg =
        static_cast<std::uint32_t>(y0) * mapW + x0;
    const std::uint32_t offEnd =
        static_cast<std::uint32_t>(y1) * mapW + x1;
    std::vector<std::uint32_t> colOffs;
    const auto colRange = vMap.voxLayers.columnIndexRange(offBeg, offEnd);
    for (int i = colRange.first; i < colRange.second; ++i) {
        const std::uint32_t off = vMap.voxLayers.columnOffset(i);
        const int cx = static_cast<int>(off % mapW);
        if (cx < x0 || cx > x1) continue;
        colOffs.push_back(off);
        const int cy = static_cast<int>(off / mapW);
        const int dxs[4] = {1, -1, 0, 0};
        const int dys[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + dxs[d], ny = cy + dys[d];
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
    // Per-column chunks (sides off — merged walls render those).
    for (const std::uint32_t off : colOffs) {
        voxCavityAppendColumn(vMap, static_cast<int>(off % mapW),
                              static_cast<int>(off / mapW), out, false);
    }
    // Row caps + merged walls (strict ranges — full coverage like
    // meshRectFull).
    std::vector<std::vector<std::uint32_t>> rowCells(y1 + 1), colCells(x1 + 1);
    for (const std::uint32_t off : colOffs) {
        const int cx = static_cast<int>(off % mapW);
        const int cy = static_cast<int>(off / mapW);
        if (cy >= y0 && cy <= y1) rowCells[cy].push_back(off);
        if (cx >= x0 && cx <= x1) colCells[cx].push_back(off);
    }
    for (int y = y0; y <= y1; ++y) {
        if (!rowCells[y].empty()) {
            voxCavityAppendRowCaps(vMap, y, 0, mapW - 1, out);
        }
        const auto& rc = rowCells[y];
        if (!rc.empty()) {
            voxCavityAppendRowWallY(vMap, y, rc.data(), rc.size(), out);
        }
    }
    for (int x = x0; x <= x1; ++x) {
        const auto& cc = colCells[x];
        if (!cc.empty()) {
            voxCavityAppendColWallX(vMap, x, cc.data(), cc.size(), out);
        }
    }
}

bool runZfightTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    session.setGameDir(ctx.gameDir.u8string());
    session.scanWorlds();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    // Real product path: full parallel rebuild of the whole world
    // (band merge included — the striped-z-fight source), then dump.
    VoxCavityOverlay overlay;
    overlay.rebuildAll();
    std::vector<VoxCavityVertex> mesh;
    overlay.debugDumpMesh(mesh);
    std::printf("[ZF] composed verts: %zu\n", mesh.size());
    // Band-count determinism: the merged mesh must be identical for any
    // band count (a band-merge bug would be core-count dependent).
    {
        std::vector<VoxCavityVertex> ref, other;
        VoxCavityOverlay o1;
        o1.rebuildAll(1);
        o1.debugDumpMesh(ref);
        auto hashOf = [](const std::vector<VoxCavityVertex>& v) {
            std::uint64_t h = 1469598103934665603ULL;
            for (const VoxCavityVertex& q : v) {
                auto mix = [&h](std::uint64_t x) { h ^= x; h *= 1099511628211ULL; };
                mix(std::uint64_t(q.x * 8.f + 0.5f));
                mix(std::uint64_t(q.y * 8.f + 0.5f));
                mix(std::uint64_t(q.z * 8.f + 0.5f));
                mix(q.diffuse);
            }
            return h;
        };
        for (int bands : {3, 7, 16}) {
            VoxCavityOverlay ob;
            ob.rebuildAll(bands);
            other.clear();
            ob.debugDumpMesh(other);
            const std::uint64_t a = hashOf(ref), b = hashOf(other);
            std::printf("[ZF] bands=%d hash %s (verts %zu vs %zu); ", bands,
                        a == b ? "MATCH" : "DIFFER", ref.size(), other.size());
            if (a != b) {
                // Multiset diff at quad granularity: print quads that the
                // banded mesh has MORE of than the single-band reference.
                std::map<std::string, int> cnt;
                for (size_t i = 0; i + 6 <= other.size(); i += 6) {
                    std::string key;
                    for (int k = 0; k < 6; ++k) {
                        const VoxCavityVertex& q = other[i + k];
                        char buf[96];
                        snprintf(buf, sizeof(buf), "%.0f,%.0f,%.0f,%X;",
                                 q.x * 4.f, q.y * 4.f, q.z * 4.f, q.diffuse);
                        key += buf;
                    }
                    cnt[key]++;
                }
                int extra = 0;
                for (size_t i = 0; i + 6 <= ref.size(); i += 6) {
                    std::string key;
                    for (int k = 0; k < 6; ++k) {
                        const VoxCavityVertex& q = ref[i + k];
                        char buf[96];
                        snprintf(buf, sizeof(buf), "%.0f,%.0f,%.0f,%X;",
                                 q.x * 4.f, q.y * 4.f, q.z * 4.f, q.diffuse);
                        key += buf;
                    }
                    if (cnt[key] > 0) --cnt[key];
                }
                for (auto& kv : cnt) {
                    if (kv.second > 0 && extra < 4) {
                        std::printf("[ZF] extra x%d: %.80s; ", kv.second,
                                    kv.first.c_str());
                        ++extra;
                    }
                }
            }
            TEST_REQUIRE(a == b, "rebuildAll mesh differs with bands=%d",
                         bands);
        }
    }

    std::vector<Quad> quads;
    for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
        const VoxCavityVertex& a = mesh[i];
        float x0 = a.x, x1 = a.x, y0 = a.y, y1 = a.y, z0 = a.z, z1 = a.z;
        for (int k = 1; k < 6; ++k) {
            const VoxCavityVertex& v = mesh[i + k];
            x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
            y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
            z0 = std::min(z0, v.z); z1 = std::max(z1, v.z);
        }
        Quad q{x0, y0, x1, y1, z0, z1, -1};
        if (z1 - z0 < 0.01f) q.plane = 0;
        else if (x1 - x0 < 0.01f) q.plane = 1;
        else if (y1 - y0 < 0.01f) q.plane = 2;
        else continue;
        quads.push_back(q);
    }
    std::printf("[ZF] quads: %zu\n", quads.size());

    // Bucket by (plane, constant coordinate) — z-fights only happen
    // between coplanar quads; whole-mesh scan (band boundaries anywhere).
    std::map<std::pair<int, int>, std::vector<int>> buckets;
    for (size_t i = 0; i < quads.size(); ++i) {
        const float c = quads[i].plane == 0
                            ? (quads[i].z0 + quads[i].z1) * 0.5f
                            : quads[i].plane == 1
                                  ? (quads[i].x0 + quads[i].x1) * 0.5f
                                  : (quads[i].y0 + quads[i].y1) * 0.5f;
        buckets[{quads[i].plane, static_cast<int>(c * 4.0f + 0.5f)}]
            .push_back(static_cast<int>(i));
    }
    size_t fights = 0;
    for (auto& kv : buckets) {
        const std::vector<int>& idx = kv.second;
        for (size_t a = 0; a < idx.size(); ++a) {
            for (size_t b = a + 1; b < idx.size(); ++b) {
                const Quad& qa = quads[idx[a]];
                const Quad& qb = quads[idx[b]];
                const float ox = std::min(qa.x1, qb.x1) -
                                 std::max(qa.x0, qb.x0);
                const float oy = std::min(qa.y1, qb.y1) -
                                 std::max(qa.y0, qb.y0);
                const float oz = std::min(qa.z1, qb.z1) -
                                 std::max(qa.z0, qb.z0);
                if (ox > 0.01f && oy > 0.01f && oz > 0.01f) {
                    ++fights;
                    if (fights <= 12) {
                        std::printf("[ZF] fight p%d "
                            "a(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) vs "
                            "b(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                            qa.plane, qa.x0, qa.y0, qa.z0, qa.x1, qa.y1, qa.z1,
                            qb.x0, qb.y0, qb.z0, qb.x1, qb.y1, qb.z1);
                    }
                }
            }
        }
    }
    std::printf("[ZF] coplanar overlaps: %zu\n", fights);
    TEST_REQUIRE(fights == 0, "coplanar overlapping overlay quads: %zu",
                 fights);
    // FULL cap coverage: EVERY cell of the 500-plateau (VxD==500) must
    // have exactly one cap quad at z=500 — a fresh-load hole anywhere on
    // the plateau shows the striped heightfield floor beneath.
    {
        int multi = 0;
        const CapCellCoverage cover = capCellCoverage(mesh, &multi);
        int missing=0, plateau=0;
        int missPrint = 0;
        for (int y=2278;y<2518;++y)
            for (int x=2050;x<2290;++x) {
                const int off=y*vMap.H_SIZE+x;
                if (vMap.VxDBuf[off]!=500) continue;
                ++plateau;
                if (cover.find({x,y}) == cover.end()) {
                    ++missing;
                    if(missPrint<8){std::printf("[ZF] hole at %d,%d (disp=%d); ",x,y,vMap.cavityDisplayHeight(off));++missPrint;}
                }
            }
        std::printf("[ZF] FULL cap coverage: plateau=%d missing=%d multi=%d; ", plateau, missing, multi);
        TEST_REQUIRE(missing == 0 && multi == 0,
                     "interpolated cap coverage has %d missing and %d duplicate cells",
                     missing, multi);
        // Stripe regression: relief shading of an EDGE cell used to paint
        // the whole merged cap row (run-start light). Signature: rows of
        // the flat plateau whose cells are predominantly dark. Legitimate
        // variation (painted decals, edge rim cells) is per-cell, not
        // per-row, so require every row to be mostly lit at the plateau's
        // modal light. Hue is ignored on purpose — the gray painted arrow
        // on this plateau is data, not a defect.
        {
            std::map<int, int> lightVotes;
            for (int y=2278;y<2518;++y)
                for (int x=2050;x<2290;++x) {
                    const int off=y*vMap.H_SIZE+x;
                    if (vMap.VxDBuf[off]!=500) continue;
                    ++lightVotes[vMap.RnrBuf[off] & 0x7F];
                }
            int modalLight = -1, modalN = 0;
            for (auto& kv : lightVotes) {
                if (kv.second > modalN) { modalN = kv.second; modalLight = kv.first; }
            }
            int badRows = 0, rows = 0;
            for (int y=2278;y<2518;++y) {
                int lit = 0, capped = 0;
                for (int x=2050;x<2290;++x) {
                    const int off=y*vMap.H_SIZE+x;
                    if (vMap.VxDBuf[off]!=500) continue;
                    ++capped;
                    if ((vMap.RnrBuf[off] & 0x7F) * 10 >= modalLight * 8) ++lit;
                }
                // Rim rows of a round island are ALL cliff cells (every
                // cell's neighbor across the rim is lower) — legitimately
                // dark as a band. The stripe defect painted WIDE INTERIOR
                // rows, so only those are policed.
                if (capped < 100) continue;
                ++rows;
                if (lit * 100 < capped * 90) {
                    ++badRows;
                    if (badRows <= 5) {
                        std::printf("[ZF] dark row y=%d lit %d/%d; ", y, lit, capped);
                    }
                }
            }
            std::printf("[ZF] row light: modal=%d rows=%d bad=%d; ",
                        modalLight, rows, badRows);
            TEST_REQUIRE(modalLight > 0 && badRows == 0,
                         "striped cap rows: %d of %d rows mostly dark "
                         "(modal light %d)",
                         badRows, rows, modalLight);
        }
        // Cap color histogram over the plateau (diagnostic print) —
        // из бейка текстуры (меш несёт белые квады с UV).
        {
            std::map<std::uint32_t, int> hist;
            for (int y=2278;y<2518;++y)
                for (int x=2050;x<2290;++x) {
                    const std::uint32_t c = overlay.debugCapColor(x, y);
                    if (c != 0) hist[c]++;
                }
            std::printf("[ZF] cap colors=%zu:", hist.size());
            int shown=0;
            for (auto& kv : hist) {
                if (shown++ < 40) std::printf(" %06Xx%d", kv.first & 0xFFFFFFu, kv.second);
            }
            std::printf("; ");
        }
    }
    session.unloadWorld();
    return true;
}

// Level-stamp remesh repro: incremental rebuildRect after a round
// Level-to-500 stamp must leave the cylinder cap colors identical to a
// full rebuild (striped caps after a level op = incremental remesh bug).
bool runRemeshReproTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    session.setGameDir(ctx.gameDir.u8string());
    session.scanWorlds();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    VoxCavityOverlay overlay;
    // Цвет кепки — в бейке текстуры оверлея (меш: белые квады с UV).
    auto capColors = [&overlay]() {
        std::map<std::pair<int, int>, std::uint32_t> m;
        for (int x = 2140; x < 2220; ++x)
            for (int y = 2360; y < 2450; ++y) {
                const std::uint32_t c = overlay.debugCapColor(x, y);
                if (c != 0) m[{x, y}] = c;
            }
        return m;
    };

    overlay.rebuildAll();
    const auto base = capColors();

    // 3D repro: cone, Level base 500, Shape Height 500, Set-to-Height at
    // the cylinder centre — dump the surface along the stamp diameter.
    {
        auto dumpSurf = [](const char* tag, int cx, int cy, int r) {
            std::printf("[REM] %s y=%d x=%d..%d: ", tag, cy, cx - r, cx + r);
            for (int x = cx - r; x <= cx + r; ++x) {
                const int off = cy * vMap.H_SIZE + x;
                std::printf("%d ", vMap.SGetAlt(off) >> 5);
            }
        };
        dumpSurf("before", 2177, 2405, 8);
        MapEditor::BrushTool probeBrush(session);
        MapEditor::BrushParams pp;
        pp.shape = MapEditor::BrushShape::FilledCircle;
        pp.radius = 8;
        pp.effect = MapEditor::BrushEffect::AlignVariableH;
        pp.heightLevel = 500;
        pp.shape3d = MapEditor::Shape3DKind::Cone;
        pp.shapeHeight = 500;
        pp.mode = MapEditor::BrushMode::Dam;
        TEST_REQUIRE(probeBrush.applyStamp(2177, 2405, pp),
                     "cone stamp refused");
        dumpSurf("after ", 2177, 2405, 8);
        // Level DOWN through the bands: 1000 -> 300. The dig crosses the
        // transparent bands [253..300]/[301..400] — does the column end at
        // the target 300, or collapse/strip along the way?
        {
            const int off = 2405 * vMap.H_SIZE + 2177;
            std::printf("[REM] down-level center: surf before=%d; ",
                        vMap.SGetAlt(off) >> 5);
            MapEditor::BrushParams dp;
            dp.shape = MapEditor::BrushShape::FilledCircle;
            dp.radius = 3;
            dp.effect = MapEditor::BrushEffect::AlignVariableH;
            dp.heightLevel = 300;
            dp.mode = MapEditor::BrushMode::Dam;
            TEST_REQUIRE(probeBrush.applyStamp(2177, 2405, dp),
                         "down-level stamp refused");
            const int idx =
                vMap.voxLayers.findColumn(static_cast<std::uint32_t>(off));
            int count = 0;
            const VoxLayerRange* rr =
                idx >= 0 ? vMap.voxLayers.columnRanges(idx, &count) : nullptr;
            std::printf("after=%d layers=%d [", vMap.SGetAlt(off) >> 5,
                        count);
            for (int k = 0; rr && k < count; ++k)
                std::printf("%d-%d%s ", rr[k].z0, rr[k].z1,
                            (rr[k].flags & 1) ? "T" : "");
            std::printf("]; disp=%d; ", vMap.cavityDisplayHeight(off));
        }
    }

    MapEditor::BrushTool brush(session);
    MapEditor::BrushParams p;
    p.shape = MapEditor::BrushShape::FilledCircle;
    p.radius = 20;
    p.effect = MapEditor::BrushEffect::AlignVariableH;
    p.heightLevel = 500;
    p.mode = MapEditor::BrushMode::Dam;
    TEST_REQUIRE(brush.applyStamp(2177, 2405, p), "level stamp refused");

    overlay.debugRebuildRect(2177 - 21, 2405 - 21, 2177 + 21, 2405 + 21);
    const auto incr = capColors();
    std::vector<VoxCavityVertex> incrMesh;
    overlay.debugDumpMesh(incrMesh);

    overlay.rebuildAll();
    const auto full = capColors();
    std::vector<VoxCavityVertex> fullMesh;
    overlay.debugDumpMesh(fullMesh);

    size_t changedVsBase = 0, changedVsFull = 0, n = 0;
    for (const auto& kv : full) {
        const auto it = base.find(kv.first);
        if (it == base.end()) continue;
        ++n;
        const auto ii = incr.find(kv.first);
        const std::uint32_t iv = ii != incr.end() ? ii->second : 0;
        if (iv != it->second) ++changedVsBase;
        if (iv != kv.second) ++changedVsFull;
    }
    std::printf("[REM] cap cells=%zu changedByIncremental vs base=%zu vs full=%zu; ",
                n, changedVsBase, changedVsFull);
    TEST_REQUIRE(changedVsBase == 0 && changedVsFull == 0,
                 "incremental remesh changed cap colors: vs base %zu, vs full %zu",
                 changedVsBase, changedVsFull);

    int incrDuplicates = 0, fullDuplicates = 0;
    const CapCellCoverage incrCoverage =
        capCellCoverage(incrMesh, &incrDuplicates);
    const CapCellCoverage fullCoverage =
        capCellCoverage(fullMesh, &fullDuplicates);
    TEST_REQUIRE(incrDuplicates == 0 && fullDuplicates == 0,
                 "incremental/full cap meshes contain duplicate cells");
    if (incrCoverage != fullCoverage) {
        std::printf("[REM] cap signature sizes incr=%zu full=%zu; ",
                    incrCoverage.size(), fullCoverage.size());
        int shown = 0;
        for (const auto& kv : fullCoverage) {
            const auto it = incrCoverage.find(kv.first);
            if (it != incrCoverage.end() && it->second == kv.second) continue;
            std::printf("cell(%d,%d) full=[%d %d %d %d] ", kv.first.first,
                        kv.first.second, kv.second[0], kv.second[1],
                        kv.second[2], kv.second[3]);
            if (it == incrCoverage.end()) {
                std::printf("incr=missing; ");
            } else {
                std::printf("incr=[%d %d %d %d]; ", it->second[0],
                            it->second[1], it->second[2], it->second[3]);
            }
            if (++shown >= 6) break;
        }
    }
    TEST_REQUIRE(incrCoverage == fullCoverage,
                 "incremental cap splicing changed cell coverage or corner heights");
    session.unloadWorld();
    return true;
}

// Parallelepiped paint regression (793,3415): one shared column structure// (solid base, transparent band, solid top — the "lower" and "upper"
// parallelepipeds render from the SAME columns: base top = the cavity cut,
// upper = the overlay cap). A stamp at the TOP height must color only the
// cap; a stamp at the FLOOR (cut) height must color only the floor.
bool runParallelepipedProbeTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    session.setGameDir(ctx.gameDir.u8string());
    session.scanWorlds();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    const int px = 793, py = 3415;
    const int off = py * vMap.H_SIZE + px;
    TEST_REQUIRE(vMap.VxDBuf[off] == 500 && vMap.cavityDisplayHeight(off) == 301,
                 "the parallelepiped fixture moved: VxD=%d disp=%d",
                 vMap.VxDBuf[off], vMap.cavityDisplayHeight(off));
    // Un-painted floor renders through the palette WITH the dam row of
    // Sur2Col (Rnr bit 7 set) — the Geo row reads almost black (the
    // «dark top face» report).
    TEST_REQUIRE((vMap.getColor32(off) & 0xFFFFFFu) ==
                     vMap.Sur2Col32[vMap.SurBuf[off]][vMap.RnrBuf[off]],
                 "floor color drifted from the dam palette row: %06X vs %06X",
                 vMap.getColor32(off) & 0xFFFFFFu,
                 vMap.Sur2Col32[vMap.SurBuf[off]][vMap.RnrBuf[off]]);

    // Цвет кепки теперь в бейке текстуры оверлея (меш несёт белые квады
    // с UV — трилинейная фильтрация как у террейна).
    auto capColorAt = [&](int x, int y) {
        VoxCavityOverlay ov;
        ov.rebuildAll(1);
        return ov.debugCapColor(x, y) & 0xFFFFFFu;
    };
    auto stampRed = [&](int h) {
        MapEditor::BrushTool brush(session);
        MapEditor::BrushParams p;
        p.shape = MapEditor::BrushShape::FilledCircle;
        p.radius = 20;
        p.effect = MapEditor::BrushEffect::SetColor;
        p.colorRgb = 0xFF0000;
        p.paintHeight = h;
        p.paintDepth = 1;
        p.mode = MapEditor::BrushMode::Dam;
        TEST_REQUIRE(brush.applyStamp(px, py, p), "paint refused");
    };
    auto isRed = [](std::uint32_t c) {
        return (c & 0xFF0000u) >= 0x900000u && (c & 0x00FFFFu) <= 0x004040u;
    };

    // Case A: stamp at the top height — cap red, floor untouched.
    stampRed(500);
    TEST_REQUIRE(isRed(capColorAt(px, py)),
                 "top stamp did not reach the cap: %06X",
                 capColorAt(px, py) & 0xFFFFFFu);
    TEST_REQUIRE(!isRed(vMap.getColor32(off) & 0xFFFFFFu),
                 "top stamp leaked onto the floor: %06X",
                 vMap.getColor32(off) & 0xFFFFFFu);
    // Undo must clean BOTH surfaces: the swap restores SurColBuf by a raw
    // write — without a surVersion bump the cavity overlay never rebuilds
    // and the cap stays red while the floor repaints clean (the report).
    const std::uint64_t surBefore = vMap.surVersion;
    TEST_REQUIRE(session.undo(), "undo refused");
    TEST_REQUIRE(vMap.surVersion > surBefore,
                 "undo did not bump surVersion — the overlay gate drops "
                 "the cap rebuild");
    TEST_REQUIRE(!isRed(capColorAt(px, py)),
                 "undo left the cap red: %06X",
                 capColorAt(px, py) & 0xFFFFFFu);
    TEST_REQUIRE(!isRed(vMap.getColor32(off) & 0xFFFFFFu),
                 "undo left the floor red: %06X",
                 vMap.getColor32(off) & 0xFFFFFFu);
    session.unloadWorld();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be reloaded.");
    vMap.WorldRender();
    // Case B: stamp at the cut floor — floor red, cap untouched.
    stampRed(301);
    TEST_REQUIRE(isRed(vMap.getColor32(off) & 0xFFFFFFu),
                 "floor stamp did not reach the floor: %06X",
                 vMap.getColor32(off) & 0xFFFFFFu);
    TEST_REQUIRE(!isRed(capColorAt(px, py)),
                 "floor stamp leaked onto the cap: %06X",
                 capColorAt(px, py) & 0xFFFFFFu);
    TEST_REQUIRE(session.undo(), "undo refused");
    TEST_REQUIRE(!isRed(vMap.getColor32(off) & 0xFFFFFFu),
                 "undo left the floor red: %06X",
                 vMap.getColor32(off) & 0xFFFFFFu);
    session.unloadWorld();
    return true;
}

// Paint-at-height regression: a red SetColor stamp on the cylinder top
// must color the ISLAND CAP (z=500), wherever the paint lands:
//   h=-1 / h>=surface — surface decal (SurColBuf): the cap must honor it
//     (regression: decal paints were invisible on cavity columns);
//   h=surface-1 — solid range exactly on the top voxel: cap must show it
//     (regression: capInfo reported solid ranges with flags==0, which
//     RowCaps read as "no range" and replaced with the inherit color);
//   h inside the transparent bands — cap inherits the paint from below;
//   h=display floor — the heightfield floor shows it.
bool runPaintReproTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    session.setGameDir(ctx.gameDir.u8string());
    session.scanWorlds();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    const int off = 2405 * vMap.H_SIZE + 2177;
    // Кепка красит 0xFF0000 светом колонки ровно как shade() мешера —
    // а тот теперь тождественен vrtMap::trueColor32 (затенение террейна).
    const unsigned light = vMap.RnrBuf[off] & 0x7F;
    const std::uint32_t shadedRed =
        vrtMap::trueColor32(0xFF000000u | 0xFF0000u, light);
    // Цвет кепки теперь в бейке текстуры оверлея (меш несёт белые квады
    // с UV — трилинейная фильтрация как у террейна).
    auto capColorAt = [&](int x, int y) {
        VoxCavityOverlay ov;
        ov.rebuildAll(1);
        return ov.debugCapColor(x, y) & 0xFFFFFFu;
    };
    auto stampRed = [&](int h) {
        MapEditor::BrushTool brush(session);
        MapEditor::BrushParams p;
        p.shape = MapEditor::BrushShape::FilledCircle;
        p.radius = 20;
        p.effect = MapEditor::BrushEffect::SetColor;
        p.colorRgb = 0xFF0000;
        p.paintHeight = h;
        p.paintDepth = 1;
        p.mode = MapEditor::BrushMode::Dam;
        TEST_REQUIRE(brush.applyStamp(2177, 2405, p), "paint refused");
    };
    auto reload = [&]() {
        session.unloadWorld();
        TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                     "the EXTDEMO world could not be reloaded.");
        vMap.WorldRender();
    };

    stampRed(-1); // "top" paint — surface decal
    TEST_REQUIRE(capColorAt(2177, 2405) == shadedRed,
                 "decal paint on the top vanished from the island cap: %06X",
                 capColorAt(2177, 2405) & 0xFFFFFFu);
    reload();
    stampRed(499); // solid range exactly on the top voxel
    TEST_REQUIRE(capColorAt(2177, 2405) == shadedRed,
                 "top-voxel layer paint vanished from the island cap: %06X",
                 capColorAt(2177, 2405) & 0xFFFFFFu);
    reload();
    stampRed(253); // the display floor — heightfield territory
    const std::uint32_t floorCol = vMap.getColor32(off) & 0xFFFFFFu;
    TEST_REQUIRE((floorCol & 0xFF0000u) >= 0xC00000u &&
                     (floorCol & 0x00FFFFu) == 0,
                 "floor paint mismatch: floor %06X", floorCol);
    // Покраска ПОЛА (по срезу) — поверхность полости, а не тело острова:
    // на кепку не наследуется (маркер OVER_VOID), кепка остаётся цвета
    // тела — жёлтым.
    const unsigned light2 = vMap.RnrBuf[off] & 0x7F;
    const std::uint32_t shadedYellow =
        vrtMap::trueColor32(0xFF000000u | 0xFFFF00u, light2);
    TEST_REQUIRE(capColorAt(2177, 2405) == shadedYellow,
                 "floor paint leaked onto the island cap: %06X",
                 capColorAt(2177, 2405) & 0xFFFFFFu);
    session.unloadWorld();
    return true;
}


} // namespace

// Level-stamp wall regression: leveling ONE column at the island edge must
// not punch transparent holes through the side walls of the UN-leveled
// columns. rebuildRect regenerates wall ROWS from the rect's local
// candidates only — a stored row entry spans the whole map width, so the
// pre-fix code replaced the entire row's walls with the local few and the
// island side went see-through outside the stamp. The incremental mesh
// after the stamp must be byte-identical to a full rebuild.
bool runWallSideRegressionTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    session.setGameDir(ctx.gameDir.u8string());
    session.scanWorlds();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    auto dumpMesh = [](std::vector<VoxCavityVertex>& out) {
        VoxCavityOverlay ov;
        ov.rebuildAll(1);
        ov.debugDumpMesh(out);
    };
    std::vector<VoxCavityVertex> before;
    dumpMesh(before);

    // Level ONE column right at the disc edge ("one outer wall leveled"):
    // take the widest row of the disc and find its eastern 500-edge.
    const int ey = 2405;
    int ex = -1;
    for (int x = 2400; x > 2100; --x) {
        if (vMap.VxDBuf[ey * vMap.H_SIZE + x] == 500) {
            ex = x;
            break;
        }
    }
    TEST_REQUIRE(ex > 0, "the island edge fixture moved (no 500-column)");
    {
        MapEditor::BrushTool brush(session);
        MapEditor::BrushParams p;
        p.shape = MapEditor::BrushShape::FilledCircle;
        p.radius = 2;
        p.effect = MapEditor::BrushEffect::Align4ZP;
        p.mode = MapEditor::BrushMode::Dam;
        TEST_REQUIRE(brush.applyStamp(ex, ey, p), "level stamp refused");
    }

    VoxCavityOverlay incremental;
    incremental.rebuildAll(1);
    // Mirror drainPending's expansion (one cell of neighbor context).
    incremental.debugRebuildRect(ex - 3, ey - 3, ex + 3, ey + 3);
    std::vector<VoxCavityVertex> incr;
    incremental.debugDumpMesh(incr);

    std::vector<VoxCavityVertex> full;
    dumpMesh(full);

    std::printf("[WALL] verts before=%zu incr=%zu full=%zu; ",
                before.size(), incr.size(), full.size());
    // Coverage comparison: the incremental path may SPLIT a merged wall
    // run at the rect band boundary (two adjacent quads instead of one),
    // so compare unioned per-plane geometry instead of raw quad streams.
    // Vertex colors are checked independently below: a gradient cannot be
    // represented by one diffuse value in this geometric coverage key.
    auto coverage = [](const std::vector<VoxCavityVertex>& m) {
        // key: plane axis+coord, z range -> union of along-intervals
        std::map<std::tuple<int, int, int, int>,
                 std::vector<std::pair<int, int>>>
            cov;
        for (size_t i = 0; i + 6 <= m.size(); i += 6) {
            if (isCapQuad(m.data() + i)) continue;
            float x0 = m[i].x, x1 = m[i].x, y0 = m[i].y, y1 = m[i].y;
            float z0 = m[i].z, z1 = m[i].z;
            for (int k = 1; k < 6; ++k) {
                x0 = std::min(x0, m[i + k].x);
                x1 = std::max(x1, m[i + k].x);
                y0 = std::min(y0, m[i + k].y);
                y1 = std::max(y1, m[i + k].y);
                z0 = std::min(z0, m[i + k].z);
                z1 = std::max(z1, m[i + k].z);
            }
            int axis; // 0 = x-const plane, 1 = y-const, 2 = z-const
            float coord, lo, hi;
            if (x1 - x0 < 0.01f) {
                axis = 0; coord = x0; lo = y0; hi = y1;
            } else if (y1 - y0 < 0.01f) {
                axis = 1; coord = y0; lo = x0; hi = x1;
            } else {
                axis = 2; coord = z0; lo = x0; hi = x1;
            }
            auto& v = cov[std::make_tuple(
                axis, static_cast<int>(coord * 8.f + 0.5f),
                static_cast<int>(z0 * 2.f + 0.5f),
                static_cast<int>(z1 * 2.f + 0.5f))];
            v.push_back({static_cast<int>(lo * 8.f + 0.5f),
                         static_cast<int>(hi * 8.f + 0.5f)});
        }
        for (auto& kv : cov) {
            auto& v = kv.second;
            std::sort(v.begin(), v.end());
            std::vector<std::pair<int, int>> u;
            for (auto& iv : v) {
                if (!u.empty() && iv.first <= u.back().second) {
                    u.back().second = std::max(u.back().second, iv.second);
                } else {
                    u.push_back(iv);
                }
            }
            v.swap(u);
        }
        return cov;
    };
    const auto covIncr = coverage(incr);
    const auto covFull = coverage(full);
    if (covIncr != covFull) {
        size_t shown = 0;
        for (const auto& kv : covFull) {
            const auto it = covIncr.find(kv.first);
            if (it == covIncr.end() || it->second != kv.second) {
                if (shown++ < 4) {
                    std::printf("[WALL] coverage diff key(a%d c%d z%d-%d): full=[",
                                std::get<0>(kv.first), std::get<1>(kv.first),
                                std::get<2>(kv.first), std::get<3>(kv.first));
                    for (auto& iv : kv.second)
                        std::printf("%d-%d ", iv.first, iv.second);
                    std::printf("] incr=[");
                    const auto it2 = covIncr.find(kv.first);
                    if (it2 != covIncr.end())
                        for (auto& iv : it2->second)
                            std::printf("%d-%d ", iv.first, iv.second);
                    std::printf("]; ");
                }
            }
        }
    }
    if (covIncr != covFull) {
        auto dumpWallQuads = [](const char* tag,
                                const std::vector<VoxCavityVertex>& mesh) {
            std::printf("[WALLQ] %s: ", tag);
            for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
                const VoxCavityVertex& a = mesh[i];
                const VoxCavityVertex& b = mesh[i + 1];
                const VoxCavityVertex& c = mesh[i + 2];
                const VoxCavityVertex& d = mesh[i + 5];
                if (std::fabs(a.x - 2291.3f) > 0.01f ||
                    std::fabs(b.x - a.x) > 0.01f || a.y < 2407.f ||
                    a.y > 2415.f || b.y < 2407.f || b.y > 2416.f ||
                    a.z < 390.f) {
                    continue;
                }
                std::printf("(%.2f,%.2f,%.1f)->(%.2f,%.2f,%.1f) top "
                            "(%.2f,%.2f,%.1f)/(%.2f,%.2f,%.1f); ",
                            a.x, a.y, a.z, b.x, b.y, b.z,
                            d.x, d.y, d.z, c.x, c.y, c.z);
            }
        };
        dumpWallQuads("incremental", incr);
        dumpWallQuads("full", full);
    }
    TEST_REQUIRE(covIncr == covFull,
                 "incremental rebuild after a level stamp lost wall coverage "
                 "(transparent side holes)");
    TEST_REQUIRE(capCellCoverage(incr) == capCellCoverage(full),
                 "incremental rebuild changed interpolated cap coverage");
    auto cornerColors = [](const std::vector<VoxCavityVertex>& mesh) {
        std::map<std::tuple<int, int, int>, std::uint32_t> colors;
        for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
            float x0 = mesh[i].x, x1 = mesh[i].x;
            float y0 = mesh[i].y, y1 = mesh[i].y;
            float z0 = mesh[i].z, z1 = mesh[i].z;
            for (int k = 1; k < 6; ++k) {
                x0 = std::min(x0, mesh[i + k].x);
                x1 = std::max(x1, mesh[i + k].x);
                y0 = std::min(y0, mesh[i + k].y);
                y1 = std::max(y1, mesh[i + k].y);
                z0 = std::min(z0, mesh[i + k].z);
                z1 = std::max(z1, mesh[i + k].z);
            }
            if (z1 - z0 < 0.01f ||
                (x1 - x0 >= 0.01f && y1 - y0 >= 0.01f)) {
                continue;
            }
            for (int k : {0, 1, 2, 5}) {
                const VoxCavityVertex& v = mesh[i + k];
                const auto key = std::make_tuple(
                    static_cast<int>(v.x * 8.f + 0.5f),
                    static_cast<int>(v.y * 8.f + 0.5f),
                    static_cast<int>(v.z * 8.f + 0.5f));
                const auto inserted = colors.emplace(key, v.diffuse);
                if (!inserted.second && inserted.first->second != v.diffuse) {
                    inserted.first->second = 0;
                }
            }
        }
        return colors;
    };
    const auto cornersIncr = cornerColors(incr);
    const auto cornersFull = cornerColors(full);
    size_t cornerMismatches = 0;
    for (const auto& kv : cornersFull) {
        const auto it = cornersIncr.find(kv.first);
        if (it == cornersIncr.end() || it->second != kv.second) {
            ++cornerMismatches;
        }
    }
    TEST_REQUIRE(cornerMismatches == 0,
                 "incremental wall clipping left %zu different smoothing "
                 "colors than a full rebuild", cornerMismatches);
    TEST_REQUIRE(incr != before, "the level stamp changed nothing?");
    session.unloadWorld();
    return true;
}

MAP_EDITOR_REGISTER_TEST("Cavity overlay z-fight analysis (EXTDEMO "
                         "cylinder)",
                         130, runZfightTests);
MAP_EDITOR_REGISTER_TEST("Parallelepiped top/floor paint selectivity", 134,
                         runParallelepipedProbeTests);
MAP_EDITOR_REGISTER_TEST("Top paint lands on the island cap (cylinder)",
                         133, runPaintReproTests);
MAP_EDITOR_REGISTER_TEST("Level-stamp remesh repro (cylinder cap)",
                         131, runRemeshReproTests);
// Mesh-content regression for the smoothing layout: caps must be WHITE
// quads with corner UVs into the W×H bake; merged walls carry smoothly
// interpolated vertex colors; interior slab faces keep vertex colors.
bool runUvLayoutTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    session.setGameDir(ctx.gameDir.u8string());
    session.scanWorlds();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    VoxCavityOverlay ov;
    ov.rebuildAll(1);
    std::vector<VoxCavityVertex> m;
    ov.debugDumpMesh(m);
    const float mapW = static_cast<float>(vMap.H_SIZE);

    int caps = 0, walls = 0, slabs = 0;
    for (size_t i = 0; i + 6 <= m.size(); i += 6) {
        float x0 = m[i].x, x1 = m[i].x, y0 = m[i].y, y1 = m[i].y;
        float z0 = m[i].z, z1 = m[i].z;
        for (int k = 1; k < 6; ++k) {
            x0 = std::min(x0, m[i + k].x); x1 = std::max(x1, m[i + k].x);
            y0 = std::min(y0, m[i + k].y); y1 = std::max(y1, m[i + k].y);
            z0 = std::min(z0, m[i + k].z); z1 = std::max(z1, m[i + k].z);
        }
        const std::uint32_t d = m[i].diffuse;
        if (z1 - z0 < 0.01f && z0 > 499.5f && z0 < 500.5f &&
            x0 >= 2050.f && x1 <= 2290.f && y0 >= 2278.f && y1 <= 2518.f) {
            // Cap quad on the cylinder plateau: white, UV = corner/mapW.
            ++caps;
            TEST_REQUIRE(d == 0xFFFFFFFFu, "cap quad not white: %X", d);
            const float uA = x0 / mapW, uB = x1 / mapW;
            const float vA = y0 / mapW, vB = y1 / mapW;
            bool uvOk = true;
            for (int k = 0; k < 6; ++k) {
                const VoxCavityVertex& q = m[i + k];
                const bool uOk = (q.u == uA || q.u == uB);
                const bool vOk = (q.v == vA || q.v == vB);
                const bool cornerOk =
                    (q.u == uA) == (q.x == x0) || (q.u == uB) == (q.x == x1);
                if (!uOk || !vOk || !cornerOk) uvOk = false;
            }
            TEST_REQUIRE(uvOk, "cap quad UV mismatch: u=%f v=%f", m[i].u,
                         m[i].v);
        } else if (z1 - z0 > 0.01f && (x1 - x0 < 0.01f || y1 - y0 < 0.01f) &&
                   x0 >= 2050.f && x1 <= 2290.f && y0 >= 2278.f &&
                   y1 <= 2518.f) {
            // Merged wall quad on the cylinder: interpolated vertex colors.
            ++walls;
            TEST_REQUIRE((d & 0xFF000000u) == 0xFF000000u,
                         "wall quad lost its opaque vertex color: %X", d);
        }
    }
    // Interior slab faces (chunks) keep shaded colors, not white.
    const int off = 2405 * vMap.H_SIZE + 2177;
    const int ci = vMap.voxLayers.findColumn(static_cast<std::uint32_t>(off));
    (void)ci;
    std::printf("[UVL] caps=%d walls=%d (slab faces stay colored); ",
                caps, walls);
    TEST_REQUIRE(caps > 100, "no cap quads sampled: %d", caps);
    TEST_REQUIRE(walls > 100, "no wall quads sampled: %d", walls);

    const CapCellCoverage interpolated = capCellCoverage(m);
    auto expectedHeight = [](int x, int y) {
        x = vMap.XCYCL(x);
        y = vMap.YCYCL(y);
        const int off = y * vMap.H_SIZE + x;
        const int dam = vMap.VxDBuf[off];
        const int display = vMap.cavityDisplayHeight(off);
        if (display >= 0) return display < dam ? dam : display;
        return dam != 0 ? dam : static_cast<int>(vMap.VxGBuf[off]);
    };
    int slopedCells = 0, largeCliffCells = 0, heightMismatches = 0;
    for (const auto& kv : interpolated) {
        const int x = kv.first.first, y = kv.first.second;
        if (x < 2050 || x >= 2290 || y < 2278 || y >= 2518) continue;
        const auto& actual = kv.second;
        const int cellHeight = vMap.VxDBuf[y * vMap.H_SIZE + x];
        auto expectedCorner = [&](int cx, int cy) {
            const int corner = expectedHeight(cx, cy);
            const int delta = corner - cellHeight;
            return (delta >= -10 && delta <= 10 ? corner : cellHeight) * 256;
        };
        const std::array<int, 4> expected = {
            expectedCorner(x, y), expectedCorner(x + 1, y),
            expectedCorner(x + 1, y + 1), expectedCorner(x, y + 1)};
        const int rightDelta = expectedHeight(x + 1, y) - cellHeight;
        if (rightDelta < -10 || rightDelta > 10) ++largeCliffCells;
        if (actual != expected) {
            if (++heightMismatches <= 4) {
                std::printf("[UVH] cell(%d,%d) actual=[%d %d %d %d] "
                            "expected=[%d %d %d %d]; ", x, y,
                            actual[0], actual[1], actual[2], actual[3],
                            expected[0], expected[1], expected[2], expected[3]);
            }
        }
        if (actual[0] != actual[1] || actual[0] != actual[2] ||
            actual[0] != actual[3]) {
            ++slopedCells;
        }
    }
    std::printf("[UVH] interpolated cap cells=%d cliff cells=%d height "
                "mismatches=%d; ", slopedCells, largeCliffCells,
                heightMismatches);
    TEST_REQUIRE(heightMismatches == 0,
                 "cap corners do not match the thresholded display surface");
    TEST_REQUIRE(largeCliffCells > 0,
                 "the cylinder fixture no longer exercises the >10 cliff rule");

    VoxCavityVertex a{2155.f, 2278.f, 500.f};
    VoxCavityVertex b{2186.f, 2278.f, 500.f};
    VoxCavityVertex c{2186.f, 2279.f, 500.f};
    VoxCavityVertex d{2155.f, 2279.f, 500.f};
    VoxCavityVertex spatial[6] = {a, b, c, a, c, d};
    TEST_REQUIRE(ov.debugSpatialQuadVertices(spatial, true) == 12,
                 "a cap crossing a tile boundary must split into two quads.");
    b.x = c.x = 2176.f;
    spatial[0] = spatial[3] = a;
    spatial[1] = b;
    spatial[2] = spatial[4] = c;
    spatial[5] = d;
    TEST_REQUIRE(ov.debugSpatialQuadVertices(spatial, true) == 6,
                 "an exact upper tile edge must not cause a zero-progress split.");
    // TEMP probe: is the terrain's visible pattern the leveled-material
    // (LvdTex) branch of getColor32? Compare getColor32 (terrain color)
    // vs the plain palette fallback (what the overlay would use) on the
    // floor ring around the cylinder.
    std::printf("[UVL] leveled probe (x y atr getCol32 palFallback): ");
    for (int y = 2445; y <= 2455; y += 2) {
        for (int x = 2320; x <= 2340; x += 2) {
            const int o2 = y * vMap.H_SIZE + x;
            std::printf("(%d,%d a=%02X g=%06X p=%06X) ", x, y,
                        vMap.AtrBuf[o2] & At_LEVELED,
                        vMap.getColor32(o2) & 0xFFFFFFu,
                        vMap.defaultColumnRgb(o2) & 0xFFFFFFu);
        }
    }
    std::printf("\n");
    session.unloadWorld();
    return true;
}

// Dig-through-island regression: digging down through a transparent band
// must descend smoothly (the void under the new surface fills solid),
// not collapse the whole column to the solid below the band — the old
// collapse ate islands in one step ("columns instantly disappearing,
// chunky data").
bool runTerraformSmoothnessProbe()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    session.setGameDir(ctx.gameDir.u8string());
    session.scanWorlds();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    const int cx = 2177, cy = 2405;
    const int off = cy * vMap.H_SIZE + cx;
    std::printf("[SM] dig center 500->150 in 25-voxel steps (cap z / "
                "verts / disp):\n");
    for (int target = 475; target >= 150; target -= 25) {
        MapEditor::BrushTool brush(session);
        MapEditor::BrushParams p;
        p.shape = MapEditor::BrushShape::FilledCircle;
        p.radius = 3;
        p.effect = MapEditor::BrushEffect::AlignVariableH;
        p.heightLevel = target;
        p.mode = MapEditor::BrushMode::Dam;
        TEST_REQUIRE(brush.applyStamp(cx, cy, p), "dig stamp refused");
        VoxCavityOverlay ov;
        ov.rebuildAll(1);
        std::vector<VoxCavityVertex> m;
        ov.debugDumpMesh(m);
        // cap z at center: topmost flat quad over the cell
        float capZ = -1;
        for (size_t i = 0; i + 6 <= m.size(); i += 6) {
            float x0 = m[i].x, x1 = m[i].x, y0 = m[i].y, y1 = m[i].y;
            float z0 = m[i].z, z1 = m[i].z;
            for (int k = 1; k < 6; ++k) {
                x0 = std::min(x0, m[i + k].x); x1 = std::max(x1, m[i + k].x);
                y0 = std::min(y0, m[i + k].y); y1 = std::max(y1, m[i + k].y);
                z0 = std::min(z0, m[i + k].z); z1 = std::max(z1, m[i + k].z);
            }
            if (z1 - z0 < 0.01f && (int)x0 <= cx && cx < (int)x1 &&
                (int)y0 <= cy && cy < (int)y1) {
                capZ = std::max(capZ, z0);
            }
        }
        const int idx =
            vMap.voxLayers.findColumn(static_cast<std::uint32_t>(off));
        int count = 0;
        const VoxLayerRange* rr =
            idx >= 0 ? vMap.voxLayers.columnRanges(idx, &count) : nullptr;
        std::printf("[SM] target=%d VxD=%d capZ=%.0f verts=%zu disp=%d "
                    "layers=%d\n",
                    target, vMap.VxDBuf[off], capZ, m.size(),
                    vMap.cavityDisplayHeight(off), count);
        (void)rr;
        // Exact-level stamps: the dam height must track the target every
        // step (the old collapse snapped 425 -> 253 when the dig crossed
        // the transparent band).
        TEST_REQUIRE(vMap.VxDBuf[off] == target,
                     "dig jumped: target=%d VxD=%d", target,
                     vMap.VxDBuf[off]);
    }
    session.unloadWorld();
    return true;
}

bool runTilePublishRegressionTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    MapEditor::EditorSession session;
    session.setGameDir(ctx.gameDir.u8string());
    session.scanWorlds();
    TEST_REQUIRE(session.loadWorldByName("EXTDEMO"),
                 "the EXTDEMO world could not be loaded.");
    vMap.WorldRender();

    VoxCavityOverlay overlay;
    overlay.rebuildAll(1);

    const int ey = 2405;
    int ex = -1;
    for (int x = 2400; x > 2100; --x) {
        if (vMap.VxDBuf[ey * vMap.H_SIZE + x] == 500) {
            ex = x;
            break;
        }
    }
    TEST_REQUIRE(ex > 0, "the island edge fixture moved (no 500-column)");
    MapEditor::BrushTool brush(session);
    MapEditor::BrushParams p;
    p.shape = MapEditor::BrushShape::FilledCircle;
    p.radius = 2;
    p.effect = MapEditor::BrushEffect::Align4ZP;
    p.mode = MapEditor::BrushMode::Dam;
    TEST_REQUIRE(brush.applyStamp(ex, ey, p), "edge terraform stamp refused");

    const int x0 = ex - 3, y0 = ey - 3;
    const int x1 = ex + 3, y1 = ey + 3;
    overlay.debugRebuildRect(x0, y0, x1, y1);
    std::vector<VoxCavityVertex> partial, full;
    overlay.debugDumpTileMesh(x0, y0, x1, y1, true, partial);
    overlay.debugDumpTileMesh(x0, y0, x1, y1, false, full);

    size_t untouchedUpper = 0;
    for (const VoxCavityVertex& v : full) {
        if (v.z > 499.5f && v.z < 500.5f &&
            (v.x < x0 || v.x > x1 + 1 || v.y < y0 || v.y > y1 + 1)) {
            ++untouchedUpper;
        }
    }
    std::printf("[TILE] edge=(%d,%d) partial=%zu full=%zu untouched upper "
                "vertices=%zu; ",
                ex, ey, partial.size(), full.size(), untouchedUpper);
    TEST_REQUIRE(untouchedUpper > 100,
                 "tile regression did not sample untouched upper-cylinder "
                 "geometry");
    if (partial != full) {
        const size_t common = std::min(partial.size(), full.size());
        size_t different = partial.size() != full.size();
        for (size_t i = 0; i < common; ++i) {
            if (partial[i] == full[i]) continue;
            ++different;
            if (different <= 3) {
                const VoxCavityVertex& a = partial[i];
                const VoxCavityVertex& b = full[i];
                std::printf("[TILE] vertex %zu partial=(%.3f,%.3f,%.3f "
                            "%08X %.5f,%.5f) full=(%.3f,%.3f,%.3f %08X "
                            "%.5f,%.5f); ",
                            i, a.x, a.y, a.z, a.diffuse, a.u, a.v,
                            b.x, b.y, b.z, b.diffuse, b.u, b.v);
            }
        }
        TEST_REQUIRE(false,
                     "partial tile publish lost or changed %zu vertices / "
                     "attributes near the cylinder edge (partial=%zu full=%zu)",
                     different, partial.size(), full.size());
    }
    session.unloadWorld();
    return true;
}

MAP_EDITOR_REGISTER_TEST("Tile publish preserves cylinder edge geometry", 143,
                         runTilePublishRegressionTests);
MAP_EDITOR_REGISTER_TEST("Dig through island descends smoothly", 137,
                         runTerraformSmoothnessProbe);
MAP_EDITOR_REGISTER_TEST("Overlay mesh smoothing layout (caps/walls)",
                         136, runUvLayoutTests);
MAP_EDITOR_REGISTER_TEST("Level-stamp wall side regression (island edge)",
                         135, runWallSideRegressionTests);

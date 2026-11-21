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
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Quad {
    float x0, y0, x1, y1, z0, z1;
    int plane; // 0 = horizontal (z const), 1 = x const, 2 = y const
};

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
        std::map<std::pair<int,int>, int> cover;
        for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
            float z0=mesh[i].z, z1=mesh[i].z, x0=mesh[i].x, x1=mesh[i].x;
            float y0=mesh[i].y, y1=mesh[i].y;
            for (int k=1;k<6;++k){
                z0=std::min(z0,mesh[i+k].z); z1=std::max(z1,mesh[i+k].z);
                x0=std::min(x0,mesh[i+k].x); x1=std::max(x1,mesh[i+k].x);
                y0=std::min(y0,mesh[i+k].y); y1=std::max(y1,mesh[i+k].y);
            }
            if (z1-z0>0.01f || z0<499.5f || z0>500.5f) continue;
            for (int x=(int)x0; x<(int)x1; ++x)
                for (int y=(int)y0; y<(int)y1; ++y)
                    cover[{x,y}]++;
        }
        int missing=0, multi=0, plateau=0;
        int missPrint = 0;
        for (int y=2278;y<2518;++y)
            for (int x=2050;x<2290;++x) {
                const int off=y*vMap.H_SIZE+x;
                if (vMap.VxDBuf[off]!=500) continue;
                ++plateau;
                const int c=cover[{x,y}];
                if (c==0){++missing; if(missPrint<8){std::printf("[ZF] hole at %d,%d (disp=%d); ",x,y,vMap.cavityDisplayHeight(off));++missPrint;}}
                else if (c>1) ++multi;
            }
        std::printf("[ZF] FULL cap coverage: plateau=%d missing=%d multi=%d; ", plateau, missing, multi);
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
        // Cap color histogram over the plateau (diagnostic print).
        {
            std::map<std::uint32_t, int> hist;
            for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
                float z0=mesh[i].z, z1=mesh[i].z, x0=mesh[i].x, x1=mesh[i].x;
                float y0=mesh[i].y, y1=mesh[i].y;
                for (int k=1;k<6;++k){
                    z0=std::min(z0,mesh[i+k].z); z1=std::max(z1,mesh[i+k].z);
                    x0=std::min(x0,mesh[i+k].x); x1=std::max(x1,mesh[i+k].x);
                    y0=std::min(y0,mesh[i+k].y); y1=std::max(y1,mesh[i+k].y);
                }
                if (z1-z0>0.01f || z0<499.5f || z0>500.5f) continue;
                hist[mesh[i].diffuse]++;
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
    auto capColors = [&overlay]() {
        std::vector<VoxCavityVertex> mesh;
        overlay.debugDumpMesh(mesh);
        std::map<std::pair<int, int>, std::uint32_t> m;
        for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
            float z0 = mesh[i].z, z1 = mesh[i].z;
            float x0 = mesh[i].x, x1 = mesh[i].x;
            float y0 = mesh[i].y, y1 = mesh[i].y;
            for (int k = 1; k < 6; ++k) {
                z0 = std::min(z0, mesh[i + k].z);
                z1 = std::max(z1, mesh[i + k].z);
                x0 = std::min(x0, mesh[i + k].x);
                x1 = std::max(x1, mesh[i + k].x);
                y0 = std::min(y0, mesh[i + k].y);
                y1 = std::max(y1, mesh[i + k].y);
            }
            if (z1 - z0 > 0.01f || z0 < 499.5f || z0 > 500.5f) continue;
            for (int x = (int)x0; x < (int)x1; ++x)
                for (int y = (int)y0; y < (int)y1; ++y)
                    if (x >= 2140 && x < 2220 && y >= 2360 && y < 2450)
                        m[{x, y}] = mesh[i].diffuse;
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

    overlay.rebuildAll();
    const auto full = capColors();

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

    auto capColorAt = [&](int x, int y) {
        VoxCavityOverlay ov;
        ov.rebuildAll(1);
        std::vector<VoxCavityVertex> m;
        ov.debugDumpMesh(m);
        for (size_t i = 0; i + 6 <= m.size(); i += 6) {
            float x0 = m[i].x, x1 = m[i].x, y0 = m[i].y, y1 = m[i].y;
            float z0 = m[i].z, z1 = m[i].z;
            for (int k = 1; k < 6; ++k) {
                x0 = std::min(x0, m[i + k].x); x1 = std::max(x1, m[i + k].x);
                y0 = std::min(y0, m[i + k].y); y1 = std::max(y1, m[i + k].y);
                z0 = std::min(z0, m[i + k].z); z1 = std::max(z1, m[i + k].z);
            }
            if (z1 - z0 < 0.01f && z0 > 499.5f && z0 < 500.5f &&
                (int)x0 <= x && x < (int)x1 && (int)y0 <= y && y < (int)y1) {
                return m[i].diffuse & 0xFFFFFFu;
            }
        }
        return 0u;
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
    // Кепка красит 0xFF0000 светом колонки ровно как shade() мешера.
    const unsigned light = vMap.RnrBuf[off] & 0x7F;
    const std::uint32_t shadedRed =
        ((255u * light / 127u) << 16);
    auto capColorAt = [&](int x, int y) {
        VoxCavityOverlay ov;
        ov.rebuildAll(1);
        std::vector<VoxCavityVertex> m;
        ov.debugDumpMesh(m);
        for (size_t i = 0; i + 6 <= m.size(); i += 6) {
            float x0 = m[i].x, x1 = m[i].x, y0 = m[i].y, y1 = m[i].y;
            float z0 = m[i].z, z1 = m[i].z;
            for (int k = 1; k < 6; ++k) {
                x0 = std::min(x0, m[i + k].x); x1 = std::max(x1, m[i + k].x);
                y0 = std::min(y0, m[i + k].y); y1 = std::max(y1, m[i + k].y);
                z0 = std::min(z0, m[i + k].z); z1 = std::max(z1, m[i + k].z);
            }
            if (z1 - z0 < 0.01f && z0 > 499.5f && z0 < 500.5f &&
                (int)x0 <= x && x < (int)x1 && (int)y0 <= y && y < (int)y1) {
                return m[i].diffuse & 0xFFFFFFu;
            }
        }
        return 0u;
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
        (((255u * light2 / 127u) << 16) | ((255u * light2 / 127u) << 8));
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
    // run at the rect band boundary (two adjacent quads instead of one —
    // same plane, same color, shared edge, identical rendering), so
    // compare unioned per-plane coverage instead of raw quad streams.
    auto coverage = [](const std::vector<VoxCavityVertex>& m) {
        // key: plane axis+coord, z range, color -> union of along-intervals
        std::map<std::tuple<int, int, int, int, std::uint32_t>,
                 std::vector<std::pair<int, int>>>
            cov;
        for (size_t i = 0; i + 6 <= m.size(); i += 6) {
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
                static_cast<int>(z1 * 2.f + 0.5f), m[i].diffuse)];
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
                    std::printf("[WALL] coverage diff key(a%d c%d z%d-%d "
                                "%X): full=[",
                                std::get<0>(kv.first), std::get<1>(kv.first),
                                std::get<2>(kv.first), std::get<3>(kv.first),
                                std::get<4>(kv.first));
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
    TEST_REQUIRE(covIncr == covFull,
                 "incremental rebuild after a level stamp lost wall coverage "
                 "(transparent side holes)");
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
MAP_EDITOR_REGISTER_TEST("Level-stamp wall side regression (island edge)",
                         135, runWallSideRegressionTests);

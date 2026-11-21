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

    {
        const int offc = 2405 * vMap.H_SIZE + 2177;
        const int offe = 2405 * vMap.H_SIZE + 2140;
        std::printf("[ZF] cut center: disp=%d surf=%d; edge: disp=%d surf=%d; ", vMap.cavityDisplayHeight(offc), vMap.SGetAlt(offc) >> 5, vMap.cavityDisplayHeight(offe), vMap.SGetAlt(offe) >> 5);
    }
    // Real product path: full parallel rebuild of the whole world
    // (band merge included — the striped-z-fight source), then dump.
    VoxCavityOverlay overlay;
    overlay.rebuildAll();
    std::vector<VoxCavityVertex> mesh;
    overlay.debugDumpMesh(mesh);
    std::printf("[ZF] composed verts: %zu\n", mesh.size());

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
    // Cap coverage: every interior cylinder cell must have EXACTLY ONE
    // cap quad at z=500 (holes would show the striped floor beneath).
    {
        std::map<std::pair<int,int>, int> cover;
        for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
            const VoxCavityVertex& a = mesh[i];
            if (a.z < 499.5f || a.z > 500.5f) continue;
            float x0=a.x,x1=a.x,y0=a.y,y1=a.y;
            for (int k=1;k<6;++k){
                x0=std::min(x0,mesh[i+k].x); x1=std::max(x1,mesh[i+k].x);
                y0=std::min(y0,mesh[i+k].y); y1=std::max(y1,mesh[i+k].y);
            }
            for (int x=(int)x0; x<(int)x1; ++x)
                for (int y=(int)y0; y<(int)y1; ++y)
                    if (x>=2160 && x<2200 && y>=2390 && y<2420)
                        cover[{x,y}]++;
        }
        int missing=0, multi=0;
        for (int x=2160;x<2200;++x)
            for (int y=2390;y<2420;++y) {
                const int c = cover[{x,y}];
                if (c==0) missing++;
                else if (c>1) multi++;
            }
        std::printf("[ZF] cap coverage 40x30: missing=%d multi=%d; ", missing, multi);
    }
    // Software raster of the cylinder patch from a tilted south view
    // (like the report screenshot): z-buffer + flat quad colors. Stripes
    // visible here => in the overlay data; not visible => render-side
    // pairing with the heightfield.
    {
        const int W = 120, H = 60;
        std::vector<float> zb(W * H, -1e9f);
        std::vector<std::uint32_t> img(W * H, 0xFF202020u);
        const float ex = 2177.f, ey = 2310.f, ez = 560.f;
        // basis: forward (north, slightly down), right, up
        float fx = 0.f, fy = 0.9f, fz = -0.4359f;
        const float fl = 700.f;
        float upx = 0.f, upy = 0.4359f, upz = 0.9f;
        auto project = [&](const VoxCavityVertex& v, float& sx, float& sy,
                           float& depth) {
            const float dx = v.x - ex, dy = v.y - ey, dz = v.z - ez;
            depth = dx * fx + dy * fy + dz * fz;
            sx = W * 0.5f + dx * fl / depth;
            sy = H * 0.55f - (dx * 0.f + dy * upy + dz * upz) * fl / depth;
            return true;
        };
        for (size_t i = 0; i + 6 <= mesh.size(); i += 6) {
            float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
            for (int k = 0; k < 6; ++k) {
                mnx = std::min(mnx, mesh[i + k].x);
                mxx = std::max(mxx, mesh[i + k].x);
                mny = std::min(mny, mesh[i + k].y);
                mxy = std::max(mxy, mesh[i + k].y);
            }
            if (mnx > 2240.f || mxx < 2120.f || mny > 2460.f || mxy < 2350.f)
                continue;
            float sx[6], sy[6], dp[6];
            for (int k = 0; k < 6; ++k) project(mesh[i + k], sx[k], sy[k], dp[k]);
            float qmnx = 1e9f, qmxx = -1e9f, qmny = 1e9f, qmxy = -1e9f;
            for (int k = 0; k < 6; ++k) {
                qmnx = std::min(qmnx, sx[k]); qmxx = std::max(qmxx, sx[k]);
                qmny = std::min(qmny, sy[k]); qmxy = std::max(qmxy, sy[k]);
            }
            for (int py = (int)qmny; py <= (int)qmxy; ++py)
                for (int px = (int)qmnx; px <= (int)qmxx; ++px) {
                    if (px < 0 || px >= W || py < 0 || py >= H) continue;
                    const float cxx = px + 0.5f, cyy = py + 0.5f;
                    auto inTri = [&](int t0, int t1, int t2) {
                        const float x0 = sx[t0], y0 = sy[t0];
                        const float x1 = sx[t1], y1 = sy[t1];
                        const float x2 = sx[t2], y2 = sy[t2];
                        const float d1 = (cxx-x1)*(y0-y1)-(x0-x1)*(cyy-y1);
                        const float d2 = (cxx-x2)*(y1-y2)-(x1-x2)*(cyy-y2);
                        const float d3 = (cxx-x0)*(y2-y0)-(x2-x0)*(cyy-y0);
                        const bool neg = (d1<0)||(d2<0)||(d3<0);
                        const bool pos = (d1>0)||(d2>0)||(d3>0);
                        return !(neg && pos);
                    };
                    if (!inTri(0, 1, 2) && !inTri(3, 4, 5)) continue;
                    const float d = (dp[0] + dp[1] + dp[2]) / 3.f;
                    if (d > zb[py * W + px]) {
                        zb[py * W + px] = d;
                        img[py * W + px] = mesh[i].diffuse;
                    }
                }
        }
        std::printf("[ZF] raster:\n");
        for (int py = 0; py < H; py += 2) {
            std::string line;
            for (int px = 0; px < W; ++px) {
                const std::uint32_t c = img[py * W + px];
                const unsigned r = (c >> 16) & 0xFF;
                line += r > 200 ? '#' : r > 120 ? '+' : r > 60 ? '.' : ' ';
            }
            std::printf("%s\n", line.c_str());
        }
    }

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Cavity overlay z-fight analysis (EXTDEMO "
                         "cylinder)",
                         130, runZfightTests);

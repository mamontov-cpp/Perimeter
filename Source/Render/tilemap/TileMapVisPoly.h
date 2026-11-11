// ============================================================================
// TileMapVisPoly.h — rasterizes the camera visibility polygon into the
// terrain tile grid (one byte per 64x64-cell tile).
//
// Extracted from TileMap.cpp so the map_editor headless tests can exercise
// the REAL implementation (TilemapVisPolyTests) — the tile grid exceeds the
// old 128x128 stack arrays on maps above 8192 per side (256 tiles at 2^14,
// 512 at 2^15), which used to assert in Debug and corrupt memory in Release.
//
// Kept dependency-free on purpose: std + Vect2f only. xm::floor/ceil of the
// original are std::floor/ceil — floor/ceil are exact operations, the
// results are bit-identical.
// ============================================================================

#ifndef TILE_MAP_VIS_POLY_H
#define TILE_MAP_VIS_POLY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Vect2f (xmath.h) — kept as an include so the header compiles standalone.
#include "xmath.h"

inline void fillVisPoly(uint8_t* buf, const std::vector<Vect2f>& vert,
                        int VISMAP_W, int VISMAP_H)
{
    if (vert.empty()) return;
    // Динамические размеры: на картах больше 8192 клеток сетка тайлов
    // превышает 128 (256 при 2^14, 512 при 2^15). lx индексируется по H,
    // rx по H — берём максимум на всякий случай.
    std::vector<float> lx(std::max(VISMAP_W, VISMAP_H));
    std::vector<float> rx(std::max(VISMAP_W, VISMAP_H));
    int i, y, ytop, ybot;

    // find top/bottom y
    ytop = static_cast<int>(std::floor(vert[0].y));
    ybot = static_cast<int>(std::ceil(vert[0].y));
    for (i = 1; i < static_cast<int>(vert.size()); i++) {
        float y = vert[i].y;
        if (y < ytop) ytop = static_cast<int>(std::floor(y));
        if (y > ybot) ybot = static_cast<int>(std::ceil(vert[i].y));
    }

    for (i = 0; i < VISMAP_H; i++) {
        lx[i] = static_cast<float>(VISMAP_W - 1);
        rx[i] = 0;
    }

    // render edges
    for (i = 0; i < static_cast<int>(vert.size()); i++) {
        int i2 = (i + 1 >= static_cast<int>(vert.size())) ? 0 : i + 1;
        float x1, x2, y1, y2, t;
        x1 = vert[i].x;
        y1 = vert[i].y;
        x2 = vert[i2].x;
        y2 = vert[i2].y;

        if (y1 > y2) { t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }

        int iy1 = static_cast<int>(y1), iy2 = static_cast<int>(y2);
        if (iy1 > iy2) continue;

        float dy = (y2 == y1) ? 1 : (y2 - y1);
        float dy1 = 1 / dy;
        for (y = std::max(iy1, 0); y <= std::min(iy2, VISMAP_H - 1); y++) {
            float ix1 = x1 + (y - y1) * (x2 - x1) * dy1;
            float ix2 = x1 + (y - y1 + 1) * (x2 - x1) * dy1;
            if (y == iy1) ix1 = x1;
            if (y == iy2) ix2 = x2;

            lx[y] = std::min(std::min(lx[y], ix1), ix2);
            rx[y] = std::max(std::max(rx[y], ix1), ix2);
        }
    }

    // fill the buffer
    for (y = std::max(0, ytop); y <= std::min(ybot, VISMAP_H - 1); y++) {
        if (lx[y] > rx[y]) continue;
        int x1 = static_cast<int>(std::max(std::floor(lx[y]), 0.0f));
        int x2 = static_cast<int>(std::min(std::ceil(rx[y]), static_cast<float>(VISMAP_W)));
        if (x1 >= x2) continue;
        // x2 <= VISMAP_W by construction — no row overrun.
        memset(buf + y * VISMAP_W + x1, 1, x2 - x1);
    }
}

#endif // TILE_MAP_VIS_POLY_H

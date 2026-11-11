// ============================================================================
// SceneTrace.h — fixed-point DDA ray march against the terrain heightfield.
//
// Extracted from cScene::Trace (Scene.cpp) so the map_editor headless tests
// can exercise the REAL implementation (SceneTraceTests). The march keeps
// the ray position in 18-bit fixed point — 64-bit accumulators: at 2^14+
// map coordinates coord*2^18 exceeds int32 (16384<<18 = 2^32) and the ray
// "starts" from a wrapped position (waypoints/terraform marks landing at
// clamped wrong spots); heights near the 8191 voxel cap broke z the same
// way. floor/ceil are exact — std matches xm bit-for-bit here.
// ============================================================================

#ifndef SCENE_TRACE_H
#define SCENE_TRACE_H

#include <cmath>
#include <cstdint>
#include <functional>

#ifndef PREC_TRACE_RAY
#define PREC_TRACE_RAY 18
#endif

namespace scene_trace {

/// Marches the segment direction (pStart -> pFinish, unbounded march like
/// the original: only the DIRECTION matters, the hit runs until the ray
/// leaves the map or drops below the terrain). `heightAt(x, y)` returns the
/// terrain whole-voxel height. On a hit writes the cell to out and returns
/// true.
inline bool traceRay(float xb, float yb, float zb,
                     float xe, float ye, float ze,
                     int x_size, int y_size,
                     const std::function<int(int, int)>& heightAt,
                     int zeroHeight,
                     bool ignore_height, bool ignore_bounds,
                     float* outX, float* outY, float* outZ)
{
    float dx = xe - xb, dy = ye - yb, dz = ze - zb;
    float dxAbs = std::fabs(dx), dyAbs = std::fabs(dy);
    int64_t dx_, dy_, dz_;

    if (dxAbs > dyAbs) {
        dy_ = static_cast<int64_t>(std::llround(dy * (1 << PREC_TRACE_RAY) / dxAbs));
        dz_ = static_cast<int64_t>(std::llround(dz * (1 << PREC_TRACE_RAY) / dxAbs));
        dx_ = ((dx >= 0) ? 1 : -1) << PREC_TRACE_RAY;
    } else if (dyAbs > FLT_EPSILON) {
        dx_ = static_cast<int64_t>(std::llround(dx * (1 << PREC_TRACE_RAY) / dyAbs));
        dz_ = static_cast<int64_t>(std::llround(dz * (1 << PREC_TRACE_RAY) / dyAbs));
        dy_ = ((dy >= 0) ? 1 : -1) << PREC_TRACE_RAY;
    } else {
        if (outX) {
            *outX = xb;
            *outY = yb;
            *outZ = ignore_height ? static_cast<float>(zeroHeight)
                                  : static_cast<float>(heightAt(
                                        static_cast<int>(std::lround(xb)),
                                        static_cast<int>(std::lround(yb))));
        }
        return true;
    }

    if (!ignore_bounds) {
        if (xb < 0) {
            if (dxAbs) {
                xb = (0 - xb) / dxAbs, yb += xb * dy, zb += xb * dz, xb = 0;
            } else {
                return false;
            }
        }
        if (yb < 0) {
            if (dyAbs) {
                yb = (0 - yb) / dyAbs, xb += yb * dx, zb += yb * dz, yb = 0;
            } else {
                return false;
            }
        }
        if (xb >= x_size) {
            if (dxAbs) {
                xb = (xb - (x_size - 1)) / dxAbs;
                yb += xb * dy;
                zb += xb * dz;
                xb = x_size - 1;
            } else {
                return false;
            }
        }
        if (yb >= y_size) {
            if (dyAbs) {
                yb = (yb - (y_size - 1)) / dyAbs;
                xb += yb * dx;
                zb += yb * dz;
                yb = y_size - 1;
            } else {
                return false;
            }
        }
    }

    int64_t xb_ = static_cast<int64_t>(std::llround(xb * (1 << PREC_TRACE_RAY)));
    int64_t yb_ = static_cast<int64_t>(std::llround(yb * (1 << PREC_TRACE_RAY)));
    int64_t zb_ = static_cast<int64_t>(std::llround(zb * (1 << PREC_TRACE_RAY)));
    while (true) {
        xb_ += dx_;
        yb_ += dy_;
        zb_ += dz_;
        int xbp = static_cast<int>(xb_ >> PREC_TRACE_RAY);
        int ybp = static_cast<int>(yb_ >> PREC_TRACE_RAY);
        int zbp = static_cast<int>(zb_ >> PREC_TRACE_RAY);
        bool inside_bounds = xbp >= 0 && xbp < x_size &&
                             ybp >= 0 && ybp < y_size;
        if (!inside_bounds) {
            if (!ignore_bounds) {
                break;
            }
            int max_edge = x_size > y_size ? x_size : y_size;
            if (xbp < (-max_edge) || xbp > (x_size + max_edge) ||
                ybp < (-max_edge) || ybp > (y_size + max_edge)) {
                return false;
            }
        }

        int z_height = 0;
        if (ignore_height) {
            z_height = zeroHeight;
        } else if (inside_bounds) {
            z_height = heightAt(xbp, ybp);
        }

        if (z_height >= zbp) {
            if (outX) {
                *outX = static_cast<float>(xbp);
                *outY = static_cast<float>(ybp);
                *outZ = static_cast<float>(z_height);
            }
            return true;
        }
    }

    return false;
}

} // namespace scene_trace

#endif // SCENE_TRACE_H

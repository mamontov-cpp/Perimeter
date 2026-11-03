#pragma once

#include <algorithm>
#include <cmath>

// ============================================================================
// MapEditorCore — 3D shape brushes (cone / pyramid / truncated cone /
// truncated pyramid / half torus / half ellipsoid).
//
// Pure analytic profile math, Qt-free and vMap-free: for a column at map
// cells (dx, dy) relative to the stamp centre, shape3DProfile() returns the
// normalized height of the shape's surface there, 1.0 at the highest point
// and 0.0 at the base boundary (and everywhere outside). The brush multiplies
// the profile by the Shape Height setting (and adds the Height base) to get
// the per-cell target — see BrushTool::applyShape3D.
//
// The shapes are solids of a base circle or a regular polygon (circumradius =
// the brush radius, vertex count / first-vertex angle shared with the 2D
// polygon brushes) with an optional top circle/polygon shifted by the upper
// part offset (oblique cone/pyramid/frusta). The torus ignores the offset
// (its axis is the stamp centre); its "upper part radius" is the ring's INNER
// radius. All angles in degrees on the map plane (0 = +x, growing towards +y,
// clockwise on screen — the same convention BrushMasks::regularPolygonVertices
// uses).
// ============================================================================

namespace MapEditor {

/// The 3D brush solid. None = the plain 2D stamp geometry is in effect.
enum class Shape3DKind
{
    None,
    Cone,
    Pyramid,
    TruncatedCone,
    TruncatedPyramid,
    Torus,
    HalfEllipsoid,
};

/// Is this shape built on a regular-polygon base (uses sides + angleDeg)?
inline bool shape3DIsPolygonal(Shape3DKind kind)
{
    return kind == Shape3DKind::Pyramid ||
           kind == Shape3DKind::TruncatedPyramid;
}

/// Does this shape take the upper part XY offset (oblique axis)?
inline bool shape3DHasTopOffset(Shape3DKind kind)
{
    return kind == Shape3DKind::Cone || kind == Shape3DKind::Pyramid ||
           kind == Shape3DKind::TruncatedCone ||
           kind == Shape3DKind::TruncatedPyramid;
}

/// Does this shape take the upper part radius? (truncated cone top radius,
/// torus inner radius.)
inline bool shape3DHasTopRadius(Shape3DKind kind)
{
    return kind == Shape3DKind::TruncatedCone ||
           kind == Shape3DKind::Torus;
}

/// Does this shape take the upper part side length? (truncated pyramid).
inline bool shape3DHasTopSide(Shape3DKind kind)
{
    return kind == Shape3DKind::TruncatedPyramid;
}

/// Geometry of one 3D brush stamp. All lengths in map cells; angles in
/// degrees. Fields that do not apply to the active kind are ignored (and
/// clampShape3DSettings leaves them alone).
struct Shape3DSettings
{
    double radius = 8.0;    ///< base circumradius (the brush radius).
    int sides = 6;          ///< regular-polygon base vertex count.
    double angleDeg = 0.0;  ///< first-vertex angle offset.
    double topOffsetX = 0.0; ///< upper part centre offset (cells).
    double topOffsetY = 0.0;
    double topRadius = 0.0; ///< truncated-cone top radius / torus inner radius.
    double topSide = 0.0;   ///< truncated-pyramid top side length.
};

namespace detail {

inline double deg2rad(double deg)
{
    return deg * 3.14159265358979323846 / 180.0;
}

/// Boundary radius of the regular N-gon with circumradius R whose first
/// VERTEX sits at `angleDeg`, at the direction `thetaDeg`: the side line
/// sits at the apothem distance, its normal pointing at the mid-edge
/// directions (vertex angle + half sector).
inline double polygonBoundaryRadius(double thetaDeg, double radius, int sides,
                                    double angleDeg)
{
    const int n = std::max(3, sides);
    const double sector = 2.0 * 3.14159265358979323846 / n;
    const double apothem = radius * std::cos(3.14159265358979323846 / n);
    // Offset from the direction, wrapped so 0 points at a side's midpoint.
    double delta = deg2rad(thetaDeg - angleDeg);
    delta -= sector * std::floor(delta / sector); // [0, sector)
    delta -= sector * 0.5;                        // [-sector/2, +sector/2)
    return apothem / std::cos(delta);
}

/// The top-circle/polygon circumradius a truncated pyramid takes from its top
/// side length.
inline double topCircumradiusFromSide(double topSide, int sides)
{
    const int n = std::max(3, sides);
    return topSide / (2.0 * std::sin(3.14159265358979323846 / n));
}

/// Frustum profile: column p = (dx, dy) (base-centre-relative), axis offset
/// off interpolated 0 -> off with z, boundary radius r(z) = r0 + (r1-r0)*z.
/// The top surface of the column is the first crossing of
/// |p - off*z| = r0 + (r1-r0)*z in [0, 1]; a column still inside the solid at
/// z = 1 sits under the top cap (profile 1), any other no-crossing column is
/// outside (profile 0).
inline double frustumProfile(double dx, double dy, double offX, double offY,
                             double r0, double r1)
{
    if (r1 > r0) {
        r1 = r0; // the top never exceeds the base
    }
    // |p - off*z|^2 - (r0 + (r1-r0)*z)^2 = a*z^2 + b*z + c, p absolute.
    const double a = offX * offX + offY * offY - (r1 - r0) * (r1 - r0);
    const double b = -2.0 * (dx * offX + dy * offY) - 2.0 * r0 * (r1 - r0);
    const double c = dx * dx + dy * dy - r0 * r0;
    auto rootIn01 = [](double z) { return z >= 0.0 && z <= 1.0; };
    if (std::abs(a) < 1e-12) {
        if (std::abs(b) < 1e-12) {
            return c <= 0.0 ? 1.0 : 0.0;
        }
        const double z = -c / b;
        if (rootIn01(z)) {
            return z;
        }
    } else {
        const double disc = b * b - 4.0 * a * c;
        if (disc >= 0.0) {
            const double sq = std::sqrt(disc);
            const double z1 = (-b - sq) / (2.0 * a);
            const double z2 = (-b + sq) / (2.0 * a);
            const double zLo = std::min(z1, z2);
            const double zHi = std::max(z1, z2);
            // The column enters the solid at z = 0 (convex footprint), so the
            // first crossing going up is the surface; the second root is the
            // far side of the solid, hidden underneath.
            if (rootIn01(zLo)) {
                return zLo;
            }
            if (rootIn01(zHi)) {
                return zHi;
            }
        }
    }
    // No side crossing: either under the top cap (inside at z = 1) or outside.
    const double pTopX = dx - offX;
    const double pTopY = dy - offY;
    return std::sqrt(pTopX * pTopX + pTopY * pTopY) <= r1 ? 1.0 : 0.0;
}

} // namespace detail

/// Clamp the settings to what the shape can represent: the upper part stays
/// strictly inside the base (offset norm / top radius / derived top
/// circumradius <= radius - 0.5 cells), the top radius never exceeds the
/// base, and the polygon side count is at least 3.
inline Shape3DSettings clampShape3DSettings(Shape3DKind kind,
                                            const Shape3DSettings& in)
{
    Shape3DSettings s = in;
    s.radius = std::max(0.5, s.radius);
    const double maxTop = std::max(0.0, s.radius - 0.5);
    if (shape3DHasTopOffset(kind)) {
        const double norm = std::sqrt(s.topOffsetX * s.topOffsetX +
                                      s.topOffsetY * s.topOffsetY);
        if (norm > maxTop && norm > 0.0) {
            const double scale = maxTop / norm;
            s.topOffsetX *= scale;
            s.topOffsetY *= scale;
        }
    }
    if (shape3DHasTopRadius(kind)) {
        s.topRadius = std::clamp(s.topRadius, 0.0, maxTop);
    }
    if (shape3DHasTopSide(kind)) {
        const double maxSide =
            2.0 * maxTop *
            std::sin(3.14159265358979323846 / std::max(3, s.sides));
        s.topSide = std::clamp(s.topSide, 0.0, std::max(0.0, maxSide));
    }
    s.sides = std::max(3, s.sides);
    return s;
}

/// Normalized surface height [0..1] of the shape at the column (dx, dy) in
/// cells relative to the BASE centre. 1 at the highest point, 0 at and beyond
/// the base boundary. The settings should be clamped first
/// (clampShape3DSettings).
inline double shape3DProfile(Shape3DKind kind, const Shape3DSettings& s,
                             double dx, double dy)
{
    const double d = std::sqrt(dx * dx + dy * dy);
    switch (kind) {
    case Shape3DKind::Cone: {
        const double dc = std::sqrt((dx - s.topOffsetX) * (dx - s.topOffsetX) +
                                    (dy - s.topOffsetY) * (dy - s.topOffsetY));
        return std::max(0.0, 1.0 - dc / s.radius);
    }
    case Shape3DKind::Pyramid: {
        const double dc = std::sqrt((dx - s.topOffsetX) * (dx - s.topOffsetX) +
                                    (dy - s.topOffsetY) * (dy - s.topOffsetY));
        if (dc <= 0.0) {
            return 1.0;
        }
        const double theta = std::atan2(dy - s.topOffsetY,
                                        dx - s.topOffsetX) *
                             180.0 / 3.14159265358979323846;
        const double rho = detail::polygonBoundaryRadius(theta, s.radius,
                                                         s.sides, s.angleDeg);
        return std::max(0.0, 1.0 - dc / rho);
    }
    case Shape3DKind::TruncatedCone:
        return detail::frustumProfile(dx, dy, s.topOffsetX, s.topOffsetY,
                                      s.radius, s.topRadius);
    case Shape3DKind::TruncatedPyramid: {
        const double px = dx - s.topOffsetX;
        const double py = dy - s.topOffsetY;
        if (px == 0.0 && py == 0.0) {
            return 1.0;
        }
        const double theta = std::atan2(py, px) * 180.0 /
                             3.14159265358979323846;
        const double rho0 = detail::polygonBoundaryRadius(theta, s.radius,
                                                          s.sides, s.angleDeg);
        const double rho1 = detail::polygonBoundaryRadius(
            theta, detail::topCircumradiusFromSide(s.topSide, s.sides),
            s.sides, s.angleDeg);
        return detail::frustumProfile(dx, dy, s.topOffsetX, s.topOffsetY,
                                      rho0, rho1);
    }
    case Shape3DKind::Torus: {
        const double rMid = (s.radius + s.topRadius) * 0.5;
        const double rTube = std::max(1e-9, (s.radius - s.topRadius) * 0.5);
        const double x = (d - rMid) / rTube;
        return std::sqrt(std::max(0.0, 1.0 - x * x));
    }
    case Shape3DKind::HalfEllipsoid: {
        const double x = d / s.radius;
        return std::sqrt(std::max(0.0, 1.0 - x * x));
    }
    case Shape3DKind::None:
        break;
    }
    return 0.0;
}

} // namespace MapEditor

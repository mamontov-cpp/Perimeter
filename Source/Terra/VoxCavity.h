// ============================================================================
// VoxCavity.h — triangulation of transparent (empty) voxel cavities in the
// dam into a raw triangle soup, for the cavity overlay mesh (MapEditor 3D
// view AND the game — see VoxCavityOverlay). Pure Terra: no Render
// dependency; the consumer wraps the triangles into a scene object.
//
// What is emitted for a column that carries transparent ranges (the
// heightfield itself is CUT at the cavity — TerraInterface GetZ/GetZf
// return vrtMap::cavityDisplayHeight, the base under the lowest transparent
// range — so the overlay renders the cavity contents as free geometry):
//   * the column is split into SOLID segments by its transparent ranges;
//   * every segment gets the faces that border on emptiness:
//       - top face at z=s1+1 for every segment — the TOPMOST segment's cap
//         is the island's top (the heightfield no longer spans the cavity
//         column), lower segments' tops are slabs under upper cavities —
//         EXCEPT the floor plane at the display height, which the
//         heightfield renders (re-emitting it would z-fight);
//       - bottom face at z=s0 when the voxel below is transparent (the
//         island/slab underside);
//       - side voxel quads on ALL FOUR directions where the neighbor
//         column is empty at that voxel; a boundary plane between two
//         cavity columns is drawn once (by the east/north column — the
//         west/south neighbor draws its own east/north face), while
//         against a non-cavity neighbor (lower terrain, no overlay
//         chunks) the column draws its own west/south faces — otherwise a
//         floating block loses exactly those two sides (4 faces of 6);
//   * faces are emitted in BOTH windings — the overlay must be visible from
//     inside the cavity and outside regardless of the pipeline culling
//     convention;
//   * side planes are pushed a small epsilon into the neighbor cell to
//     avoid z-fighting with the heightfield cliffs of SOLID neighbors.
// Vertex colors: the solid voxel's layer color (VoxColLayers) or the column
// default, shaded per face direction (top 1.0, sides 0.75, bottom 0.55) and
// the column's RnrBuf light — the cavity reads as 3D, not flat paint.
// ============================================================================

#ifndef VOX_CAVITY_H
#define VOX_CAVITY_H

#include <cstdint>
#include <vector>

class vrtMap;

// Layout kept IDENTICAL to the render stack's sVertexXYZDT1 (24 bytes) so
// the overlay publish is a bulk memcpy, not a per-vertex conversion — the
// Debug field-by-field loop cost milliseconds per flatten.
struct VoxCavityVertex
{
	float x = 0.f;
	float y = 0.f;
	float z = 0.f;
	std::uint32_t diffuse = 0xFF000000u; // ARGB
	float u = 0.f;
	float v = 0.f;
};

/// Appends the cavity geometry of one map column (cell x, y) to `out`
/// (3 vertices per triangle, both windings per face). No-op when the
/// column has no transparent voxel ranges.
void voxCavityAppendColumn(vrtMap& m, int x, int y,
                           std::vector<VoxCavityVertex>& out);

/// Island caps for one map row, merged: consecutive cavity columns in
/// [x0..x1] whose top sits at the SAME height with the SAME top-voxel
/// color become ONE quad (the cap is a flat surface — per-column quads
/// multiplied the mesh by the cavity area).
void voxCavityAppendRowCaps(vrtMap& m, int y, int x0, int x1,
                            std::vector<VoxCavityVertex>& out);

#endif // VOX_CAVITY_H

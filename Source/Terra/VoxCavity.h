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

// Layout kept IDENTICAL to the render stack's sVertexXYZDT2 (32 bytes) so
// the overlay publish is a bulk memcpy, not a per-vertex conversion — the
// Debug field-by-field loop cost milliseconds per flatten. uv — раскладка
// в бейк-цвета оверлея; uv2 — МИРОВЫЕ UV (x/SizeX, y/SizeY) для лайтмапа
// террейна (тот же мультипликатор, что у террейна: bake×lightmap×color).
struct VoxCavityVertex
{
	float x = 0.f;
	float y = 0.f;
	float z = 0.f;
	std::uint32_t diffuse = 0xFF000000u; // ARGB
	float u = 0.f;
	float v = 0.f;
	float u2 = 0.f;
	float v2 = 0.f;

	bool operator==(const VoxCavityVertex& o) const
	{
		return x == o.x && y == o.y && z == o.z && diffuse == o.diffuse &&
		       u == o.u && v == o.v && u2 == o.u2 && v2 == o.v2;
	}
	bool operator!=(const VoxCavityVertex& o) const { return !(*this == o); }
};

// Мировые UV вершины для лайтмапа террейна.
void voxCavityWorldUv(const vrtMap& m, float x, float y, float& u,
                      float& v);

// Текстурная раскладка цветов кепок: топ-даун текстура РОВНО W×H
// (движок требует степени двойки), клетка (x,y) — тексель (x,y), UV кепок
// — по УГЛАМ клеток: u = x/W лежит между текселями, билинейная выборка
// сглаживает границы клеток ровно как у террейна. Стены/потолки плит
// рисуются отдельной нодой БЕЗ текстуры (шейдер с пустым слотом сэмплит
// белую emptyTexture — tex*color = color), их UV не используется.
float voxCavityCapU(const vrtMap& m, int x);
float voxCavityCapV(const vrtMap& m, int y);
void voxCavityWhiteUv(const vrtMap& m, float& u, float& v);

/// Appends the cavity geometry of one map column (cell x, y) to `out`
/// (3 vertices per triangle, both windings per face). No-op when the
/// column has no transparent voxel ranges. `emitSides=false` skips the
/// side (wall) faces — the overlay merges walls across columns per
/// row/column (voxCavityAppendRowWallY/voxCavityAppendColWallX), keeping
/// the per-column chunks small; tests and one-off probes keep `true`.
void voxCavityAppendColumn(vrtMap& m, int x, int y,
                           std::vector<VoxCavityVertex>& out,
                           bool emitSides = true);

/// Прямоугольник одной серии стены (источник пикселей стенных текстур):
/// [along0..along1) — координата вдоль линии (x для +y стен, y для +x),
/// [z0..z1) — высоты; color — ЗАТЕНЁННЫЙ цвет серии (тот, что раньше
/// жил в diffuse квадов).
/// Ёмкость стенных текстур по высоте (воксели): статическая степень
/// двойки. Стены выше лимита сэмплят завёрнутые строки — карт выше 2048
/// вокселей нет (EXTDEMO: 500, движковый максимум 8191). Совпадает с
/// масштабом v в UV эмиссии стен (v = z / VOX_WALL_TEX_ZCAP).
const int VOX_WALL_TEX_ZCAP = 2048;

struct VoxWallRun
{
	int along0;
	int along1;
	int z0;
	int z1;
	std::uint32_t color;
};

/// SIDE WALLS, merged across consecutive cells (the per-column chunks
/// emit up to ~100 quads per cavity column once trucks level the ground
/// around/through an island — 700k+ vertices drawn every frame. Walls
/// are constant-color runs; merging coplanar neighbours collapses a run
/// of N identical cells into ONE quad, like the caps).
///   * Row form: the +y boundary walls owned by cells of row y; `offs`
///     is the sorted candidate list for that row (cavity columns AND
///     their solid neighbours — a plain solid cell can own a cliff wall
///     into a cavity and is not in the layer store's column list).
///     Every y-facing plane is drawn exactly once (south column's walk).
///   * Column form: the +x boundary walls owned by cells of column x;
///     `offs` sorted by y. Every x-facing plane exactly once.
///
/// Квады стен эмитятся БЕЛЫМИ с UV в текстуру (row walls: u = x/W, v =
/// z/Z; col walls: u = y/W, v = z/Z — углы по границам клеток/вокселей,
/// билинейная выборка сглаживает соседей как у террейна). Затенённый цвет
/// дублируется в `runs` (может быть nullptr) — источник пикселей той
/// текстуры. Стены и плиты внутри плит (voxCavityAppendColumn/emitSide)
/// НЕ текстурируются — рисуются вершинными цветами отдельной нодой.
void voxCavityAppendRowWallY(vrtMap& m, int y, const std::uint32_t* offs,
                             size_t n, std::vector<VoxCavityVertex>& out,
                             std::vector<VoxWallRun>* runs = nullptr);
void voxCavityAppendColWallX(vrtMap& m, int x, const std::uint32_t* offs,
                             size_t n, std::vector<VoxCavityVertex>& out,
                             std::vector<VoxWallRun>* runs = nullptr);

/// Island caps for one map row, merged: consecutive cavity columns in
/// [x0..x1] whose top sits at the SAME height with the SAME top-voxel
/// color become ONE quad (the cap is a flat surface — per-column quads
/// multiplied the mesh by the cavity area).
///
/// Цвет кепки идёт в ТЕКСТУРУ (мешер оверлея печёт топ-даун текстуру и
/// рисует кепки белыми с UV — трилинейная фильтрация сглаживает их так же,
/// как террейн), поэтому квады несут белый diffuse. `cellColors` (может
/// быть nullptr) получает ЗАТЕНЁННЫЙ цвет каждой capped-клетки строки
/// (индекс x-x0, 0xFF000000-прозрачно-чёрный для прочих) — источник пикселей
/// той самой текстуры.
void voxCavityAppendRowCaps(vrtMap& m, int y, int x0, int x1,
                            std::vector<VoxCavityVertex>& out,
                            std::vector<std::uint32_t>* cellColors = nullptr);

#endif // VOX_CAVITY_H

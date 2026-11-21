// ============================================================================
// VoxCavityOverlay — renders transparent (empty) voxel cavities in the dam
// as a voxel-segment overlay mesh on top of the heightfield terrain, for
// the GAME and the MapEditor's TerrainGpuView alike.
//
// Why: the terrain mesh (cTileMap) is a pure heightfield — one surface per
// cell — and cannot show a floating island or an interior cavity. The
// overlay triangulates the solid segments of every column that carries
// transparent voxel ranges (see Terra/VoxCavity.h) and draws them as real
// geometry: island sides and undersides, cavity floors and walls.
//
// Update flow: cTileMap::UpdateMap fires the registered callback for every
// terrain update — terraform in the game (logic thread!), brush stamps and
// world loads in the editor — with the affected rect. The rect is expanded
// by one cell (side faces depend on the neighbors) and the cavity geometry
// inside it is rebuilt incrementally (per-column chunks keyed by cell
// offset). Maps without transparent voxels cost one binary search per
// update.
//
// Thread safety: updates may arrive on the logic thread (game); the chunk
// state is mutex-guarded and the render node copies under its own lock.
// ============================================================================

#ifndef VOX_CAVITY_OVERLAY_H
#define VOX_CAVITY_OVERLAY_H

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "../inc/Umath.h"
#include "../../Terra/VoxCavity.h"

class cScene;
class cTileMap;
class cRawMeshObj;

class VoxCavityOverlay
{
public:
	VoxCavityOverlay() = default;
	~VoxCavityOverlay();
	VoxCavityOverlay(const VoxCavityOverlay&) = delete;
	VoxCavityOverlay& operator=(const VoxCavityOverlay&) = delete;

	/// Attach the render node to the scene and start listening to the
	/// tilemap updates; rebuilds the whole map once (CreateMap/SetBuffer
	/// fired its full-map update before we could register).
	void init(cScene* scene, cTileMap* tilemap);
	/// Unregister and release the node (call BEFORE the tilemap dies).
	void shutdown();

	bool active() const { return node_ != nullptr; }

	/// Called by the overlay node each frame (render thread): coalesces
	/// the pending update rects into ONE rebuild per frame at most. Without
	/// this, game terraforming — many UpdateMap callbacks per second —
	/// rebuilds the mesh on every op ("the whole world remeshes" slowdown).
	void drainPending();

private:
	static void onUpdateMap(const Vect2i& pos1, const Vect2i& pos2,
	                        void* data);
	/// Rebuild the cavity columns whose offset lies in the inclusive cell
	/// rect (already expanded by the caller for the neighbor side faces).
	void rebuildRect(int x0, int y0, int x1, int y1);

	/// Total overlay vertex count (chunks + cap rows + merged walls) -
	/// test/diagnostic support; cheap, reads the stored mesh only.
	size_t debugVertexCount() const
	{
		// A5: rebuild идёт на фоновом потоке — читаем состояние под локом.
		std::lock_guard<std::mutex> lock(mutex_);
		size_t total = 0;
		for (const Chunk& ch : chunks_) {
			total += ch.verts.size();
		}
		for (const auto& row : capRows_) {
			total += row.second.size();
		}
		for (const auto& row : wallRows_) {
			total += row.second.size();
		}
		for (const auto& col : wallCols_) {
			total += col.second.size();
		}
		return total;
	}
	void rebuildAll();


	/// Pending update rects from the tilemap callback (logic thread in the
	/// game) — drained by drainPending(), one rect at a time through the
	/// cheap reject (most game updates — moving units etc. — touch no
	/// cavity at all).
	/// A5: сама обработка ректов (rebuildRect, ~50–300 мс на больших
	/// полостях) идёт на фоновом потоке — кадр, в котором назрел drain,
	/// больше не ждёт её целиком (PreDraw → drainPending раньше встраивал
	/// rebuild в кадр, игра дёргалась на каждый drain). Поток рендера
	/// публикует только готовый снимок: Draw/PreDraw ноды берут КРАТКИЙ
	/// лок самого RawMeshObj, а не overlay-мьютекс. join в shutdown().
	std::thread worker_;
	std::mutex pendingMutex_;
	std::vector<std::pair<Vect2i, Vect2i>> pending_;
	std::chrono::steady_clock::time_point lastRebuild_{};
	// Combined heights+layers version at the last drain: pure color/vision
	// updates (moving units) bump neither — their rects are dropped whole.
	std::uint64_t lastSeenVersion_ = 0;
	std::uint64_t lastTerraVersion_ = 0;
	std::uint64_t lastLayersVersion_ = 0;


	cScene* scene_ = nullptr;
	cTileMap* tilemap_ = nullptr;
	cRawMeshObj* node_ = nullptr;

	// Per-column geometry, each column owning its vertices — an edit
	// rebuilds ONLY the columns it touches (the world is segmented, no
	// whole-mesh copy per update). The flat render buffer is concatenated
	// lazily and published to the node only when something changed.
	struct Chunk
	{
		std::uint32_t off;
		std::vector<VoxCavityVertex> verts;
		// Hash of every terrain input the mesher reads for this column
		// (own + 4-neighbor heights, light, default colors and layer
		// ranges) - a rebuild skips regenerating the chunk while the
		// hash is unchanged (game terraform rects re-fire constantly,
		// but only a handful of columns actually change).
		std::uint64_t hash = 0;
	};
	mutable std::mutex mutex_; // mutable: debugVertexCount() читает под локом
	std::vector<Chunk> chunks_; // sorted by off
	// Полный (не инкрементальный) меш ректа в ЛОКАЛЬНЫЕ контейнеры:
	// rebuildAll гонит горизонтальные полосы мира на потоках (входы —
	// только vMap, состояние оверлея не трогаем), потом мержит.
	// Строгие границы: каждый владелец строки/колонки покрывается
	// ровно один раз (без -1 расширения инкрементального пути).
	struct RectMeshes
	{
		std::vector<Chunk> chunks; // sorted by off
		std::map<int, std::vector<VoxCavityVertex>> capRows;
		std::map<int, std::vector<VoxCavityVertex>> wallRows;
		std::map<int, std::vector<VoxCavityVertex>> wallCols;
	};
	void meshRectFull(int x0, int y0, int x1, int y1,
	                  RectMeshes& out) const;
	// Flatten + setTriangles (вызывать под mutex_).
	void publishLocked();

	// Island caps, merged per map row (flat surfaces — per-column caps
	// made the mesh ~1M vertices). Regenerated for every row the dirty
	// rect spans, across the FULL row width (runs merge beyond the rect).
	std::map<int, std::vector<VoxCavityVertex>> capRows_;
	// Side walls, merged along the row/column like the caps: +y walls
	// owned by row y, +x walls owned by column x (each plane drawn once).
	// Пер-колоночные стены давали ~700k вершин КАЖДЫЙ КАДР
	// (RawMeshObj::Draw 20+ ms) — мерж схлопывает серии одинаковых
	// колонок в один квад.
	std::map<int, std::vector<VoxCavityVertex>> wallRows_;
	std::map<int, std::vector<VoxCavityVertex>> wallCols_;
	bool dirty_ = false;
};

#endif // VOX_CAVITY_OVERLAY_H

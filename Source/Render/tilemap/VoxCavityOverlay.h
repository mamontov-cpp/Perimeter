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
#include <condition_variable>
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
class cTexture;

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

	bool active() const { return chunksNode_ != nullptr; }

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
		for (const auto& kv : chunks_) {
			total += kv.second.verts.size();
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
public:
	/// bands <= 0 — auto (hardware_concurrency, ≤16); иначе принудительно
	/// (тесты детерминизма мержа полос).
	void rebuildAll(int bands = 0);
	/// Test/diagnostic: rebuildAll оставляет состояние оверлея —
	/// доступ из headless-тестов (node_ не нужен, publish no-op).
	/// Копия всего текущего меша (chunks, потом caps, walls — порядок
	/// рендер-флэттена) для анализов геометрии (z-fight проверки и т.п.).
	void debugDumpMesh(std::vector<VoxCavityVertex>& out) const;
	size_t debugSpatialQuadVertices(const VoxCavityVertex* quad,
	                                bool capSurface = false);
	/// Test hook: spatially tiled vertices in the 128x128 tiles touched by
	/// the rect. partial=true uses the incremental publish filter; false is
	/// the full-publish reference. Each part is appended in render-node order.
	void debugDumpTileMesh(int x0, int y0, int x1, int y1, bool partial,
	                       std::vector<VoxCavityVertex>& out) const;
	/// Test hook: инкрементальный rebuild одного ректа (как из drain).
	void debugRebuildRect(int x0, int y0, int x1, int y1)
	{
		rebuildRect(x0, y0, x1, y1);
	}

	/// Test hook: ЗАТЕНЁННЫЙ цвет кепки клетки (x,y) из бейка текстуры,
	/// 0x00000000 когда кепки нет. В рендере цвет кепки идёт из mipmapped
	/// текстуры (квады белые с UV), поэтому меш больше цвета не несёт.
	std::uint32_t debugCapColor(int x, int y) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const int w = static_cast<int>(capTexW_);
		if (x < 0 || y < 0 || x >= w ||
		    y >= static_cast<int>(capTexH_)) {
			return 0;
		}
		return capTex_[static_cast<size_t>(y) * w + x];
	}

private:
	/// Одна строка бейка перепечена: залить тексели клеток (row [0..W-1]
	/// цветов — по клетке, прочие в 0) и пометить текстуру грязной.
	void bakeCapRowLocked(int y, const std::vector<std::uint32_t>& cellColors,
	                      int x0 = 0);
	struct CapGpuTile;
	void syncCapTileLocked(std::uint64_t key, CapGpuTile& tile);
	void syncCapTextureLocked();
	void markCapTileDirty(int tx, int ty);
	/// Ленивое выделение бейка (headless-тесты не зовут init). Под mutex_.
	void ensureCapTexLocked();
	void workerLoop();


	/// Pending update rects from the tilemap callback (logic thread in the
	/// game) — drained by drainPending(), one rect at a time through the
	/// cheap reject (most game updates — moving units etc. — touch no
	/// cavity at all).
	// A persistent worker owns one batch at a time. A busy worker leaves
	// pending_ untouched; shutdown is the only path that joins the thread.
	std::thread worker_;
	std::condition_variable workerCv_;
	std::mutex workerMutex_;
	std::vector<std::pair<Vect2i, Vect2i>> workerRects_;
	bool workerWorkPending_ = false;
	bool workerBusy_ = false;
	bool workerStop_ = false;
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
	// Четыре ноды — по текстурному режиму:
	//   chunksNode_     — внутренние грани плит (вершинные цвета)
	//   capsNode_       — кепки (текстура capTex_)
	//   rowWallsNode_   — +y стены (интерполированные вершинные цвета)
	//   colWallsNode_   — +x стены (интерполированные вершинные цвета)
	cRawMeshObj* chunksNode_ = nullptr;
	cRawMeshObj* capsNode_ = nullptr;
	cRawMeshObj* rowWallsNode_ = nullptr;
	cRawMeshObj* colWallsNode_ = nullptr;

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
	std::map<std::uint32_t, Chunk> chunks_; // cell offset -> slab geometry
	// Полный (не инкрементальный) меш мира в ЛОКАЛЬНЫЕ контейнеры:
	// rebuildAll собирает ГЛОБАЛЬНЫЙ список кандидатов (кавити-колонки +
	// твёрдые соседи), а полосы потоков делят его: чанки — срез списка,
	// кепки/+y стены — диапазон строк, +x стены — диапазон колонок с
	// полным охватом по y. Каждая строка/колонка обсчитывается один раз с
	// полным списком — выход не зависит от числа полос (иначе мерж серий
	// на границах полос делал меш зависимым от числа ядер).
	struct RectMeshes
	{
		std::vector<Chunk> chunks; // sorted by off
		std::map<int, std::vector<VoxCavityVertex>> capRows;
		std::map<int, std::vector<VoxCavityVertex>> wallRows;
		std::map<int, std::vector<VoxCavityVertex>> wallCols;
	};
	struct SpatialTiles;
	void buildSpatialTilesLocked(const Vect2i* dirtyLo,
	                             const Vect2i* dirtyHi,
	                             SpatialTiles& out) const;
	// Build spatial tile snapshots and publish them (under mutex_).
	void publishLocked(const Vect2i* dirtyLo = nullptr,
	                   const Vect2i* dirtyHi = nullptr);

	// Island caps, merged per map row where corner heights permit (per-column
	// caps made the mesh ~1M vertices). Dirty rows regenerate only their local
	// x-span; unaffected quads are preserved or clipped at the span edges.
	std::map<int, std::vector<VoxCavityVertex>> capRows_;
	// Side walls, merged along the row/column like the caps: +y walls
	// owned by row y, +x walls owned by column x (each plane drawn once).
	// Пер-колоночные стены давали ~700k вершин КАЖДЫЙ КАДР
	// (RawMeshObj::Draw 20+ ms) — мерж схлопывает серии одинаковых
	// колонок в один квад.
	std::map<int, std::vector<VoxCavityVertex>> wallRows_;
	std::map<int, std::vector<VoxCavityVertex>> wallCols_;
	bool dirty_ = false;

	// CPU colors remain one grid for simple row updates/debugging. GPU caps
	// use dirty 256-cell tiles backed by 512x512 textures with a one-cell
	// neighbor border and edge replication. Textures are created lazily and
	// cached for reuse until shutdown; geometry tiles borrow them in Draw.
	// Walls and slabs remain untextured vertex-colored geometry.
	std::vector<std::uint32_t> capTex_;
	std::uint32_t capTexW_ = 0;
	std::uint32_t capTexH_ = 0;
	bool capTexDirty_ = false;
	struct CapGpuTile
	{
		cTexture* texture = nullptr;
		bool dirty = true;
		std::vector<std::uint32_t> upload;
		std::vector<std::uint32_t> mipA;
		std::vector<std::uint32_t> mipB;
		std::vector<std::uint32_t> dilateScratch;
	};
	std::map<std::uint64_t, CapGpuTile> capTiles_;
};

#endif // VOX_CAVITY_OVERLAY_H

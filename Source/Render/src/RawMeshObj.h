#ifndef _RAW_MESH_OBJ_H_
#define _RAW_MESH_OBJ_H_

#include "UnkObj.h"
#include "VertexFormat.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

class DrawBuffer;

class cRawMeshObj : public cUnkObj
{
public:
	using TileKey = std::uint64_t;
	using TileTriangles = std::map<TileKey, std::vector<sVertexXYZDT2>>;
	using TileTextures = std::map<TileKey, cTexture*>;

	static constexpr int kTileSize = 128;
	static constexpr size_t kChunkQuads = 15000;

	cRawMeshObj();
	~cRawMeshObj() override;

	void setTiles(TileTriangles tiles, const TileTextures& textures = {},
	              const Vect2i* dirtyLo = nullptr,
	              const Vect2i* dirtyHi = nullptr);
	void clear();

	void PreDraw(cCamera* camera) override;
	void Draw(cCamera* camera) override;

protected:
	virtual cTexture* drawLightMap() { return nullptr; }

public:
	struct Snapshot
	{
		std::vector<sVertexXYZDT2> triangles;
		Vect3f mins;
		Vect3f maxs;
	};
private:
	struct TileState
	{
		std::shared_ptr<const Snapshot> snapshot;
		cTexture* texture = nullptr;
	};
	using TileStates = std::map<TileKey, TileState>;
	struct RenderTile
	{
		std::vector<std::unique_ptr<DrawBuffer>> buffers;
		std::shared_ptr<const Snapshot> uploaded;
	};

	std::mutex mutex_;
	TileStates tiles_;
	std::map<TileKey, RenderTile> renderTiles_;
};

#endif // _RAW_MESH_OBJ_H_

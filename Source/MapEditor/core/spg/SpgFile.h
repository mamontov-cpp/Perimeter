#pragma once

#include "SpgTextCoding.h"
#include "XPrmDocument.h"

#include "BuildingCatalog.h"

#include <functional>
#include <string>
#include <vector>

// ============================================================================
// MapEditorCore — SpgFile (T8.1).
//
// Schema-level access to a battle map (.spg = MissionDescriptionPrm + SavePrm)
// on top of the byte-preserving XPrmDocument. Covers what the editor edits:
//
//   - header: worldName, version, player count / per-player data
//     (playersData), playersShufflingIndices, activePlayerID;
//   - SavePrm: per-player spawn geometry — frame position, its squad
//     (position + stablePosition), the frame slots (child units), the
//     per-player camera path — moved together as one group (T8.3);
//   - manualData.zeroLayerHeight (T8.4).
//
// Everything else in the file survives untouched (unknown/legacy nodes
// included) — that is XPrmDocument's contract.
// ============================================================================

namespace MapEditor {

class SpgFile
{
public:
    /// MissionDescriptionPrm.playersData[i]. All strings are UTF-8 on this
    /// API boundary: `name` is transcoded from/to the file encoding that
    /// load() detected (Windows-1251 for the shipped Russian maps), so the
    /// raw bytes in the document keep the original encoding on save. The
    /// identifier fields (realPlayerType/belligerent/difficulty) are ASCII.
    struct PlayerData
    {
        int playerID = 0;
        std::string realPlayerType;   ///< identifier (REAL_PLAYER_TYPE_...)
        std::string belligerent;      ///< identifier (BELLIGERENT_...)
        int colorIndex = 0;
        int clan = 0;
        std::string difficulty;       ///< identifier (DIFFICULTY_...)
        int handicap = 100;
        std::string name;             ///< UTF-8 (file-encoded on disk)

    };

    bool load(const std::string& path);
    bool save(const std::string& path) const;
    /// Rewrites the mission descriptor's originalSaveName to this map's own
    /// resource-relative path — see the implementation for why the game's
    /// "Again" restart depends on it. Called by save().
    void syncOriginalSaveName(const std::string& path) const;

    bool loaded() const { return mission_ && save_ != nullptr; }

    // -----------------------------------------------------------------------
    // MissionDescriptionPrm
    // -----------------------------------------------------------------------

    std::string version() const;
    std::string worldName() const;
    bool setWorldName(const std::string& name);
    int playerCount() const;

    PlayerData playerData(int index) const;
    bool setPlayerData(int index, const PlayerData& data);

    /// playersShufflingIndices (identity after setPlayerCount).
    std::vector<int> shufflingIndices() const;
    bool setShufflingIndices(const std::vector<int>& indices);

    // -----------------------------------------------------------------------
    // SavePrm
    // -----------------------------------------------------------------------

    /// manualData.zeroLayerHeight (absent -> 0).
    int zeroLayerHeight() const;
    bool setZeroLayerHeight(int height);

    /// Frame anchor of a player's spawn group (map coordinates).
    bool playerAnchor(int playerIndex, double& x, double& y) const;

    /// The first path point of the player's camera spline
    /// (manualData.cameras element `playerIndex`): position{x,y} plus
    /// psi/theta (radians, the game CameraCoordinate convention) and
    /// distance. False when the cameras array is absent/empty or the spline
    /// path has no points (the shipped Worlds/*.spg mission stubs have
    /// "cameras = { 0; }").
    bool playerCamera(int playerIndex, double& x, double& y, double& psi,
                      double& theta, double& distance) const;

    /// Move the whole spawn group of a player by the delta. `dz` shifts
    /// every 3D z (the caller derives it from the terrain height difference
    /// under the anchor, so the models keep their vertical offset).
    bool movePlayerGroup(int playerIndex, double dx, double dy, double dz);

    /// Place the group's frame anchor at (x, y); z of every 3D position
    /// shifts by `dz` (see movePlayerGroup).
    bool setPlayerAnchor(int playerIndex, double x, double y, double dz);

    /// Like setPlayerAnchor, but records the move in a per-document undo
    /// history so undoPlayerMove()/redoPlayerMove() can walk it back (the
    /// Place Spawn mode uses this). A new move discards the redo tail. The
    /// history is cleared by load(). False when the anchor cannot be read.
    bool setPlayerAnchorUndoable(int playerIndex, double x, double y,
                                 double dz);
    /// Revert the last recorded anchor move (the whole group moves back).
    bool undoPlayerMove();
    /// Re-apply the last undone anchor move.
    bool redoPlayerMove();
    bool canUndoPlayerMove() const { return !playerMoveUndo_.empty(); }
    bool canRedoPlayerMove() const { return !playerMoveRedo_.empty(); }
    /// Drop both histories (load() does this; save() keeps them).
    void clearPlayerMoveHistory();

    /// Start-landing normalization (run before saving, together with
    /// setFramesInstalledForStart which stores the frames "installed"):
    ///
    /// The game installs a frame at battle start at the STORED z (the frame
    /// body has gravity = 0 — RigidBodyPrm "Unit Frame"), and only while the
    /// frame bottom is at or slightly above the zero layer
    /// (`position().z + boxMin().z - vMap.hZeroPlast < 2` is one-sided,
    /// IronFrame.cpp:173). The grounded pose of the frame model is
    /// `terrain height under the frame + kFrameOriginGroundOffset` — this
    /// holds across every shipped attached=true map regardless of faction
    /// (PENTOLL/XELA/COMPASS: frame z - ground = 114.58). The COMPASS
    /// template stores a HOVER pose instead (ground + 114.58 + ~37, the
    /// RigidBodyPrm flying_height = 40), which is correct only while the
    /// frame waits to be installed — installed from that z it hangs in the
    /// air (the EXTDEMO/MapDemo bug).
    ///
    /// So, per player: the frame z (and the packed frame slots riding with
    /// it) is set to the grounded pose over `terrainHeight(frameX, frameY)`
    /// (falling back to `zeroLayerHeight` off-map), and the squad z is
    /// pulled down to `zeroLayerHeight` when it rests above it (a squad on
    /// low terrain below the zero layer stays put — the one-sided game
    /// check counts it as landed). `terrainHeight` returns whole-voxel
    /// height or a negative value off-map. Idempotent.
    static constexpr double kFrameOriginGroundOffset = 114.58;
    bool restPlayersOnZeroLayer(
        int zeroLayerHeight,
        const std::function<int(int, int)>& terrainHeight);

    /// "Installed at start" normalization (run before saving, after
    /// restPlayersOnZeroLayer): every shipped battle map except the
    /// COMPASS family stores the player frames as attached+powered with a
    /// unique positive zeroLayerCounter — universalLoad then starts them
    /// ATTACHED (the Quant attach path rests the frame on the zero layer
    /// immediately, no hovering until a script/player installs it) and
    /// loadZeroLayer pre-paints the zeroplast disc under each frame via
    /// placeZeroLayer. The COMPASS template (and every editor map cloned
    /// from it) has attached = false / powered = false / counter = 0 — the
    /// frame hovers in ISOLATED mode at the stored z until manually
    /// installed (the EXTDEMO/MapDemo "frame floats at start" bug). This
    /// brings every frame to the installed state: attached = true,
    /// powered = true, zeroLayerCounter = a fresh unique id (existing
    /// nonzero counters of other units are kept and excluded). Idempotent.
    bool setFramesInstalledForStart();

    // -----------------------------------------------------------------------
    // T8.4 — new battle maps from a template file
    // -----------------------------------------------------------------------

    /// Resize playersData/players/cameras/shuffling to `count` players:
    /// extra players are dropped, missing ones are cloned from the last
    /// existing entry (ids renumbered, activePlayerID = 0). False when the
    /// file is not loaded or count is out of 1..8.
    bool setPlayerCount(int count);

    /// Drops ONE player (arbitrary index) from playersData/players/cameras,
    /// renumbers the remaining ids and rebuilds the identity shuffling —
    /// the arbitrary-index variant of setPlayerCount's shrink path. False
    /// when the index is invalid or the last player would be removed.
    bool removePlayer(int index);

    /// manualData.cameras (nullptr when absent) — used by validation.
    XPrmObject* camerasArray();

    // -----------------------------------------------------------------------
    // Pre-placed world buildings — SavePrm.nobodysBuildings (the "neutral
    // buildings" the game loads via terPlayer::loadUnit, Source/Game/
    // Player.cpp:998). Serialized as typed "struct SaveUnitBuildingData" /
    // "struct SaveUnitCommandCenterData" (Command Center) /
    // "struct SaveUnitProtectorData" (Core) array elements — see the shipped
    // mission maps (e.g. Missions/02PYRAMIDA.spg). The panel edits only the
    // geometry (position, rotation), the HP (damageMolecula) and the
    // building set; every other field of a placed building is written with
    // the game's own defaults for a constructed building.
    // -----------------------------------------------------------------------

    struct PlacedBuilding
    {
        std::size_t index = 0; ///< element index in nobodysBuildings.objects.
        int unitId = 0;
        int attributeId = -1;  ///< terUnitAttributeID (see BuildingCatalog).
        double x = 0;
        double y = 0;
        double z = 0;
        double angleDeg = 0;   ///< rotation around Z.
        int hp = 100;          ///< percent (damageMolecula).
    };

    /// Current buildings in file order (skipped: entries whose attributeID
    /// is not in the placeable catalog — they are preserved untouched).
    std::vector<PlacedBuilding> buildings() const;

    /// Appends a building entry. `unitId` must be unique in the document
    /// (see maxUnitId()). z is stored as given (the caller adds the
    /// building's catalog zOffset to the zero layer height).
    bool addBuilding(const PlacedBuilding& building);
    /// Moves/rotates an existing building.
    bool setBuildingPose(std::size_t index, double x, double y,
                         double angleDeg);
    /// Rewrites the damageMolecula for an existing building.
    bool setBuildingHp(std::size_t index, int hp);
    /// Removes an existing building.
    bool removeBuilding(std::size_t index);
    /// Maximum unitID anywhere under SavePrm (frames, squads, slots,
    /// buildings) — new units take maxUnitId() + 1.
    int maxUnitId() const;

    // -----------------------------------------------------------------------
    // Scum nests — paired SCUM_SPOT mounds (SavePrm.nobodysBuildings,
    // attributes 43..46) + FILTH_SPOT spawners (SavePrm.filth.objects,
    // "struct SaveUnitFilthData"), wired together by a generated
    // Scripts/Triggers/<world>.scr chain registered in every player's
    // manualData TriggerChainNames. See ScumSpotCatalog.h and
    // docs/architecture.md.
    // -----------------------------------------------------------------------

    /// Editable filth spawner parameters (SaveUnitFilthData). Enum values are
    /// stored as ordinals; filthType names go through kFilthTypeNames.
    struct FilthParams
    {
        int filthType = 1;          ///< terFilthSpotID (1 = ANTS).
        float attackWidth = -1.f;
        float attackDirection = 0.f;
        bool sleep = false;
        float firstSleepTime = 0.f;
        float sleepPeriod = 20.f;
        float deltaSleepPeriod = 0.f;
        float attackPeriond = 20.f;   ///< [sic] — the game's field name.
        float deltaAttackPeriond = 0.f;
        int creatureNum = 10;
        int activatingUnit = 0;       ///< terUnitAttributeID (0 = NONE).
        float activatingDistance = 100.f;
        int attackPlayer = 0;         ///< terFilthAttackType (0 = ALL).
        bool initialGeoprocess = true;
        bool createFirst = true;
    };

    struct ScumSpot
    {
        std::size_t index = 0;   ///< row in scumSpots() (not a file offset).
        int buildingIndex = -1;  ///< element index in nobodysBuildings.objects.
        int filthIndex = -1;     ///< element index in filth.objects (-1 = none).
        int attributeId = 43;
        int unitId = 0;
        double x = 0, y = 0, z = 0;
        double angleDeg = 0;
        int hp = 100;
        bool paired = false;     ///< a FILTH_SPOT spawner exists beneath.
        std::string label;       ///< the nest's identity label (see below).
        bool moundLabeled = false; ///< the MOUND carries `label` (trigger-
                                   ///  capable; foreign maps may pair mounds
                                   ///  with no label of their own).
        FilthParams filth;
    };

    /// Mounds of the four SCUM_SPOT types in nobodysBuildings, each paired
    /// with the nearest FILTH_SPOT spawner within kPairDistance (editor
    /// placements coincide exactly). Mounds without a nearby spawner are
    /// listed unpaired (paired = false, filthIndex = -1).
    std::vector<ScumSpot> scumSpots() const;

    /// Appends the mound + its spawner. Fills unitId (mound and spawner get
    /// maxUnitId()+1 / +2), label ("scum_spot_<unitId>") and the file rows.
    /// `spot.z` must already include the catalog zOffset.
    bool addScumSpot(ScumSpot& spot);
    /// Moves/rotates the mound AND its paired spawner together.
    bool setScumSpotPose(std::size_t index, double x, double y,
                         double angleDeg);
    bool setScumSpotHp(std::size_t index, int hp);
    /// Rewrites the paired spawner's parameters (unpaired spots: false).
    bool setScumSpotFilth(std::size_t index, const FilthParams& filth);
    /// Removes the mound and its paired spawner.
    bool removeScumSpot(std::size_t index);

    /// Migrates the pre-fix label scheme (label on the spawner only) to the
    /// current one: the label moves to the MOUND (watched by the kill
    /// condition) and the spawner gets "<label>_gen" (the action target).
    /// Idempotent; runs before saving. Returns true when anything changed.
    bool normalizeScumNestLabels();

    // -----------------------------------------------------------------------
    // Static nature objects — SavePrm.environment ("struct
    // SaveUnitNatureData": SaveUnitData base + modelName/visible/natureFlag/
    // chain fields). The per-instance model comes from the save entry (the
    // attribute's own model is a placeholder); `radius` scales the model
    // (scale = radius / native bound radius, Nature.cpp setModelName).
    // HP is not editable — always a full damageMolecula. z is stored and
    // used verbatim (no re-snap on load).
    // -----------------------------------------------------------------------

    struct NatureObject
    {
        std::size_t index = 0;  ///< row in natureObjects().
        int unitId = 0;
        std::string modelName;  ///< M3D path, e.g. RESOURCE\Models\...
        double x = 0, y = 0, z = 0;
        double angleDeg = 0;
        double radius = 10.0;
        bool destroyable = true;   ///< NATURE_FLAG_DESTROY.
        bool collisions = true;    ///< NATURE_FLAG_REAL.
        bool lighting = false;     ///< NATURE_FLAG_LIGHT.
        std::string label;
    };

    std::vector<NatureObject> natureObjects() const;
    /// Appends an entry. Fills unitId (maxUnitId()+1) and the file row.
    bool addNatureObject(NatureObject& object);
    /// Moves/rotates/re-heights an existing object.
    bool setNatureObjectPose(std::size_t index, double x, double y, double z,
                             double angleDeg);
    /// Rewrites the size and the natureFlag bits.
    bool setNatureObjectShape(std::size_t index, double radius,
                              bool destroyable, bool collisions,
                              bool lighting);
    bool removeNatureObject(std::size_t index);

    /// Adds/removes `chainFile` (e.g. "EXTDEMO.scr") in EVERY player's
    /// TriggerChainNames, and purges it from the legacy lowercase
    /// triggerChainNames. Add is a no-op when already registered. Call before
    /// save(); pair with writing/deleting the .scr file itself.
    bool syncScumTriggerChain(const std::string& chainFile, bool enable);

    /// The generated trigger chain text (TriggerChain XPrm serialization):
    /// START -> per-pair activate/deactivate triggers watching the mound's
    /// attribute near the labeled spawner (50 u radius), mirroring the
    /// shipped 03isoland.scr pattern.
    std::string buildScumTriggerScript(const std::string& chainName) const;

    /// The terFilthSpotID table for UI combos (ordinal -> identifier).
    static int filthTypeCount();
    static const char* filthTypeIdentifier(int ordinal);

private:
    XPrmObject* playersDataArray();
    XPrmObject* playersArray();
    XPrmObject* playerFrame(int playerIndex);
    static XPrmObject* elementObject(XPrmObject* array, int index);
    XPrmObject* filthArray(bool create);

    /// One recorded spawn relocation: the delta passed to movePlayerGroup.
    /// Undo applies the negated delta, redo re-applies it — the values are
    /// plain shifts, so the round trip restores the stored floats exactly
    /// (no absolute re-write).
    struct PlayerMoveStep
    {
        int playerIndex = 0;
        double dx = 0;
        double dy = 0;
        double dz = 0;
    };
    std::vector<PlayerMoveStep> playerMoveUndo_;
    std::vector<PlayerMoveStep> playerMoveRedo_;

    mutable XPrmDocument doc_; // syncOriginalSaveName patches the raw text
    XPrmObject* mission_ = nullptr;
    XPrmObject* save_ = nullptr;
    /// Encoding of the editable strings (player names) in the loaded file —
    /// detected at load() from the names, preserved by save().
    SpgTextEncoding textEncoding_ = SpgTextEncoding::Utf8;
};

} // namespace MapEditor

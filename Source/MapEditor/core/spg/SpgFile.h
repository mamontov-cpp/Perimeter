#pragma once

#include "XPrmDocument.h"

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
    /// MissionDescriptionPrm.playersData[i].
    struct PlayerData
    {
        int playerID = 0;
        std::string realPlayerType;   ///< identifier (REAL_PLAYER_TYPE_...)
        std::string belligerent;      ///< identifier (BELLIGERENT_...)
        int colorIndex = 0;
        int clan = 0;
        std::string difficulty;       ///< identifier (DIFFICULTY_...)
        int handicap = 100;
        std::string name;

    };

    bool load(const std::string& path);
    bool save(const std::string& path) const;
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

    // -----------------------------------------------------------------------
    // T8.4 — new battle maps from a template file
    // -----------------------------------------------------------------------

    /// Resize playersData/players/cameras/shuffling to `count` players:
    /// extra players are dropped, missing ones are cloned from the last
    /// existing entry (ids renumbered, activePlayerID = 0). False when the
    /// file is not loaded or count is out of 1..8.
    bool setPlayerCount(int count);

    /// manualData.cameras (nullptr when absent) — used by validation.
    XPrmObject* camerasArray();

private:
    XPrmObject* playersDataArray();
    XPrmObject* playersArray();
    XPrmObject* playerFrame(int playerIndex);
    static XPrmObject* elementObject(XPrmObject* array, int index);

    XPrmDocument doc_;
    XPrmObject* mission_ = nullptr;
    XPrmObject* save_ = nullptr;
};

} // namespace MapEditor

#pragma once

#include <string>

// ============================================================================
// MapEditorCore — GameCameraPreset.
//
// The "camera like in game" preset of the viewport: the main game's camera
// model (terCameraType, Source/Game/CameraManager.cpp) set up the way the
// game itself sets it for the first player of a world.
//
// The game picks a battle camera through the ActionSetCamera trigger: it
// looks up the manualData.cameras spline named "Camera<playerIndex>"
// ("UserCamera<playerIndex>" first in saved games). When the spline exists
// and useAsSpline is false, the camera coordinate is its first path point —
// position{x,y} + psi/theta (radians) + distance. The shipped worlds carry
// such splines in RESOURCE/Multiplayer/<world>.spg (FORTOP's first player:
// psi=1.56223, theta=0.314159, distance=1000 — an oblique view over the
// spawn), while the RESOURCE/Worlds/<world>.spg mission stubs have an empty
// cameras array ("cameras = { 0; }" — the first scalar of an XPrm
// collection is its element count) and no frame units.
//
// Without any camera data the game camera stays at the terCameraType
// constructor defaults — psi=0, theta=0 (nadir), distance=300
// (CAMERA_ZOOM_MIN) — looking at (500,500); the preset then aims at the
// provided fallback centre instead.
// ============================================================================

namespace MapEditor {

struct GameCameraPreset
{
    double centerX = 0;      ///< look-at x, map cells
    double centerY = 0;      ///< look-at y, map cells
    double psiDeg = 0;       ///< CameraCoordinate psi, degrees
    double thetaDeg = 0;     ///< CameraCoordinate theta (0 = nadir), degrees
    double distance = 300;   ///< CameraCoordinate distance (the game zoom)
    /// True when the whole state (centre + angles + distance) came from the
    /// first player's camera spline of a world .spg; false — the game
    /// defaults over the fallback centre.
    bool fromSpg = false;
};

/// Resolve the preset for a world: read the first player's camera from the
/// world's .spg files, first camera spline point — the same lookup the game
/// performs at battle start. Candidate files (first existing wins):
/// RESOURCE/Multiplayer/<worldName>.spg, RESOURCE/Battle/<worldName>.spg,
/// RESOURCE/Worlds/<worldName>.spg; inside a file the cameras array is
/// tried, then the first player's frame anchor. When nothing is found the
/// game defaults over (fallbackCenterX, fallbackCenterY) are returned.
GameCameraPreset resolveGameCameraPreset(const std::string& gameDir,
                                         const std::string& worldName,
                                         double fallbackCenterX,
                                         double fallbackCenterY);

} // namespace MapEditor

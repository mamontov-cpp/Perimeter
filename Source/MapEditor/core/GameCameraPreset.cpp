// ============================================================================
// MapEditorCore — GameCameraPreset implementation.
// ============================================================================

#include "GameCameraPreset.h"

#include "spg/SpgFile.h"

#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace MapEditor {

namespace {

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

/// Try one .spg file: the first camera spline point, then the player frame
/// anchor. Returns true when the preset was filled from the file.
bool applySpgFile(const fs::path& path, GameCameraPreset& preset)
{
    SpgFile spg;
    if (!spg.load(path.u8string())) {
        return false;
    }

    // The per-player camera spline ("Camera0") — the exact state
    // ActionSetCamera puts the game camera into at battle start.
    double x = 0;
    double y = 0;
    double psi = 0;
    double theta = 0;
    double distance = 0;
    if (spg.playerCamera(0, x, y, psi, theta, distance)) {
        preset.centerX = x;
        preset.centerY = y;
        preset.psiDeg = psi * kRadToDeg;
        preset.thetaDeg = theta * kRadToDeg;
        preset.distance = distance;
        preset.fromSpg = true;
        return true;
    }

    // No camera spline — at least centre on the first player's frame.
    if (spg.playerAnchor(0, x, y)) {
        preset.centerX = x;
        preset.centerY = y;
        preset.fromSpg = true;
        return true;
    }
    return false;
}

} // namespace

GameCameraPreset resolveGameCameraPreset(const std::string& gameDir,
                                         const std::string& worldName,
                                         double fallbackCenterX,
                                         double fallbackCenterY)
{
    GameCameraPreset preset;
    preset.centerX = fallbackCenterX;
    preset.centerY = fallbackCenterY;
    if (worldName.empty()) {
        return preset;
    }

    // The lookup order follows where the game keeps playable maps for a
    // world; Multiplayer is where the per-player cameras live.
    const fs::path root = fs::u8path(gameDir) / "RESOURCE";
    const char* dirs[] = {"Multiplayer", "Battle", "Worlds"};
    for (const char* dir : dirs) {
        const fs::path spgPath = root / dir /
                                 fs::u8path(worldName + ".spg");
        if (fs::exists(spgPath) && applySpgFile(spgPath, preset)) {
            return preset;
        }
    }
    return preset;
}

} // namespace MapEditor

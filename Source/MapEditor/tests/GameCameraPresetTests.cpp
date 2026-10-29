// ============================================================================
// "Camera like in game" preset (GameCameraPreset).
//
//   72  resolveGameCameraPreset: the first player's camera spline of the
//       shipped FORTOP world (RESOURCE/Multiplayer/FORTOP.spg, "Camera0")
//       becomes the full preset (centre + psi/theta + distance); worlds
//       without any .spg fall back to the given centre with the game
//       start-of-battle defaults.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "errh_wrapper.h"

#include "GameCameraPreset.h"
#include "spg/SpgFile.h"

#include <cmath>
#include <string>

namespace {

bool nearly(double a, double b, double eps = 1e-3)
{
    return std::fabs(a - b) < eps;
}

} // namespace

bool runGameCameraPresetTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    // --- SpgFile::playerCamera reads the FORTOP first-player spline.
    MapEditor::SpgFile spg;
    const std::string spgPath =
        (ctx.gameDir / "RESOURCE" / "Multiplayer" / "FORTOP.spg").u8string();
    TEST_REQUIRE(spg.load(spgPath), "Multiplayer/FORTOP.spg load failed.");
    double ax = 0;
    double ay = 0;
    double psi = 0;
    double theta = 0;
    double distance = 0;
    TEST_REQUIRE(spg.playerCamera(0, ax, ay, psi, theta, distance),
                 "player 0 camera spline missing.");
    TEST_REQUIRE(nearly(ax, 354.791) && nearly(ay, 306.425),
                 "camera 0 position is (%f, %f), expected ~(354.79, 306.43).",
                 ax, ay);
    TEST_REQUIRE(nearly(psi, 1.56223, 1e-4) && nearly(theta, 0.314159, 1e-4) &&
                     nearly(distance, 1000.0, 1e-2),
                 "camera 0 angles/distance mismatch (psi=%f theta=%f d=%f).",
                 psi, theta, distance);

    // --- The preset resolver picks the same camera for the FORTOP world.
    const MapEditor::GameCameraPreset fortop =
        MapEditor::resolveGameCameraPreset(ctx.gameDir.u8string(), "FORTOP",
                                           1.0, 2.0);
    TEST_REQUIRE(fortop.fromSpg,
                 "FORTOP has a camera spline — fromSpg must be true.");
    TEST_REQUIRE(nearly(fortop.centerX, ax) && nearly(fortop.centerY, ay),
                 "preset centre (%f, %f) differs from the spline (%f, %f).",
                 fortop.centerX, fortop.centerY, ax, ay);
    TEST_REQUIRE(nearly(fortop.psiDeg, psi * 180.0 / 3.14159265358979323846,
                        1e-2) &&
                     nearly(fortop.thetaDeg,
                            theta * 180.0 / 3.14159265358979323846, 1e-2) &&
                     nearly(fortop.distance, 1000.0, 1e-2),
                 "preset angles/distance not converted from radians.");

    // --- A world without any .spg: fallback centre + game defaults.
    const MapEditor::GameCameraPreset fallback =
        MapEditor::resolveGameCameraPreset(ctx.gameDir.u8string(),
                                           "NO_SUCH_WORLD", 11.0, 22.0);
    TEST_REQUIRE(!fallback.fromSpg, "missing .spg must fall back.");
    TEST_REQUIRE(nearly(fallback.centerX, 11.0) &&
                     nearly(fallback.centerY, 22.0),
                 "fallback centre not preserved.");
    TEST_REQUIRE(nearly(fallback.psiDeg, 0.0) &&
                     nearly(fallback.thetaDeg, 0.0) &&
                     nearly(fallback.distance, 300.0),
                 "game defaults are psi=0 theta=0 distance=300.");

    // --- The Worlds/FORTOP.spg mission stub has no cameras and no frames:
    // resolver must not use it (Multiplayer wins first anyway), direct read
    // reports no camera.
    MapEditor::SpgFile stub;
    const std::string stubPath =
        (ctx.gameDir / "RESOURCE" / "Worlds" / "FORTOP.spg").u8string();
    TEST_REQUIRE(stub.load(stubPath), "Worlds/FORTOP.spg load failed.");
    TEST_REQUIRE(!stub.playerCamera(0, ax, ay, psi, theta, distance),
                 "the mission stub must not expose a camera spline.");

    // --- Empty world name: no path probing, straight fallback.
    const MapEditor::GameCameraPreset empty =
        MapEditor::resolveGameCameraPreset(ctx.gameDir.u8string(), "", 5.0,
                                           6.0);
    TEST_REQUIRE(!empty.fromSpg && nearly(empty.centerX, 5.0) &&
                     nearly(empty.centerY, 6.0),
                 "empty world name must fall back.");

    return true;
}

MAP_EDITOR_REGISTER_TEST("Game camera preset", 72, runGameCameraPresetTests);

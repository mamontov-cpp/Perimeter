// ============================================================================
// T8.4 — the File/New Battle Map chain on a wizard-created world:
// New World wizard → SpgFile from a template battle map (world name,
// player count, zeroLayerHeight from the world, spawn anchors moved with
// terrain z-correction) → save → reload with the new settings intact.
//
//   72  full-chain regression for the "New Battle Map does nothing" report
//       (the original bug was UI-side — a stale `ok` flag silently dropped
//       the pre-filled name — this guards the file-level chain it drives).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "WorldCreation.h"
#include "errh_wrapper.h"
#include "spg/SpgFile.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif
#include "terra.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

bool runNewBattleMapFlowTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    // Content root with the wizard's template world, a template battle map
    // and the shared resources — the layout the app flow works with.
    const fs::path root = ctx.tempRoot / "newbattlemap";
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "RESOURCE" / "Worlds", ec);
        fs::create_directories(root / "RESOURCE" / "Battle", ec);
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx", root / "RESOURCE" / "GeoTx");
        if (fs::exists(ctx.gameDir / "Perimeter.ini")) {
            fs::copy_file(ctx.gameDir / "Perimeter.ini",
                          root / "Perimeter.ini",
                          fs::copy_options::overwrite_existing, ec);
        }
        fs::copy_file(ctx.gameDir / "RESOURCE" / "Battle" / "COMPASS.spg",
                      root / "RESOURCE" / "Battle" / "COMPASS.spg",
                      fs::copy_options::overwrite_existing, ec);
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.tempWorldDir, root / "RESOURCE" / "Worlds" / ctx.worldName);
    }

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(root.u8string()), "setGameDir.");
    TEST_REQUIRE(session.scanWorlds(), "scanWorlds.");

    // Create the new world through the New World wizard's code path
    // (unique name: a leftover folder from an earlier run must not fail
    // the creation).
    const std::string worldName =
        "MAPDEMO" + std::to_string(std::time(nullptr) % 100000);
    MapEditor::NewWorldParams params;
    params.name = worldName;
    params.mapPowerX = 11;
    params.mapPowerY = 11;
    params.hZeroPlast = 30;
    params.geoLayerHeight = 20;
    params.flatSurface = true;
    const MapEditor::WorldCreateResult created =
        MapEditor::createNewWorld(session, params, ctx.worldName);
    TEST_REQUIRE(created.ok, "world creation failed: %s",
                 created.error.c_str());

    // ---- The exact flow of File/New Battle Map ----------------------------
    MapEditor::SpgFile map;
    const std::string templatePath =
        (root / "RESOURCE" / "Battle" / "COMPASS.spg").u8string();
    TEST_REQUIRE(map.load(templatePath), "template load failed.");

    TEST_REQUIRE(map.setWorldName(session.currentWorldName()),
                 "setWorldName failed.");
    TEST_REQUIRE(map.setPlayerCount(4), "setPlayerCount failed.");
    TEST_REQUIRE(map.setZeroLayerHeight(session.zeroLayerHeight()),
                 "setZeroLayerHeight failed.");

    const int mapW = static_cast<int>(session.mapWidth());
    const int mapH = static_cast<int>(session.mapHeight());
    const double radiusX = mapW * 0.35;
    const double radiusY = mapH * 0.35;
    for (int i = 0; i < 4; ++i) {
        double ax = 0;
        double ay = 0;
        TEST_REQUIRE(map.playerAnchor(i, ax, ay), "playerAnchor failed.");
        const int oldTerrain =
            session.terrainHeightAt(int(std::lround(ax)),
                                    int(std::lround(ay)));
        const double angle = 2.0 * 3.14159265358979323846 * i / 4;
        const double nx = mapW / 2.0 + radiusX * std::cos(angle);
        const double ny = mapH / 2.0 + radiusY * std::sin(angle);
        const int newTerrain =
            session.terrainHeightAt(int(std::lround(nx)),
                                    int(std::lround(ny)));
        const double dz =
            (oldTerrain < 0 || newTerrain < 0)
                ? 0.0
                : static_cast<double>(newTerrain - oldTerrain);
        TEST_REQUIRE(map.setPlayerAnchor(i, nx, ny, dz),
                     "setPlayerAnchor failed.");
    }

    const fs::path target = root / "RESOURCE" / "Battle" / "MYMAP.spg";
    TEST_REQUIRE(map.save(target.u8string()), "map save failed.");

    // Re-load: the saved map must carry the new world and spawn layout.
    MapEditor::SpgFile check;
    TEST_REQUIRE(check.load(target.u8string()) && check.loaded(),
                 "the saved map does not reload.");
    TEST_REQUIRE(check.worldName() == worldName,
                 "worldName is '%s', expected '%s'.",
                 check.worldName().c_str(), worldName.c_str());
    TEST_REQUIRE(check.playerCount() == 4, "player count lost.");
    TEST_REQUIRE(check.zeroLayerHeight() == params.hZeroPlast,
                 "zeroLayerHeight is %d, expected %d.",
                 check.zeroLayerHeight(), params.hZeroPlast);
    double ax = 0;
    double ay = 0;
    TEST_REQUIRE(check.playerAnchor(0, ax, ay), "anchor missing after save.");
    TEST_REQUIRE(std::abs(ax - (mapW / 2.0 + radiusX)) < 1.0 &&
                     std::abs(ay - mapH / 2.0) < 1.0,
                 "spawn 0 is at (%f, %f), expected the ellipse anchor.",
                 ax, ay);
    return true;
}

MAP_EDITOR_REGISTER_TEST("New battle map flow", 72, runNewBattleMapFlowTests);

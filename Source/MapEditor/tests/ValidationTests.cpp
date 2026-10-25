// ============================================================================
// T9 — validation framework.
//
//   75  validateWorldFiles on the temp world (no errors), volume balance on
//       a fresh flat 64x64 world (dig-only imbalance warning, tolerant mode
//       info), battle map validation on COMPASS.spg (clean; error for a
//       missing world and an out-of-bounds spawn), autofixes (shuffling
//       identity + zeroLayerHeight).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "Validation.h"
#include "WorldCreation.h"
#include "WorldFiles.h"
#include "errh_wrapper.h"
#include "spg/SpgFile.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

bool buildRoot(const fs::path& gameDir, const fs::path& templateDir,
               const std::string& worldName, const fs::path& root)
{
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(gameDir / "RESOURCE" / "GeoTx",
                                             root / "RESOURCE" / "GeoTx");
    if (fs::exists(gameDir / "Perimeter.ini")) {
        fs::copy_file(gameDir / "Perimeter.ini", root / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return false;
        }
    }
    MapEditor::Tests::copyDirectoryRecursive(
        templateDir, root / "RESOURCE" / "Worlds" / worldName);
    return true;
}

} // namespace

bool runValidationTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempWorldDir.empty() && !ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the temporary world.");

    // --- T9.2: static world checks on the saved temp world -----------------
    {
        const MapEditor::WorldFiles files(ctx.tempWorldDir);
        const MapEditor::ValidationReport report =
            MapEditor::validateWorldFiles(files);
        TEST_REQUIRE(!report.hasErrors(),
                     "the saved temp world must validate without errors "
                     "(%d errors).",
                     report.count(MapEditor::Severity::Error));
    }

    // --- T9.3: volume balance on a flat world ------------------------------
    {
        const fs::path root = ctx.tempRoot / "validation";
        TEST_REQUIRE(buildRoot(ctx.gameDir, ctx.tempWorldDir, ctx.worldName,
                               root),
                     "could not build the validation root.");

        MapEditor::EditorSession session;
        TEST_REQUIRE(session.setGameDir(root.u8string()) && session.scanWorlds(),
                     "session setup failed.");

        MapEditor::NewWorldParams params;
        params.name = "VALFLAT";
        params.mapPowerX = 6;
        params.mapPowerY = 6;
        params.hZeroPlast = 40;
        params.geoLayerHeight = 25;
        params.flatSurface = true;
        const MapEditor::WorldCreateResult created =
            MapEditor::createNewWorld(session, params, ctx.worldName);
        TEST_REQUIRE(created.ok, "flat world creation failed: %s",
                     created.error.c_str());

        long long fill = 0;
        long long dig = 0;
        MapEditor::ValidationIssue balance = MapEditor::validateVolumeBalance(
            MapEditor::VolumeBalanceOptions{}, fill, dig);
        TEST_REQUIRE(fill == 0 && dig == (40 - 25) * 64 * 64,
                     "flat-world volumes are fill=%lld dig=%lld, expected "
                     "0 / %lld.",
                     fill, dig, static_cast<long long>(15) * 64 * 64);
        TEST_REQUIRE(balance.severity == MapEditor::Severity::Warning,
                     "an all-pit world must warn about the imbalance.");

        MapEditor::VolumeBalanceOptions tolerant;
        tolerant.absoluteTolerance = 1LL << 40;
        balance = MapEditor::validateVolumeBalance(tolerant, fill, dig);
        TEST_REQUIRE(balance.severity == MapEditor::Severity::Info,
                     "a generous tolerance must yield info.");
    }

    // --- T9.4: battle map checks ------------------------------------------
    const fs::path source = ctx.gameDir / "RESOURCE" / "Battle" / "COMPASS.spg";
    TEST_REQUIRE(fs::exists(source), "COMPASS.spg missing.");
    const fs::path worldsRoot = ctx.gameDir / "RESOURCE" / "Worlds";

    {
        MapEditor::SpgFile spg;
        TEST_REQUIRE(spg.load(source.u8string()), "load failed.");
        const MapEditor::ValidationReport report =
            MapEditor::validateBattleMap(spg, worldsRoot);
        TEST_REQUIRE(!report.hasErrors(),
                     "COMPASS.spg must validate without errors (%d).",
                     report.count(MapEditor::Severity::Error));
        TEST_REQUIRE(!report.hasWarnings(),
                     "COMPASS.spg must validate without warnings (%d).",
                     report.count(MapEditor::Severity::Warning));

        // Missing world -> error.
        spg.setWorldName("NO_SUCH_WORLD");
        MapEditor::ValidationReport broken =
            MapEditor::validateBattleMap(spg, worldsRoot);
        TEST_REQUIRE(broken.hasErrors(),
                     "a missing world must be an error.");

        // Out-of-bounds spawn -> error with a "go to" cell.
        spg.setWorldName("COMPASS");
        TEST_REQUIRE(spg.setPlayerAnchor(0, -5.0, -5.0, 0.0),
                     "anchor move failed.");
        broken = MapEditor::validateBattleMap(spg, worldsRoot);
        bool found = false;
        for (const MapEditor::ValidationIssue& issue : broken.issues) {
            if (issue.severity == MapEditor::Severity::Error &&
                issue.x == 0 && issue.y == 0) {
                found = true;
            }
        }
        TEST_REQUIRE(found,
                     "an out-of-bounds spawn must be an error with a "
                     "clamped go-to cell.");

        // Autofixes: shuffling identity + zeroLayerHeight.
        TEST_REQUIRE(MapEditor::autofixZeroLayerHeight(spg, 42),
                     "autofixZeroLayerHeight failed.");
        TEST_REQUIRE(spg.zeroLayerHeight() == 42,
                     "zeroLayerHeight autofix did not stick.");
        TEST_REQUIRE(MapEditor::autofixShufflingIndices(spg),
                     "autofixShufflingIndices failed.");
        const std::vector<int> shuffling = spg.shufflingIndices();
        TEST_REQUIRE(shuffling.size() == 4 && shuffling[0] == 0 &&
                         shuffling[1] == 1 && shuffling[2] == 2 &&
                         shuffling[3] == 3,
                     "shuffling autofix must write the identity.");
    }

    return true;
}

MAP_EDITOR_REGISTER_TEST("Validation", 75, runValidationTests);

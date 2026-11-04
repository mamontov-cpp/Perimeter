// ============================================================================
// Worlds from mods — Mods/<Mod>/RESOURCE/Worlds/<World>.
//
// The game directory may contain a Mods folder that duplicates the game
// structure; worlds under Mods/<Mod>/RESOURCE/Worlds must appear in the
// scanned list as "Mod / World" (WorldInfo::modName) and load through Terra's
// content keys like base worlds. A mod world may even share the folder name
// with a base world — the loader matches wTable entries by the content key,
// not the name.
//
// Checked here on a self-contained content root:
//   * base world + a same-named world under Mods/TestMod are both scanned
//     (base first, then the mod entry with displayName "TestMod / <name>");
//   * loading the mod entry loads the MOD copy and loading the base entry
//     loads the BASE copy (told apart by an edited hZeroPlast);
//   * a game dir without Mods still scans fine (all other tests).
//
// Runs after the world round-trip (order 97) and reuses its temp world.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// Read the hZeroPlast value from a world.ini ([Global Parameters]).
bool readZeroPlast(const fs::path& iniPath, int& value)
{
    std::ifstream in(iniPath);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::string::size_type pos = line.find("hZeroPlast");
        if (pos == std::string::npos) {
            continue;
        }
        const std::string::size_type eq = line.find('=');
        if (eq == std::string::npos) {
            return false;
        }
        value = std::atoi(line.substr(eq + 1).c_str());
        return true;
    }
    return false;
}

/// Rewrite the hZeroPlast value in a world.ini.
bool writeZeroPlast(const fs::path& iniPath, int value)
{
    std::ifstream in(iniPath);
    if (!in) {
        return false;
    }
    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(in, line)) {
        if (line.find("hZeroPlast") != std::string::npos) {
            std::ostringstream out;
            out << "hZeroPlast=" << value;
            // Preserve a trailing comment if there was one.
            const std::string::size_type comment = line.find(';');
            if (comment != std::string::npos) {
                out << " " << line.substr(comment);
            }
            line = out.str();
            replaced = true;
        }
        lines.push_back(line);
    }
    in.close();
    if (!replaced) {
        return false;
    }
    std::ofstream out(iniPath, std::ios::trunc);
    for (const std::string& l : lines) {
        out << l << "\n";
    }
    return out.good();
}

bool runModWorldsTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    MapEditor::setupErrHWrapper();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "earlier tests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "earlier tests did not provide the world name.");

    // Self-contained content root: the base world plus a SAME-NAMED copy
    // under Mods/TestMod with a different hZeroPlast.
    const fs::path loadRoot = ctx.tempRoot / "load_root_mods";
    int baseZp = 0;
    int modZp = 0;
    const fs::path baseDir =
        loadRoot / "RESOURCE" / "Worlds" / fs::u8path(ctx.worldName);
    const fs::path modDir = loadRoot / "Mods" / "TestMod" / "RESOURCE" /
                            "Worlds" / fs::u8path(ctx.worldName);
    {
        std::error_code ec;
        fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
        fs::create_directories(loadRoot / "Mods" / "TestMod" / "RESOURCE" /
                                   "Worlds",
                               ec);
        TEST_REQUIRE(!ec, "could not create the mod content root.");
        MapEditor::Tests::copyDirectoryRecursive(
            ctx.gameDir / "RESOURCE" / "GeoTx", loadRoot / "RESOURCE" / "GeoTx");
        fs::copy_file(ctx.gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                      fs::copy_options::overwrite_existing, ec);
        TEST_REQUIRE(!ec, "could not copy Perimeter.ini into the load root.");

        MapEditor::Tests::copyDirectoryRecursive(ctx.tempWorldDir, baseDir);
        MapEditor::Tests::copyDirectoryRecursive(ctx.tempWorldDir, modDir);

        int zp = 0;
        TEST_REQUIRE(readZeroPlast(baseDir / "world.ini", zp),
                     "could not read the base world.ini hZeroPlast.");
        baseZp = zp;
        modZp = (zp + 17) % 256;
        TEST_REQUIRE(writeZeroPlast(modDir / "world.ini", modZp),
                     "could not rewrite the mod world.ini hZeroPlast.");
    }

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds(), "EditorSession::scanWorlds() failed.");

    // The list holds the base world and the mod world (same folder name),
    // base first; the mod entry carries modName and the display name.
    int baseIdx = -1;
    int modIdx = -1;
    for (size_t i = 0; i < session.worldCount(); ++i) {
        const MapEditor::WorldInfo& wi = session.getWorldList()[i];
        if (wi.name != ctx.worldName) {
            continue;
        }
        if (wi.modName.empty()) {
            baseIdx = static_cast<int>(i);
        } else if (wi.modName == "TestMod") {
            modIdx = static_cast<int>(i);
        }
    }
    TEST_REQUIRE(baseIdx >= 0, "the base world must be in the scanned list.");
    TEST_REQUIRE(modIdx >= 0,
                 "the mod world must be in the scanned list.");
    TEST_REQUIRE(session.getWorldList()[static_cast<size_t>(modIdx)]
                         .displayName() ==
                     "TestMod / " + ctx.worldName,
                 "the mod world must display as \"Mod / World\".");
    std::printf("[PASS] mod worlds appear in the scan as \"Mod / "
                "World\".\n");

    // Loading each entry resolves by content key: the mod entry loads the
    // mod copy, the base entry loads the base copy — despite the shared
    // folder name.
    TEST_REQUIRE(session.loadWorld(static_cast<size_t>(modIdx)),
                 "loading the mod world failed.");
    TEST_REQUIRE(session.zeroLayerHeight() == modZp,
                 "the mod copy's hZeroPlast (%d) must win, got %d.",
                 modZp, session.zeroLayerHeight());
    TEST_REQUIRE(session.loadWorld(static_cast<size_t>(baseIdx)),
                 "loading the base world failed.");
    TEST_REQUIRE(session.zeroLayerHeight() == baseZp,
                 "the base copy's hZeroPlast (%d) must win, got %d.",
                 baseZp, session.zeroLayerHeight());
    std::printf("[PASS] same-named base and mod worlds load their own "
                "copies (key-matched wTable lookup).\n");

    // loadWorldByName prefers the base game world on the clash.
    TEST_REQUIRE(session.loadWorldByName(ctx.worldName),
                 "loadWorldByName failed.");
    TEST_REQUIRE(session.zeroLayerHeight() == baseZp,
                 "loadWorldByName must prefer the base world.");
    std::printf("[PASS] loadWorldByName prefers the base world on a name "
                "clash.\n");

    // Save writes back into the MOD folder, not the base one: change
    // hZeroPlast live, save, and re-read both world.ini copies.
    TEST_REQUIRE(session.loadWorld(static_cast<size_t>(modIdx)),
                 "re-loading the mod world failed.");
    {
        const std::optional<MapEditor::WorldParams> params =
            session.currentWorldParams();
        TEST_REQUIRE(params.has_value(), "could not read world params.");
        const int newZp = (baseZp + 41) % 256;
        MapEditor::WorldParams edited = *params;
        edited.hZeroPlast = newZp;
        TEST_REQUIRE(session.applyWorldSettings(edited),
                     "applyWorldSettings failed.");
        TEST_REQUIRE(session.saveWorld(),
                     "saving the mod world failed.");
        int modRead = -1;
        int baseRead = -1;
        TEST_REQUIRE(readZeroPlast(modDir / "world.ini", modRead) &&
                         modRead == newZp,
                     "the MOD copy must receive the saved hZeroPlast "
                     "(got %d, want %d).",
                     modRead, newZp);
        TEST_REQUIRE(readZeroPlast(baseDir / "world.ini", baseRead) &&
                         baseRead == baseZp,
                     "the base copy must stay untouched (got %d, want %d).",
                     baseRead, baseZp);
    }
    std::printf("[PASS] saving the mod world writes into the mod folder "
                "(base copy untouched).\n");

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("Mod worlds", 97, runModWorldsTests);

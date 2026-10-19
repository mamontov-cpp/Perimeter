// ============================================================================
// MapEditorCore — Preflight validation.
// ============================================================================

#include "preflight.h"

#include <filesystem>
#include <cstdio>
#include <algorithm>

namespace fs = std::filesystem;

namespace MapEditor {

// ---------------------------------------------------------------------------
// Preflight check
// ---------------------------------------------------------------------------

PreflightResult preflightCheck(const std::string& gameDir)
{
    PreflightResult result;

    // 1. Game directory must exist and be a directory.
    if (!fs::exists(gameDir)) {
        result.errors.push_back("Game directory does not exist: " + gameDir);
        return result;
    }
    if (!fs::is_directory(gameDir)) {
        result.errors.push_back("Game path is not a directory: " + gameDir);
        return result;
    }

    // 2. RESOURCE/Worlds/ must exist.
    const fs::path worldsDir = fs::path(gameDir) / "RESOURCE" / "Worlds";
    if (!fs::exists(worldsDir)) {
        result.errors.push_back("RESOURCE/Worlds/ does not exist in: " + gameDir);
        return result;
    }
    if (!fs::is_directory(worldsDir)) {
        result.errors.push_back("RESOURCE/Worlds/ is not a directory: " + gameDir);
        return result;
    }

    // 3. At least one world folder.
    std::vector<std::string> worldNames;
    for (const auto& entry : fs::directory_iterator(worldsDir)) {
        if (entry.is_directory()) {
            worldNames.push_back(entry.path().filename().u8string());
        }
    }
    std::sort(worldNames.begin(), worldNames.end());

    if (worldNames.empty()) {
        result.errors.push_back("No world folders found in RESOURCE/Worlds/");
        return result;
    }

    result.info.push_back("Found " + std::to_string(worldNames.size()) +
                          " world(s): " + worldNames[0]);
    if (worldNames.size() > 1) {
        result.info.back() += ", " + worldNames[1] + ", ...";
    }

    // 4. First world must have output.vmp and world.ini.
    const fs::path firstWorld = worldsDir / worldNames[0];
    const fs::path vmpFile = firstWorld / "output.vmp";
    const fs::path worldIni = firstWorld / "world.ini";

    if (!fs::exists(vmpFile)) {
        result.errors.push_back("First world (" + worldNames[0] +
                                ") is missing output.vmp");
    }
    if (!fs::exists(worldIni)) {
        result.errors.push_back("First world (" + worldNames[0] +
                                ") is missing world.ini");
    }

    // Optional files — just info.
    const std::vector<std::string> optionalFiles = {
        "inGeo.act", "inDam.act", "geoPal.xml", "geoTx.xml",
        "leveledSurfaceTexture.tga", "cache.tga", "map.tga",
    };
    for (const auto& fname : optionalFiles) {
        if (fs::exists(firstWorld / fname)) {
            result.info.back() += " [OK:" + fname + "]";
        } else {
            result.warnings.push_back("First world (" + worldNames[0] +
                                      ") missing optional file: " + fname);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Printing helpers
// ---------------------------------------------------------------------------

void printPreflightResult(const PreflightResult& result, const std::string& gameDir)
{
    printf("=== Preflight check for: %s ===\n\n", gameDir.c_str());

    if (result.errors.empty() && result.warnings.empty()) {
        printf("[OK] All checks passed.\n");
    }

    for (const auto& msg : result.errors) {
        fprintf(stderr, "[ERROR] %s\n", msg.c_str());
    }
    for (const auto& msg : result.warnings) {
        fprintf(stderr, "[WARN]  %s\n", msg.c_str());
    }
    for (const auto& msg : result.info) {
        printf("[INFO]  %s\n", msg.c_str());
    }

    printf("\nResult: %s\n", result.ok ? "PASS" : "FAIL");
}

} // namespace MapEditor

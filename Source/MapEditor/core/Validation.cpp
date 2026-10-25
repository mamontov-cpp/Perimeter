// ============================================================================
// MapEditorCore — Validation implementation (T9).
// ============================================================================

#include "Validation.h"

#include "EditorSession.h"
#include "WorldParams.h"
#include "spg/SpgFile.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif
#include "terra.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace MapEditor {

namespace {

ValidationIssue issue(Severity severity, const std::string& message)
{
    ValidationIssue result;
    result.severity = severity;
    result.message = message;
    return result;
}

} // namespace

bool ValidationReport::hasErrors() const
{
    return count(Severity::Error) > 0;
}

bool ValidationReport::hasWarnings() const
{
    return count(Severity::Warning) > 0;
}

int ValidationReport::count(Severity severity) const
{
    return static_cast<int>(std::count_if(
        issues.begin(), issues.end(),
        [severity](const ValidationIssue& i) { return i.severity == severity; }));
}

// ---------------------------------------------------------------------------
// T9.2 — world files
// ---------------------------------------------------------------------------

ValidationReport validateWorldFiles(const WorldFiles& files)
{
    ValidationReport report;

    // Mandatory files (vrtMap::fullLoad's requirements, plan §2).
    struct Required
    {
        const char* name;
        bool mandatory;
    };
    const Required required[] = {
        {WorldFiles::OutputVmpFile, true},
        {WorldFiles::IniFileName, true},
        {WorldFiles::GeoPalFile, true},
        {WorldFiles::DamPalFile, true},
        {WorldFiles::GeoPalXmlFile, true},
        {WorldFiles::GeoTxXmlFile, true},
        {WorldFiles::LeveledTextureFile, true},
        {WorldFiles::CacheTgaFile, true},
        {WorldFiles::MapTgaFile, false}, // desirable (UI) but not xasserted
    };
    for (const Required& req : required) {
        if (!files.exists(req.name)) {
            report.issues.push_back(issue(
                req.mandatory ? Severity::Error : Severity::Warning,
                std::string("Missing file: ") + req.name));
        }
    }

    // world.ini: sizes, hZeroPlast.
    const std::optional<IniDocument> ini = files.readIni();
    if (!ini) {
        report.issues.push_back(
            issue(Severity::Error, "world.ini cannot be read."));
        return report;
    }
    const auto power = files.readMapPower();
    if (!power || (*power).first < 0 || (*power).first > 14 || (*power).second < 0 ||
        (*power).second > 14) {
        report.issues.push_back(issue(
            Severity::Error,
            "Map Power X/Y missing or out of range (0..14) in world.ini."));
    }

    const std::optional<int> hzp = files.readHZeroPlast();
    if (!hzp || *hzp < 0 || *hzp > 255) {
        report.issues.push_back(issue(
            Severity::Error,
            "hZeroPlast missing or out of range (0..255) in world.ini."));
    }

    // Palettes must be 768 bytes (writePalettes validates on write; check the
    // actual files here).
    GeoDamPalettes palettes;
    if (files.exists(WorldFiles::GeoPalFile) &&
        !files.readPalettes(palettes)) {
        report.issues.push_back(issue(
            Severity::Error,
            "inGeo.act / inDam.act are not 768-byte RGB palettes."));
    }

    // leveledSurfaceTexture.tga against the map size from the ini.
    if (power && files.exists(WorldFiles::LeveledTextureFile)) {
        std::vector<std::uint8_t> bytes;
        if (files.readLeveledTexture(bytes)) {
            std::vector<std::string> errors = WorldFiles::validateLeveledTexture(
                bytes, 1 << (*power).first, 1 << (*power).second);
            for (std::string& error : errors) {
                report.issues.push_back(
                    issue(Severity::Error, "leveledSurfaceTexture.tga: " + error));
            }
        }
    }

    // hardness.bin (optional): header must match the map size.
    if (files.exists(WorldFiles::HardnessFile)) {
        HardnessData hardness;
        if (power && (!files.readHardness(hardness) ||
                      hardness.width !=
                          static_cast<std::uint16_t>((1 << (*power).first) / 16) ||
                      hardness.height !=
                          static_cast<std::uint16_t>((1 << (*power).second) / 4))) {
            report.issues.push_back(issue(
                Severity::Warning,
                "hardness.bin header does not match the map size "
                "(expected width = H/16, height = V/4 grid cells)."));
        }
    }

    return report;
}

// ---------------------------------------------------------------------------
// T9.3 — volume balance
// ---------------------------------------------------------------------------

ValidationIssue validateVolumeBalance(const VolumeBalanceOptions& options,
                                      long long& fillVolume,
                                      long long& digVolume)
{
    fillVolume = 0;
    digVolume = 0;
    if (vMap.VxGBuf == nullptr) {
        return issue(Severity::Error,
                     "Volume balance needs a loaded world.");
    }
    const int hzp = static_cast<int>(vMap.hZeroPlast);
    for (unsigned y = 0; y < vMap.V_SIZE; ++y) {
        for (unsigned x = 0; x < vMap.H_SIZE; ++x) {
            const long long h =
                static_cast<long long>(vMap.SGetAlt(x, y) >> VX_FRACTION);
            if (h > hzp) {
                fillVolume += h - hzp;
            } else {
                digVolume += hzp - h;
            }
        }
    }

    const long long diff = std::llabs(fillVolume - digVolume);
    long long tolerance = options.absoluteTolerance;
    if (tolerance == 0 && digVolume > 0) {
        tolerance = static_cast<long long>(
            std::ceil(digVolume * options.imbalancePercent / 100.0));
    }
    if (tolerance < 1) {
        tolerance = 1;
    }

    if (diff <= tolerance) {
        return issue(Severity::Info,
                     "Volume balance is within tolerance (fill " +
                         std::to_string(fillVolume) + ", dig " +
                         std::to_string(digVolume) +
                         " cell*voxel, difference " + std::to_string(diff) +
                         ").");
    }
    return issue(Severity::Warning,
                 "Volume imbalance: fill " + std::to_string(fillVolume) +
                     " vs dig " + std::to_string(digVolume) +
                     " cell*voxel (difference " + std::to_string(diff) +
                     ", tolerance " + std::to_string(tolerance) +
                     "). Levelling the whole map to the zeroplast will need "
                     "import/export of soil.");
}

// ---------------------------------------------------------------------------
// T9.4 — battle maps
// ---------------------------------------------------------------------------

ValidationReport validateBattleMap(const SpgFile& battleMap,
                                   const fs::path& worldsRoot)
{
    ValidationReport report;
    if (!battleMap.loaded()) {
        report.issues.push_back(
            issue(Severity::Error, "No battle map is loaded."));
        return report;
    }

    // The world must exist in RESOURCE/Worlds.
    const std::string worldName = battleMap.worldName();
    bool worldFound = false;
    int worldWidth = 0;
    int worldHeight = 0;
    std::error_code ec;
    if (!worldName.empty() && fs::is_directory(worldsRoot, ec)) {
        for (const fs::directory_entry& entry :
             fs::directory_iterator(worldsRoot, ec)) {
            if (!entry.is_directory(ec) ||
                entry.path().filename().u8string() != worldName) {
                continue;
            }
            worldFound = true;
            const WorldFiles worldFiles(entry.path());
            if (const auto power = worldFiles.readMapPower()) {
                worldWidth = 1 << (*power).first;
                worldHeight = 1 << (*power).second;
            }
            break;
        }
    }
    if (worldName.empty() || !worldFound) {
        report.issues.push_back(issue(
            Severity::Error,
            "The world \"" + worldName + "\" does not exist in " +
                worldsRoot.u8string() + "."));
    }

    // Spawns inside the map bounds and on a readable surface.
    for (int i = 0; i < battleMap.playerCount(); ++i) {
        double x = 0;
        double y = 0;
        if (!battleMap.playerAnchor(i, x, y)) {
            report.issues.push_back(issue(
                Severity::Error,
                "Player " + std::to_string(i) + " has no frame position."));
            continue;
        }
        if (worldWidth > 0 &&
            (x < 0 || y < 0 || x >= worldWidth || y >= worldHeight)) {
            ValidationIssue out =
                issue(Severity::Error,
                      "Player " + std::to_string(i) + " spawn (" +
                          std::to_string(x) + ", " + std::to_string(y) +
                          ") is outside the world (" +
                          std::to_string(worldWidth) + " x " +
                          std::to_string(worldHeight) + ").");
            out.x = std::clamp(static_cast<int>(x), 0, worldWidth - 1);
            out.y = std::clamp(static_cast<int>(y), 0, worldHeight - 1);
            report.issues.push_back(out);
        }
    }

    // Names unique.
    std::set<std::string> names;
    for (int i = 0; i < battleMap.playerCount(); ++i) {
        const std::string name = battleMap.playerData(i).name;
        if (name.empty()) {
            report.issues.push_back(issue(
                Severity::Warning,
                "Player " + std::to_string(i) + " has an empty name."));
            continue;
        }
        if (!names.insert(name).second) {
            report.issues.push_back(issue(
                Severity::Warning,
                "Player name \"" + name + "\" is not unique."));
        }
    }

    // Cameras: one per player.
    XPrmObject* manual = const_cast<SpgFile&>(battleMap).camerasArray();
    if (!manual) {
        report.issues.push_back(
            issue(Severity::Warning, "The battle map has no cameras block."));
    } else if (static_cast<int>(manual->elementCount()) <
               battleMap.playerCount()) {
        report.issues.push_back(issue(
            Severity::Warning,
            "Cameras (" + std::to_string(manual->elementCount()) +
                ") are fewer than players (" +
                std::to_string(battleMap.playerCount()) + ")."));
    }

    // The shuffling block must cover all players.
    if (static_cast<int>(battleMap.shufflingIndices().size()) !=
        battleMap.playerCount()) {
        report.issues.push_back(issue(
            Severity::Warning,
            "playersShufflingIndices does not cover all players (use the "
            "autofix to normalize)."));
    }

    return report;
}

// ---------------------------------------------------------------------------
// T9.5 — autofixes
// ---------------------------------------------------------------------------

bool autofixShufflingIndices(SpgFile& battleMap)
{
    if (!battleMap.loaded()) {
        return false;
    }
    std::vector<int> identity(static_cast<std::size_t>(battleMap.playerCount()));
    for (std::size_t i = 0; i < identity.size(); ++i) {
        identity[i] = static_cast<int>(i);
    }
    return battleMap.setShufflingIndices(identity);
}

bool autofixZeroLayerHeight(SpgFile& battleMap, int height)
{
    return battleMap.loaded() && battleMap.setZeroLayerHeight(height);
}

} // namespace MapEditor

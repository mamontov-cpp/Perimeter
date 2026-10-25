#pragma once

#include "WorldFiles.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
// MapEditorCore — Validation (T9).
//
// Preflight-style checks over world folders and battle maps with a unified
// issue report (error / warning / info + an optional "go to" map cell).
// The checks never call ErrH.Abort paths: everything is read from the files
// directly (world.ini sizes, palette lengths, texture headers) — the loaded
// world is only needed for the volume balance scan (T9.3).
// ============================================================================

namespace MapEditor {

class SpgFile;

enum class Severity
{
    Info,
    Warning,
    Error,
};

struct ValidationIssue
{
    Severity severity = Severity::Info;
    std::string message;
    /// "Go to" target in map cells (-1,-1 = none).
    int x = -1;
    int y = -1;
};

struct ValidationReport
{
    std::vector<ValidationIssue> issues;

    bool hasErrors() const;
    bool hasWarnings() const;
    int count(Severity severity) const;
};

struct VolumeBalanceOptions
{
    /// Allowed |fill - dig| as a percent of the pit volume ("допуск").
    double imbalancePercent = 5.0;
    /// ... or an absolute cell*voxel budget (0 = not used).
    long long absoluteTolerance = 0;
};

/// T9.2 — static checks of one world folder (files, sizes, ini values,
/// palettes, leveled texture header, hardness.bin). `gameDir`/`worldsRoot`
/// optional — used for nothing file-level, kept for future checks.
ValidationReport validateWorldFiles(const WorldFiles& files);

/// T9.3 — the volume balance of the loaded world: volume of terrain above
/// the zeroplast plane against the volume of pits below it (cell area is
/// normalized to 1, the unit is cell*voxel). Requires a loaded world.
/// Returns the issues (a warning when out of tolerance) and the volumes via
/// the out-parameters.
ValidationIssue validateVolumeBalance(const VolumeBalanceOptions& options,
                                      long long& fillVolume,
                                      long long& digVolume);

/// T9.4 — checks of one battle map: the world exists in RESOURCE/Worlds,
/// the spawn anchors are inside the world bounds, per-player cameras exist,
/// player names are unique. `worldsRoot` = <game>/RESOURCE/Worlds.
ValidationReport validateBattleMap(const SpgFile& battleMap,
                                   const std::filesystem::path& worldsRoot);

// T9.5 — autofixes (each returns true when applied).
/// playersShufflingIndices := identity.
bool autofixShufflingIndices(SpgFile& battleMap);
/// manualData.zeroLayerHeight := value.
bool autofixZeroLayerHeight(SpgFile& battleMap, int height);

} // namespace MapEditor

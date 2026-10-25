#pragma once

#include "WorldFiles.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// ============================================================================
// MapEditorCore — WorldParams (T5.1).
//
// Editable snapshot of a world's parameters as they live across the three
// places the game keeps them:
//
//   world.ini [Global Parameters] — hZeroPlast, GeoNet/Section/MinimalSquare
//   powers (the powers drive terrain generation and take full effect after a
//   world reload; hZeroPlast is applied to the loaded vMap immediately);
//   world.ini [Storage] / output.vmp header — the S2T0/S2T1 compression of the
//   voxel data. The game detects the format from the 4-byte id in the file
//   header itself, the world.ini key is documentation; the editor keeps both
//   in sync;
//   world.ini [3DF Parameters] — geo-layer noise scaling (kmx/kmy/kmz) and
//   the legacy function index (the _TX3D_LIBRARY_ build reads the actual 3D
//   texture from geoTx.xml / geoLattice.bin instead);
//   Perimeter.ini [TD] Color ZeroLayer — the game-wide zeroplast tint.
//   Missing key means the game's built-in default (1 11 51 70).
// ============================================================================

namespace MapEditor {

/// Location of the game-wide zeroplast color (game root, next to RESOURCE/).
namespace GameIni {
inline constexpr const char* PerimeterIniFile   = "Perimeter.ini";
inline constexpr const char* TdSection          = "TD";
inline constexpr const char* ColorZeroLayerKey  = "Color ZeroLayer";
} // namespace GameIni

/// Zeroplast tint bytes exactly as analyzeINI parses them from
/// Perimeter.ini: "<r> <g> <b> <a>", plain 0..255. Defaults are the game's
/// built-in values (VMAP.cpp:392) used when the key is absent.
struct ZeroLayerColor
{
    int r = 1;
    int g = 11;
    int b = 51;
    int a = 70;

    bool operator==(const ZeroLayerColor& other) const
    {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

/// All parameters the World Settings dialog edits.
struct WorldParams
{
    // [Global Parameters]
    int mapPowerX = 0;             ///< H_SIZE = 1 << mapPowerX (read-only for a loaded world).
    int mapPowerY = 0;             ///< V_SIZE = 1 << mapPowerY.
    int hZeroPlast = 0;            ///< Zero-layer height in voxels (0..255).
    int geoNetPower = 6;           ///< GeoNet Power.
    int sectionSizePower = 7;      ///< Section Size Power.
    int minimalSquarePower = 4;    ///< Minimal Square Power (stored as power, the ini holds the power too).

    // [Storage]
    std::string storageVersion = "1.4";
    int compressedFormat = 0;      ///< 0 = S2T0 (uncompressed), 1 = S2T1 (zlib).

    // [3DF Parameters]
    int f3dKmx = 200;              ///< Geo noise X scaling (s_f3d::setKM).
    int f3dKmy = 200;
    int f3dKmz = 200;
    int f3dFunction = 1;           ///< 0 = marble, 1 = tree (legacy outside _GEOTOOL_).

    // Perimeter.ini [TD] Color ZeroLayer
    ZeroLayerColor zeroLayerColor;

    int mapWidth() const { return 1 << mapPowerX; }
    int mapHeight() const { return 1 << mapPowerY; }

    /// Read everything for one world folder: world.ini plus the real
    /// compression format from the output.vmp header plus the zeroplast
    /// color from <gameDir>/Perimeter.ini. std::nullopt when world.ini
    /// cannot be read.
    static std::optional<WorldParams>
    readFromWorld(const WorldFiles& files, const std::filesystem::path& gameDir);

    /// Write every editable field back (world.ini RMW + Perimeter.ini).
    /// Map powers are written too — for a loaded world they describe the
    /// world exactly as it is; the new-world wizard writes them before the
    /// first save. Returns false when any file could not be written.
    bool writeToIni(const WorldFiles& files, const std::filesystem::path& gameDir) const;

    /// Parse "r g b a" the way analyzeINI's XBuffer scan does (leading
    /// integers, whitespace separated). std::nullopt when fewer than four
    /// numbers are present.
    static std::optional<ZeroLayerColor> parseZeroLayerColor(std::string_view text);

    /// Format as "r g b a" (no trailing space — Perimeter.ini grows on every
    /// save otherwise, see the sKeyGeoPal::saveKeyGeoPal comment in f3d.cpp).
    static std::string zeroLayerColorToString(const ZeroLayerColor& color);
};

/// Read the zeroplast color from <gameDir>/Perimeter.ini. `stored` reports
/// whether the key existed; when it did not, `color` is the game default.
/// Returns false when Perimeter.ini exists but cannot be parsed at all.
bool readZeroLayerColor(const std::filesystem::path& gameDir,
                        ZeroLayerColor& color, bool& stored);

/// Write the zeroplast color into <gameDir>/Perimeter.ini, preserving the
/// rest of the file byte-for-byte (IniDocument round-trip).
bool writeZeroLayerColor(const std::filesystem::path& gameDir,
                         const ZeroLayerColor& color);

/// Compression id of an output.vmp file read from its header:
/// 0 = S2T0, 1 = S2T1, -1 = missing file / unknown id / too short.
int vmpCompressionOfFile(const std::filesystem::path& outputVmp);

/// Convert output.vmp between S2T0 (mode 0) and S2T1 (mode 1) in place —
/// the single-file equivalent of vrtMap::compressWorlds (VMAP.cpp:90), kept
/// editor-side because that one iterates every world and ErrH.Aborts on
/// failure. A file already in the requested mode is left untouched (true).
/// Uses the same XBuffer compress/uncompress as the game, so the payloads
/// stay bit-identical to what compressWorlds produces.
bool convertVmpCompression(const std::filesystem::path& outputVmp, int mode);

} // namespace MapEditor

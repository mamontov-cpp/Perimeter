#pragma once

#include "WorldFiles.h"

#include <filesystem>
#include <optional>
#include <string>

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
//   texture from geoTx.xml / geoLattice.bin instead).
//
// Note: zeroplast tint (`Perimeter.ini [TD] Color ZeroLayer`) is a
// system-wide game setting, not a per-world parameter, and is therefore
// not exposed by this struct.
// ============================================================================

namespace MapEditor {

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

    int mapWidth() const { return 1 << mapPowerX; }
    int mapHeight() const { return 1 << mapPowerY; }

    /// Read everything for one world folder: world.ini plus the real
    /// compression format from the output.vmp header. std::nullopt when
    /// world.ini cannot be read.
    static std::optional<WorldParams>
    readFromWorld(const WorldFiles& files);

    /// Write every editable field back (world.ini RMW).
    /// Map powers are written too — for a loaded world they describe the
    /// world exactly as it is; the new-world wizard writes them before the
    /// first save. Returns false when any file could not be written.
    bool writeToIni(const WorldFiles& files) const;
};

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

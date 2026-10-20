#pragma once

#include "IniDocument.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ============================================================================
// MapEditorCore — WorldFiles.
//
// File-level access to one world folder (RESOURCE/Worlds/<world>/).
//
// Everything here works on ordinary files and never touches Terra's runtime
// state, so it is usable before a world is loaded (new-world wizard, preflight
// validation) as well as after it was saved. Terra writes through the content
// root, therefore the world.ini helpers always re-read the file from disk
// (read-modify-write) instead of caching a document that Terra might replace.
//
// Covered files (names mirror the vrtMap constants in Terra/VMAP.cpp):
//   world.ini                  all sections; order, comments and unknown keys
//                              are preserved
//   inGeo.act / inDam.act      256 x 3 BGR palettes (768 bytes each)
//   hardness.bin               uint16 width/height + packed 2-bit hardness
//                              (optional file, see vrtMap::loadHardness)
//   geoPal.xml / geoTx.xml     geo layer palette and 3D texture (XML)
//   leveledSurfaceTexture.tga  indexed TGA, validated like
//                              vrtMap::loadLeveledTexture does
// ============================================================================

namespace MapEditor {

/// Section and key names of world.ini exactly as Terra reads and writes them
/// (vrtMap::analyzeINI — Terra/VMAP.cpp; s_f3d::saveVariable and
/// sKeyGeoPal — Terra/f3d.cpp). Centralised so later tasks edit the same names
/// the game uses.
namespace WorldIni {

// Sections
inline constexpr const char* GlobalParameters        = "Global Parameters";
inline constexpr const char* Storage                 = "Storage";
inline constexpr const char* CreationParameters      = "Creation Parameters";
inline constexpr const char* RenderingParameters     = "Rendering Parameters";
inline constexpr const char* RenderParameters        = "Render Parameters";
inline constexpr const char* DynamicPalette          = "Dynamic Palette";
inline constexpr const char* GeoPalParameters        = "GeoPal Parameters";
inline constexpr const char* ThreeDFParameters       = "3DF Parameters";
inline constexpr const char* VisualizationParameters = "Visualization Parameters";

// [Global Parameters]
inline constexpr const char* MapPowerX          = "Map Power X";
inline constexpr const char* MapPowerY          = "Map Power Y";
inline constexpr const char* GeoNetPower        = "GeoNet Power";
inline constexpr const char* SectionSizePower   = "Section Size Power";
inline constexpr const char* MinimalSquarePower = "Minimal Square Power";
inline constexpr const char* HZeroPlast         = "hZeroPlast";

// [Storage]
inline constexpr const char* Version               = "Version";
inline constexpr const char* CompressedFormatUsing = "Compressed Format Using";
inline constexpr const char* FileName              = "File Name";

// [Creation Parameters]
inline constexpr const char* MeshValue          = "Mesh Value";
inline constexpr const char* NoiseAmplitude     = "Noise Amplitude";
inline constexpr const char* DefaultTerrainType = "Default Terrain Type";

// [3DF Parameters]
inline constexpr const char* ScalingXYZ = "ScalingXYZ";
inline constexpr const char* Function   = "Function";

// [GeoPal Parameters]
inline constexpr const char* NumbersKey = "NumbersKey";
inline constexpr const char* Colors     = "Colors";
inline constexpr const char* Keys       = "Keys";

} // namespace WorldIni

/// Palettes of inGeo.act / inDam.act: 256 colors of three bytes each in BGR
/// order — the same layout as vrtMap::GeoPal / vrtMap::DamPal.
struct GeoDamPalettes
{
    static constexpr std::size_t SizeBytes = 768; // 256 * 3

    std::array<std::uint8_t, SizeBytes> geo{};
    std::array<std::uint8_t, SizeBytes> dam{};
};

/// hardness.bin payload (see vrtMap::loadHardness / saveHardness):
/// uint16 width, uint16 height, then width * height bytes. Every byte packs
/// four horizontally adjacent grid cells with two bits each. A grid cell is
/// 4x4 map cells (kmGrid = 2) and four cells share a byte, so a full-map file
/// has width = mapWidth / 16 and height = mapHeight / 4.
struct HardnessData
{
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<std::uint8_t> packed; // width * height bytes

    std::size_t cellCount() const
    {
        return static_cast<std::size_t>(width) * height;
    }

    /// Header dimensions the game expects for a map of the given pixel size.
    static std::pair<std::uint16_t, std::uint16_t>
    dimensionsForMap(int mapWidth, int mapHeight);
};

/// The part of the TGA header (Terra/tgai.h) needed to validate
/// leveledSurfaceTexture.tga.
struct LeveledTextureHeader
{
    std::uint8_t  idLength      = 0;
    std::uint8_t  colorMapType  = 0;
    std::uint8_t  imageType     = 0;
    std::uint16_t colorMapStart = 0;
    std::uint16_t colorMapLength = 0;
    std::uint8_t  colorMapDepth = 0;
    std::uint16_t width         = 0;
    std::uint16_t height        = 0;
    std::uint8_t  pixelDepth    = 0;

    /// Parse the 18-byte TGA header; std::nullopt when the buffer is shorter.
    static std::optional<LeveledTextureHeader>
    fromBytes(const std::vector<std::uint8_t>& bytes);

    /// True when width and height are non-zero powers of two.
    bool isPowerOfTwoSized() const;
};

/// File-level access to a world folder.
class WorldFiles
{
public:
    static constexpr const char* IniFileName         = "world.ini";
    static constexpr const char* GeoPalFile          = "inGeo.act";
    static constexpr const char* DamPalFile          = "inDam.act";
    static constexpr const char* HardnessFile        = "hardness.bin";
    static constexpr const char* GeoPalXmlFile       = "geoPal.xml";
    static constexpr const char* GeoTxXmlFile        = "geoTx.xml";
    static constexpr const char* LeveledTextureFile  = "leveledSurfaceTexture.tga";
    static constexpr const char* MapTgaFile          = "map.tga"; // radar/minimap image (WorldImages, T2.4)
    static constexpr const char* CacheTgaFile        = "cache.tga"; // full-surface snapshot the editor regenerates on save

    WorldFiles() = default;
    explicit WorldFiles(std::filesystem::path worldDir);

    const std::filesystem::path& worldDir() const;
    void setWorldDir(std::filesystem::path worldDir);

    /// Path of a file inside the world folder (no existence check).
    std::filesystem::path filePath(std::string_view fileName) const;

    /// True when the named file exists inside the world folder.
    bool exists(std::string_view fileName) const;

    // -----------------------------------------------------------------------
    // world.ini — generic access to every section and key
    // -----------------------------------------------------------------------

    /// Parse world.ini; std::nullopt when the file is missing.
    std::optional<IniDocument> readIni() const;

    /// Write the whole document back. The caller is responsible for having
    /// read it first (see the note at the top about Terra writes).
    bool writeIni(const IniDocument& document) const;

    std::optional<std::string> readIniString(std::string_view section,
                                             std::string_view key) const;
    std::optional<int> readIniInt(std::string_view section,
                                  std::string_view key) const;

    /// Read-modify-write helpers: only the given key is touched, everything
    /// else in world.ini is preserved byte-for-byte.
    bool writeIniString(std::string_view section, std::string_view key,
                        std::string_view value) const;
    bool writeIniInt(std::string_view section, std::string_view key,
                     int value) const;

    // -----------------------------------------------------------------------
    // world.ini — named helpers for the fields later tasks rewrite
    // -----------------------------------------------------------------------

    /// (Map Power X, Map Power Y): H_SIZE = 1 << X, V_SIZE = 1 << Y.
    std::optional<std::pair<int, int>> readMapPower() const;
    bool writeMapPower(int powerX, int powerY) const;

    std::optional<int> readHZeroPlast() const;
    bool writeHZeroPlast(int height) const;

    std::optional<std::string> readStorageVersion() const;
    bool writeStorageVersion(std::string_view version) const;

    /// [Storage] Compressed Format Using: 0 = S2T0 (uncompressed),
    /// 1 = S2T1 (zlib).
    std::optional<int> readStorageCompressedFormat() const;
    bool writeStorageCompressedFormat(int mode) const;

    // -----------------------------------------------------------------------
    // inGeo.act / inDam.act
    // -----------------------------------------------------------------------

    /// Both files must exist and be exactly 768 bytes long.
    bool readPalettes(GeoDamPalettes& palettes) const;
    bool writePalettes(const GeoDamPalettes& palettes) const;

    // -----------------------------------------------------------------------
    // hardness.bin (optional; the game works without it)
    // -----------------------------------------------------------------------

    /// Reads the header and the packed payload; rejects a size mismatch.
    bool readHardness(HardnessData& hardness) const;
    bool writeHardness(const HardnessData& hardness) const;

    // -----------------------------------------------------------------------
    // geoPal.xml / geoTx.xml
    // -----------------------------------------------------------------------

    /// Copy both XML files from another world (template/new-world wizard).
    /// Returns false — and copies nothing — when either source is missing.
    bool copyLayerXmlFrom(const std::filesystem::path& sourceWorldDir) const;

    std::optional<std::string> readGeoPalXml() const;
    std::optional<std::string> readGeoTxXml() const;
    bool writeGeoPalXml(std::string_view xml) const;
    bool writeGeoTxXml(std::string_view xml) const;

    // -----------------------------------------------------------------------
    // leveledSurfaceTexture.tga
    // -----------------------------------------------------------------------

    bool readLeveledTexture(std::vector<std::uint8_t>& bytes) const;

    /// Validate the texture against a mapWidth x mapHeight map and store it.
    /// Nothing is written when validation fails (errors are logged).
    bool writeLeveledTexture(const std::vector<std::uint8_t>& bytes,
                             int mapWidth, int mapHeight) const;

    /// Validation errors for the texture bytes on a mapWidth x mapHeight map.
    /// An empty result means vrtMap::loadLeveledTexture() accepts the file.
    static std::vector<std::string>
    validateLeveledTexture(const std::vector<std::uint8_t>& bytes,
                           int mapWidth, int mapHeight);

    // -----------------------------------------------------------------------
    // Generic auxiliary file handling (new-world wizard, "save as")
    // -----------------------------------------------------------------------

    /// Copy one file from another world into this world folder, overwriting
    /// an existing target. Useful for up.tga, geoLattice.bin, etc.
    bool copyAuxFile(const std::filesystem::path& sourceWorldDir,
                     std::string_view fileName) const;

private:
    std::filesystem::path worldDir_;

    std::optional<std::string> readTextFile(std::string_view fileName) const;
    bool writeTextFile(std::string_view fileName, std::string_view content) const;
};

} // namespace MapEditor

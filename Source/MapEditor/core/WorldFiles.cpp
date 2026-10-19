// ============================================================================
// MapEditorCore — WorldFiles implementation.
// ============================================================================

#include "WorldFiles.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace MapEditor {

namespace fs = std::filesystem;

namespace {

/// Read a whole file; logs and returns std::nullopt on failure.
std::optional<std::vector<std::uint8_t>> readBytes(const fs::path& path)
{
    if (path.empty()) {
        fprintf(stderr, "WorldFiles: no world directory set.\n");
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        fprintf(stderr, "WorldFiles: cannot open '%s' for reading.\n",
                path.u8string().c_str());
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    return bytes;
}

/// Write a whole file, creating parent directories; logs on failure.
bool writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    if (path.empty()) {
        fprintf(stderr, "WorldFiles: no world directory set.\n");
        return false;
    }
    std::error_code ec;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            fprintf(stderr, "WorldFiles: cannot create '%s': %s\n",
                    parent.u8string().c_str(), ec.message().c_str());
            return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        fprintf(stderr, "WorldFiles: cannot open '%s' for writing.\n",
                path.u8string().c_str());
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream.good()) {
        fprintf(stderr, "WorldFiles: error writing '%s'.\n",
                path.u8string().c_str());
        return false;
    }
    return true;
}

std::uint16_t readLe16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

bool isPowerOfTwo(std::uint16_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / paths
// ---------------------------------------------------------------------------

WorldFiles::WorldFiles(std::filesystem::path worldDir)
    : worldDir_(std::move(worldDir))
{
}

const std::filesystem::path& WorldFiles::worldDir() const
{
    return worldDir_;
}

void WorldFiles::setWorldDir(std::filesystem::path worldDir)
{
    worldDir_ = std::move(worldDir);
}

std::filesystem::path WorldFiles::filePath(std::string_view fileName) const
{
    if (worldDir_.empty()) {
        return {};
    }
    return worldDir_ / fs::u8path(std::string(fileName));
}

bool WorldFiles::exists(std::string_view fileName) const
{
    const fs::path path = filePath(fileName);
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::exists(path, ec);
}

// ---------------------------------------------------------------------------
// world.ini — generic access
// ---------------------------------------------------------------------------

std::optional<IniDocument> WorldFiles::readIni() const
{
    IniDocument document;
    if (!document.load(filePath(IniFileName))) {
        fprintf(stderr, "WorldFiles: cannot read '%s'.\n",
                filePath(IniFileName).u8string().c_str());
        return std::nullopt;
    }
    return document;
}

bool WorldFiles::writeIni(const IniDocument& document) const
{
    if (!document.save(filePath(IniFileName))) {
        fprintf(stderr, "WorldFiles: cannot write '%s'.\n",
                filePath(IniFileName).u8string().c_str());
        return false;
    }
    return true;
}

std::optional<std::string> WorldFiles::readIniString(std::string_view section,
                                                     std::string_view key) const
{
    const auto document = readIni();
    if (!document) {
        return std::nullopt;
    }
    return document->getValue(section, key);
}

std::optional<int> WorldFiles::readIniInt(std::string_view section,
                                          std::string_view key) const
{
    const auto document = readIni();
    if (!document) {
        return std::nullopt;
    }
    return document->getIntValue(section, key);
}

bool WorldFiles::writeIniString(std::string_view section, std::string_view key,
                                std::string_view value) const
{
    auto document = readIni();
    if (!document) {
        return false;
    }
    document->setValue(section, key, value);
    return writeIni(*document);
}

bool WorldFiles::writeIniInt(std::string_view section, std::string_view key,
                             int value) const
{
    return writeIniString(section, key, std::to_string(value));
}

// ---------------------------------------------------------------------------
// world.ini — named helpers
// ---------------------------------------------------------------------------

std::optional<std::pair<int, int>> WorldFiles::readMapPower() const
{
    const auto powerX = readIniInt(WorldIni::GlobalParameters, WorldIni::MapPowerX);
    const auto powerY = readIniInt(WorldIni::GlobalParameters, WorldIni::MapPowerY);
    if (!powerX || !powerY) {
        return std::nullopt;
    }
    return std::make_pair(*powerX, *powerY);
}

bool WorldFiles::writeMapPower(int powerX, int powerY) const
{
    auto document = readIni();
    if (!document) {
        return false;
    }
    document->setIntValue(WorldIni::GlobalParameters, WorldIni::MapPowerX, powerX);
    document->setIntValue(WorldIni::GlobalParameters, WorldIni::MapPowerY, powerY);
    return writeIni(*document);
}

std::optional<int> WorldFiles::readHZeroPlast() const
{
    return readIniInt(WorldIni::GlobalParameters, WorldIni::HZeroPlast);
}

bool WorldFiles::writeHZeroPlast(int height) const
{
    return writeIniInt(WorldIni::GlobalParameters, WorldIni::HZeroPlast, height);
}

std::optional<std::string> WorldFiles::readStorageVersion() const
{
    return readIniString(WorldIni::Storage, WorldIni::Version);
}

bool WorldFiles::writeStorageVersion(std::string_view version) const
{
    return writeIniString(WorldIni::Storage, WorldIni::Version, version);
}

std::optional<int> WorldFiles::readStorageCompressedFormat() const
{
    return readIniInt(WorldIni::Storage, WorldIni::CompressedFormatUsing);
}

bool WorldFiles::writeStorageCompressedFormat(int mode) const
{
    return writeIniInt(WorldIni::Storage, WorldIni::CompressedFormatUsing, mode);
}

// ---------------------------------------------------------------------------
// inGeo.act / inDam.act
// ---------------------------------------------------------------------------

bool WorldFiles::readPalettes(GeoDamPalettes& palettes) const
{
    const auto geo = readBytes(filePath(GeoPalFile));
    const auto dam = readBytes(filePath(DamPalFile));
    if (!geo || !dam) {
        return false;
    }
    if (geo->size() != GeoDamPalettes::SizeBytes) {
        fprintf(stderr, "WorldFiles: '%s' has %zu bytes, expected %zu.\n",
                filePath(GeoPalFile).u8string().c_str(), geo->size(),
                GeoDamPalettes::SizeBytes);
        return false;
    }
    if (dam->size() != GeoDamPalettes::SizeBytes) {
        fprintf(stderr, "WorldFiles: '%s' has %zu bytes, expected %zu.\n",
                filePath(DamPalFile).u8string().c_str(), dam->size(),
                GeoDamPalettes::SizeBytes);
        return false;
    }

    std::copy(geo->begin(), geo->end(), palettes.geo.begin());
    std::copy(dam->begin(), dam->end(), palettes.dam.begin());
    return true;
}

bool WorldFiles::writePalettes(const GeoDamPalettes& palettes) const
{
    const std::vector<std::uint8_t> geo(palettes.geo.begin(), palettes.geo.end());
    const std::vector<std::uint8_t> dam(palettes.dam.begin(), palettes.dam.end());

    // Attempt both writes so that a failure on one file does not leave the
    // palettes silently out of sync.
    const bool geoOk = writeBytes(filePath(GeoPalFile), geo);
    const bool damOk = writeBytes(filePath(DamPalFile), dam);
    return geoOk && damOk;
}

// ---------------------------------------------------------------------------
// hardness.bin
// ---------------------------------------------------------------------------

std::pair<std::uint16_t, std::uint16_t>
HardnessData::dimensionsForMap(int mapWidth, int mapHeight)
{
    // A grid cell is 4x4 map pixels (kmGrid = 2) and four cells are packed
    // into one byte, i.e. 16 map pixels per width unit; rows are byte-height.
    return {static_cast<std::uint16_t>(mapWidth / 16),
            static_cast<std::uint16_t>(mapHeight / 4)};
}

bool WorldFiles::readHardness(HardnessData& hardness) const
{
    const auto bytes = readBytes(filePath(HardnessFile));
    if (!bytes) {
        return false;
    }
    if (bytes->size() < 4) {
        fprintf(stderr, "WorldFiles: '%s' is truncated (no header).\n",
                filePath(HardnessFile).u8string().c_str());
        return false;
    }

    const std::uint16_t width = readLe16(*bytes, 0);
    const std::uint16_t height = readLe16(*bytes, 2);
    const std::size_t expected = static_cast<std::size_t>(width) * height;
    if (width == 0 || height == 0 || bytes->size() != 4 + expected) {
        fprintf(stderr,
                "WorldFiles: '%s' has an invalid layout (%u x %u, %zu bytes, "
                "expected %zu).\n",
                filePath(HardnessFile).u8string().c_str(),
                static_cast<unsigned>(width), static_cast<unsigned>(height),
                bytes->size(), 4 + expected);
        return false;
    }

    hardness.width = width;
    hardness.height = height;
    hardness.packed.assign(bytes->begin() + 4, bytes->end());
    return true;
}

bool WorldFiles::writeHardness(const HardnessData& hardness) const
{
    if (hardness.width == 0 || hardness.height == 0) {
        fprintf(stderr, "WorldFiles: refusing to write '%s' with a zero size.\n",
                filePath(HardnessFile).u8string().c_str());
        return false;
    }
    if (hardness.packed.size() != hardness.cellCount()) {
        fprintf(stderr,
                "WorldFiles: refusing to write '%s': %zu bytes for a %u x %u "
                "grid (expected %zu).\n",
                filePath(HardnessFile).u8string().c_str(),
                hardness.packed.size(), static_cast<unsigned>(hardness.width),
                static_cast<unsigned>(hardness.height), hardness.cellCount());
        return false;
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(4 + hardness.packed.size());
    bytes.push_back(static_cast<std::uint8_t>(hardness.width & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((hardness.width >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(hardness.height & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((hardness.height >> 8) & 0xFF));
    bytes.insert(bytes.end(), hardness.packed.begin(), hardness.packed.end());
    return writeBytes(filePath(HardnessFile), bytes);
}

// ---------------------------------------------------------------------------
// geoPal.xml / geoTx.xml
// ---------------------------------------------------------------------------

std::optional<std::string> WorldFiles::readTextFile(std::string_view fileName) const
{
    const auto bytes = readBytes(filePath(fileName));
    if (!bytes) {
        return std::nullopt;
    }
    return std::string(bytes->begin(), bytes->end());
}

bool WorldFiles::writeTextFile(std::string_view fileName,
                               std::string_view content) const
{
    const std::vector<std::uint8_t> bytes(content.begin(), content.end());
    return writeBytes(filePath(fileName), bytes);
}

std::optional<std::string> WorldFiles::readGeoPalXml() const
{
    return readTextFile(GeoPalXmlFile);
}

std::optional<std::string> WorldFiles::readGeoTxXml() const
{
    return readTextFile(GeoTxXmlFile);
}

bool WorldFiles::writeGeoPalXml(std::string_view xml) const
{
    return writeTextFile(GeoPalXmlFile, xml);
}

bool WorldFiles::writeGeoTxXml(std::string_view xml) const
{
    return writeTextFile(GeoTxXmlFile, xml);
}

bool WorldFiles::copyLayerXmlFrom(const std::filesystem::path& sourceWorldDir) const
{
    std::error_code ec;
    const fs::path sourceGeo = sourceWorldDir / fs::u8path(GeoPalXmlFile);
    const fs::path sourceTx = sourceWorldDir / fs::u8path(GeoTxXmlFile);
    if (!fs::exists(sourceGeo, ec) || !fs::exists(sourceTx, ec)) {
        fprintf(stderr,
                "WorldFiles: '%s' does not contain both %s and %s.\n",
                sourceWorldDir.u8string().c_str(), GeoPalXmlFile, GeoTxXmlFile);
        return false;
    }
    return copyAuxFile(sourceWorldDir, GeoPalXmlFile) &&
           copyAuxFile(sourceWorldDir, GeoTxXmlFile);
}

// ---------------------------------------------------------------------------
// leveledSurfaceTexture.tga
// ---------------------------------------------------------------------------

std::optional<LeveledTextureHeader>
LeveledTextureHeader::fromBytes(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < 18) {
        return std::nullopt;
    }
    LeveledTextureHeader header;
    header.idLength = bytes[0];
    header.colorMapType = bytes[1];
    header.imageType = bytes[2];
    header.colorMapStart = readLe16(bytes, 3);
    header.colorMapLength = readLe16(bytes, 5);
    header.colorMapDepth = bytes[7];
    header.width = readLe16(bytes, 12);
    header.height = readLe16(bytes, 14);
    header.pixelDepth = bytes[16];
    return header;
}

bool LeveledTextureHeader::isPowerOfTwoSized() const
{
    return isPowerOfTwo(width) && isPowerOfTwo(height);
}

bool WorldFiles::readLeveledTexture(std::vector<std::uint8_t>& bytes) const
{
    const auto data = readBytes(filePath(LeveledTextureFile));
    if (!data) {
        return false;
    }
    bytes = *data;
    return true;
}

std::vector<std::string>
WorldFiles::validateLeveledTexture(const std::vector<std::uint8_t>& bytes,
                                   int mapWidth, int mapHeight)
{
    std::vector<std::string> errors;

    const auto header = LeveledTextureHeader::fromBytes(bytes);
    if (!header) {
        errors.emplace_back("file is too small to contain an 18-byte TGA header");
        return errors;
    }

    // Rules enforced by vrtMap::loadLeveledTexture (Terra/VMAP.cpp) plus the
    // assumptions its pixel loop makes about the file layout.
    if (header->colorMapType == 0 || header->colorMapDepth != 24) {
        errors.emplace_back("must be an indexed TGA with a 24-bit color map");
    }
    if (header->idLength != 0) {
        errors.emplace_back(
            "TGA ID field must be empty (the game does not skip it)");
    }
    if (header->imageType != 1) {
        errors.emplace_back(
            "must be an uncompressed color-mapped TGA (image type 1)");
    }
    if (header->pixelDepth != 8) {
        errors.emplace_back(
            "pixel depth must be 8 bits (one palette index per texel)");
    }

    const std::uint64_t paletteBytes =
        static_cast<std::uint64_t>(header->colorMapDepth) *
        header->colorMapLength / 8;
    if (static_cast<std::uint64_t>(header->colorMapStart) * 3 + paletteBytes >
        GeoDamPalettes::SizeBytes) {
        errors.emplace_back(
            "color map does not fit the game's 256-entry palette");
    }

    if (header->width == 0 || header->height == 0) {
        errors.emplace_back("width and height must be non-zero");
    } else {
        if (header->width > mapWidth || header->height > mapHeight) {
            errors.emplace_back(
                "size " + std::to_string(header->width) + "x" +
                std::to_string(header->height) + " exceeds the map size " +
                std::to_string(mapWidth) + "x" + std::to_string(mapHeight));
        }
        if (!header->isPowerOfTwoSized()) {
            errors.emplace_back("width and height must be powers of two");
        }
        const std::uint64_t needed =
            18 + paletteBytes +
            static_cast<std::uint64_t>(header->width) * header->height;
        if (bytes.size() < needed) {
            errors.emplace_back("file is truncated (expected at least " +
                                std::to_string(needed) + " bytes, got " +
                                std::to_string(bytes.size()) + ")");
        }
    }

    return errors;
}

bool WorldFiles::writeLeveledTexture(const std::vector<std::uint8_t>& bytes,
                                     int mapWidth, int mapHeight) const
{
    const std::vector<std::string> errors =
        validateLeveledTexture(bytes, mapWidth, mapHeight);
    if (!errors.empty()) {
        fprintf(stderr, "WorldFiles: '%s' rejected:\n",
                filePath(LeveledTextureFile).u8string().c_str());
        for (const std::string& error : errors) {
            fprintf(stderr, "  - %s\n", error.c_str());
        }
        return false;
    }
    return writeBytes(filePath(LeveledTextureFile), bytes);
}

// ---------------------------------------------------------------------------
// Auxiliary files
// ---------------------------------------------------------------------------

bool WorldFiles::copyAuxFile(const std::filesystem::path& sourceWorldDir,
                             std::string_view fileName) const
{
    if (worldDir_.empty()) {
        fprintf(stderr, "WorldFiles: no world directory set.\n");
        return false;
    }

    const fs::path source = sourceWorldDir / fs::u8path(std::string(fileName));
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        fprintf(stderr, "WorldFiles: source file '%s' does not exist.\n",
                source.u8string().c_str());
        return false;
    }

    fs::create_directories(worldDir_, ec);
    if (ec) {
        fprintf(stderr, "WorldFiles: cannot create '%s': %s\n",
                worldDir_.u8string().c_str(), ec.message().c_str());
        return false;
    }

    fs::copy_file(source, filePath(fileName),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fprintf(stderr, "WorldFiles: cannot copy '%s' to '%s': %s\n",
                source.u8string().c_str(),
                filePath(fileName).u8string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

} // namespace MapEditor

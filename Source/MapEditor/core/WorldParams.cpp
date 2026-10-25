// ============================================================================
// MapEditorCore — WorldParams implementation (T5.1).
// ============================================================================

#include "WorldParams.h"

#include "IniDocument.h"

#include <charconv>
#include <cstdio>
#include <cstring>

// XTool — XStream/XBuffer give the exact same VMP compression code path the
// game uses (vrtMap::compressWorlds), and vrtMap::sVmpHeader is the on-disk
// header layout.
#include "xstream.h"
#include "xbuffer.h"
#include "terra.h"

namespace fs = std::filesystem;

namespace MapEditor {

namespace {

std::optional<int> iniInt(const IniDocument& ini, const char* section,
                          const char* key)
{
    return ini.getIntValue(section, key);
}

int iniIntOr(const IniDocument& ini, const char* section, const char* key,
             int fallback)
{
    const std::optional<int> value = ini.getIntValue(section, key);
    return value ? *value : fallback;
}

} // namespace

// ---------------------------------------------------------------------------
// WorldParams
// ---------------------------------------------------------------------------

std::optional<ZeroLayerColor>
WorldParams::parseZeroLayerColor(std::string_view text)
{
    int values[4] = {0, 0, 0, 0};
    int count = 0;
    const char* begin = text.data();
    const char* const end = begin + text.size();
    while (count < 4) {
        while (begin != end && (*begin == ' ' || *begin == '\t')) {
            ++begin;
        }
        if (begin == end) {
            break;
        }
        int value = 0;
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc{}) {
            break;
        }
        values[count++] = value;
        begin = ptr;
    }
    if (count < 4) {
        return std::nullopt;
    }
    ZeroLayerColor color;
    color.r = values[0];
    color.g = values[1];
    color.b = values[2];
    color.a = values[3];
    return color;
}

std::string WorldParams::zeroLayerColorToString(const ZeroLayerColor& color)
{
    // Space separated like the game writes it; no trailing space (the ini
    // writer would accumulate one per save, see f3d.cpp sKeyGeoPal).
    std::string out = std::to_string(color.r);
    out += ' ';
    out += std::to_string(color.g);
    out += ' ';
    out += std::to_string(color.b);
    out += ' ';
    out += std::to_string(color.a);
    return out;
}

std::optional<WorldParams>
WorldParams::readFromWorld(const WorldFiles& files,
                           const std::filesystem::path& gameDir)
{
    const std::optional<IniDocument> ini = files.readIni();
    if (!ini) {
        return std::nullopt;
    }

    WorldParams params;
    params.mapPowerX = iniIntOr(*ini, WorldIni::GlobalParameters,
                                WorldIni::MapPowerX, 0);
    params.mapPowerY = iniIntOr(*ini, WorldIni::GlobalParameters,
                                WorldIni::MapPowerY, 0);
    params.hZeroPlast = iniIntOr(*ini, WorldIni::GlobalParameters,
                                 WorldIni::HZeroPlast, 0);
    params.geoNetPower = iniIntOr(*ini, WorldIni::GlobalParameters,
                                  WorldIni::GeoNetPower, 6);
    params.sectionSizePower = iniIntOr(*ini, WorldIni::GlobalParameters,
                                       WorldIni::SectionSizePower, 7);
    params.minimalSquarePower = iniIntOr(*ini, WorldIni::GlobalParameters,
                                         WorldIni::MinimalSquarePower, 4);

    params.storageVersion = files.readStorageVersion().value_or("1.4");

    // The truth about the compression lives in the file header; the ini key
    // is only documentation the editor keeps in sync.
    params.compressedFormat =
        vmpCompressionOfFile(files.filePath(WorldFiles::OutputVmpFile));
    if (params.compressedFormat < 0) {
        // No/unknown output.vmp — fall back to the stored documentation key.
        params.compressedFormat =
            files.readStorageCompressedFormat().value_or(0);
    }

    // ScalingXYZ holds three integers "kmx kmy kmz"; the s_f3d constructor
    // defaults (200 200 200) apply when the key is missing or short.
    params.f3dKmx = params.f3dKmy = params.f3dKmz = 200;
    if (const std::optional<std::string> triple =
            ini->getValue(WorldIni::ThreeDFParameters, WorldIni::ScalingXYZ)) {
        int km[3] = {200, 200, 200};
        int count = 0;
        const char* begin = triple->data();
        const char* const end = begin + triple->size();
        while (count < 3) {
            while (begin != end && (*begin == ' ' || *begin == '\t')) {
                ++begin;
            }
            if (begin == end) {
                break;
            }
            int value = 0;
            const auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec != std::errc{}) {
                break;
            }
            km[count++] = value;
            begin = ptr;
        }
        if (count == 3) {
            params.f3dKmx = km[0];
            params.f3dKmy = km[1];
            params.f3dKmz = km[2];
        }
    }
    params.f3dFunction = iniIntOr(*ini, WorldIni::ThreeDFParameters,
                                  WorldIni::Function, 1);

    bool stored = false;
    readZeroLayerColor(gameDir, params.zeroLayerColor, stored);

    return params;
}

bool WorldParams::writeToIni(const WorldFiles& files,
                             const std::filesystem::path& gameDir) const
{
    bool ok = true;
    ok &= files.writeIniInt(WorldIni::GlobalParameters, WorldIni::MapPowerX,
                            mapPowerX);
    ok &= files.writeIniInt(WorldIni::GlobalParameters, WorldIni::MapPowerY,
                            mapPowerY);
    ok &= files.writeIniInt(WorldIni::GlobalParameters, WorldIni::HZeroPlast,
                            hZeroPlast);
    ok &= files.writeIniInt(WorldIni::GlobalParameters, WorldIni::GeoNetPower,
                            geoNetPower);
    ok &= files.writeIniInt(WorldIni::GlobalParameters,
                            WorldIni::SectionSizePower, sectionSizePower);
    ok &= files.writeIniInt(WorldIni::GlobalParameters,
                            WorldIni::MinimalSquarePower, minimalSquarePower);
    ok &= files.writeStorageVersion(storageVersion);
    ok &= files.writeStorageCompressedFormat(compressedFormat);
    ok &= files.writeIniString(WorldIni::ThreeDFParameters,
                               WorldIni::ScalingXYZ,
                               std::to_string(f3dKmx) + " " +
                                   std::to_string(f3dKmy) + " " +
                                   std::to_string(f3dKmz));
    ok &= files.writeIniInt(WorldIni::ThreeDFParameters, WorldIni::Function,
                            f3dFunction);
    ok &= writeZeroLayerColor(gameDir, zeroLayerColor);
    return ok;
}

// ---------------------------------------------------------------------------
// Perimeter.ini [TD] Color ZeroLayer
// ---------------------------------------------------------------------------

bool readZeroLayerColor(const fs::path& gameDir, ZeroLayerColor& color,
                        bool& stored)
{
    stored = false;
    color = ZeroLayerColor{};

    const fs::path iniPath = gameDir / GameIni::PerimeterIniFile;
    if (!fs::exists(iniPath)) {
        return true; // no ini — the game runs on its built-in defaults
    }
    IniDocument ini;
    if (!ini.load(iniPath)) {
        return false;
    }
    const std::optional<std::string> value =
        ini.getValue(GameIni::TdSection, GameIni::ColorZeroLayerKey);
    if (!value || value->empty()) {
        return true;
    }
    const std::optional<ZeroLayerColor> parsed =
        WorldParams::parseZeroLayerColor(*value);
    if (!parsed) {
        return true; // present but unparseable — the game ignores it too
    }
    color = *parsed;
    stored = true;
    return true;
}

bool writeZeroLayerColor(const fs::path& gameDir, const ZeroLayerColor& color)
{
    const fs::path iniPath = gameDir / GameIni::PerimeterIniFile;
    IniDocument ini;
    if (fs::exists(iniPath)) {
        if (!ini.load(iniPath)) {
            fprintf(stderr, "WorldParams: cannot parse %s\n",
                    iniPath.u8string().c_str());
            return false;
        }
    }
    ini.setValue(GameIni::TdSection, GameIni::ColorZeroLayerKey,
                 WorldParams::zeroLayerColorToString(color));
    if (!ini.save(iniPath)) {
        fprintf(stderr, "WorldParams: cannot write %s\n",
                iniPath.u8string().c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// output.vmp compression (S2T0 / S2T1)
// ---------------------------------------------------------------------------

int vmpCompressionOfFile(const fs::path& outputVmp)
{
    std::error_code ec;
    if (!fs::exists(outputVmp, ec) || fs::file_size(outputVmp, ec) <
                                          sizeof(vrtMap::sVmpHeader)) {
        return -1;
    }
    XStream stream;
    stream.ErrHUsed = false;
    if (!stream.open(outputVmp.u8string(), XS_IN)) {
        return -1;
    }
    vrtMap::sVmpHeader header;
    stream.seek(0, XS_BEG);
    stream.read(&header, sizeof(header));
    stream.close();
    if (header.cmpID("S2T0")) {
        return 0;
    }
    if (header.cmpID("S2T1")) {
        return 1;
    }
    return -1;
}

bool convertVmpCompression(const fs::path& outputVmp, int mode)
{
    if (mode != 0 && mode != 1) {
        return false;
    }

    XStream stream;
    stream.ErrHUsed = false;
    if (!stream.open(outputVmp.u8string(), XS_IN)) {
        fprintf(stderr, "WorldParams: output.vmp not found: %s\n",
                outputVmp.u8string().c_str());
        return false;
    }
    vrtMap::sVmpHeader header;
    stream.seek(0, XS_BEG);
    stream.read(&header, sizeof(header));
    const std::int64_t payloadBytes =
        static_cast<std::int64_t>(stream.size()) - stream.tell();

    XBuffer raster(0, true); // holds the uncompressed payload on exit
    if (header.cmpID("S2T0")) {
        if (mode == 0) {
            stream.close();
            return true; // already uncompressed
        }
        raster.realloc(payloadBytes);
        stream.read(raster.buf, payloadBytes);
        raster.set(payloadBytes, XB_BEG);
    } else if (header.cmpID("S2T1")) {
        if (mode == 1) {
            stream.close();
            return true; // already compressed
        }
        XBuffer packed(payloadBytes, false);
        stream.read(packed.buf, payloadBytes);
        if (packed.uncompress(raster) != 0) {
            fprintf(stderr, "WorldParams: error decompressing %s\n",
                    outputVmp.u8string().c_str());
            stream.close();
            return false;
        }
    } else {
        fprintf(stderr, "WorldParams: unknown output.vmp id in %s\n",
                outputVmp.u8string().c_str());
        stream.close();
        return false;
    }
    stream.close();

    if (mode == 1) {
        header.setID("S2T1");
        XBuffer packed(raster.length(), true);
        if (raster.compress(packed) != 0) {
            fprintf(stderr, "WorldParams: error compressing %s\n",
                    outputVmp.u8string().c_str());
            return false;
        }
        raster = std::move(packed);
    } else {
        header.setID("S2T0");
    }

    if (!stream.open(outputVmp.u8string(), XS_OUT)) {
        fprintf(stderr, "WorldParams: cannot reopen for write: %s\n",
                outputVmp.u8string().c_str());
        return false;
    }
    stream.seek(0, XS_BEG);
    stream.write(&header, sizeof(header));
    stream.write(raster.buf, raster.tell());
    stream.close();
    return true;
}

} // namespace MapEditor

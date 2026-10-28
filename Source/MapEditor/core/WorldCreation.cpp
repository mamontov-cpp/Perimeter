// ============================================================================
// MapEditorCore — WorldCreation implementation (T5.2).
// ============================================================================

#include "WorldCreation.h"

#include "EditorSession.h"
#include "WorldFiles.h"
#include "WorldParams.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <vector>

// Terra — sVmpHeader for the flat output.vmp, vMap for the post-load pass
// over the flat surface.
#include "terra.h"

namespace fs = std::filesystem;

namespace MapEditor {

namespace {

/// Files copied from the template world. The first group is mandatory (the
/// game loader xasserts without them), the rest is copied when present.
constexpr const char* kMandatoryTemplateFiles[] = {
    WorldFiles::GeoPalFile,        // inGeo.act
    WorldFiles::DamPalFile,        // inDam.act
    WorldFiles::GeoPalXmlFile,     // geoPal.xml
    WorldFiles::GeoTxXmlFile,      // geoTx.xml
    WorldFiles::LeveledTextureFile // leveledSurfaceTexture.tga
};
constexpr const char* kOptionalTemplateFiles[] = {
    "geoLattice.bin",
    WorldFiles::MapTgaFile, // map.tga — regenerated on save anyway
    "up.tga",
};

/// Write a flat S2T0 output.vmp: geo voxels at `geoHeight`, no dams, no
/// attributes, surface index 0 (f3d.recalcWorld() repaints the surface after
/// the load).
bool writeFlatVmp(const fs::path& worldDir, int hSize, int vSize,
                  int geoHeight)
{
    const std::vector<unsigned char> geoRow(static_cast<std::size_t>(hSize),
                                            static_cast<unsigned char>(geoHeight));
    const std::vector<unsigned char> zeroRow(static_cast<std::size_t>(hSize), 0);

    vrtMap::sVmpHeader header{}; // zero-init: id, XS, YS set below, NX/NY = 0
    header.setID("S2T0");
    header.XS = hSize;
    header.YS = vSize;

    XStream out;
    out.ErrHUsed = false;
    if (!out.open((worldDir / WorldFiles::OutputVmpFile).u8string(), XS_OUT)) {
        return false;
    }
    out.write(&header, sizeof(header));
    for (int y = 0; y < vSize; ++y) {
        out.write(geoRow.data(), geoRow.size()); // VxG
    }
    for (int y = 0; y < vSize; ++y) {
        out.write(zeroRow.data(), zeroRow.size()); // VxD
    }
    for (int y = 0; y < vSize; ++y) {
        out.write(zeroRow.data(), zeroRow.size()); // Atr
    }
    for (int y = 0; y < vSize; ++y) {
        out.write(zeroRow.data(), zeroRow.size()); // Sur
    }
    out.close();
    return true;
}

/// The template's leveledSurfaceTexture.tga is an 8-bit indexed TGA whose
/// size can exceed the new (smaller) map — Terra's loader asserts on that
/// (VMAP.cpp loadLeveledTexture: "size texture > size map") and Debug builds
/// abort. Downscale the copied file nearest-neighbour in index space (the
/// palette entries are the DAM surface types, they must survive verbatim)
/// by the smallest power-of-two factor that fits both axes, keeping the
/// texture a power of two. Files already small enough are left untouched.
bool resizeLeveledTextureIfNeeded(const fs::path& worldDir, int mapPowerX,
                                  int mapPowerY)
{
    const fs::path path = worldDir / WorldFiles::LeveledTextureFile;
    if (!fs::exists(path)) {
        return true; // optional file
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "WorldCreation: cannot read '%s'.\n",
                path.u8string().c_str());
        return false;
    }
    unsigned char header[18] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in) {
        return false;
    }
    // Uncompressed 256-colour, 8-bit indices, no ID field (the Terra loader
    // does not skip one either).
    if (header[0] != 0 || header[1] != 1 || header[2] != 1 ||
        header[16] != 8 || header[7] != 24 ||
        (header[3] | (header[4] << 8)) != 0 || // CMapStart
        (header[5] | (header[6] << 8)) != 256) { // CMapLenght
        fprintf(stderr, "WorldCreation: '%s' has an unsupported TGA layout.\n",
                path.u8string().c_str());
        return false;
    }
    const unsigned w = static_cast<unsigned>(header[12]) |
                       (static_cast<unsigned>(header[13]) << 8);
    const unsigned h = static_cast<unsigned>(header[14]) |
                       (static_cast<unsigned>(header[15]) << 8);
    const unsigned maxW = 1u << mapPowerX;
    const unsigned maxH = 1u << mapPowerY;
    if (w == 0 || h == 0 || (w <= maxW && h <= maxH)) {
        return true;
    }

    int shift = 0;
    while ((w >> shift) > maxW || (h >> shift) > maxH) {
        ++shift;
    }
    const unsigned newW = w >> shift;
    const unsigned newH = h >> shift;

    std::vector<unsigned char> palette(static_cast<std::size_t>(768));
    in.read(reinterpret_cast<char*>(palette.data()),
            static_cast<std::streamsize>(palette.size()));
    if (!in) {
        return false;
    }
    // Read the index plane top-down (bit 5 of the descriptor says whether
    // the file stores rows bottom-up).
    const bool bottomUp = (header[17] & 0x20) == 0;
    std::vector<unsigned char> indices(static_cast<std::size_t>(w) * h);
    if (bottomUp) {
        for (unsigned row = 0; row < h; ++row) {
            in.read(reinterpret_cast<char*>(&indices[(h - 1 - row) * w]),
                    static_cast<std::streamsize>(w));
        }
    } else {
        in.read(reinterpret_cast<char*>(indices.data()),
                static_cast<std::streamsize>(w) * h);
    }
    in.close();
    if (in.bad()) {
        return false;
    }

    std::vector<unsigned char> scaled(static_cast<std::size_t>(newW) * newH);
    for (unsigned y = 0; y < newH; ++y) {
        for (unsigned x = 0; x < newW; ++x) {
            scaled[y * newW + x] = indices[(y << shift) * w + (x << shift)];
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "WorldCreation: cannot write '%s'.\n",
                path.u8string().c_str());
        return false;
    }
    header[12] = static_cast<unsigned char>(newW & 0xFF);
    header[13] = static_cast<unsigned char>((newW >> 8) & 0xFF);
    header[14] = static_cast<unsigned char>(newH & 0xFF);
    header[15] = static_cast<unsigned char>((newH >> 8) & 0xFF);
    header[17] = 0x20; // canonical top-left origin on output
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(palette.data()),
              static_cast<std::streamsize>(palette.size()));
    out.write(reinterpret_cast<const char*>(scaled.data()),
              static_cast<std::streamsize>(scaled.size()));
    if (!out.good()) {
        fprintf(stderr, "WorldCreation: failed writing '%s'.\n",
                path.u8string().c_str());
        return false;
    }
    return true;
}

} // namespace

bool isValidWorldName(const std::string& name)
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
        if (c == '.' && (name == "." || name == "..")) {
            return false;
        }
    }
    return true;
}

WorldCreateResult createNewWorld(EditorSession& session,
                                 const NewWorldParams& params,
                                 const std::string& templateWorldName)
{
    WorldCreateResult result;
    if (!session.hasGameDir()) {
        result.error = "No game directory is selected.";
        return result;
    }
    if (!isValidWorldName(params.name)) {
        result.error = "The world name is empty or contains forbidden characters.";
        return result;
    }
    if (params.mapPowerX < 6 || params.mapPowerY < 6 ||
        params.mapPowerX > 14 || params.mapPowerY > 14) {
        result.error = "Map size powers must be within 6..14.";
        return result;
    }
    if (params.hZeroPlast < 0 || params.hZeroPlast > 255 ||
        params.geoLayerHeight < 0 || params.geoLayerHeight >= params.hZeroPlast) {
        result.error =
            "The geo layer height must be below the zero-layer height.";
        return result;
    }

    const fs::path gameDir = fs::u8path(session.getGameDir());
    const fs::path worldsRoot = gameDir / "RESOURCE" / "Worlds";
    const fs::path templateDir = worldsRoot / fs::u8path(templateWorldName);
    if (templateWorldName.empty() || !fs::is_directory(templateDir)) {
        result.error = "Template world not found: " + templateWorldName;
        return result;
    }

    const fs::path newDir = worldsRoot / fs::u8path(params.name);
    std::error_code ec;
    if (fs::exists(newDir, ec)) {
        result.error = "A world with that name already exists.";
        return result;
    }
    fs::create_directories(newDir, ec);
    if (ec) {
        result.error = "Cannot create the world folder: " + ec.message();
        return result;
    }

    // ------------------------------------------------------------------
    // Aux files from the template.
    // ------------------------------------------------------------------
    const WorldFiles templateFiles(templateDir);
    const WorldFiles newFiles(newDir);
    for (const char* fileName : kMandatoryTemplateFiles) {
        if (!templateFiles.exists(fileName)) {
            result.error =
                std::string("Template world is missing ") + fileName;
            fs::remove_all(newDir, ec);
            return result;
        }
        fs::copy_file(templateDir / fileName, newDir / fileName,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            result.error =
                std::string("Cannot copy ") + fileName + ": " + ec.message();
            fs::remove_all(newDir, ec);
            return result;
        }
    }
    for (const char* fileName : kOptionalTemplateFiles) {
        if (templateFiles.exists(fileName)) {
            fs::copy_file(templateDir / fileName, newDir / fileName,
                          fs::copy_options::overwrite_existing, ec);
            ec.clear();
        }
    }

    // Terra asserts (and aborts in Debug) when the copied leveled texture is
    // larger than the new map — downscale it to fit.
    if (!resizeLeveledTextureIfNeeded(newDir, params.mapPowerX,
                                      params.mapPowerY)) {
        result.error = "Cannot resize the copied leveledSurfaceTexture.tga.";
        fs::remove_all(newDir, ec);
        return result;
    }

    // ------------------------------------------------------------------
    // world.ini — the template's copy with the wizard's values written in.
    // ------------------------------------------------------------------
    fs::copy_file(templateDir / WorldFiles::IniFileName,
                  newDir / WorldFiles::IniFileName,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        result.error = "Cannot copy world.ini: " + ec.message();
        fs::remove_all(newDir, ec);
        return result;
    }
    bool ok = newFiles.writeMapPower(params.mapPowerX, params.mapPowerY);
    ok &= newFiles.writeHZeroPlast(params.hZeroPlast);
    ok &= newFiles.writeIniInt(WorldIni::GlobalParameters,
                               WorldIni::GeoNetPower, params.geoNetPower);
    ok &= newFiles.writeIniInt(WorldIni::GlobalParameters,
                               WorldIni::SectionSizePower,
                               params.sectionSizePower);
    ok &= newFiles.writeIniInt(WorldIni::GlobalParameters,
                               WorldIni::MinimalSquarePower,
                               params.minimalSquarePower);
    ok &= newFiles.writeStorageVersion("1.4");
    ok &= newFiles.writeStorageCompressedFormat(
        params.storageCompressed == 1 ? 1 : 0);
    ok &= newFiles.writeIniString(WorldIni::Storage, WorldIni::FileName,
                                  "output");
    ok &= newFiles.writeIniString(WorldIni::ThreeDFParameters,
                                  WorldIni::ScalingXYZ,
                                  std::to_string(params.f3dKmx) + " " +
                                      std::to_string(params.f3dKmy) + " " +
                                      std::to_string(params.f3dKmz));
    ok &= newFiles.writeIniInt(WorldIni::ThreeDFParameters,
                               WorldIni::Function, params.f3dFunction);
    if (!ok) {
        result.error = "Cannot write world.ini values.";
        fs::remove_all(newDir, ec);
        return result;
    }

    // ------------------------------------------------------------------
    // Flat mode: pre-write a flat output.vmp. Generated mode: leave the
    // folder without output.vmp — the loader runs buildWorld() itself.
    // ------------------------------------------------------------------
    if (params.flatSurface) {
        const int hSize = 1 << params.mapPowerX;
        const int vSize = 1 << params.mapPowerY;
        if (!writeFlatVmp(newDir, hSize, vSize, params.geoLayerHeight)) {
            result.error = "Cannot write the flat output.vmp.";
            fs::remove_all(newDir, ec);
            return result;
        }
    }

    // ------------------------------------------------------------------
    // Load the new world (Terra path: fullLoad, or buildWorld first when
    // output.vmp is missing) and save it once so the images exist.
    // ------------------------------------------------------------------
    if (!session.scanWorlds()) {
        result.error = "World scan failed after creation.";
        return result;
    }
    if (!session.loadWorldByName(params.name)) {
        result.error = "The new world could not be loaded (see the log).";
        fs::remove_all(newDir, ec);
        session.scanWorlds();
        return result;
    }

    if (params.flatSurface) {
        // The flat vmp carries surface index 0 — derive the real surface
        // indices from the geo noise texture (the same call the game uses
        // after terrain edits) and recompute the lighting.
        f3d.recalcWorld();
        vMap.WorldRender();
    }

    // Persist the target compression (saveWorld applies it after save3Buf).
    if (params.storageCompressed == 1) {
        std::optional<WorldParams> live = session.currentWorldParams();
        if (live) {
            live->compressedFormat = 1;
            session.applyWorldSettings(*live);
        }
    }

    if (!session.saveWorld()) {
        result.error = "The new world was created but the initial save failed.";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace MapEditor

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

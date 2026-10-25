#pragma once

#include <string>

// ============================================================================
// MapEditorCore — WorldCreation (T5.2).
//
// Creation of a brand-new world folder (RESOURCE/Worlds/<name>) from a
// template world that already lives in the same game installation:
//
//   - aux files copied from the template: the palettes (inGeo.act/inDam.act),
//     the geo layer XMLs (geoPal.xml/geoTx.xml), geoLattice.bin,
//     leveledSurfaceTexture.tga (all mandatory for the game loader) and
//     up.tga when present;
//   - world.ini copied from the template and rewritten with the wizard's
//     values ([Global Parameters], [Storage], [3DF Parameters]) — everything
//     the template carries beyond that (rendering tables, dynamic palette)
//     survives untouched;
//   - terrain: either a flat surface written directly as an S2T0 output.vmp
//     (geo voxels at the requested geo layer height, no dams) or Terra's own
//     procedural generation, which the loader runs by itself when the new
//     world has no output.vmp (vrtMap::selectUsedWorld → buildWorld);
//   - the new world is then loaded through the session and saved once, so
//     cache.tga / map.tga exist from the very start.
// ============================================================================

namespace MapEditor {

class EditorSession;

/// Wizard input (T5.2).
struct NewWorldParams
{
    std::string name;              ///< folder name under RESOURCE/Worlds/.
    int mapPowerX = 11;            ///< H_SIZE = 1 << mapPowerX.
    int mapPowerY = 11;            ///< V_SIZE = 1 << mapPowerY.
    int hZeroPlast = 30;           ///< zero-layer height (0..255).
    int geoLayerHeight = 20;       ///< flat-mode voxel height, < hZeroPlast.
    int geoNetPower = 6;
    int sectionSizePower = 7;
    int minimalSquarePower = 4;
    int f3dKmx = 200;
    int f3dKmy = 200;
    int f3dKmz = 200;
    int f3dFunction = 1;
    bool flatSurface = true;       ///< false = procedural buildWorld().
    int storageCompressed = 0;     ///< 0 = S2T0, 1 = S2T1 (applied on save).
};

/// Result with a human-readable error message (empty on success).
struct WorldCreateResult
{
    bool ok = false;
    std::string error;
};

/// Create a new world from `templateWorldName` (a folder scanned by the
/// session), then load and save it. On success the session holds the new
/// world (unsaved buffer edits are none — it was saved inside).
WorldCreateResult createNewWorld(EditorSession& session,
                                 const NewWorldParams& params,
                                 const std::string& templateWorldName);

/// True when `name` is a usable world folder name (non-empty, no path
/// separators, none of the Windows-reserved characters).
bool isValidWorldName(const std::string& name);

} // namespace MapEditor

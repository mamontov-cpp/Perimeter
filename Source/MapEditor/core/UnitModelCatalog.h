#pragma once

// ============================================================================
// MapEditorCore — the visual M3D model per placeable building.
//
// GENERATED from Source/Scripts/UnitAttribute.prm (do not edit by hand;
// regenerate when the game's attributes change). The game compiles
// UnitAttribute.prm into its executable (scripts_export::unitAttributeDataTable)
// and never reads the .prm at runtime, so the editor cannot discover model
// names from a game folder either — this table is static game data, identical
// for every mod.
//
// Per attribute the game uses ModelNameArray[0] as the constructed model
// (ModelNameArray[1] is the build-site variant) and derives the render scale
// in GeometryAttribute::initGeometryAttribute (Source/Units/UnitAttribute.cpp):
//   modelScale = BoundRadius > 0 ? BoundRadius / nativeBoundRadius
//                                : BoundScale          (default 1)
// (debuScales are debug cheats that default to 1). The editor replicates the
// same formula at load time using the loaded model's native bound radius.
// ============================================================================

#include <cstddef>

namespace MapEditor {

struct UnitModelInfo
{
    int id;                 ///< terUnitAttributeID ordinal (matches BuildingCatalog).
    const char* modelFile;  ///< ModelNameArray[0], e.g. "RESOURCE\\MODELS\\MAIN\\coreup.m3d".
    double boundRadius;     ///< Absolute target bound radius; 0 = use boundScale.
    double boundScale;      ///< Relative scale when boundRadius is 0.
};

/// Model table for the 43 placeable buildings (BuildingCatalog order, ids 0..42).
inline constexpr UnitModelInfo kUnitModels[] = {
    {0, "RESOURCE\\MODELS\\MAIN\\coreup.m3d", 0.0, 1.0},
    {1, "RESOURCE\\MODELS\\MAIN\\transmiter.m3d", 0.0, 1.0},
    {2, "RESOURCE\\MODELS\\MAIN\\amplifer.m3d", 0.0, 1.0},
    {3, "RESOURCE\\MODELS\\MAIN\\solider_factory.m3d", 0.0, 1.0},
    {4, "RESOURCE\\MODELS\\MAIN\\oficer_factory.m3d", 0.0, 1.0},
    {5, "RESOURCE\\MODELS\\MAIN\\technic_factory.m3d", 0.0, 1.0},
    {6, "RESOURCE\\MODELS\\MAIN\\Command.m3d", 0.0, 1.0},
    {7, "RESOURCE\\MODELS\\MAIN\\laser_st_up1.m3d", 0.0, 1.5},
    {8, "RESOURCE\\MODELS\\MAIN\\rocket_st_up1.m3d", 0.0, 1.5},
    {9, "RESOURCE\\MODELS\\MAIN\\bomb_st_up1.m3d", 0.0, 1.5},
    {10, "RESOURCE\\MODELS\\MAIN\\fly_st.m3d", 0.0, 1.5},
    {11, "RESOURCE\\MODELS\\MAIN\\subterra_st.m3d", 0.0, 1.5},
    {12, "RESOURCE\\MODELS\\MAIN\\electro_st.m3d", 0.0, 1.0},
    {13, "RESOURCE\\MODELS\\MAIN\\exodus_st.m3d", 0.0, 1.5},
    {14, "RESOURCE\\MODELS\\MAIN\\imperia_st.m3d", 0.0, 1.5},
    {15, "RESOURCE\\MODELS\\MAIN\\harkbackers_st.m3d", 0.0, 1.5},
    {16, "RESOURCE\\MODELS\\MAIN\\laser_st_up1.m3d", 0.0, 1.5},
    {17, "RESOURCE\\MODELS\\MAIN\\rocket_st_up1.m3d", 0.0, 1.5},
    {18, "RESOURCE\\MODELS\\MAIN\\bomb_st_up1.m3d", 0.0, 1.5},
    {19, "RESOURCE\\MODELS\\MAIN\\fly_st.m3d", 0.0, 1.5},
    {20, "RESOURCE\\MODELS\\MAIN\\subterra_st.m3d", 0.0, 1.5},
    {21, "RESOURCE\\MODELS\\MAIN\\electro_st.m3d", 0.0, 1.0},
    {22, "RESOURCE\\MODELS\\MAIN\\exodus_st.m3d", 0.0, 1.5},
    {23, "RESOURCE\\MODELS\\MAIN\\imperia_st.m3d", 0.0, 1.5},
    {24, "RESOURCE\\MODELS\\MAIN\\harkbackers_st.m3d", 0.0, 1.5},
    {25, "RESOURCE\\MODELS\\MAIN\\laser_st_up1.m3d", 0.0, 1.5},
    {26, "RESOURCE\\MODELS\\MAIN\\rocket_st_up1.m3d", 0.0, 1.5},
    {27, "RESOURCE\\MODELS\\MAIN\\bomb_st_up1.m3d", 0.0, 1.5},
    {28, "RESOURCE\\MODELS\\MAIN\\electro_st.m3d", 0.0, 1.0},
    {29, "RESOURCE\\MODELS\\MAIN\\exodus_st.m3d", 0.0, 1.5},
    {30, "RESOURCE\\MODELS\\MAIN\\imperia_st.m3d", 0.0, 1.5},
    {31, "RESOURCE\\MODELS\\MAIN\\harkbackers_st.m3d", 0.0, 1.5},
    {32, "RESOURCE\\MODELS\\MAIN\\Teleport_alfa.m3d", 0.0, 1.0},
    {33, "RESOURCE\\MODELS\\MAIN\\Teleport_omega.m3d", 0.0, 1.0},
    {34, "RESOURCE\\MODELS\\MAIN\\bomb.m3d", 0.0, 1.0},
    {35, "RESOURCE\\MODELS\\MAIN\\laser_r.m3d", 0.0, 1.0},
    {36, "RESOURCE\\MODELS\\MAIN\\electro_gun.m3d", 0.0, 1.0},
    {37, "RESOURCE\\MODELS\\MAIN\\rocket_launcher.m3d", 0.0, 1.0},
    {38, "RESOURCE\\MODELS\\MAIN\\haubitz.m3d", 0.0, 1.0},
    {39, "RESOURCE\\MODELS\\MAIN\\subchaser.m3d", 0.0, 1.0},
    {40, "RESOURCE\\MODELS\\MAIN\\scum_disruptor.m3d", 0.0, 1.5},
    {41, "RESOURCE\\MODELS\\MAIN\\ballistic.m3d", 0.0, 1.0},
    {42, "RESOURCE\\MODELS\\MAIN\\filth_navigator.m3d", 0.0, 1.5},
};
inline constexpr std::size_t kUnitModelCount =
    sizeof(kUnitModels) / sizeof(kUnitModels[0]);

/// Lookup by terUnitAttributeID (nullptr when unknown).
inline const UnitModelInfo* unitModelInfo(int attributeId)
{
    if (attributeId < 0 ||
        static_cast<std::size_t>(attributeId) >= kUnitModelCount) {
        return nullptr;
    }
    const UnitModelInfo* info = &kUnitModels[attributeId];
    return info->id == attributeId ? info : nullptr;
}

} // namespace MapEditor

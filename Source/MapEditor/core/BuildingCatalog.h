#pragma once

// ============================================================================
// MapEditorCore — the catalog of pre-placed buildings for battle maps.
//
// The placeable set is the game's structure block (terUnitAttributeID
// 0..47, Source/Scripts/UnitAttribute.inl) MINUS UNIT_ATTRIBUTE_FRAME (the
// player base, edited via the Battle Map panel), UNIT_ATTRIBUTE_SCUM_SPOT*
// and UNIT_ATTRIBUTE_BLACK_HOLE (mission-script objects). IDs 0..42 cover
// exactly the remaining buildings — SCUM_SPOTs (43..46), BLACK_HOLE (47)
// and FRAME (49) are all outside the range.
//
// Per-building editor data the game derives from its M3D models, captured
// here as static tables:
//   * zOffset — the building's saved z sits zOffset ABOVE the zero layer:
//     terBuilding::setPose snaps z = hZeroPlast - boxMin().z, so the offset
//     is -boxMin().z. Measured from the shipped mission/multiplayer .spg
//     files (z - manualData.zeroLayerHeight); missing entries default to 10.
//   * radius — approximate footprint in world units for the "no
//     intersecting buildings" placement rule (the game uses the model
//     bounds; the editor has no M3D loader, so these are approximations).
//   * molecula — max element counts per damage type (hard), from
//     UnitAttribute.prm damageMolecula; the panel HP percent maps onto
//     damageMolecula.elementsDead in SaveUnitData.
//   * kind — the XPrm struct the game writes for the building in
//     nobodysBuildings (CORE -> SaveUnitProtectorData, COMMANDER ->
//     SaveUnitCommandCenterData, everything else SaveUnitBuildingData).
// ============================================================================

#include <array>
#include <cstdint>
#include <string>

namespace MapEditor {

enum class BuildingKind
{
    Building,      ///< "struct SaveUnitBuildingData"
    CommandCenter, ///< "struct SaveUnitCommandCenterData" (+ squad field)
    Protector,     ///< "struct SaveUnitProtectorData" (CORE, + 4 fields)
};

struct BuildingInfo
{
    int id;                  ///< terUnitAttributeID ordinal.
    const char* identifier;  ///< "UNIT_ATTRIBUTE_CORE" — written to .spg.
    const char* name;        ///< Display name (English short).
    BuildingKind kind;
    double zOffset;          ///< Saved z = zeroLayerHeight + zOffset.
    double radius;           ///< Footprint approximation (world units).
    std::array<int, 5> molecula; ///< Max damage elements per type.
};

/// The 43 placeable buildings, in ID order (0..42).
inline constexpr BuildingInfo kPlaceableBuildings[] = {
    {0, "UNIT_ATTRIBUTE_CORE", "Core", BuildingKind::Protector, 14.1105, 60.0, {0, 0, 0, 0, 75}},
    {1, "UNIT_ATTRIBUTE_RELAY", "Relay", BuildingKind::Building, 12.4774, 20.0, {0, 0, 0, 0, 100}},
    {2, "UNIT_ATTRIBUTE_COLLECTOR", "Collector", BuildingKind::Building, 10.9355, 25.0, {0, 0, 0, 0, 1000}},
    {3, "UNIT_ATTRIBUTE_SOLDIER_PLANT", "Soldier Plant", BuildingKind::Building, 19.5647, 40.0, {0, 0, 0, 0, 250}},
    {4, "UNIT_ATTRIBUTE_OFFICER_PLANT", "Officer Plant", BuildingKind::Building, 9.4755, 40.0, {0, 0, 0, 0, 250}},
    {5, "UNIT_ATTRIBUTE_TECHNIC_PLANT", "Technic Plant", BuildingKind::Building, 10.6148, 40.0, {0, 0, 0, 0, 250}},
    {6, "UNIT_ATTRIBUTE_COMMANDER", "Command Center", BuildingKind::CommandCenter, 8.1355, 45.0, {0, 0, 0, 0, 500}},
    {7, "UNIT_ATTRIBUTE_LASER_STATION1", "Laser Lab 1", BuildingKind::Building, 16.3084, 40.0, {0, 0, 0, 0, 600}},
    {8, "UNIT_ATTRIBUTE_ROCKET_STATION1", "Rocket Lab 1", BuildingKind::Building, 16.4945, 40.0, {0, 0, 0, 0, 1000}},
    {9, "UNIT_ATTRIBUTE_BOMB_STATION1", "Bomb Lab 1", BuildingKind::Building, 15.058, 40.0, {0, 0, 0, 0, 1600}},
    {10, "UNIT_ATTRIBUTE_FLY_STATION1", "Flying Lab 1", BuildingKind::Building, 17.311, 40.0, {0, 0, 0, 0, 1400}},
    {11, "UNIT_ATTRIBUTE_SUBTERRA_STATION1", "Subterra Lab 1", BuildingKind::Building, 12.7923, 40.0, {0, 0, 0, 0, 2000}},
    {12, "UNIT_ATTRIBUTE_ELECTRO_STATION1", "Electro Lab 1", BuildingKind::Building, 12.0, 40.0, {0, 0, 0, 0, 2000}},
    {13, "UNIT_ATTRIBUTE_EXODUS_STATION1", "Exodus Lab 1", BuildingKind::Building, 36.056, 40.0, {0, 0, 0, 0, 2600}},
    {14, "UNIT_ATTRIBUTE_EMPIRE_STATION1", "Empire Lab 1", BuildingKind::Building, 24.7217, 40.0, {0, 0, 0, 0, 2600}},
    {15, "UNIT_ATTRIBUTE_HARKBACK_STATION1", "Harkback Lab 1", BuildingKind::Building, 24.0, 40.0, {0, 0, 0, 0, 2600}},
    {16, "UNIT_ATTRIBUTE_LASER_STATION2", "Laser Lab 2", BuildingKind::Building, 16.3084, 40.0, {0, 0, 0, 0, 900}},
    {17, "UNIT_ATTRIBUTE_ROCKET_STATION2", "Rocket Lab 2", BuildingKind::Building, 16.4945, 40.0, {0, 0, 0, 0, 1500}},
    {18, "UNIT_ATTRIBUTE_BOMB_STATION2", "Bomb Lab 2", BuildingKind::Building, 15.058, 40.0, {0, 0, 0, 0, 2400}},
    {19, "UNIT_ATTRIBUTE_FLY_STATION2", "Flying Lab 2", BuildingKind::Building, 17.311, 40.0, {0, 0, 0, 0, 2100}},
    {20, "UNIT_ATTRIBUTE_SUBTERRA_STATION2", "Subterra Lab 2", BuildingKind::Building, 12.7923, 40.0, {0, 0, 0, 0, 3000}},
    {21, "UNIT_ATTRIBUTE_ELECTRO_STATION2", "Electro Lab 2", BuildingKind::Building, 12.0, 40.0, {0, 0, 0, 0, 3500}},
    {22, "UNIT_ATTRIBUTE_EXODUS_STATION2", "Exodus Lab 2", BuildingKind::Building, 36.056, 40.0, {0, 0, 0, 0, 3900}},
    {23, "UNIT_ATTRIBUTE_EMPIRE_STATION2", "Empire Lab 2", BuildingKind::Building, 24.7217, 40.0, {0, 0, 0, 0, 3900}},
    {24, "UNIT_ATTRIBUTE_HARKBACK_STATION2", "Harkback Lab 2", BuildingKind::Building, 24.0, 40.0, {0, 0, 0, 0, 3900}},
    {25, "UNIT_ATTRIBUTE_LASER_STATION3", "Laser Lab 3", BuildingKind::Building, 16.3084, 40.0, {0, 0, 0, 0, 1350}},
    {26, "UNIT_ATTRIBUTE_ROCKET_STATION3", "Rocket Lab 3", BuildingKind::Building, 16.4945, 40.0, {0, 0, 0, 0, 2250}},
    {27, "UNIT_ATTRIBUTE_BOMB_STATION3", "Bomb Lab 3", BuildingKind::Building, 15.058, 40.0, {0, 0, 0, 0, 3600}},
    {28, "UNIT_ATTRIBUTE_ELECTRO_STATION3", "Electro Lab 3", BuildingKind::Building, 12.0, 40.0, {0, 0, 0, 0, 5000}},
    {29, "UNIT_ATTRIBUTE_EXODUS_STATION3", "Exodus Lab 3", BuildingKind::Building, 36.056, 40.0, {0, 0, 0, 0, 5850}},
    {30, "UNIT_ATTRIBUTE_EMPIRE_STATION3", "Empire Lab 3", BuildingKind::Building, 24.7217, 40.0, {0, 0, 0, 0, 5850}},
    {31, "UNIT_ATTRIBUTE_HARKBACK_STATION3", "Harkback Lab 3", BuildingKind::Building, 24.0, 40.0, {0, 0, 0, 0, 5850}},
    {32, "UNIT_ATTRIBUTE_CORRIDOR_ALPHA", "Corridor Alpha", BuildingKind::Building, 30.0, 45.0, {0, 0, 0, 0, 1000}},
    {33, "UNIT_ATTRIBUTE_CORRIDOR_OMEGA", "Corridor Omega", BuildingKind::Building, 8.212, 45.0, {0, 0, 0, 0, 5000}},
    {34, "UNIT_ATTRIBUTE_STATIC_BOMB", "Static Bomb", BuildingKind::Building, 5.0, 15.0, {0, 0, 0, 0, 10000}},
    {35, "UNIT_ATTRIBUTE_LASER_CANNON", "Laser Cannon", BuildingKind::Building, 5.2706, 20.0, {0, 0, 0, 64, 128}},
    {36, "UNIT_ATTRIBUTE_ELECTRO_CANNON", "Electro Cannon", BuildingKind::Building, 5.0, 20.0, {0, 0, 0, 64, 128}},
    {37, "UNIT_ATTRIBUTE_ROCKET_LAUNCHER", "Rocket Launcher", BuildingKind::Building, 2.3848, 20.0, {0, 0, 0, 64, 128}},
    {38, "UNIT_ATTRIBUTE_GUN_HOWITZER", "Howitzer", BuildingKind::Building, 0.7026, 20.0, {0, 0, 0, 64, 128}},
    {39, "UNIT_ATTRIBUTE_GUN_SUBCHASER", "Subchaser", BuildingKind::Building, 3.5241, 20.0, {0, 0, 0, 64, 128}},
    {40, "UNIT_ATTRIBUTE_GUN_SCUM_DISRUPTOR", "Scum Disruptor", BuildingKind::Building, 5.0, 20.0, {0, 0, 0, 64, 128}},
    {41, "UNIT_ATTRIBUTE_GUN_BALLISTIC", "Ballistic Gun", BuildingKind::Building, 5.0, 20.0, {0, 0, 0, 64, 128}},
    {42, "UNIT_ATTRIBUTE_GUN_FILTH_NAVIGATOR", "Filth Navigator", BuildingKind::Building, 5.0, 20.0, {0, 0, 0, 64, 128}},
};
inline constexpr std::size_t kPlaceableBuildingCount =
    sizeof(kPlaceableBuildings) / sizeof(kPlaceableBuildings[0]);

/// The charged-shot superweapons (their first shot waits for the weapon
/// charge): Scum Disruptor, the Missile Gun (Ballistic) and the Filth
/// Navigator. Their initial charge is the spg `weaponChargeLevel` (0..1) —
/// only these get the Charge control in the Buildings panel.
inline bool isChargeableSuperweapon(int attributeId)
{
    return attributeId == 40 || // UNIT_ATTRIBUTE_GUN_SCUM_DISRUPTOR
           attributeId == 41 || // UNIT_ATTRIBUTE_GUN_BALLISTIC (Missile Gun)
           attributeId == 42;   // UNIT_ATTRIBUTE_GUN_FILTH_NAVIGATOR
}

/// Lookup by terUnitAttributeID (nullptr when not placeable).
inline const BuildingInfo* buildingInfo(int attributeId)
{
    if (attributeId < 0 ||
        static_cast<std::size_t>(attributeId) >= kPlaceableBuildingCount) {
        return nullptr;
    }
    const BuildingInfo* info = &kPlaceableBuildings[attributeId];
    return info->id == attributeId ? info : nullptr;
}

} // namespace MapEditor

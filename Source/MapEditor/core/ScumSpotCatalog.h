#pragma once

#include <array>

// ============================================================================
// MapEditorCore — the catalog of placeable scum nests (SCUM_SPOT*).
//
// The four infestation mounds (terUnitAttributeID 43..46,
// Source/Scripts/UnitAttribute.inl) — visible environment structures
// (UnitAttribute.prm:4860+, models filth_spot*.m3d). Each placed nest is
// paired with an invisible UNIT_ATTRIBUTE_FILTH_SPOT spawner saved in
// SavePrm.filth.objects (SaveUnitFilthData); the pairing + activation
// wiring is generated as a Scripts/Triggers/<world>.scr trigger chain
// (see docs/architecture.md).
//
// zOffset mirrors BuildingCatalog: saved z = zeroLayerHeight + offset; the
// game re-snaps z on load anyway (terBuilding::setPose). Measured from the
// shipped 03ISOLAND.spg (SCUM_SPOT at zeroLayer 90, z 94.4335).
//
// kMinScumSpotDistance is the placement rule from the spec: two nests must
// not be closer than this (also keeps the 50-unit trigger proximity radius
// unambiguous).
// ============================================================================

namespace MapEditor {

struct ScumSpotInfo
{
    int id;                 ///< terUnitAttributeID ordinal (43..46).
    const char* identifier; ///< "UNIT_ATTRIBUTE_SCUM_SPOT" — written to .spg.
    const char* name;       ///< Display name.
    double zOffset;         ///< Saved z = zeroLayerHeight + zOffset.
    std::array<int, 5> molecula; ///< Max damage elements (UnitAttribute.prm).
};

inline constexpr ScumSpotInfo kScumSpots[] = {
    {43, "UNIT_ATTRIBUTE_SCUM_SPOT", "Scum Spot", 4.4335, {0, 0, 0, 0, 500}},
    {44, "UNIT_ATTRIBUTE_SCUM_SPOT2", "Scum Spot 2", 4.4335, {0, 0, 0, 0, 200}},
    {45, "UNIT_ATTRIBUTE_SCUM_SPOT3", "Scum Spot 3", 4.4335, {0, 0, 0, 0, 200}},
    {46, "UNIT_ATTRIBUTE_SCUM_SPOT4", "Scum Spot 4", 4.4335, {0, 0, 0, 0, 500}},
};
inline constexpr int kScumSpotCount = 4;
inline constexpr double kMinScumSpotDistance = 150.0;

/// Lookup by terUnitAttributeID (nullptr when not a scum spot).
inline const ScumSpotInfo* scumSpotInfo(int attributeId)
{
    for (const ScumSpotInfo& info : kScumSpots) {
        if (info.id == attributeId) {
            return &info;
        }
    }
    return nullptr;
}

} // namespace MapEditor

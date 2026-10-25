#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
// MapEditorCore — MarkerStore (T7.1).
//
// Editor-only map markers in a sidecar file NEXT to the world folder:
//   RESOURCE/Worlds/<world>.medit.json
// The game never reads it and nothing is exported into .spg — markers exist
// only for the editor (§3 item 9 of the plan).
//
// The file is plain JSON (schema below) parsed/serialized with a small
// built-in reader — MapEditorCore stays Qt-free and dependency-free:
//
// {
//   "version": 1,
//   "world": "COMPASS",
//   "nextId": 4,
//   "markers": [
//     {"id": 1, "type": "point", "label": "Spawn idea",
//      "note": "...", "color": "#ffff00", "x": 120, "y": 300, "radius": 0}
//   ]
// }
//
// type is one of "unit" | "building" | "zone" | "point"; radius (>0) is only
// meaningful for "zone". color is "#RRGGBB".
// ============================================================================

namespace MapEditor {

struct MapMarkerData
{
    std::uint64_t id = 0;
    std::string type = "point"; // unit | building | zone | point
    std::string label;
    std::string note;
    unsigned int colorRgb = 0xFFFF00; // 0xRRGGBB
    int x = 0;                        // map cells
    int y = 0;
    int radius = 0;                   // zone radius in cells (0 = plain dot)

    bool operator==(const MapMarkerData& other) const
    {
        return id == other.id && type == other.type && label == other.label &&
               note == other.note && colorRgb == other.colorRgb &&
               x == other.x && y == other.y && radius == other.radius;
    }
};

class MarkerStore
{
public:
    /// Sidecar path for a world folder: <…>/Worlds/<name>.medit.json.
    static std::filesystem::path
    sidecarPathFor(const std::filesystem::path& worldFolder);

    /// True when the string is one of the four marker types.
    static bool isValidType(const std::string& type);

    // -----------------------------------------------------------------------
    // CRUD (ids are assigned by the store and never reused)
    // -----------------------------------------------------------------------

    const std::vector<MapMarkerData>& markers() const { return markers_; }
    std::size_t size() const { return markers_.size(); }
    void clear() { markers_.clear(); }

    /// Add a copy without id (it is assigned) — returns the new id.
    std::uint64_t add(const MapMarkerData& marker);
    /// Replace the marker with the same id. Returns false when not found.
    bool update(const MapMarkerData& marker);
    /// Remove by id. Returns false when not found.
    bool remove(std::uint64_t id);
    const MapMarkerData* find(std::uint64_t id) const;

    // -----------------------------------------------------------------------
    // Persistence
    // -----------------------------------------------------------------------

    /// Parse a sidecar file. A missing file is not an error (false, store
    /// cleared); a malformed one leaves the store cleared too (false).
    bool load(const std::filesystem::path& path);
    /// Write the sidecar (pretty-printed, 2-space indent).
    bool save(const std::filesystem::path& path, const std::string& worldName) const;

    /// Parse from memory (tests). Returns false on malformed input.
    bool parseJson(const std::string& text, const std::string& worldName);
    /// Serialize to memory (tests).
    std::string toJson(const std::string& worldName) const;

private:
    std::vector<MapMarkerData> markers_;
    std::uint64_t nextId_ = 1;
};

} // namespace MapEditor

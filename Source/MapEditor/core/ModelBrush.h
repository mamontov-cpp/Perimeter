#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
// MapEditorCore — ModelBrush: a 3D model imported as a heightmap brush.
//
// importPly() reads a PLY model (ASCII or binary little-endian) and treats
// it as a SOLID: the XY bounding rectangle of its vertices is rasterized
// into a grid, and each cell's height is the TOP of the model above that
// column — with faces present every (fan-triangulated) face is sampled at
// the cell centre and the highest surface wins, so the interior of a closed
// mesh is covered by its top faces, not left as holes; face-less models
// (point clouds) fall back to the per-vertex maximum. Heights are
// normalized to [0,1] over the model's z range; the colour comes from the
// winning surface (barycentric interpolation of the vertex colours). Vertex
// colours are red/green/blue properties; a PLY header comment naming a
// texture file ("comment ... texture <file>", case-insensitive) switches the
// colours to that texture sampled by the UV properties (24-bit TGA, loaded
// relative to the .ply). A model with neither texture nor vertex colours
// paints WHITE. Columns with no surface above them (real holes) get height 0
// and the model's average colour.
//
// The brush stamps the raster as a rectangle (nearest-resampled to the
// scale-spin size): "place" sets the dam to the scaled height and paints the
// quantized Dam palette colours, "add"/"subtract" offset the covered area's
// average height by the local model height (see BrushTool::applyModelBrush).
// The colour quantization itself reuses ImageDecal::prepare (same k-means
// with the unused-colour accounting) — the caller turns the per-cell RGB
// into an RGBA image and passes it there.
//
// Qt-free so the headless tests drive the import directly.
// ============================================================================

namespace MapEditor::ModelBrush {

/// The model as a height/colour raster: the placement rectangle.
struct ModelRaster
{
    int width = 0;   ///< grid columns (model X).
    int height = 0;  ///< grid rows (model Y).
    /// width*height normalized max heights (row-major, 0..1; an empty cell
    /// — a hole — is 0, the bottom of the model's z range).
    std::vector<double> normHeight;
    /// width*height top colours, 0x00RRGGBB (holes take the model's average
    /// colour so the whole rectangle paints).
    std::vector<std::uint32_t> rgb;

    bool empty() const
    {
        return width <= 0 || height <= 0 ||
               normHeight.size() != static_cast<std::size_t>(width) * height ||
               rgb.size() != static_cast<std::size_t>(width) * height;
    }
};

/// importPly() result. `error` is a user-facing description (the UI prefixes
/// it with "Unable to import from PLY the model <name>").
struct ImportResult
{
    bool ok = false;
    std::string error;
    ModelRaster model;
};

/// Longest raster side the import produces (cells) — the same cap the image
/// brushes use.
inline constexpr int kMaxRasterSide = 512;

/// Read a PLY model into the placement raster. Returns ok=false with a
/// description of the first problem (unreadable file, bad header, missing
/// vertex x/y/z, unsupported binary big-endian, missing/invalid texture...).
[[nodiscard]] ImportResult importPly(const std::filesystem::path& path);

/// Nearest-neighbour resample to (newWidth, newHeight) — the X/Y scale
/// spins. Heights and colours are never interpolated.
ModelRaster resample(const ModelRaster& source, int newWidth, int newHeight);

} // namespace MapEditor::ModelBrush

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// ============================================================================
// MapEditorCore — WorldImages (T2.4, groundwork for T6.1).
//
// Qt-free image helpers shared by the world-save pipeline (cache.tga /
// map.tga generation inside EditorSession::saveWorld) and the minimap panel
// (ui/MinimapWidget.cpp), so the displayed minimap and the saved files can
// never drift apart: both go through the same raster and same TGA code.
//
// All raster buffers handed around here are 24-bit BGR byte triples,
// row-major, top-left origin — the layout the game's own TGA reader
// (Terra/tgai.h) works with and the layout writeTga24 produces.
// ============================================================================

namespace MapEditor::WorldImages {

/// Maximum size of the map.tga radar/minimap texture. PerimeterShellUI reads
/// the file with an arbitrary size; the game worlds ship at most 256x256 and
/// the editor's generator follows that.
inline constexpr unsigned kMinimapMaxSize = 256;

/// Write a 24-bit uncompressed true-color TGA (top-left origin) from a BGR
/// byte buffer; `bgr` must hold width * height * 3 bytes.
bool writeTga24(const std::filesystem::path& path, unsigned width,
                unsigned height, const std::vector<unsigned char>& bgr);

/// Read a 24-bit uncompressed true-color TGA. Both origin descriptors are
/// accepted (bottom-left rows are flipped on load); the output is always a
/// top-left BGR buffer. Returns false for anything else (palette, RLE-
/// compressed, 16/32-bit — files this editor never produces for map.tga), in
/// which case the outputs are left cleared.
bool loadTga24(const std::filesystem::path& path, unsigned& width,
               unsigned& height, std::vector<unsigned char>& bgr);

/// Nearest-neighbour downscale of a 24-bit BGR raster (the shared core of
/// map.tga generation and the minimap's scaled display).
void downscaleNearest(const std::vector<unsigned char>& source,
                      unsigned sourceWidth, unsigned sourceHeight,
                      unsigned targetWidth, unsigned targetHeight,
                      std::vector<unsigned char>& target);

/// Render the loaded world into a V_SIZE x H_SIZE BGR buffer. Rasterisation
/// goes through ViewportRenderer with a fresh instance — its defaults are the
/// Composite filter and no overlays (i.e. plain vrtMap::getColor32()), so the
/// generated files and the on-screen viewport never drift apart pixel-wise.
/// Returns false when no world is loaded.
bool renderToBgr(std::vector<unsigned char>& bgr, unsigned& width,
                 unsigned& height);

/// cache.tga — 24-bit H_SIZE x V_SIZE snapshot of the whole surface. Terra's
/// fullLoad() xasserts on this file, so it must be (re)generated on every save.
bool generateCacheTga(const std::filesystem::path& worldDir);

/// map.tga — the radar/minimap texture (PerimeterShellUI reads it with an
/// arbitrary size): the full surface downscaled to at most kMinimapMaxSize
/// on each axis.
bool generateMapTga(const std::filesystem::path& worldDir);

} // namespace MapEditor::WorldImages

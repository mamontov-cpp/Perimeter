#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// MapEditorCore — ImageDecal (true-color image stamp).
//
// Converts a user image into an RGBA stamp for the ImageDecal brush. There is
// NO palette quantization: every fully opaque pixel keeps its exact 24-bit
// color (0xAARRGGBB), which the brush paints into vrtMap's true-color layer
// (vrtMap::SurColBuf, saved in the heights16.bin sidecar — see
// docs/map_logic.md). The nearest Dam palette entry is still computed per
// pixel, so SurBuf keeps a sensible surface type (materials, palette
// fallback) without the palette limiting the visible color.
//
// Everything here is Qt-free and operates on plain RGBA bytes, so the headless
// tests drive it directly.
// ============================================================================

namespace MapEditor::ImageDecal {

/// Largest accepted image side, in pixels. 2048x2048 covers a full quarter
/// of a 4096-cell map; larger pictures are better applied in slices.
inline constexpr int kMaxImageSize = 2048;

/// An RGBA image stamp: one 0xAARRGGBB entry per pixel, AA=0 = no paint
/// (transparent). `indices` carries the nearest Dam palette entry per pixel
/// (meaningful for opaque pixels only).
struct DecalImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;  ///< width * height, row-major.
    std::vector<std::uint8_t> indices;  ///< same size, nearest palette entry.

    bool empty() const
    {
        return width <= 0 || height <= 0 ||
               pixels.size() != static_cast<std::size_t>(width) * height;
    }
    std::uint32_t at(int x, int y) const
    {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
    std::uint8_t indexAt(int x, int y) const
    {
        return indices[static_cast<std::size_t>(y) * width + x];
    }
};

/// Result of prepare(). `error` is user-facing text when ok == false.
struct PrepareResult
{
    bool ok = false;
    std::string error;
    DecalImage image;
};

/// Convert an RGBA image (width*height*4 bytes, R,G,B,A) into a true-color
/// decal. Only fully opaque pixels (alpha == 255) paint — an "almost opaque"
/// pixel is transparent for the decal and must never paint. `damPaletteRgb`
/// points at the 256*3 RGB bytes of vrtMap::DamPal and is used solely for the
/// nearest-palette-entry side band written into `image.indices`.
[[nodiscard]] PrepareResult prepare(const unsigned char* rgba, int width,
                                    int height,
                                    const unsigned char* damPaletteRgb);

/// Nearest palette entry (squared RGB distance) — the SurBuf side band.
int nearestPaletteEntry(const unsigned char* palette, std::uint32_t rgb);

/// Nearest-neighbour rescale of a decal (the brush scale spinbox). Transparent
/// pixels stay transparent; colors and palette indices are never interpolated.
DecalImage scaleNearest(const DecalImage& source, double factor);

} // namespace MapEditor::ImageDecal

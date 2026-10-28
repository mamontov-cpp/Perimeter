#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// MapEditorCore — ImageDecal (palette-limited image stamp).
//
// Converts a user image into a small palette-index image that the ImageDecal
// brush paints into SurBuf of dam cells (the surface colour index of the Dam
// layer). The decal never carries RGB at paint time: every pixel is a Dam
// palette entry, so the stamp is exactly as cheap as the SetColor brush.
//
// The conversion is a palette-aware k-means (see docs/architecture.md §5.15):
//
//   1. The image is rejected when it is larger than kMaxImageSize on either
//      axis, and pixels that are not fully opaque (alpha != 255) are skipped
//      entirely — an "almost opaque" pixel never paints.
//   2. The image colours are matched against the world's Dam palette; the
//      palette entries the world's dam layer already uses are collected too.
//      A world with fewer than two free palette entries cannot absorb a
//      colourful image and gets a user-facing error instead.
//   3. The candidate palette entries (image matches + entries in use, padded
//      to at least kMinPaletteColors) become the k-means initial centres.
//   4. k-means over the opaque pixels produces the final colours; colours
//      that are not in the palette yet are reported in `paletteWrites` so the
//      caller can add them to the Dam palette.
//
// Everything here is Qt-free and operates on plain RGBA bytes, so the headless
// tests drive it directly.
// ============================================================================

namespace MapEditor::ImageDecal {

/// Largest accepted image side, in pixels (the brush stamp is a map patch —
/// 512x512 already covers a quarter of a 1024-cell map).
inline constexpr int kMaxImageSize = 512;

/// Minimum number of palette colours the k-means starts with. The candidate
/// list is padded with free palette entries up to this count, so the result
/// can always express at least this many shades even when the world's dam
/// layer uses only a handful of colours.
inline constexpr int kMinPaletteColors = 64;

/// A palette-index image: one entry per pixel, -1 = no paint (transparent).
struct DecalImage
{
    int width = 0;
    int height = 0;
    std::vector<int> indices; ///< width * height, row-major, -1 = transparent.

    bool empty() const
    {
        return width <= 0 || height <= 0 ||
               indices.size() != static_cast<std::size_t>(width) * height;
    }
    int at(int x, int y) const
    {
        return indices[static_cast<std::size_t>(y) * width + x];
    }
};

/// One Dam palette entry the caller has to write (a colour the k-means
/// produced that the palette did not contain yet).
struct PaletteWrite
{
    int index = 0;
    std::uint32_t rgb = 0; ///< 0x00RRGGBB.
};

/// Result of prepare(). `error` is user-facing text when ok == false.
struct PrepareResult
{
    bool ok = false;
    std::string error;
    DecalImage image;
    std::vector<PaletteWrite> paletteWrites;

    /// Diagnostics for the status bar / tests.
    int unusedDamColors = 0;    ///< Dam entries the world's dam does not use.
    int imageForeignColors = 0; ///< distinct image colours absent from the palette.
    int candidateColors = 0;    ///< size of the k-means start list (step 3).
    int resultColors = 0;       ///< non-empty k-means clusters (colours used).
};

/// Convert an RGBA image (width*height*4 bytes, R,G,B,A) into a palette-index
/// decal for the given Dam palette.
///
/// `damPaletteRgb` points at the 256*3 RGB bytes of vrtMap::DamPal,
/// `damEntryInUse` at 256 flags (non-zero = the world's dam layer already
/// paints with that entry).
PrepareResult prepare(const unsigned char* rgba, int width, int height,
                      const unsigned char* damPaletteRgb,
                      const unsigned char* damEntryInUse);

/// Nearest-neighbour rescale of a decal (the brush scale spinbox). Transparent
/// pixels stay transparent, palette indices are never interpolated.
DecalImage scaleNearest(const DecalImage& source, double factor);

} // namespace MapEditor::ImageDecal

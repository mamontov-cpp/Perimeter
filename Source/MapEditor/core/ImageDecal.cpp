// ============================================================================
// MapEditorCore — ImageDecal implementation. See ImageDecal.h.
// ============================================================================

#include "ImageDecal.h"

#include <algorithm>
#include <climits>

namespace MapEditor::ImageDecal {

namespace {

inline std::uint32_t rgbOf(const unsigned char* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           static_cast<std::uint32_t>(p[2]);
}

} // namespace

// ---------------------------------------------------------------------------

int nearestPaletteEntry(const unsigned char* palette, std::uint32_t rgb)
{
    const int r = static_cast<int>((rgb >> 16) & 0xFF);
    const int g = static_cast<int>((rgb >> 8) & 0xFF);
    const int b = static_cast<int>(rgb & 0xFF);
    int best = 0;
    long bestDistance = LONG_MAX;
    for (int i = 0; i < 256; ++i) {
        const int dr = static_cast<int>(palette[i * 3 + 0]) - r;
        const int dg = static_cast<int>(palette[i * 3 + 1]) - g;
        const int db = static_cast<int>(palette[i * 3 + 2]) - b;
        const long distance = dr * dr + dg * dg + db * db;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

PrepareResult prepare(const unsigned char* rgba, int width, int height,
                      const unsigned char* damPaletteRgb)
{
    PrepareResult result;
    if (rgba == nullptr || damPaletteRgb == nullptr ||
        width <= 0 || height <= 0) {
        result.error = "The image is empty.";
        return result;
    }
    if (width > kMaxImageSize || height > kMaxImageSize) {
        result.error =
            "The image is too large (" + std::to_string(width) + "x" +
            std::to_string(height) + " px). The maximum size of an image is " +
            std::to_string(kMaxImageSize) + " px on each side.";
        return result;
    }

    // True-color pass-through: the pixel colors go into the stamp verbatim —
    // no k-means, no palette pressure. Only fully opaque pixels paint.
    DecalImage image;
    image.width = width;
    image.height = height;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    image.pixels.assign(count, 0);
    image.indices.assign(count, 0);
    bool anyOpaque = false;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* pixel = rgba + i * 4;
        if (pixel[3] != 255) {
            continue; // stays transparent (pixel value 0)
        }
        anyOpaque = true;
        const std::uint32_t rgb = rgbOf(pixel);
        image.pixels[i] = 0xFF000000u | rgb;
        image.indices[i] = static_cast<std::uint8_t>(
            nearestPaletteEntry(damPaletteRgb, rgb));
    }
    if (!anyOpaque) {
        result.error = "The image has no fully opaque pixels to stamp.";
        return result;
    }

    result.ok = true;
    result.image = std::move(image);
    return result;
}

DecalImage scaleNearest(const DecalImage& source, double factor)
{
    DecalImage scaled;
    if (source.empty() || factor <= 0.0) {
        return scaled;
    }
    scaled.width = std::max(
        1, static_cast<int>(source.width * factor + 0.5));
    scaled.height = std::max(
        1, static_cast<int>(source.height * factor + 0.5));
    const std::size_t count =
        static_cast<std::size_t>(scaled.width) * scaled.height;
    scaled.pixels.resize(count, 0);
    scaled.indices.resize(count, 0);
    for (int y = 0; y < scaled.height; ++y) {
        const int sy = std::min(
            source.height - 1,
            static_cast<int>(static_cast<long long>(y) * source.height /
                             scaled.height));
        for (int x = 0; x < scaled.width; ++x) {
            const int sx = std::min(
                source.width - 1,
                static_cast<int>(static_cast<long long>(x) * source.width /
                                 scaled.width));
            const std::size_t dst =
                static_cast<std::size_t>(y) * scaled.width + x;
            const std::size_t src = static_cast<std::size_t>(sy) * source.width + sx;
            scaled.pixels[dst] = source.pixels[src];
            scaled.indices[dst] = source.indices[src];
        }
    }
    return scaled;
}

} // namespace MapEditor::ImageDecal

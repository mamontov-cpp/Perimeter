// ============================================================================
// MapEditorCore — WorldImages implementation (T2.4, groundwork for T6.1).
// ============================================================================

#include "WorldImages.h"

#include "ViewportRenderer.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace MapEditor::WorldImages {

bool writeTga24(const std::filesystem::path& path, unsigned width,
                unsigned height, const std::vector<unsigned char>& bgr)
{
    if (width == 0 || height == 0 ||
        bgr.size() < static_cast<std::size_t>(width) * height * 3) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "WorldImages: cannot write '%s'.\n",
                path.u8string().c_str());
        return false;
    }
    unsigned char header[18] = {};
    header[2] = 2; // uncompressed true-color
    header[12] = static_cast<unsigned char>(width & 0xFF);
    header[13] = static_cast<unsigned char>((width >> 8) & 0xFF);
    header[14] = static_cast<unsigned char>(height & 0xFF);
    header[15] = static_cast<unsigned char>((height >> 8) & 0xFF);
    header[16] = 24;
    header[17] = 0x20; // top-left origin
    out.write(reinterpret_cast<const char*>(header), 18);
    out.write(reinterpret_cast<const char*>(bgr.data()),
              static_cast<std::streamsize>(width) * height * 3);
    return out.good();
}

bool loadTga24(const std::filesystem::path& path, unsigned& width,
               unsigned& height, std::vector<unsigned char>& bgr)
{
    width = 0;
    height = 0;
    bgr.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "WorldImages: cannot read '%s'.\n",
                path.u8string().c_str());
        return false;
    }

    unsigned char header[18] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in) {
        return false;
    }
    // Uncompressed true-color only.
    if (header[2] != 2) {
        fprintf(stderr,
                "WorldImages: '%s' is not an uncompressed 24-bit TGA.\n",
                path.u8string().c_str());
        return false;
    }
    const unsigned w = static_cast<unsigned>(header[12]) |
                       (static_cast<unsigned>(header[13]) << 8);
    const unsigned h = static_cast<unsigned>(header[14]) |
                       (static_cast<unsigned>(header[15]) << 8);
    if (header[16] != 24 || w == 0 || h == 0) {
        return false;
    }
    // Descriptor bit 5: 1 = top-left, 0 = bottom-left origin.
    const bool bottomLeft = (header[17] & 0x20) == 0;

    std::vector<unsigned char> file(static_cast<std::size_t>(w) * h * 3);
    in.read(reinterpret_cast<char*>(file.data()),
            static_cast<std::streamsize>(file.size()));
    if (!in) {
        fprintf(stderr, "WorldImages: '%s' is truncated.\n",
                path.u8string().c_str());
        return false;
    }

    if (bottomLeft) {
        std::vector<unsigned char> flipped(file.size());
        for (unsigned y = 0; y < h; ++y) {
            const auto from = static_cast<std::size_t>(h - 1 - y) * w * 3;
            const auto to = static_cast<std::size_t>(y) * w * 3;
            std::copy_n(file.data() + from, static_cast<std::size_t>(w) * 3,
                        flipped.data() + to);
        }
        bgr = std::move(flipped);
    } else {
        bgr = std::move(file);
    }
    width = w;
    height = h;
    return true;
}

void downscaleNearest(const std::vector<unsigned char>& source,
                      unsigned sourceWidth, unsigned sourceHeight,
                      unsigned targetWidth, unsigned targetHeight,
                      std::vector<unsigned char>& target)
{
    if (sourceWidth == 0 || sourceHeight == 0 || targetWidth == 0 ||
        targetHeight == 0 ||
        source.size() <
            static_cast<std::size_t>(sourceWidth) * sourceHeight * 3) {
        target.clear();
        return;
    }
    target.assign(static_cast<std::size_t>(targetWidth) * targetHeight * 3, 0);
    for (unsigned y = 0; y < targetHeight; ++y) {
        const unsigned sy = std::min(y * sourceHeight / targetHeight,
                                     sourceHeight - 1);
        for (unsigned x = 0; x < targetWidth; ++x) {
            const unsigned sx = std::min(x * sourceWidth / targetWidth,
                                         sourceWidth - 1);
            const std::size_t src =
                (static_cast<std::size_t>(sy) * sourceWidth + sx) * 3;
            const std::size_t dst =
                (static_cast<std::size_t>(y) * targetWidth + x) * 3;
            target[dst + 0] = source[src + 0];
            target[dst + 1] = source[src + 1];
            target[dst + 2] = source[src + 2];
        }
    }
}

bool renderToBgr(std::vector<unsigned char>& bgr, unsigned& width,
                 unsigned& height)
{
    width = 0;
    height = 0;
    bgr.clear();

    ViewportRenderer renderer;
    if (!renderer.bindWorld()) {
        return false;
    }
    width = static_cast<unsigned>(renderer.width());
    height = static_cast<unsigned>(renderer.height());
    bgr.assign(static_cast<std::size_t>(width) * height * 3, 0);
    const std::uint32_t* pixels = renderer.pixels();
    const std::size_t count = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t col32 = pixels[i];
        bgr[i * 3 + 0] = static_cast<unsigned char>(col32 & 0xFF);         // B
        bgr[i * 3 + 1] = static_cast<unsigned char>((col32 >> 8) & 0xFF);  // G
        bgr[i * 3 + 2] = static_cast<unsigned char>((col32 >> 16) & 0xFF); // R
    }
    return true;
}

bool generateCacheTga(const std::filesystem::path& worldDir)
{
    std::vector<unsigned char> bgr;
    unsigned width = 0;
    unsigned height = 0;
    if (!renderToBgr(bgr, width, height)) {
        return false;
    }
    return writeTga24(worldDir / "cache.tga", width, height, bgr);
}

bool generateMapTga(const std::filesystem::path& worldDir)
{
    std::vector<unsigned char> source;
    unsigned sw = 0;
    unsigned sh = 0;
    if (!renderToBgr(source, sw, sh)) {
        return false;
    }

    const unsigned tw = std::min<unsigned>(sw, kMinimapMaxSize);
    const unsigned th = std::min<unsigned>(sh, kMinimapMaxSize);
    std::vector<unsigned char> minimap;
    downscaleNearest(source, sw, sh, tw, th, minimap);
    return writeTga24(worldDir / "map.tga", tw, th, minimap);
}

} // namespace MapEditor::WorldImages

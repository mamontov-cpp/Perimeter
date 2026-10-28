// ============================================================================
// MapEditorCore — ImageDecal implementation. See ImageDecal.h.
// ============================================================================

#include "ImageDecal.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace MapEditor::ImageDecal {

namespace {

inline std::uint32_t rgbOf(const unsigned char* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           static_cast<std::uint32_t>(p[2]);
}

/// 5-bit-per-channel bucket key: the clustering works on this grid so a
/// photographic image cannot blow up the k-means point count, while the sums
/// below stay exact (the cluster centre is the true mean colour).
inline int bucketKey(int r, int g, int b)
{
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

/// One k-means point: all image pixels of one 5-bit RGB bucket.
struct Point
{
    int key = 0;
    long long sumR = 0;
    long long sumG = 0;
    long long sumB = 0;
    long long count = 0;

    double r() const { return static_cast<double>(sumR) / static_cast<double>(count); }
    double g() const { return static_cast<double>(sumG) / static_cast<double>(count); }
    double b() const { return static_cast<double>(sumB) / static_cast<double>(count); }
};

struct Center
{
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

inline double distance2(const Center& c, double r, double g, double b)
{
    const double dr = c.r - r;
    const double dg = c.g - g;
    const double db = c.b - b;
    return dr * dr + dg * dg + db * db;
}

inline std::uint32_t quantizeCenter(const Center& c)
{
    const auto channel = [](double v) {
        const long rounded = std::lround(v);
        return static_cast<std::uint32_t>(std::clamp<long>(rounded, 0, 255));
    };
    return (channel(c.r) << 16) | (channel(c.g) << 8) | channel(c.b);
}

/// Closest Dam palette entry to an RGB colour (fallback when every palette
/// slot is taken and no new colour may be added).
int nearestPaletteEntry(const unsigned char* palette, std::uint32_t rgb)
{
    const int r = static_cast<int>((rgb >> 16) & 0xFF);
    const int g = static_cast<int>((rgb >> 8) & 0xFF);
    const int b = static_cast<int>(rgb & 0xFF);
    int best = 0;
    long bestDistance = -1;
    for (int i = 0; i < 256; ++i) {
        const int dr = static_cast<int>(palette[i * 3 + 0]) - r;
        const int dg = static_cast<int>(palette[i * 3 + 1]) - g;
        const int db = static_cast<int>(palette[i * 3 + 2]) - b;
        const long distance = dr * dr + dg * dg + db * db;
        if (bestDistance < 0 || distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

constexpr int kMaxIterations = 10;

} // namespace

// ---------------------------------------------------------------------------

PrepareResult prepare(const unsigned char* rgba, int width, int height,
                      const unsigned char* damPaletteRgb,
                      const unsigned char* damEntryInUse)
{
    PrepareResult result;
    if (rgba == nullptr || damPaletteRgb == nullptr ||
        damEntryInUse == nullptr || width <= 0 || height <= 0) {
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

    // --- 1. Palette lookup table + exact image histogram (opaque pixels) ---
    std::map<std::uint32_t, int> paletteIndexByRgb;
    for (int i = 0; i < 256; ++i) {
        paletteIndexByRgb.emplace(rgbOf(damPaletteRgb + i * 3), i);
    }

    std::map<std::uint32_t, int> imageHistogram;
    std::map<int, Point> points;
    int opaquePixels = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char* pixel = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
            // Only fully opaque pixels take part: an "almost opaque" pixel is
            // transparent for the decal and must never paint.
            if (pixel[3] != 255) {
                continue;
            }
            ++opaquePixels;
            const std::uint32_t rgb = rgbOf(pixel);
            ++imageHistogram[rgb];
            Point& point = points[bucketKey(pixel[0], pixel[1], pixel[2])];
            point.key = bucketKey(pixel[0], pixel[1], pixel[2]);
            point.sumR += pixel[0];
            point.sumG += pixel[1];
            point.sumB += pixel[2];
            ++point.count;
        }
    }
    if (opaquePixels == 0) {
        result.error = "The image has no fully opaque pixels to stamp.";
        return result;
    }

    // --- 2. How many palette entries are free / how colourful is the image ---
    int usedDamColors = 0;
    for (int i = 0; i < 256; ++i) {
        if (damEntryInUse[i] != 0) {
            ++usedDamColors;
        }
    }
    result.unusedDamColors = 256 - usedDamColors;
    for (const auto& entry : imageHistogram) {
        if (paletteIndexByRgb.find(entry.first) == paletteIndexByRgb.end()) {
            ++result.imageForeignColors;
        }
    }
    if (result.unusedDamColors < 2 && result.imageForeignColors > 2) {
        result.error =
            "Not enough free colours in the Dam palette: the world's dam layer "
            "uses " + std::to_string(usedDamColors) + " of 256 entries (" +
            std::to_string(result.unusedDamColors) +
            " free), while the image has " +
            std::to_string(result.imageForeignColors) +
            " colours that are not in the palette. Free some Dam palette "
            "entries or reduce the number of colours in the image.";
        return result;
    }

    // --- 3. Candidate palette entries (k-means start list) ------------------
    std::vector<int> candidates;
    std::vector<unsigned char> isCandidate(256, 0);
    const auto addCandidate = [&candidates, &isCandidate](int index) {
        if (index >= 0 && index < 256 && !isCandidate[index]) {
            isCandidate[index] = 1;
            candidates.push_back(index);
        }
    };
    // (a) palette entries whose colour appears in the image;
    for (const auto& entry : imageHistogram) {
        const auto it = paletteIndexByRgb.find(entry.first);
        if (it != paletteIndexByRgb.end()) {
            addCandidate(it->second);
        }
    }
    // (b) palette entries the world's dam layer already paints with;
    for (int i = 0; i < 256; ++i) {
        if (damEntryInUse[i] != 0) {
            addCandidate(i);
        }
    }
    // (c) pad with free entries so the result can express at least
    //     kMinPaletteColors shades even for a nearly monochrome world.
    for (int i = 0; i < 256 && static_cast<int>(candidates.size()) < kMinPaletteColors; ++i) {
        if (damEntryInUse[i] == 0) {
            addCandidate(i);
        }
    }
    result.candidateColors = static_cast<int>(candidates.size());

    // Initial centres: the candidate colours, deduplicated.
    std::vector<Center> centers;
    std::map<std::uint32_t, int> seenCenters;
    for (int index : candidates) {
        const std::uint32_t rgb = rgbOf(damPaletteRgb + index * 3);
        if (seenCenters.emplace(rgb, 0).second) {
            Center center;
            center.r = static_cast<double>((rgb >> 16) & 0xFF);
            center.g = static_cast<double>((rgb >> 8) & 0xFF);
            center.b = static_cast<double>(rgb & 0xFF);
            centers.push_back(center);
        }
    }

    // --- 4. k-means over the opaque pixels ---------------------------------
    std::vector<Point> pointList;
    pointList.reserve(points.size());
    for (const auto& entry : points) {
        pointList.push_back(entry.second);
    }

    std::vector<int> assignment(pointList.size(), -1);
    std::vector<long long> counts(centers.size(), 0);
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        std::vector<long long> sumR(centers.size(), 0);
        std::vector<long long> sumG(centers.size(), 0);
        std::vector<long long> sumB(centers.size(), 0);
        std::fill(counts.begin(), counts.end(), 0);
        bool changed = false;
        for (std::size_t i = 0; i < pointList.size(); ++i) {
            const Point& point = pointList[i];
            const double r = point.r();
            const double g = point.g();
            const double b = point.b();
            std::size_t best = 0;
            double bestDistance = distance2(centers[0], r, g, b);
            for (std::size_t c = 1; c < centers.size(); ++c) {
                const double distance = distance2(centers[c], r, g, b);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = c;
                }
            }
            if (assignment[i] != static_cast<int>(best)) {
                assignment[i] = static_cast<int>(best);
                changed = true;
            }
            counts[best] += point.count;
            sumR[best] += point.sumR;
            sumG[best] += point.sumG;
            sumB[best] += point.sumB;
        }
        for (std::size_t c = 0; c < centers.size(); ++c) {
            if (counts[c] > 0) {
                centers[c].r = static_cast<double>(sumR[c]) / static_cast<double>(counts[c]);
                centers[c].g = static_cast<double>(sumG[c]) / static_cast<double>(counts[c]);
                centers[c].b = static_cast<double>(sumB[c]) / static_cast<double>(counts[c]);
            }
        }
        if (!changed) {
            break;
        }
    }

    // Drop clusters that captured no pixel — a padded palette colour far from
    // every image colour must not consume a palette entry.
    std::vector<Center> finalCenters;
    std::vector<int> centerOfOld(centers.size(), -1);
    for (std::size_t c = 0; c < centers.size(); ++c) {
        if (counts[c] > 0) {
            centerOfOld[c] = static_cast<int>(finalCenters.size());
            finalCenters.push_back(centers[c]);
        }
    }
    if (finalCenters.empty()) {
        result.error = "The image could not be reduced to a palette.";
        return result;
    }

    // --- 5. Map every cluster to a Dam palette entry ------------------------
    // A colour the palette already contains reuses that entry; a new colour
    // takes the first free entry; if none is left it snaps to the closest
    // existing palette colour (no existing dam colour is ever overwritten).
    std::vector<unsigned char> claimed(256, 0);
    std::vector<int> entryOfCenter(finalCenters.size(), -1);
    for (std::size_t c = 0; c < finalCenters.size(); ++c) {
        const std::uint32_t rgb = quantizeCenter(finalCenters[c]);
        const auto it = paletteIndexByRgb.find(rgb);
        if (it != paletteIndexByRgb.end()) {
            entryOfCenter[c] = it->second;
            claimed[it->second] = 1;
            continue;
        }
        int slot = -1;
        for (int index = 0; index < 256 && slot < 0; ++index) {
            if (damEntryInUse[index] == 0 && claimed[index] == 0) {
                slot = index;
            }
        }
        if (slot >= 0) {
            claimed[slot] = 1;
            entryOfCenter[c] = slot;
            PaletteWrite write;
            write.index = slot;
            write.rgb = rgb;
            result.paletteWrites.push_back(write);
        } else {
            entryOfCenter[c] = nearestPaletteEntry(damPaletteRgb, rgb);
        }
    }
    result.resultColors = static_cast<int>(finalCenters.size());

    // --- 6. Build the palette-index image ----------------------------------
    // Every pixel belongs to a 5-bit bucket, and every bucket has one cluster.
    std::map<int, int> entryOfBucket;
    for (std::size_t i = 0; i < pointList.size(); ++i) {
        const int old = assignment[i];
        if (old < 0 || centerOfOld[static_cast<std::size_t>(old)] < 0) {
            continue;
        }
        entryOfBucket[pointList[i].key] =
            entryOfCenter[static_cast<std::size_t>(centerOfOld[static_cast<std::size_t>(old)])];
    }

    result.image.width = width;
    result.image.height = height;
    result.image.indices.assign(static_cast<std::size_t>(width) * height, -1);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char* pixel = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
            if (pixel[3] != 255) {
                continue; // stays -1: transparent pixels never paint
            }
            const auto it = entryOfBucket.find(bucketKey(pixel[0], pixel[1], pixel[2]));
            if (it != entryOfBucket.end()) {
                result.image.indices[static_cast<std::size_t>(y) * width + x] = it->second;
            }
        }
    }

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------

DecalImage scaleNearest(const DecalImage& source, double factor)
{
    DecalImage scaled;
    if (source.empty() || factor <= 0.0) {
        return scaled;
    }
    scaled.width = std::max(1, static_cast<int>(std::lround(source.width * factor)));
    scaled.height = std::max(1, static_cast<int>(std::lround(source.height * factor)));
    scaled.indices.resize(static_cast<std::size_t>(scaled.width) * scaled.height, -1);
    for (int y = 0; y < scaled.height; ++y) {
        const int sy = std::clamp(
            static_cast<int>(static_cast<double>(y) * source.height / scaled.height),
            0, source.height - 1);
        for (int x = 0; x < scaled.width; ++x) {
            const int sx = std::clamp(
                static_cast<int>(static_cast<double>(x) * source.width / scaled.width),
                0, source.width - 1);
            scaled.indices[static_cast<std::size_t>(y) * scaled.width + x] =
                source.at(sx, sy);
        }
    }
    return scaled;
}

} // namespace MapEditor::ImageDecal

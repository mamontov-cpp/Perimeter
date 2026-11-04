// ============================================================================
// MapEditorCore — ModelBrush implementation: PLY import + raster resample.
// ============================================================================

#include "ModelBrush.h"

#include "WorldImages.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace MapEditor::ModelBrush {

namespace {

// ---------------------------------------------------------------------------
// PLY scalar property types (size in bytes + little-endian decode).
// ---------------------------------------------------------------------------

enum class PlyType
{
    I8, U8, I16, U16, I32, U32, F32, F64, Unknown
};

PlyType plyType(const std::string& name)
{
    if (name == "char" || name == "int8") return PlyType::I8;
    if (name == "uchar" || name == "uint8") return PlyType::U8;
    if (name == "short" || name == "int16") return PlyType::I16;
    if (name == "ushort" || name == "uint16") return PlyType::U16;
    if (name == "int" || name == "int32") return PlyType::I32;
    if (name == "uint" || name == "uint32") return PlyType::U32;
    if (name == "float" || name == "float32") return PlyType::F32;
    if (name == "double" || name == "float64") return PlyType::F64;
    return PlyType::Unknown;
}

int plyTypeSize(PlyType t)
{
    switch (t) {
    case PlyType::I8:
    case PlyType::U8: return 1;
    case PlyType::I16:
    case PlyType::U16: return 2;
    case PlyType::I32:
    case PlyType::U32:
    case PlyType::F32: return 4;
    case PlyType::F64: return 8;
    case PlyType::Unknown: return 0;
    }
    return 0;
}

double decodeLE(const unsigned char* p, PlyType t)
{
    switch (t) {
    case PlyType::I8: return static_cast<double>(*reinterpret_cast<const signed char*>(p));
    case PlyType::U8: return static_cast<double>(*p);
    case PlyType::I16: {
        std::int16_t v = 0;
        std::memcpy(&v, p, 2);
        return v;
    }
    case PlyType::U16: {
        std::uint16_t v = 0;
        std::memcpy(&v, p, 2);
        return v;
    }
    case PlyType::I32: {
        std::int32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    }
    case PlyType::U32: {
        std::uint32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    }
    case PlyType::F32: {
        float v = 0;
        std::memcpy(&v, p, 4);
        return v;
    }
    case PlyType::F64: {
        double v = 0;
        std::memcpy(&v, p, 8);
        return v;
    }
    case PlyType::Unknown: return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Header model.
// ---------------------------------------------------------------------------

struct PlyProperty
{
    bool isList = false;
    PlyType type = PlyType::Unknown;
    PlyType countType = PlyType::Unknown;
    std::string name;
};

struct PlyElement
{
    std::string name;
    std::size_t count = 0;
    std::vector<PlyProperty> props;
};

struct PlyHeader
{
    bool ascii = true;
    std::vector<PlyElement> elements;
    std::vector<std::string> comments;
};

/// Parse the header lines out of the file buffer. Returns the byte offset
/// just past "end_header", or 0 when the header is unusable (`error`).
std::size_t parseHeader(const std::vector<char>& file, PlyHeader& header,
                        std::string& error)
{
    std::size_t pos = 0;
    auto nextLine = [&](std::string& line) -> bool {
        if (pos >= file.size()) {
            return false;
        }
        std::size_t end = pos;
        while (end < file.size() && file[end] != '\n') {
            ++end;
        }
        line.assign(file.data() + pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pos = end < file.size() ? end + 1 : end;
        return true;
    };

    std::string line;
    if (!nextLine(line) || line != "ply") {
        error = "not a PLY file (the first line is not \"ply\").";
        return 0;
    }
    PlyElement* current = nullptr;
    bool gotFormat = false;
    while (nextLine(line)) {
        std::istringstream in(line);
        std::string keyword;
        in >> keyword;
        if (keyword == "comment") {
            header.comments.push_back(line.substr(8));
        } else if (keyword == "format") {
            std::string format;
            in >> format;
            gotFormat = true;
            if (format == "ascii") {
                header.ascii = true;
            } else if (format == "binary_little_endian") {
                header.ascii = false;
            } else {
                error = "unsupported PLY format \"" + format +
                        "\" (only ascii and binary_little_endian are "
                        "supported).";
                return 0;
            }
        } else if (keyword == "element") {
            PlyElement element;
            in >> element.name >> element.count;
            header.elements.push_back(element);
            current = &header.elements.back();
        } else if (keyword == "property" && current) {
            PlyProperty prop;
            std::string typeName;
            in >> typeName;
            if (typeName == "list") {
                std::string countName;
                in >> countName >> typeName;
                prop.isList = true;
                prop.countType = plyType(countName);
            }
            prop.type = plyType(typeName);
            in >> prop.name;
            current->props.push_back(prop);
        } else if (keyword == "end_header") {
            if (!gotFormat) {
                error = "the PLY header has no format line.";
                return 0;
            }
            return pos;
        }
    }
    error = "the PLY header is not terminated by end_header.";
    return 0;
}

/// The texture file named in a header comment, when there is one: a comment
/// containing "texture" (case-insensitive), its last token the file name.
std::string textureFromComments(const std::vector<std::string>& comments)
{
    for (const std::string& comment : comments) {
        std::string lower = comment;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower.find("texture") == std::string::npos) {
            continue;
        }
        std::istringstream in(comment);
        std::string token;
        std::string last;
        while (in >> token) {
            last = token;
        }
        if (!last.empty()) {
            return last;
        }
    }
    return std::string();
}

int colorComponent(double value, PlyType type)
{
    // uchar colours are 0..255, float colours are 0..1.
    if (type == PlyType::F32 || type == PlyType::F64) {
        value *= 255.0;
    }
    return std::clamp(static_cast<int>(std::lround(value)), 0, 255);
}

std::uint32_t sampleTextureBgr(const std::vector<unsigned char>& bgr, int tw,
                               int th, double u, double v)
{
    // Clamp into [0,1] (edge-repeat) and sample nearest; v = 0 is the
    // texture bottom (the OBJ-style convention), the loaded raster is
    // top-left.
    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const int x = std::clamp(static_cast<int>(u * tw), 0, tw - 1);
    const int y = std::clamp(static_cast<int>((1.0 - v) * th), 0, th - 1);
    const std::size_t off =
        (static_cast<std::size_t>(y) * tw + x) * 3;
    const int b = bgr[off + 0];
    const int g = bgr[off + 1];
    const int r = bgr[off + 2];
    return static_cast<std::uint32_t>((r << 16) | (g << 8) | b);
}

} // namespace

ImportResult importPly(const std::filesystem::path& path)
{
    ImportResult result;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "the file cannot be opened.";
        return result;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    file.close();

    PlyHeader header;
    std::string error;
    const std::size_t bodyPos = parseHeader(bytes, header, error);
    if (bodyPos == 0) {
        result.error = error;
        return result;
    }

    // The vertex element is required and must carry x/y/z.
    const PlyElement* vertexElement = nullptr;
    for (const PlyElement& element : header.elements) {
        if (element.name == "vertex") {
            vertexElement = &element;
            break;
        }
    }
    if (!vertexElement) {
        result.error = "the PLY has no vertex element.";
        return result;
    }
    auto propIndex = [&](const char* name) {
        for (std::size_t i = 0; i < vertexElement->props.size(); ++i) {
            if (vertexElement->props[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const int ix = propIndex("x");
    const int iy = propIndex("y");
    const int iz = propIndex("z");
    if (ix < 0 || iy < 0 || iz < 0) {
        result.error =
            "the vertex element has no x/y/z properties.";
        return result;
    }
    const int ir = propIndex("red");
    const int ig = propIndex("green");
    const int ib = propIndex("blue");
    const bool hasVertexColor =
        ir >= 0 && ig >= 0 && ib >= 0 &&
        vertexElement->props[ir].type != PlyType::Unknown &&
        vertexElement->props[ig].type != PlyType::Unknown &&
        vertexElement->props[ib].type != PlyType::Unknown;
    int iu = propIndex("texture_u");
    int iv = propIndex("texture_v");
    if (iu < 0 || iv < 0) {
        iu = propIndex("u");
        iv = propIndex("v");
    }
    if (iu < 0 || iv < 0) {
        iu = propIndex("s");
        iv = propIndex("t");
    }
    const bool hasUv = iu >= 0 && iv >= 0;

    // Optional texture named by a header comment (24-bit TGA next to the
    // .ply), sampled per vertex through the UVs.
    std::vector<unsigned char> texBgr;
    int texW = 0;
    int texH = 0;
    const std::string textureName = textureFromComments(header.comments);
    if (!textureName.empty()) {
        const std::filesystem::path texturePath =
            path.parent_path() / textureName;
        unsigned w = 0;
        unsigned h = 0;
        if (!WorldImages::loadTga24(texturePath, w, h, texBgr)) {
            result.error = "the texture \"" + textureName +
                           "\" named in a header comment cannot be read "
                           "(a 24-bit uncompressed TGA next to the .ply is "
                           "required).";
            return result;
        }
        texW = static_cast<int>(w);
        texH = static_cast<int>(h);
    }
    const bool useTexture = texW > 0 && texH > 0;
    if (useTexture && !hasUv) {
        result.error =
            "the PLY names a texture but the vertices have no UV "
            "properties (texture_u/texture_v, u/v or s/t).";
        return result;
    }
    // No texture and no vertex colours: the model paints WHITE (the brush
    // still quantizes it into the Dam palette).
    const bool defaultWhite = !useTexture && !hasVertexColor;

    // Read every element in order; keep the vertices AND the faces (the
    // solid is rasterized through its faces — see below), skip the rest.
    struct V3
    {
        double x = 0;
        double y = 0;
        double z = 0;
        std::uint32_t rgb = 0;
    };
    std::vector<V3> vertices;
    vertices.reserve(vertexElement->count);
    std::vector<std::vector<int>> faces;
    const auto isIndexProp = [](const std::string& name) {
        return name == "vertex_indices" || name == "vertex_index";
    };

    std::size_t cursor = bodyPos;
    auto readToken = [&](std::string& token) -> bool {
        while (cursor < bytes.size() &&
               std::isspace(static_cast<unsigned char>(bytes[cursor]))) {
            ++cursor;
        }
        if (cursor >= bytes.size()) {
            return false;
        }
        std::size_t end = cursor;
        while (end < bytes.size() &&
               !std::isspace(static_cast<unsigned char>(bytes[end]))) {
            ++end;
        }
        token.assign(bytes.data() + cursor, end - cursor);
        cursor = end;
        return true;
    };
    auto readScalarBytes = [&](PlyType type, double& value) -> bool {
        const int size = plyTypeSize(type);
        if (type == PlyType::Unknown || cursor + size > bytes.size()) {
            return false;
        }
        value = decodeLE(reinterpret_cast<const unsigned char*>(bytes.data() + cursor), type);
        cursor += size;
        return true;
    };

    for (const PlyElement& element : header.elements) {
        const bool isVertex = &element == vertexElement;
        const bool isFace = element.name == "face";
        for (std::size_t n = 0; n < element.count; ++n) {
            V3 v;
            std::vector<int> faceIndices;
            bool bad = false;
            for (std::size_t p = 0; p < element.props.size() && !bad; ++p) {
                const PlyProperty& prop = element.props[p];
                if (header.ascii) {
                    std::string token;
                    if (prop.isList) {
                        // ASCII scalars (incl. the list count) are text.
                        bad = !readToken(token);
                        const long count =
                            bad ? 0 : std::strtol(token.c_str(), nullptr, 10);
                        for (long i = 0; i < count && !bad; ++i) {
                            bad = !readToken(token);
                            if (isFace && isIndexProp(prop.name) && !bad) {
                                faceIndices.push_back(
                                    std::atoi(token.c_str()));
                            }
                        }
                        continue;
                    }
                    bad = !readToken(token);
                    double value = 0;
                    if (!bad) {
                        char* endPtr = nullptr;
                        value = std::strtod(token.c_str(), &endPtr);
                        bad = endPtr == token.c_str();
                    }
                    if (bad) {
                        break;
                    }
                    if (isVertex) {
                        if (static_cast<int>(p) == ix) v.x = value;
                        else if (static_cast<int>(p) == iy) v.y = value;
                        else if (static_cast<int>(p) == iz) v.z = value;
                        else if (static_cast<int>(p) == ir)
                            v.rgb |= static_cast<std::uint32_t>(colorComponent(value, prop.type)) << 16;
                        else if (static_cast<int>(p) == ig)
                            v.rgb |= static_cast<std::uint32_t>(colorComponent(value, prop.type)) << 8;
                        else if (static_cast<int>(p) == ib)
                            v.rgb |= static_cast<std::uint32_t>(colorComponent(value, prop.type));
                    }
                } else {
                    // Binary body.
                    if (prop.isList) {
                        double count = 0;
                        bad = !readScalarBytes(prop.countType, count);
                        for (double i = 0; i < count && !bad; ++i) {
                            double item = 0;
                            bad = !readScalarBytes(prop.type, item);
                            if (isFace && isIndexProp(prop.name) && !bad) {
                                faceIndices.push_back(
                                    static_cast<int>(item));
                            }
                        }
                        continue;
                    }
                    double value = 0;
                    bad = !readScalarBytes(prop.type, value);
                    if (bad) {
                        break;
                    }
                    if (isVertex) {
                        if (static_cast<int>(p) == ix) v.x = value;
                        else if (static_cast<int>(p) == iy) v.y = value;
                        else if (static_cast<int>(p) == iz) v.z = value;
                        else if (static_cast<int>(p) == ir)
                            v.rgb |= static_cast<std::uint32_t>(colorComponent(value, prop.type)) << 16;
                        else if (static_cast<int>(p) == ig)
                            v.rgb |= static_cast<std::uint32_t>(colorComponent(value, prop.type)) << 8;
                        else if (static_cast<int>(p) == ib)
                            v.rgb |= static_cast<std::uint32_t>(colorComponent(value, prop.type));
                    }
                }
            }
            if (bad) {
                result.error = "the file ends in the middle of the \"" +
                               element.name + "\" element.";
                return result;
            }
            if (isVertex) {
                vertices.push_back(v);
            } else if (isFace && faceIndices.size() >= 3) {
                faces.push_back(std::move(faceIndices));
            }
        }
    }
    if (defaultWhite) {
        for (V3& v : vertices) {
            v.rgb = 0xFFFFFF;
        }
    }

    // Second pass for the UV sampling: kept separate so the token loop above
    // stays simple — re-walk the vertex element recording UV per vertex.
    if (useTexture) {
        std::vector<double> us(vertices.size(), 0.0);
        std::vector<double> vs(vertices.size(), 0.0);
        cursor = bodyPos;
        std::size_t vertexIndex = 0;
        bool bad = false;
        for (const PlyElement& element : header.elements) {
            const bool isVertex = &element == vertexElement;
            for (std::size_t n = 0; n < element.count && !bad; ++n) {
                for (std::size_t p = 0; p < element.props.size(); ++p) {
                    const PlyProperty& prop = element.props[p];
                    std::string token;
                    double value = 0;
                    if (header.ascii) {
                        if (prop.isList) {
                            bad = !readToken(token);
                            double count = bad ? 0 : std::strtod(token.c_str(), nullptr);
                            for (double i = 0; i < count && !bad; ++i) {
                                bad = !readToken(token);
                            }
                            continue;
                        }
                        bad = !readToken(token);
                        if (!bad) {
                            value = std::strtod(token.c_str(), nullptr);
                        }
                    } else {
                        if (prop.isList) {
                            double count = 0;
                            bad = !readScalarBytes(prop.countType, count);
                            for (double i = 0; i < count && !bad; ++i) {
                                double ignored = 0;
                                bad = !readScalarBytes(prop.type, ignored);
                            }
                            continue;
                        }
                        bad = !readScalarBytes(prop.type, value);
                    }
                    if (bad) {
                        break;
                    }
                    if (isVertex) {
                        if (static_cast<int>(p) == iu &&
                            vertexIndex < us.size()) {
                            us[vertexIndex] = value;
                        } else if (static_cast<int>(p) == iv &&
                                   vertexIndex < vs.size()) {
                            vs[vertexIndex] = value;
                        }
                    }
                }
                if (isVertex) {
                    ++vertexIndex;
                }
            }
        }
        if (bad || vertexIndex != vertices.size()) {
            result.error = "the file ends in the middle of the vertex data.";
            return result;
        }
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            vertices[i].rgb =
                sampleTextureBgr(texBgr, texW, texH, us[i], vs[i]);
        }
    }

    if (vertices.empty()) {
        result.error = "the model has no vertices.";
        return result;
    }

    // Bounding rectangle and the z range.
    double minX = vertices.front().x;
    double maxX = minX;
    double minY = vertices.front().y;
    double maxY = minY;
    double minZ = vertices.front().z;
    double maxZ = minZ;
    for (const V3& v : vertices) {
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
        minY = std::min(minY, v.y);
        maxY = std::max(maxY, v.y);
        minZ = std::min(minZ, v.z);
        maxZ = std::max(maxZ, v.z);
    }
    const double extentX = maxX - minX;
    const double extentY = maxY - minY;

    // Grid: up to kMaxRasterSide on the long side, aspect preserved. A
    // single-column model still gets one cell on its zero extent.
    int gridW = 1;
    int gridH = 1;
    {
        const double longest = std::max(extentX, extentY);
        const int target = std::clamp(
            static_cast<int>(std::lround(longest)), 1, kMaxRasterSide);
        if (extentX >= extentY) {
            gridW = target;
            gridH = extentY > 0
                ? std::max(1, static_cast<int>(
                       std::lround(target * extentY / extentX)))
                : 1;
        } else {
            gridH = target;
            gridW = extentX > 0
                ? std::max(1, static_cast<int>(
                       std::lround(target * extentX / extentY)))
                : 1;
        }
    }

    ModelRaster model;
    model.width = gridW;
    model.height = gridH;
    model.normHeight.assign(static_cast<std::size_t>(gridW) * gridH, 0.0);
    model.rgb.assign(static_cast<std::size_t>(gridW) * gridH, 0);

    std::vector<double> cellMaxZ(static_cast<std::size_t>(gridW) * gridH,
                                 -std::numeric_limits<double>::max());
    std::vector<char> cellHas(static_cast<std::size_t>(gridW) * gridH, 0);
    const double cellW = extentX > 0 ? extentX / gridW : 1.0;
    const double cellH = extentY > 0 ? extentY / gridH : 1.0;
    long long rSum = 0;
    long long gSum = 0;
    long long bSum = 0;
    for (const V3& v : vertices) {
        rSum += (v.rgb >> 16) & 0xFF;
        gSum += (v.rgb >> 8) & 0xFF;
        bSum += v.rgb & 0xFF;
    }
    const std::size_t n = vertices.size();
    const std::uint32_t averageRgb = static_cast<std::uint32_t>(
        ((rSum / static_cast<long long>(n)) << 16) |
        ((gSum / static_cast<long long>(n)) << 8) |
        (bSum / static_cast<long long>(n)));

    // The model is a SOLID: a grid cell's height is the top of the model
    // above that column. With faces present each cell samples the cell
    // centre against every (fan-triangulated) face and keeps the HIGHEST
    // surface — the interior of a closed mesh is covered by its top faces,
    // not left as holes. Face-less models (point clouds) fall back to the
    // per-vertex maximum.
    const auto offer = [&](int gx, int gy, double z, double r, double g,
                           double b) {
        const std::size_t cell = static_cast<std::size_t>(gy) * gridW + gx;
        if (!cellHas[cell] || z > cellMaxZ[cell]) {
            cellMaxZ[cell] = z;
            cellHas[cell] = 1;
            const auto chan = [](double c) {
                return static_cast<std::uint32_t>(
                    std::clamp(static_cast<int>(std::lround(c)), 0, 255));
            };
            model.rgb[cell] = (chan(r) << 16) | (chan(g) << 8) | chan(b);
        }
    };
    if (!faces.empty()) {
        const double kEps = 1e-9;
        for (const std::vector<int>& face : faces) {
            for (std::size_t t = 1; t + 1 < face.size(); ++t) {
                const int i0 = face[0];
                const int i1 = face[t];
                const int i2 = face[t + 1];
                if (i0 < 0 || i1 < 0 || i2 < 0 ||
                    i0 >= static_cast<int>(vertices.size()) ||
                    i1 >= static_cast<int>(vertices.size()) ||
                    i2 >= static_cast<int>(vertices.size())) {
                    continue; // a broken index — skip the triangle
                }
                const V3& a = vertices[static_cast<std::size_t>(i0)];
                const V3& b = vertices[static_cast<std::size_t>(i1)];
                const V3& c = vertices[static_cast<std::size_t>(i2)];
                // Signed double area; the barycentric numerators below share
                // this denominator (same vertex order).
                const double d = (b.y - c.y) * (a.x - c.x) +
                                 (c.x - b.x) * (a.y - c.y);
                if (std::abs(d) < kEps) {
                    continue; // degenerate (vertical sliver) triangle
                }
                // Cell range under the triangle's XY bbox.
                const double triMinX = std::min({a.x, b.x, c.x});
                const double triMaxX = std::max({a.x, b.x, c.x});
                const double triMinY = std::min({a.y, b.y, c.y});
                const double triMaxY = std::max({a.y, b.y, c.y});
                const int gx0 = std::clamp(
                    static_cast<int>((triMinX - minX) / cellW), 0, gridW - 1);
                const int gx1 = std::clamp(
                    static_cast<int>((triMaxX - minX) / cellW), 0, gridW - 1);
                const int gy0 = std::clamp(
                    static_cast<int>((triMinY - minY) / cellH), 0, gridH - 1);
                const int gy1 = std::clamp(
                    static_cast<int>((triMaxY - minY) / cellH), 0, gridH - 1);
                for (int gy = gy0; gy <= gy1; ++gy) {
                    const double py = minY + (gy + 0.5) * cellH;
                    for (int gx = gx0; gx <= gx1; ++gx) {
                        const double px = minX + (gx + 0.5) * cellW;
                        const double w0 =
                            ((b.y - c.y) * (px - c.x) +
                             (c.x - b.x) * (py - c.y)) / d;
                        const double w1 =
                            ((c.y - a.y) * (px - c.x) +
                             (a.x - c.x) * (py - c.y)) / d;
                        const double w2 = 1.0 - w0 - w1;
                        if (w0 < -kEps || w1 < -kEps || w2 < -kEps) {
                            continue; // outside the triangle
                        }
                        offer(gx, gy, w0 * a.z + w1 * b.z + w2 * c.z,
                              w0 * static_cast<double>((a.rgb >> 16) & 0xFF) +
                                  w1 * static_cast<double>((b.rgb >> 16) & 0xFF) +
                                  w2 * static_cast<double>((c.rgb >> 16) & 0xFF),
                              w0 * static_cast<double>((a.rgb >> 8) & 0xFF) +
                                  w1 * static_cast<double>((b.rgb >> 8) & 0xFF) +
                                  w2 * static_cast<double>((c.rgb >> 8) & 0xFF),
                              w0 * static_cast<double>(a.rgb & 0xFF) +
                                  w1 * static_cast<double>(b.rgb & 0xFF) +
                                  w2 * static_cast<double>(c.rgb & 0xFF));
                    }
                }
            }
        }
    } else {
        for (const V3& v : vertices) {
            const int gx = std::clamp(
                static_cast<int>((v.x - minX) / cellW), 0, gridW - 1);
            const int gy = std::clamp(
                static_cast<int>((v.y - minY) / cellH), 0, gridH - 1);
            offer(gx, gy, v.z, (v.rgb >> 16) & 0xFF, (v.rgb >> 8) & 0xFF,
                  v.rgb & 0xFF);
        }
    }

    const double zRange = maxZ - minZ;
    for (std::size_t cell = 0; cell < cellHas.size(); ++cell) {
        if (cellHas[cell]) {
            model.normHeight[cell] =
                zRange > 0 ? (cellMaxZ[cell] - minZ) / zRange : 1.0;
        } else {
            // A column with no surface above it (a real hole / outside the
            // solid): the bottom of the model, the average colour, so the
            // whole rectangle still paints.
            model.normHeight[cell] = 0.0;
            model.rgb[cell] = averageRgb;
        }
    }

    result.ok = true;
    result.model = std::move(model);
    return result;
}

ModelRaster resample(const ModelRaster& source, int newWidth, int newHeight)
{
    ModelRaster out;
    if (source.empty() || newWidth <= 0 || newHeight <= 0) {
        return out;
    }
    out.width = newWidth;
    out.height = newHeight;
    out.normHeight.resize(static_cast<std::size_t>(newWidth) * newHeight);
    out.rgb.resize(static_cast<std::size_t>(newWidth) * newHeight);
    for (int y = 0; y < newHeight; ++y) {
        const int sy = std::min(
            source.height - 1,
            static_cast<int>(static_cast<long long>(y) * source.height /
                             newHeight));
        for (int x = 0; x < newWidth; ++x) {
            const int sx = std::min(
                source.width - 1,
                static_cast<int>(static_cast<long long>(x) * source.width /
                                 newWidth));
            const std::size_t src =
                static_cast<std::size_t>(sy) * source.width + sx;
            const std::size_t dst =
                static_cast<std::size_t>(y) * newWidth + x;
            out.normHeight[dst] = source.normHeight[src];
            out.rgb[dst] = source.rgb[src];
        }
    }
    return out;
}

} // namespace MapEditor::ModelBrush

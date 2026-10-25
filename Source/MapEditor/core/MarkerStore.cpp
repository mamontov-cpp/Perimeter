// ============================================================================
// MapEditorCore — MarkerStore implementation (T7.1).
//
// The JSON here is deliberately hand-rolled: the sidecar schema is small and
// fully owned by the editor, and pulling a JSON library into MapEditorCore
// (Qt or third-party) for one file is not worth it. The parser accepts the
// JSON the store itself writes plus the usual human edits — objects, arrays,
// strings with escapes, numbers, true/false/null, comments are NOT allowed.
// ============================================================================

#include "MarkerStore.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace MapEditor {

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON value model
// ---------------------------------------------------------------------------

struct JsonValue
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    Kind kind = Kind::Null;
    bool boolValue = false;
    double numberValue = 0;
    std::string stringValue;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> members;

    const JsonValue* find(const std::string& key) const
    {
        if (kind != Kind::Object) {
            return nullptr;
        }
        const auto it = std::find_if(
            members.begin(), members.end(),
            [&](const auto& member) { return member.first == key; });
        return it == members.end() ? nullptr : &it->second;
    }
};

// ---------------------------------------------------------------------------
// Parser (recursive descent)
// ---------------------------------------------------------------------------

class JsonParser
{
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool parse(JsonValue& out)
    {
        pos_ = 0;
        skipWhitespace();
        if (!parseValue(out)) {
            return false;
        }
        skipWhitespace();
        return pos_ >= text_.size();
    }

private:
    void skipWhitespace()
    {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool parseValue(JsonValue& out)
    {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            return false;
        }
        const char c = text_[pos_];
        if (c == '{') {
            return parseObject(out);
        }
        if (c == '[') {
            return parseArray(out);
        }
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return parseString(out.stringValue);
        }
        if (c == 't' || c == 'f') {
            const bool isTrue = c == 't';
            const std::string literal = isTrue ? "true" : "false";
            if (text_.compare(pos_, literal.size(), literal) != 0) {
                return false;
            }
            pos_ += literal.size();
            out.kind = JsonValue::Kind::Bool;
            out.boolValue = isTrue;
            return true;
        }
        if (c == 'n') {
            if (text_.compare(pos_, 4, "null") != 0) {
                return false;
            }
            pos_ += 4;
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        return parseNumber(out);
    }

    bool parseObject(JsonValue& out)
    {
        out.kind = JsonValue::Kind::Object;
        ++pos_; // '{'
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWhitespace();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"' ||
                !parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return false;
            }
            ++pos_;
            JsonValue value;
            if (!parseValue(value)) {
                return false;
            }
            out.members.emplace_back(std::move(key), std::move(value));
            skipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parseArray(JsonValue& out)
    {
        out.kind = JsonValue::Kind::Array;
        ++pos_; // '['
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parseValue(value)) {
                return false;
            }
            out.array.push_back(std::move(value));
            skipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parseString(std::string& out)
    {
        ++pos_; // '"'
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (pos_ + 4 > text_.size()) {
                    return false;
                }
                unsigned code = 0;
                const auto [ptr, ec] = std::from_chars(
                    text_.data() + pos_, text_.data() + pos_ + 4, code, 16);
                if (ec != std::errc{} || ptr != text_.data() + pos_ + 4) {
                    return false;
                }
                pos_ += 4;
                // UTF-8 encode (BMP only — enough for the sidecar labels).
                if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default:
                return false;
            }
        }
        return false; // unterminated
    }

    bool parseNumber(JsonValue& out)
    {
        const char* begin = text_.data() + pos_;
        const char* end = text_.data() + text_.size();
        double value = 0;
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc{} || ptr == begin) {
            return false;
        }
        pos_ += static_cast<std::size_t>(ptr - begin);
        out.kind = JsonValue::Kind::Number;
        out.numberValue = value;
        return true;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void escapeInto(const std::string& text, std::string& out)
{
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(c);
            }
        }
    }
}

std::string numberLong(long long value)
{
    return std::to_string(value);
}

} // namespace

// ---------------------------------------------------------------------------
// MarkerStore
// ---------------------------------------------------------------------------

std::filesystem::path
MarkerStore::sidecarPathFor(const std::filesystem::path& worldFolder)
{
    return worldFolder.parent_path() /
           (worldFolder.filename().u8string() + ".medit.json");
}

bool MarkerStore::isValidType(const std::string& type)
{
    return type == "unit" || type == "building" || type == "zone" ||
           type == "point";
}

std::uint64_t MarkerStore::add(const MapMarkerData& marker)
{
    MapMarkerData copy = marker;
    copy.id = nextId_++;
    markers_.push_back(std::move(copy));
    return markers_.back().id;
}

bool MarkerStore::update(const MapMarkerData& marker)
{
    const auto it = std::find_if(markers_.begin(), markers_.end(),
                                 [&](const MapMarkerData& m) {
                                     return m.id == marker.id;
                                 });
    if (it == markers_.end()) {
        return false;
    }
    *it = marker;
    return true;
}

bool MarkerStore::remove(std::uint64_t id)
{
    const auto it = std::find_if(markers_.begin(), markers_.end(),
                                 [&](const MapMarkerData& m) {
                                     return m.id == id;
                                 });
    if (it == markers_.end()) {
        return false;
    }
    markers_.erase(it);
    return true;
}

const MapMarkerData* MarkerStore::find(std::uint64_t id) const
{
    const auto it = std::find_if(markers_.begin(), markers_.end(),
                                 [&](const MapMarkerData& m) {
                                     return m.id == id;
                                 });
    return it == markers_.end() ? nullptr : &*it;
}

bool MarkerStore::load(const std::filesystem::path& path)
{
    markers_.clear();
    nextId_ = 1;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false; // no sidecar yet — not an error for the caller
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "MarkerStore: cannot read '%s'.\n",
                path.u8string().c_str());
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parseJson(buffer.str(), std::string());
}

bool MarkerStore::save(const std::filesystem::path& path,
                       const std::string& worldName) const
{
    const std::string text = toJson(worldName);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "MarkerStore: cannot write '%s'.\n",
                path.u8string().c_str());
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::string MarkerStore::toJson(const std::string& worldName) const
{
    std::string out;
    out += "{\n  \"version\": 1,\n  \"world\": \"";
    escapeInto(worldName, out);
    out += "\",\n  \"nextId\": ";
    out += numberLong(static_cast<long long>(nextId_));
    out += ",\n  \"markers\": [";
    for (const MapMarkerData& m : markers_) {
        out += "\n    {\"id\": ";
        out += numberLong(static_cast<long long>(m.id));
        out += ", \"type\": \"";
        escapeInto(m.type, out);
        out += "\", \"label\": \"";
        escapeInto(m.label, out);
        out += "\", \"note\": \"";
        escapeInto(m.note, out);
        char color[10];
        std::snprintf(color, sizeof(color), "#%06x", m.colorRgb & 0xFFFFFF);
        out += "\", \"color\": \"";
        out += color;
        out += "\", \"x\": ";
        out += numberLong(m.x);
        out += ", \"y\": ";
        out += numberLong(m.y);
        out += ", \"radius\": ";
        out += numberLong(m.radius);
        out += "}";
        if (&m != &markers_.back()) {
            out += ",";
        }
    }
    out += markers_.empty() ? "]\n" : "\n  ]\n";
    out += "}\n";
    return out;
}

bool MarkerStore::parseJson(const std::string& text, const std::string& worldName)
{
    markers_.clear();
    nextId_ = 1;

    JsonValue root;
    if (!JsonParser(text).parse(root) || root.kind != JsonValue::Kind::Object) {
        return false;
    }

    // The world name is advisory — mismatched files are still accepted (the
    // caller decides whether to warn); only the schema matters here.
    (void)worldName;

    if (const JsonValue* nextId = root.find("nextId");
        nextId && nextId->kind == JsonValue::Kind::Number &&
        nextId->numberValue >= 1) {
        nextId_ = static_cast<std::uint64_t>(nextId->numberValue);
    }

    const JsonValue* array = root.find("markers");
    if (!array || array->kind != JsonValue::Kind::Array) {
        return true; // empty store
    }
    for (const JsonValue& entry : array->array) {
        if (entry.kind != JsonValue::Kind::Object) {
            continue;
        }
        MapMarkerData marker;
        const JsonValue* id = entry.find("id");
        if (!id || id->kind != JsonValue::Kind::Number || id->numberValue < 1) {
            continue;
        }
        marker.id = static_cast<std::uint64_t>(id->numberValue);

        auto readString = [&](const char* key, std::string& target) {
            if (const JsonValue* value = entry.find(key);
                value && value->kind == JsonValue::Kind::String) {
                target = value->stringValue;
            }
        };
        readString("type", marker.type);
        readString("label", marker.label);
        readString("note", marker.note);
        if (!isValidType(marker.type)) {
            marker.type = "point";
        }
        if (const JsonValue* color = entry.find("color");
            color && color->kind == JsonValue::Kind::String &&
            color->stringValue.size() == 7 && color->stringValue[0] == '#') {
            unsigned rgb = 0;
            const auto [ptr, ec] = std::from_chars(
                color->stringValue.data() + 1, color->stringValue.data() + 7,
                rgb, 16);
            if (ec == std::errc{} && ptr == color->stringValue.data() + 7) {
                marker.colorRgb = rgb & 0xFFFFFF;
            }
        }
        auto readInt = [&](const char* key, int& target) {
            if (const JsonValue* value = entry.find(key);
                value && value->kind == JsonValue::Kind::Number) {
                target = static_cast<int>(value->numberValue);
            }
        };
        readInt("x", marker.x);
        readInt("y", marker.y);
        readInt("radius", marker.radius);

        markers_.push_back(std::move(marker));
        if (marker.id >= nextId_) {
            nextId_ = marker.id + 1;
        }
    }
    return true;
}

} // namespace MapEditor

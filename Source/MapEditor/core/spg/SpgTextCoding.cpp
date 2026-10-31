// ============================================================================
// MapEditorCore — .spg user-text encoding implementation (see the header).
// ============================================================================

#include "SpgTextCoding.h"

#include <cstdint>

namespace MapEditor {

namespace {

/// Windows-1251 high half (0x80..0xFF) -> Unicode. 0x98 is unassigned in
/// the codepage and maps to U+FFFD (it cannot round-trip to a byte).
const char32_t kCp1251High[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, //
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, //
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, //
    0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, //
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, //
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, //
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, //
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, //
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, //
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, //
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, //
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, //
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, //
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F, //
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, //
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F, //
};

/// Decode one UTF-8 sequence at `pos`; returns the code point and advances
/// `pos`, or false on malformed input (caller falls back to CP1251 / '?').
bool decodeUtf8(const std::string& text, std::size_t& pos, char32_t& cp)
{
    const auto byte = [&](std::size_t i) -> std::uint8_t {
        return static_cast<std::uint8_t>(text[i]);
    };
    const std::size_t n = text.size();
    const std::uint8_t c0 = byte(pos);
    if (c0 < 0x80) {
        cp = c0;
        ++pos;
        return true;
    }
    int continuation = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (c0 >= 0xC2 && c0 <= 0xDF) {
        continuation = 1;
        value = c0 & 0x1F;
        minimum = 0x80;
    } else if (c0 >= 0xE0 && c0 <= 0xEF) {
        continuation = 2;
        value = c0 & 0x0F;
        minimum = 0x800;
    } else if (c0 >= 0xF0 && c0 <= 0xF4) {
        continuation = 3;
        value = c0 & 0x07;
        minimum = 0x10000;
    } else {
        return false; // 0x80..0xBF, 0xC0/0xC1, 0xF5..0xFF
    }
    if (pos + static_cast<std::size_t>(continuation) >= n) {
        return false;
    }
    for (int i = 1; i <= continuation; ++i) {
        const std::uint8_t ci = byte(pos + static_cast<std::size_t>(i));
        if ((ci & 0xC0) != 0x80) {
            return false;
        }
        value = (value << 6) | (ci & 0x3F);
    }
    // No overlongs, no surrogates, capped at U+10FFFF.
    if (value < minimum || (value >= 0xD800 && value <= 0xDFFF) ||
        value > 0x10FFFF) {
        return false;
    }
    pos += static_cast<std::size_t>(continuation) + 1;
    cp = value;
    return true;
}

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool isValidUtf8(const std::string& text)
{
    std::size_t pos = 0;
    while (pos < text.size()) {
        char32_t cp = 0;
        if (!decodeUtf8(text, pos, cp)) {
            return false;
        }
    }
    return true;
}

} // namespace

SpgTextEncoding detectSpgTextEncoding(const std::vector<std::string>& samples)
{
    bool sawHighBytes = false;
    for (const std::string& sample : samples) {
        for (const char c : sample) {
            if (static_cast<std::uint8_t>(c) >= 0x80) {
                sawHighBytes = true;
                break;
            }
        }
        if (sawHighBytes) {
            break;
        }
    }
    if (!sawHighBytes) {
        return SpgTextEncoding::Utf8; // ASCII: identical in both encodings
    }
    for (const std::string& sample : samples) {
        if (!isValidUtf8(sample)) {
            return SpgTextEncoding::Windows1251;
        }
    }
    return SpgTextEncoding::Utf8;
}

std::string spgTextToUtf8(const std::string& bytes, SpgTextEncoding encoding)
{
    if (encoding == SpgTextEncoding::Utf8) {
        return bytes;
    }
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto value = static_cast<std::uint8_t>(c);
        if (value < 0x80) {
            out += c;
        } else {
            appendUtf8(out, kCp1251High[value - 0x80]);
        }
    }
    return out;
}

std::string utf8ToSpgText(const std::string& utf8, SpgTextEncoding encoding)
{
    if (encoding == SpgTextEncoding::Utf8) {
        return utf8;
    }
    std::string out;
    out.reserve(utf8.size());
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        char32_t cp = 0;
        if (!decodeUtf8(utf8, pos, cp)) {
            out += '?'; // defensive: the UI hands us valid UTF-8
            ++pos;
            continue;
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
            continue;
        }
        char replacement = '?';
        for (int i = 0; i < 128; ++i) {
            if (kCp1251High[i] == cp) {
                replacement = static_cast<char>(0x80 + i);
                break;
            }
        }
        out += replacement;
    }
    return out;
}

} // namespace MapEditor

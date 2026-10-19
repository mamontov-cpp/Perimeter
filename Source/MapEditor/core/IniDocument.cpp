// ============================================================================
// MapEditorCore — IniDocument implementation.
// ============================================================================

#include "IniDocument.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace MapEditor {

namespace {

// Fallback line ending for new lines when the document gives no hint.
// Shipped world.ini files use CRLF, and SimpleIni (which rewrites them when
// Terra saves a world) writes "key = value" lines in the same style.
constexpr const char* kDefaultEol = "\r\n";

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trimmed(std::string_view text)
{
    size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// ASCII case-insensitive comparison: CSimpleIniA (used by the game) treats
/// section and key names case-insensitively.
bool asciiIEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

/// Recognise a "[Section]" line and return its trimmed name.
bool sectionNameOf(std::string_view content, std::string_view& name)
{
    const std::string_view line = trimmed(content);
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') {
        return false;
    }
    name = trimmed(line.substr(1, line.size() - 2));
    return true;
}

/// Split "key = value" into trimmed key and value parts.
bool splitKeyValue(std::string_view content, std::string_view& key,
                   std::string_view& value)
{
    const size_t eq = content.find('=');
    if (eq == std::string_view::npos) {
        return false;
    }
    key = trimmed(content.substr(0, eq));
    value = trimmed(content.substr(eq + 1));
    return !key.empty();
}

bool isBlank(std::string_view content)
{
    return trimmed(content).empty();
}

/// Integer parsing with the rules the game applies to INI values
/// (atoi()/strtol(base 0)): optional sign, decimal or "0x" hex, trailing
/// garbage ignored, overflow rejected.
std::optional<int> parseIntValue(std::string_view text)
{
    std::string_view digits = trimmed(text);
    if (digits.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    if (digits.front() == '+' || digits.front() == '-') {
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }

    int base = 10;
    if (digits.size() > 2 && digits[0] == '0' &&
        (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits.remove_prefix(2);
    }

    int value = 0;
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (result.ec != std::errc{} || result.ptr == digits.data()) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

} // namespace

// ---------------------------------------------------------------------------
// Loading / saving
// ---------------------------------------------------------------------------

bool IniDocument::load(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    parse(text);
    return true;
}

bool IniDocument::save(const std::filesystem::path& path) const
{
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    const std::string text = toString();
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

void IniDocument::parse(std::string_view text)
{
    lines_.clear();
    defaultEol_ = kDefaultEol;

    size_t pos = 0;
    while (pos < text.size()) {
        Line line;
        const size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            line.content.assign(text.substr(pos));
            pos = text.size();
        } else {
            std::string_view raw = text.substr(pos, nl - pos);
            if (!raw.empty() && raw.back() == '\r') {
                raw.remove_suffix(1);
                line.eol = "\r\n";
            } else {
                line.eol = "\n";
            }
            line.content.assign(raw);
            pos = nl + 1;
        }
        if (lines_.empty() && !line.eol.empty()) {
            defaultEol_ = line.eol;
        }
        lines_.push_back(std::move(line));
    }
}

std::string IniDocument::toString() const
{
    size_t size = 0;
    for (const Line& line : lines_) {
        size += line.content.size() + line.eol.size();
    }

    std::string text;
    text.reserve(size);
    for (const Line& line : lines_) {
        text += line.content;
        text += line.eol;
    }
    return text;
}

bool IniDocument::empty() const
{
    return lines_.empty();
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

int IniDocument::findSection(std::string_view section) const
{
    for (size_t i = 0; i < lines_.size(); ++i) {
        std::string_view name;
        if (sectionNameOf(lines_[i].content, name) && asciiIEquals(name, section)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int IniDocument::findKey(int sectionLine, std::string_view key) const
{
    const size_t start = sectionLine < 0 ? 0 : static_cast<size_t>(sectionLine) + 1;
    for (size_t i = start; i < lines_.size(); ++i) {
        std::string_view name;
        if (sectionNameOf(lines_[i].content, name)) {
            break; // reached the next section (or the first one for global keys)
        }
        std::string_view candidate;
        std::string_view value;
        if (splitKeyValue(lines_[i].content, candidate, value) &&
            asciiIEquals(candidate, key)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool IniDocument::hasSection(std::string_view section) const
{
    return section.empty() || findSection(section) >= 0;
}

bool IniDocument::hasKey(std::string_view section, std::string_view key) const
{
    int sectionLine = -1;
    if (!section.empty()) {
        sectionLine = findSection(section);
        if (sectionLine < 0) {
            return false;
        }
    }
    return findKey(sectionLine, key) >= 0;
}

std::optional<std::string> IniDocument::getValue(std::string_view section,
                                                 std::string_view key) const
{
    int sectionLine = -1;
    if (!section.empty()) {
        sectionLine = findSection(section);
        if (sectionLine < 0) {
            return std::nullopt;
        }
    }

    const int keyLine = findKey(sectionLine, key);
    if (keyLine < 0) {
        return std::nullopt;
    }

    std::string_view foundKey;
    std::string_view value;
    splitKeyValue(lines_[static_cast<size_t>(keyLine)].content, foundKey, value);
    return std::string(value);
}

std::optional<int> IniDocument::getIntValue(std::string_view section,
                                            std::string_view key) const
{
    const auto value = getValue(section, key);
    if (!value) {
        return std::nullopt;
    }
    return parseIntValue(*value);
}

std::vector<std::string> IniDocument::sectionNames() const
{
    std::vector<std::string> names;
    for (const Line& line : lines_) {
        std::string_view name;
        if (sectionNameOf(line.content, name)) {
            names.emplace_back(name);
        }
    }
    return names;
}

std::vector<std::string> IniDocument::keyNames(std::string_view section) const
{
    int sectionLine = -1;
    if (!section.empty()) {
        sectionLine = findSection(section);
        if (sectionLine < 0) {
            return {};
        }
    }

    std::vector<std::string> keys;
    const size_t start = sectionLine < 0 ? 0 : static_cast<size_t>(sectionLine) + 1;
    for (size_t i = start; i < lines_.size(); ++i) {
        std::string_view name;
        if (sectionNameOf(lines_[i].content, name)) {
            break;
        }
        std::string_view key;
        std::string_view value;
        if (splitKeyValue(lines_[i].content, key, value)) {
            keys.emplace_back(key);
        }
    }
    return keys;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

void IniDocument::setValue(std::string_view section, std::string_view key,
                           std::string_view value)
{
    const std::string_view cleanKey = trimmed(key);
    if (cleanKey.empty()) {
        return;
    }

    // The format is line based: a value cannot contain line breaks.
    std::string cleanValue;
    cleanValue.reserve(value.size());
    for (char c : value) {
        if (c != '\n' && c != '\r') {
            cleanValue.push_back(c);
        }
    }

    int sectionLine = -1;
    if (!section.empty()) {
        sectionLine = findSection(section);
        if (sectionLine < 0) {
            appendSection(section, cleanKey, cleanValue);
            return;
        }
    }

    const int keyLine = findKey(sectionLine, cleanKey);
    if (keyLine >= 0) {
        Line& line = lines_[static_cast<size_t>(keyLine)];
        const size_t eq = line.content.find('=');
        if (eq != std::string::npos) {
            // Keep the "key =" part and any separator/trailing whitespace
            // exactly as written, replace only the value itself.
            const std::string text = line.content;
            size_t valueBegin = eq + 1;
            while (valueBegin < text.size() &&
                   (text[valueBegin] == ' ' || text[valueBegin] == '\t')) {
                ++valueBegin;
            }
            size_t valueEnd = text.size();
            while (valueEnd > valueBegin &&
                   (text[valueEnd - 1] == ' ' || text[valueEnd - 1] == '\t')) {
                --valueEnd;
            }
            const std::string trailing =
                valueEnd > valueBegin ? text.substr(valueEnd) : std::string();

            line.content.assign(text, 0, valueBegin);
            line.content += cleanValue;
            line.content += trailing;
            return;
        }
    }

    insertKey(sectionLine, cleanKey, cleanValue);
}

void IniDocument::setIntValue(std::string_view section, std::string_view key,
                              int value)
{
    setValue(section, key, std::to_string(value));
}

void IniDocument::insertKey(int sectionLine, std::string_view key,
                            std::string_view value)
{
    const size_t start = sectionLine < 0 ? 0 : static_cast<size_t>(sectionLine) + 1;

    // By default sit right below the section header; if the section already
    // has keys, sit below the last of them (trailing comments stay last).
    size_t insertAt = start;
    for (size_t i = start; i < lines_.size(); ++i) {
        std::string_view name;
        if (sectionNameOf(lines_[i].content, name)) {
            break;
        }
        std::string_view existingKey;
        std::string_view existingValue;
        if (splitKeyValue(lines_[i].content, existingKey, existingValue)) {
            insertAt = i + 1;
        }
    }

    Line line;
    line.content = std::string(key);
    line.content += '=';
    line.content += value;

    if (insertAt > 0) {
        Line& previous = lines_[insertAt - 1];
        if (previous.eol.empty()) {
            // Inserting after a final line that had no terminator: keep the
            // document's "no trailing newline" property.
            previous.eol = defaultEol_;
        } else {
            line.eol = previous.eol;
        }
    } else {
        line.eol = defaultEol_;
    }

    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(insertAt),
                  std::move(line));
}

void IniDocument::appendSection(std::string_view section, std::string_view key,
                                std::string_view value)
{
    if (!lines_.empty()) {
        Line& last = lines_.back();
        if (last.eol.empty()) {
            last.eol = defaultEol_;
        }
        if (!isBlank(last.content)) {
            Line separator;
            separator.eol = last.eol;
            lines_.push_back(std::move(separator));
        }
    }

    Line header;
    header.content = '[';
    header.content += section;
    header.content += ']';
    header.eol = defaultEol_;
    lines_.push_back(std::move(header));

    Line line;
    line.content = std::string(key);
    line.content += '=';
    line.content += value;
    line.eol = defaultEol_;
    lines_.push_back(std::move(line));
}

} // namespace MapEditor

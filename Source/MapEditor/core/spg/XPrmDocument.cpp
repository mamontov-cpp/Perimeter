// ============================================================================
// MapEditorCore — XPrmDocument implementation (T8.1).
//
// Layout model: gaps (original whitespace between entries) are captured
// verbatim, and every entry's raw slice keeps its own terminator (';' for
// named entries, ',' between array elements, nothing before '}'). Render
// therefore reproduces the source byte-for-byte until an entry is edited;
// an edited entry is regenerated as `name = value;` / `value,` with the
// terminator it originally had.
// ============================================================================

#include "XPrmDocument.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace MapEditor {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isNameStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/// Whitespace and // comments (the game writer produces none; human edits
/// might — gaps keep them verbatim).
void skipGap(const std::string& text, std::size_t& pos)
{
    while (pos < text.size()) {
        const char c = text[pos];
        if (isSpace(c)) {
            ++pos;
            continue;
        }
        if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            while (pos < text.size() && text[pos] != '\n') {
                ++pos;
            }
            continue;
        }
        break;
    }
}

/// Read a quoted string starting at `pos` (text[pos] == '"'), keeping the
/// quotes; escapes stay raw. Returns false on unterminated input.
bool readQuoted(const std::string& text, std::size_t& pos, std::string& out)
{
    const std::size_t begin = pos;
    ++pos; // opening quote
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '\\') {
            if (pos >= text.size()) {
                return false;
            }
            ++pos; // the escaped character
            continue;
        }
        if (c == '"') {
            out = text.substr(begin, pos - begin);
            return true;
        }
    }
    return false;
}

/// Read an unquoted scalar token (number/identifier) up to the first
/// separator or whitespace. Returns false when empty.
bool readToken(const std::string& text, std::size_t& pos, std::string& out)
{
    const std::size_t begin = pos;
    while (pos < text.size() && text[pos] != ';' && text[pos] != ',' &&
           text[pos] != '}' && !isSpace(text[pos])) {
        ++pos;
    }
    if (pos == begin) {
        return false;
    }
    out = text.substr(begin, pos - begin);
    return true;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser
{
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool parse(std::vector<XPrmDocument::Block>& blocks,
               std::string& trailing, std::size_t& errorPos)
    {
        pos_ = 0;
        while (true) {
            const std::size_t gapBegin = pos_;
            skipGap(text_, pos_);
            if (pos_ >= text_.size()) {
                trailing = text_.substr(gapBegin, pos_ - gapBegin);
                return true;
            }
            XPrmDocument::Block block;
            if (!readToken(text_, pos_, block.name)) {
                errorPos = pos_;
                return false;
            }
            skipGap(text_, pos_);
            if (pos_ >= text_.size() || text_[pos_] != '=') {
                errorPos = pos_;
                return false;
            }
            ++pos_;
            block.object = std::make_shared<XPrmObject>();
            if (!parseObject(*block.object, errorPos)) {
                return false;
            }
            // Optional ';' after the top-level object.
            char terminator = 0;
            takeTerminator(terminator);
            block.terminator = terminator == 0 ? ';' : terminator;
            block.raw = text_.substr(gapBegin, pos_ - gapBegin);
            blocks.push_back(std::move(block));
        }
    }

private:
    /// After a value: optionally consume whitespace plus one ';' or ','.
    void takeTerminator(char& terminator)
    {
        const std::size_t save = pos_;
        skipGap(text_, pos_);
        if (pos_ < text_.size() &&
            (text_[pos_] == ';' || text_[pos_] == ',')) {
            terminator = text_[pos_];
            ++pos_;
        } else {
            terminator = 0;
            pos_ = save;
        }
    }

    /// Parse '{' entries '}'. Leaves pos_ after the closing '}'.
    bool parseObject(XPrmObject& object, std::size_t& errorPos)
    {
        skipGap(text_, pos_);
        if (pos_ >= text_.size() || text_[pos_] != '{') {
            errorPos = pos_;
            return false;
        }
        ++pos_;

        while (true) {
            const std::size_t gapBegin = pos_;
            skipGap(text_, pos_);
            if (pos_ >= text_.size()) {
                errorPos = pos_;
                return false; // unterminated
            }
            const std::string gap = text_.substr(gapBegin, pos_ - gapBegin);
            if (text_[pos_] == '}') {
                ++pos_;
                object.gaps.push_back(gap);
                return true;
            }
            object.gaps.push_back(gap);

            XPrmObject::Field field;
            const std::size_t begin = pos_;

            // Named entry? identifier '=' — otherwise an anonymous element.
            const std::size_t lookahead = pos_;
            std::string maybeName;
            if (readToken(text_, pos_, maybeName) && !maybeName.empty() &&
                isNameStart(maybeName[0])) {
                skipGap(text_, pos_);
                if (pos_ < text_.size() && text_[pos_] == '=') {
                    field.name = maybeName;
                    ++pos_;
                    skipGap(text_, pos_); // whitespace before the value
                } else {
                    pos_ = lookahead; // anonymous element
                }
            } else {
                pos_ = lookahead;
            }

            if (pos_ >= text_.size()) {
                errorPos = pos_;
                return false;
            }

            if (text_[pos_] == '"') {
                std::string quoted;
                if (!readQuoted(text_, pos_, quoted)) {
                    errorPos = pos_;
                    return false;
                }
                const std::size_t afterQuoted = pos_;
                skipGap(text_, pos_);
                if (pos_ < text_.size() && text_[pos_] == '{') {
                    // Named struct: key = "struct X" { ... }
                    field.typeTag = quoted.substr(1, quoted.size() - 2);
                    field.hasObject = true;
                    field.object = std::make_shared<XPrmObject>();
                    if (!parseObject(*field.object, errorPos)) {
                        return false;
                    }
                } else {
                    pos_ = afterQuoted;
                    field.scalar = quoted;
                }
            } else if (text_[pos_] == '{') {
                field.hasObject = true;
                field.object = std::make_shared<XPrmObject>();
                if (!parseObject(*field.object, errorPos)) {
                    return false;
                }
            } else {
                if (!readToken(text_, pos_, field.scalar)) {
                    errorPos = pos_;
                    return false;
                }
            }

            takeTerminator(field.terminator);
            field.raw = text_.substr(begin, pos_ - begin);
            object.fields.push_back(std::move(field));
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// XPrmObject
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<XPrmObject> deepCopyObject(const std::shared_ptr<XPrmObject>& src)
{
    if (!src) {
        return nullptr;
    }
    auto dst = std::make_shared<XPrmObject>();
    dst->fields = src->fields;
    dst->gaps = src->gaps;
    for (XPrmObject::Field& field : dst->fields) {
        if (field.hasObject) {
            field.object = deepCopyObject(field.object);
        }
    }
    return dst;
}

} // namespace

XPrmObject::Field XPrmObject::Field::deepCopy() const
{
    Field copy = *this;
    if (hasObject) {
        copy.object = deepCopyObject(object);
    }
    return copy;
}

XPrmObject::Field* XPrmObject::field(const std::string& name)
{
    for (Field& f : fields) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

const XPrmObject::Field* XPrmObject::field(const std::string& name) const
{
    for (const Field& f : fields) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

XPrmObject* XPrmObject::object(const std::string& name)
{
    Field* f = field(name);
    return f && f->hasObject ? f->object.get() : nullptr;
}

const XPrmObject* XPrmObject::object(const std::string& name) const
{
    const Field* f = field(name);
    return f && f->hasObject ? f->object.get() : nullptr;
}

std::string XPrmObject::scalar(const std::string& name) const
{
    const Field* f = field(name);
    return f ? f->scalar : std::string();
}

XPrmObject::Field& XPrmObject::setScalar(const std::string& name,
                                         const std::string& rawValue)
{
    Field* f = field(name);
    if (!f) {
        Field added;
        added.name = name;
        added.terminator = ';';
        fields.push_back(std::move(added));
        gaps.emplace_back(" ");
        f = &fields.back();
    }
    f->hasObject = false;
    f->object.reset();
    f->typeTag.clear();
    f->scalar = rawValue;
    f->edited = true;
    return *f;
}

XPrmObject::Field& XPrmObject::addField(Field field)
{
    if (field.terminator == 0) {
        field.terminator = field.name.empty() ? ',' : ';';
    }
    fields.push_back(std::move(field));
    gaps.emplace_back(" ");
    return fields.back();
}

void XPrmObject::removeField(const std::string& name)
{
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == name) {
            fields.erase(fields.begin() + static_cast<long>(i));
            gaps.erase(gaps.begin() + static_cast<long>(i));
            return;
        }
    }
}

void XPrmObject::render(std::string& out) const
{
    out += '{';
    for (std::size_t i = 0; i < fields.size(); ++i) {
        out += gaps[i];
        const Field& f = fields[i];
        if (!f.edited && !(f.hasObject && f.object && f.object->hasEdits())) {
            out += f.raw;
            continue;
        }
        // Regenerate the entry, reusing its original terminator.
        if (!f.name.empty()) {
            out += f.name;
            out += " = ";
        }
        if (!f.typeTag.empty()) {
            out += '"';
            out += f.typeTag;
            out += "\" ";
        }
        if (f.hasObject) {
            f.object->render(out);
        } else {
            out += f.scalar;
        }
        if (f.terminator != 0) {
            out += f.terminator;
        }
    }
    out += gaps.back();
    out += '}';
}

std::string XPrmObject::render() const
{
    std::string out;
    render(out);
    return out;
}

bool XPrmObject::hasEdits() const
{
    for (const Field& f : fields) {
        if (f.edited) {
            return true;
        }
        if (f.hasObject && f.object && f.object->hasEdits()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// XPrmDocument
// ---------------------------------------------------------------------------

XPrmDocument::Block* XPrmDocument::block(const std::string& name)
{
    for (Block& b : blocks) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

const XPrmDocument::Block* XPrmDocument::block(const std::string& name) const
{
    for (const Block& b : blocks) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

bool XPrmDocument::parse(const std::string& text)
{
    blocks.clear();
    trailing.clear();
    std::size_t errorPos = 0;
    if (!Parser(text).parse(blocks, trailing, errorPos)) {
        fprintf(stderr, "XPrmDocument: parse failed at offset %zu.\n",
                errorPos);
        blocks.clear();
        return false;
    }
    return true;
}

bool XPrmDocument::load(const std::string& path)
{
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) {
        fprintf(stderr, "XPrmDocument: cannot read '%s'.\n", path.c_str());
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse(buffer.str());
}

std::string XPrmDocument::render() const
{
    std::string out;
    for (const Block& b : blocks) {
        if (!b.edited && !(b.object && b.object->hasEdits())) {
            out += b.raw;
            continue;
        }
        out += b.name;
        out += " = ";
        if (b.object) {
            b.object->render(out);
        }
        out += b.terminator;
    }
    out += trailing;
    return out;
}

bool XPrmDocument::save(const std::string& path) const
{
    std::ofstream out(std::filesystem::u8path(path),
                      std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "XPrmDocument: cannot write '%s'.\n", path.c_str());
        return false;
    }
    const std::string text = render();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

} // namespace MapEditor

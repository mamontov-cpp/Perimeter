#pragma once

#include <memory>
#include <string>
#include <vector>

// ============================================================================
// MapEditorCore — XPrmDocument (T8.1).
//
// Byte-preserving editor for the XPrm text format the game uses for .spg
// battle maps (MissionDescriptionPrm + SavePrm) and .dat/.gmp siblings.
//
// Why a text DOM instead of Util/XPrmArchive mirror structures: a .spg holds
// the WHOLE SavePrm tree (~52 KB: frames, squads, frame slots, damage
// moleculas, 100+ UI controls, trigger data). Mirroring every struct 1:1 is
// thousands of lines and any missed field is silently DROPPED by the XPrm
// writer ("writes only what is in the structures"). The text DOM keeps every
// byte it does not touch, so a load→save round-trip of a file we never edit
// is byte-exact by construction and edits regenerate only the value they
// changed — the same contract IniDocument gives for world.ini.
//
// Grammar handled (subset the game writes):
//   document  := (name '=' object ';')*
//   object    := '{' entry* '}'
//   entry     := name '=' [string] (object | scalar) ';'
//              | (object | scalar) ','?          // anonymous array element
//   scalar    := quoted string | number | identifier (raw text is kept)
// Arrays are anonymous objects whose first entry is the element count.
//
// Every entry remembers its original slice and the gaps (whitespace and the
// ';'/',' separators) around it; render() reproduces the original text until
// an entry is marked edited.
// ============================================================================

namespace MapEditor {

class XPrmObject
{
public:
    struct Field
    {
        std::string name;    ///< empty for an anonymous array element.
        std::string typeTag; ///< tag of `key = "struct X" { ... }` (no quotes).
        bool hasObject = false;
        std::string scalar;  ///< raw scalar text (trimmed, quotes included).
        std::shared_ptr<XPrmObject> object;
        std::string raw;     ///< original slice of the whole entry incl. terminator.
        char terminator = 0; ///< ';' / ',' / 0 — reused when regenerating.
        bool edited = false; ///< raw is stale, regenerate on render.

        bool isScalar() const { return !hasObject; }

        /// Deep copy (objects are cloned, not shared).
        Field deepCopy() const;
    };

    std::vector<Field> fields;
    /// gaps[i] is the text between the '{' (or the previous entry) and
    /// fields[i]; gaps.back() is between the last entry and the '}'.
    std::vector<std::string> gaps;

    // -----------------------------------------------------------------------
    // Access
    // -----------------------------------------------------------------------

    Field* field(const std::string& name);
    const Field* field(const std::string& name) const;
    /// Child object of the named field (nullptr when absent/scalar).
    XPrmObject* object(const std::string& name);
    const XPrmObject* object(const std::string& name) const;
    /// Raw scalar of the named field ("" when absent).
    std::string scalar(const std::string& name) const;
    /// Set the scalar of the named field (creates it at the end when absent).
    Field& setScalar(const std::string& name, const std::string& rawValue);
    /// Add an entry (object or scalar) at the end; gaps gain an empty slot.
    Field& addField(Field field);
    /// Remove the named field (no-op when absent).
    void removeField(const std::string& name);

    /// Number of array elements = entries before the count entry.
    /// (playersData = { 4; {...}, {...} } — fields[0] is the count.)
    std::size_t elementCount() const
    {
        return fields.empty() ? 0 : fields.size() - 1;
    }

    // -----------------------------------------------------------------------
    // Rendering
    // -----------------------------------------------------------------------

    /// Reconstruct the object text ('{' ... '}'), keeping original bytes for
    /// everything not marked edited.
    void render(std::string& out) const;
    std::string render() const;

    /// True when this object or any descendant was edited.
    bool hasEdits() const;
};

/// One XPrm document: an ordered list of top-level `name = {...};` blocks.
class XPrmDocument
{
public:
    struct Block
    {
        std::string name;
        std::shared_ptr<XPrmObject> object;
        std::string raw;     ///< original slice including the terminator.
        char terminator = ';';
        bool edited = false;
    };

    std::vector<Block> blocks;
    /// Whitespace/comments after the last block (kept verbatim).
    std::string trailing;

    Block* block(const std::string& name);
    const Block* block(const std::string& name) const;

    /// Parse the file (false on IO error or a broken structure).
    bool load(const std::string& path);
    /// Parse from memory (tests).
    bool parse(const std::string& text);
    /// Reconstruct the document text.
    std::string render() const;
    /// Render to a file.
    bool save(const std::string& path) const;
};

} // namespace MapEditor

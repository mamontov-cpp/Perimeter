#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// MapEditorCore — IniDocument.
//
// Order-preserving reader/writer for world.ini (and similar game INI files):
//
//   - every line of the source file is kept verbatim (content plus its
//     original line ending), so a document that was not modified is written
//     back byte-for-byte; unknown sections, keys and comments survive edits;
//   - lookups are ASCII case-insensitive and values are trimmed, matching
//     SimpleIni (CSimpleIniA) which the game itself uses to read those files;
//   - values are single-line: line breaks passed to setValue() are dropped.
//     SimpleIni multi-line values ("key = <<<TAG") are preserved as plain
//     lines but are not addressable as keys.
// ============================================================================

namespace MapEditor {

class IniDocument
{
public:
    IniDocument() = default;

    // -----------------------------------------------------------------------
    // Loading / saving
    // -----------------------------------------------------------------------

    /// Read and parse a file. Returns false when it cannot be opened.
    bool load(const std::filesystem::path& path);

    /// Write the document to a file (parent directories are created).
    /// Returns false when the file cannot be written.
    bool save(const std::filesystem::path& path) const;

    /// Parse an in-memory buffer (used by tests).
    void parse(std::string_view text);

    /// Serialize; returns the exact source bytes when nothing was changed.
    std::string toString() const;

    /// True when no lines have been parsed yet.
    bool empty() const;

    // -----------------------------------------------------------------------
    // Reading
    // -----------------------------------------------------------------------

    /// True when the document contains the given section (ASCII
    /// case-insensitive). The unnamed global section (keys before the first
    /// "[section]" header) always exists.
    bool hasSection(std::string_view section) const;

    /// True when the section contains the given key.
    bool hasKey(std::string_view section, std::string_view key) const;

    /// Value with surrounding whitespace removed, or std::nullopt when the
    /// key does not exist.
    std::optional<std::string> getValue(std::string_view section,
                                         std::string_view key) const;

    /// Integer value, or std::nullopt when the key does not exist or holds
    /// no number. Accepts an optional sign and a "0x" hex prefix and ignores
    /// trailing garbage — the atoi()/strtol(base 0) rules the game relies on.
    std::optional<int> getIntValue(std::string_view section,
                                   std::string_view key) const;

    /// Names of all sections in file order (duplicates are kept as-is).
    std::vector<std::string> sectionNames() const;

    /// Names of all keys of a section in file order; empty when the section
    /// does not exist.
    std::vector<std::string> keyNames(std::string_view section) const;

    // -----------------------------------------------------------------------
    // Writing
    // -----------------------------------------------------------------------

    /// Set a value in place: existing lines keep their position, formatting
    /// and line ending. A missing key is appended to the section (a missing
    /// section, together with the key, is appended to the document).
    void setValue(std::string_view section, std::string_view key,
                  std::string_view value);

    /// Convenience wrapper over setValue().
    void setIntValue(std::string_view section, std::string_view key, int value);

private:
    struct Line
    {
        std::string content; // line without its terminator
        std::string eol;     // "\r\n", "\n", or "" for a last line w/o EOL
    };

    std::vector<Line> lines_;
    std::string defaultEol_ = "\r\n"; // line ending used for newly added lines

    /// Index of the "[section]" header line, -1 when absent (also -1 for the
    /// unnamed global section).
    int findSection(std::string_view section) const;

    /// Index of the key line inside a section (sectionLine < 0 addresses the
    /// global section), -1 when absent.
    int findKey(int sectionLine, std::string_view key) const;

    /// Append "key=value" after the last key line of an existing section.
    void insertKey(int sectionLine, std::string_view key, std::string_view value);

    /// Append a brand-new "[section]" block at the end of the document.
    void appendSection(std::string_view section, std::string_view key,
                       std::string_view value);
};

} // namespace MapEditor

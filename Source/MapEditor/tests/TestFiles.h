#pragma once

// ============================================================================
// File and INI helpers shared by the map editor tests.
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace MapEditor::Tests {

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path);

bool writeFileBytes(const std::filesystem::path& path,
                    const std::vector<std::uint8_t>& bytes);

void copyDirectoryRecursive(const std::filesystem::path& source,
                            const std::filesystem::path& target);

std::string trim(const std::string& text);

/// Parse an INI file into "<section>\x1f<key>" -> value, ignoring comments and
/// blank lines. Used for a semantic (format-independent) comparison.
std::map<std::string, std::string>
parseIni(const std::filesystem::path& path);

void printIniDiff(const std::map<std::string, std::string>& a,
                  const std::map<std::string, std::string>& b,
                  const char* aName, const char* bName);

} // namespace MapEditor::Tests

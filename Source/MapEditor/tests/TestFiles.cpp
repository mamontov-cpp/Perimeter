#include "TestFiles.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace MapEditor::Tests {

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
}

bool writeFileBytes(const std::filesystem::path& path,
                    const std::vector<std::uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

void copyDirectoryRecursive(const std::filesystem::path& source,
                            const std::filesystem::path& target)
{
    if (!std::filesystem::exists(source)) {
        return;
    }
    std::filesystem::create_directories(target);
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        const std::filesystem::path destination =
            target / entry.path().filename();
        if (entry.is_directory()) {
            copyDirectoryRecursive(entry.path(), destination);
        } else {
            std::filesystem::copy_file(
                entry.path(), destination,
                std::filesystem::copy_options::overwrite_existing);
        }
    }
}

std::string trim(const std::string& text)
{
    const char* whitespace = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(whitespace);
    return text.substr(begin, end - begin + 1);
}

std::map<std::string, std::string>
parseIni(const std::filesystem::path& path)
{
    std::map<std::string, std::string> values;
    std::ifstream file(path);
    std::string line;
    std::string section;
    while (std::getline(file, line)) {
        // Strip a possible UTF-8 BOM on the first line.
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF) {
            line.erase(0, std::min<std::size_t>(3, line.size()));
        }
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = trim(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        values[section + '\x1f' + trim(trimmed.substr(0, equals))] =
            trim(trimmed.substr(equals + 1));
    }
    return values;
}

void printIniDiff(const std::map<std::string, std::string>& a,
                  const std::map<std::string, std::string>& b,
                  const char* aName, const char* bName)
{
    for (const auto& entry : a) {
        const auto it = b.find(entry.first);
        if (it == b.end()) {
            std::fprintf(stderr, "  only in %s: '%s'='%s'\n",
                         aName, entry.first.c_str(), entry.second.c_str());
        } else if (it->second != entry.second) {
            std::fprintf(stderr, "  '%s': %s='%s' vs %s='%s'\n",
                         entry.first.c_str(), aName, entry.second.c_str(),
                         bName, it->second.c_str());
        }
    }
    for (const auto& entry : b) {
        if (a.find(entry.first) == a.end()) {
            std::fprintf(stderr, "  only in %s: '%s'='%s'\n",
                         bName, entry.first.c_str(), entry.second.c_str());
        }
    }
}

} // namespace MapEditor::Tests

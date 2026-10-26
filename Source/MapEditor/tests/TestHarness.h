#pragma once

// ============================================================================
// Minimal test harness for the map editor headless tests.
//
// Every test lives in its own translation unit and registers itself with
// MAP_EDITOR_REGISTER_TEST. The runner executes them in ascending order, which
// lets later tests build on the world that the earlier ones loaded/saved.
//
// A test returns false to signal failure. Use TEST_REQUIRE for checks; it
// prints a diagnostic with file and line and returns false from the test.
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace MapEditor::Tests {

using TestFunction = bool (*)();

/// State shared between ordered test files.
///
/// WorldRoundTripTests fills it in; the WorldFiles tests consume the saved
/// world and the values read from it. Keeping the state explicit makes the
/// dependency between the test files visible.
struct TestContext
{
    std::filesystem::path gameDir;

    // Source world (never modified).
    std::filesystem::path originalWorldDir;
    std::string worldName;
    std::string worldKey;
    std::vector<std::uint8_t> origVmpBytes;
    std::vector<std::uint8_t> origIniBytes;

    // Temporary copy of the world that the tests may write to.
    std::filesystem::path tempRoot;
    std::filesystem::path tempWorldDir;
    std::filesystem::path savedIni;

    // Post-round-trip world.ini snapshot and parsed values.
    std::vector<std::uint8_t> iniBaseline;
    std::map<std::string, std::string> savedIniValues;

    // Map size taken from the loaded world.
    int mapWidth = 0;
    int mapHeight = 0;
};

TestContext& context();

/// Register a test. Returns true (useful for a static initializer).
bool addTest(const char* name, int order, TestFunction function);

/// Print a failure diagnostic; used by TEST_REQUIRE.
void reportFailure(const char* file, int line, const char* format, ...);

/// Runs all registered tests in ascending order, optionally filtered by a
/// name subpattern. When a filter is given, only tests whose names contain
/// the subpattern are run — but all tests with a lower order value are also
/// included (they are setup tests that later tests depend on).
/// Returns the process exit code (0 = all passed, 1 = failure).
int runAll(const std::string& gameDir, const std::string& filter = "");

struct TestRegistration
{
    TestRegistration(const char* name, int order, TestFunction function)
    {
        addTest(name, order, function);
    }
};

} // namespace MapEditor::Tests

/// Register a test function at namespace scope:
///   MAP_EDITOR_REGISTER_TEST("World round-trip", 20, runWorldRoundTripTests);
#define MAP_EDITOR_REGISTER_TEST(displayName, order, function)                    \
    static const ::MapEditor::Tests::TestRegistration                             \
        function##_registration(displayName, order, &function)

/// Report a failed expectation and abort the current test.
#define TEST_REQUIRE(condition, ...)                                              \
    do {                                                                          \
        if (!(condition)) {                                                       \
            ::MapEditor::Tests::reportFailure(__FILE__, __LINE__, __VA_ARGS__);   \
            return false;                                                         \
        }                                                                         \
    } while (0)

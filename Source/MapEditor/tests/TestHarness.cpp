#include "TestHarness.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <utility>

namespace MapEditor::Tests {

namespace {

struct TestCase
{
    std::string name;
    int order = 0;
    TestFunction function = nullptr;
};

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

void cleanUp(TestContext& ctx)
{
    std::error_code error;
    if (!ctx.gameDir.empty()) {
        std::filesystem::current_path(ctx.gameDir, error);
    }
    if (!ctx.tempRoot.empty()) {
        std::filesystem::remove_all(ctx.tempRoot, error);
    }
}

} // namespace

TestContext& context()
{
    static TestContext ctx;
    return ctx;
}

bool addTest(const char* name, int order, TestFunction function)
{
    registry().push_back(TestCase{name, order, function});
    return true;
}

void reportFailure(const char* file, int line, const char* format, ...)
{
    std::fprintf(stderr, "[FAIL] %s:%d: ", file, line);

    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);

    std::fputc('\n', stderr);
    std::fflush(stderr);
}

int runAll(const std::string& gameDir, const std::string& filter)
{
    TestContext& ctx = context();
    ctx.gameDir = std::filesystem::absolute(
        std::filesystem::u8path(gameDir)).lexically_normal();

    std::vector<TestCase> tests = registry();
    std::stable_sort(tests.begin(), tests.end(),
                     [](const TestCase& a, const TestCase& b) {
                         return a.order < b.order;
                     });

    if (tests.empty()) {
        std::fprintf(stderr, "[FAIL] No tests registered.\n");
        return 1;
    }

    // Optional comma-separated name-substring filter. When given, only tests
    // whose name contains one of the patterns are run. This lets a caller
    // drive a specific subset (e.g. one brush test plus the world round-trip
    // it depends on) without the whole suite — and without aborting on an
    // unrelated failure elsewhere in the fixed-order run.
    std::vector<std::string> patterns;
    if (!filter.empty()) {
        std::size_t start = 0;
        while (start <= filter.size()) {
            const std::size_t comma = filter.find(',', start);
            std::string part = filter.substr(
                start,
                comma == std::string::npos ? std::string::npos : comma - start);
            // trim leading/trailing spaces
            const std::size_t b = part.find_first_not_of(" 	");
            const std::size_t e = part.find_last_not_of(" 	");
            if (b != std::string::npos) {
                patterns.push_back(part.substr(b, e - b + 1));
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    }

    const auto matches = [&patterns](const std::string& name) {
        if (patterns.empty()) {
            return true;
        }
        for (const std::string& p : patterns) {
            if (name.find(p) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    int runCount = 0;
    int index = 0;
    for (const TestCase& test : tests) {
        if (!matches(test.name)) {
            continue;
        }
        ++runCount;
        std::printf("\n[TEST %d] %s...\n", ++index, test.name.c_str());
        std::fflush(stdout);
        if (!test.function()) {
            std::printf("\n[FAILED] %s\n", test.name.c_str());
            std::fflush(stdout);
            cleanUp(ctx);
            return 1;
        }
        std::fflush(stdout);
    }

    if (runCount == 0) {
        std::fprintf(stderr, "[FAIL] No tests matched the filter '%s'.\n",
                     filter.c_str());
        cleanUp(ctx);
        return 1;
    }

    cleanUp(ctx);

    std::printf("\n========================================\n");
    std::printf("[ALL TESTS PASSED] (%d run)\n", runCount);
    std::printf("========================================\n");
    return 0;
}

} // namespace MapEditor::Tests

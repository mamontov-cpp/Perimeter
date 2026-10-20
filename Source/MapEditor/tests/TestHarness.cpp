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

int runAll(const std::string& gameDir)
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

    int index = 0;
    for (const TestCase& test : tests) {
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

    cleanUp(ctx);

    std::printf("\n========================================\n");
    std::printf("[ALL TESTS PASSED]\n");
    std::printf("========================================\n");
    return 0;
}

} // namespace MapEditor::Tests

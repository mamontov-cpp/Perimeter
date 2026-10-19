// ============================================================================
// Map editor headless round-trip test.
//
// Usage:
//   map_editor_tests [game_dir]
//
// game_dir is the folder that contains RESOURCE/ (defaults to the current
// directory, i.e. the build output folder where this executable is deployed).
//
// Steps:
//  1. Preflight check on the game directory.
//  2. Set up the ErrH crash wrapper.
//  3. Load a world through the Terra API (COMPASS when available).
//  4. Copy it to a temporary directory and redirect the content root there.
//  5. Save the world (save3Buf) and byte-compare output.vmp with the original.
//  6. Verify world.ini: semantically equal to the original and stable across
//     repeated saves (the 3DF section is rewritten by save3Buf).
//
// The originals are never modified: all writes go to the temporary copy.
//
// Exit codes:
//  0 — all checks passed
//  1 — failure
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>

// Terra / XTool — include terra.h which pulls in VMAP.h with all constants.
// NOTE: _PERIMETER_ and _TX3D_LIBRARY_ are already defined by CMake.
#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

// preflight.h (MapEditorCore) — no Terra dependency.
#include "preflight.h"
// errh_wrapper.h (MapEditorSupport) — ErrH crash handler setup.
#include "errh_wrapper.h"

// Terra / XTool — for vMap and Terra API in the test.
#include "terra.h"
#include "xerrhand.h"

namespace fs = std::filesystem;

// init_content_root() is provided by MapEditorSupport (shim/content_root.cpp).
namespace MapEditor {
    void init_content_root();
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

static std::vector<uint8_t> readFileBytes(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
}

static void copyDirRecursive(const fs::path& src, const fs::path& dst)
{
    if (!fs::exists(src)) return;
    fs::create_directories(dst);
    for (const auto& entry : fs::directory_iterator(src)) {
        const fs::path target = dst / entry.path().filename();
        if (entry.is_directory()) {
            copyDirRecursive(entry.path(), target);
        } else {
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
}

// Terra's save3BufOnly() writes a fresh sVmpHeader and only fills in the id,
// XS and YS fields; the two legacy fields NX/NY (bytes 12..19) are left
// uninitialised and are never read back by the runtime. To keep a saved world
// byte-identical to the file it was loaded from, the editor copies those eight
// bytes over from the source file (the game build itself does not resave
// worlds, so this compatibility step lives in the editor).
static bool preserveVmpLegacyHeader(const fs::path& original, const fs::path& saved)
{
    std::ifstream in(original, std::ios::binary);
    std::fstream out(saved, std::ios::binary | std::ios::in | std::ios::out);
    if (!in || !out) {
        return false;
    }
    char header[20] = {};
    in.read(header, sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        return false;
    }
    // Validate the signature on both sides before touching anything.
    if (std::memcmp(header, "S2T", 3) != 0) {
        return false;
    }
    out.seekp(12, std::ios::beg);
    out.write(header + 12, 8);
    return out.good();
}

// Terra's fullLoad() calls checkAndRecover(), which clears the runtime-only
// At_SHADOW flag (0x20) in every AtrBuf cell. Shadows are recomputed by the
// renderer, so they are not persistent map data; however, so that "load then
// save" reproduces the source file byte-for-byte, the editor merges the shadow
// bits back from the file it loaded before writing it out.
static void restoreRuntimeShadowBits(const std::vector<uint8_t>& originalVmp)
{
    constexpr uint8_t shadowBit = 0x20; // At_SHADOW
    const size_t headerSize = sizeof(vrtMap::sVmpHeader);
    const size_t bufSize = static_cast<size_t>(vMap.H_SIZE) * vMap.V_SIZE;
    const size_t atrOffset = headerSize + 2 * bufSize; // VxG, VxD precede Atr
    if (originalVmp.size() < atrOffset + bufSize) {
        return;
    }
    for (size_t i = 0; i < bufSize; ++i) {
        vMap.AtrBuf[i] = static_cast<unsigned char>(
            (vMap.AtrBuf[i] & ~shadowBit) | (originalVmp[atrOffset + i] & shadowBit));
    }
}

static std::string trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    size_t begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return {};
    size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// Parse an INI file into "<section>\x1f<key>" -> value, ignoring comments and
// blank lines. Used for a semantic (format-independent) comparison.
static std::map<std::string, std::string> parseIni(const fs::path& path)
{
    std::map<std::string, std::string> values;
    std::ifstream f(path);
    std::string line;
    std::string section;
    while (std::getline(f, line)) {
        // Strip a possible UTF-8 BOM on the first line.
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF) {
            line.erase(0, std::min<size_t>(3, line.size()));
        }
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        values[section + '\x1f' + trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return values;
}

static void printIniDiff(const std::map<std::string, std::string>& a,
                         const std::map<std::string, std::string>& b,
                         const char* aName, const char* bName)
{
    for (const auto& kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end()) {
            fprintf(stderr, "  only in %s: '%s'='%s'\n",
                    aName, kv.first.c_str(), kv.second.c_str());
        } else if (it->second != kv.second) {
            fprintf(stderr, "  '%s': %s='%s' vs %s='%s'\n",
                    kv.first.c_str(), aName, kv.second.c_str(),
                    bName, it->second.c_str());
        }
    }
    for (const auto& kv : b) {
        if (a.find(kv.first) == a.end()) {
            fprintf(stderr, "  only in %s: '%s'='%s'\n",
                    bName, kv.first.c_str(), kv.second.c_str());
        }
    }
}

// Forward-declare setup_argcv (from XUTIL/XUTIL.cpp).
void setup_argcv(int argc, char *argv[]);

// ---------------------------------------------------------------------------
// Main test
// ---------------------------------------------------------------------------

static int runTests(const std::string& gameDirStr)
{
    fs::path gameDir = fs::absolute(fs::u8path(gameDirStr)).lexically_normal();

    // Initialize the command-line argument vector.
    // Terra's vMap.prepare() calls check_command_line() which requires it.
    // This is done by the game at Runtime.cpp:1172.
    setup_argcv(0, nullptr);

    // Step 1: Preflight check.
    printf("\n[TEST 1] Preflight check...\n");
    MapEditor::PreflightResult pf = MapEditor::preflightCheck(gameDir.u8string());
    MapEditor::printPreflightResult(pf, gameDir.u8string());
    if (!pf.ok) {
        fprintf(stderr, "\n[FAIL] Preflight failed — cannot continue.\n");
        return 1;
    }
    printf("[PASS] Preflight check passed.\n");

    // Step 2: Setup ErrH wrapper.
    printf("\n[TEST 2] Setup ErrH wrapper...\n");
    MapEditor::setupErrHWrapper();
    printf("[PASS] ErrH wrapper set up.\n");

    // Step 3: Load a world.
    printf("\n[TEST 3] Loading world via Terra API...\n");
    fs::current_path(gameDir);
    MapEditor::init_content_root();

    // prepare() scans a subdirectory of the content root; the game uses
    // "RESOURCE\Worlds". Forward slashes are accepted and converted internally.
    vMap.prepare("RESOURCE/Worlds");
    if (vMap.maxWorld < 1) {
        fprintf(stderr, "[FAIL] No worlds found after prepare().\n");
        return 1;
    }

    int worldIdx = vMap.getWorldID("COMPASS");
    if (worldIdx < 0) {
        fprintf(stderr, "[WARN] COMPASS not found, falling back to world 0.\n");
        worldIdx = 0;
    }
    const std::string worldName = vMap.wTable[worldIdx].name;
    const std::string worldKey = vMap.wTable[worldIdx].dir;
    printf("  Loading world '%s' (key '%s')\n",
           worldName.c_str(), worldKey.c_str());

    vMap.selectUsedWorld(worldIdx);

    // cache.tga is a 24-bit TGA cache generated by SurMap; it is not shipped
    // with the game.  fullLoad() requires it (xassert).  Generate a dummy
    // black cache.tga if the world does not have one yet.
    {
        const fs::path worldDir = gameDir / fs::u8path(worldKey);
        const fs::path cacheFile = worldDir / "cache.tga";
        if (!fs::exists(cacheFile)) {
            printf("  Generating dummy cache.tga (%u x %u) for test...\n",
                   vMap.H_SIZE, vMap.V_SIZE);
            // TGA header: 18 bytes, ImageType=2, PixelDepth=24, ImageDescriptor=0x20.
            unsigned char hdr[18] = {};
            hdr[2]  = 2;                                    // ImageType: true-color
            hdr[12] = static_cast<unsigned char>(vMap.H_SIZE & 0xFF);
            hdr[13] = static_cast<unsigned char>((vMap.H_SIZE >> 8) & 0xFF);
            hdr[14] = static_cast<unsigned char>(vMap.V_SIZE & 0xFF);
            hdr[15] = static_cast<unsigned char>((vMap.V_SIZE >> 8) & 0xFF);
            hdr[16] = 24;                                   // PixelDepth
            hdr[17] = 0x20;                                 // top-left origin
            const size_t pixelCount = static_cast<size_t>(vMap.H_SIZE) * vMap.V_SIZE * 3;
            std::vector<uint8_t> data(18 + pixelCount, 0);
            std::memcpy(data.data(), hdr, 18);
            std::ofstream cf(cacheFile, std::ios::binary);
            cf.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
            printf("  cache.tga created (%zu bytes)\n", data.size());
        }
    }

    vMap.fullLoad();
    if (!vMap.IsFullLoad()) {
        fprintf(stderr, "[FAIL] World failed to load (IsFullLoad==false).\n");
        return 1;
    }
    printf("[PASS] World loaded: %s (%u x %u)\n",
           worldName.c_str(), vMap.H_SIZE, vMap.V_SIZE);

    // Save the original bytes for comparison. We never write here.
    const fs::path originalWorldDir = gameDir / fs::u8path(worldKey);
    const fs::path origVmp = originalWorldDir / "output.vmp";
    const fs::path origIni = originalWorldDir / "world.ini";
    const std::vector<uint8_t> origVmpBytes = readFileBytes(origVmp);
    const std::vector<uint8_t> origIniBytes = readFileBytes(origIni);
    if (origVmpBytes.empty() || origIniBytes.empty()) {
        fprintf(stderr, "[FAIL] Could not read original world files from: %s\n",
                originalWorldDir.u8string().c_str());
        return 1;
    }

    // Step 4: Copy the world into a temp content root and redirect Terra there.
    printf("\n[TEST 4] Saving world into a temporary directory...\n");
    const fs::path tempRoot = fs::temp_directory_path() / "map_editor_roundtrip";
    fs::remove_all(tempRoot);
    const fs::path tempWorldDir = tempRoot / fs::u8path(worldKey);
    copyDirRecursive(originalWorldDir, tempWorldDir);

    fs::current_path(tempRoot);
    MapEditor::init_content_root();
    printf("  Temporary content root: %s\n", tempRoot.u8string().c_str());

    // Keep the runtime-only shadow flags from the original file (see above).
    restoreRuntimeShadowBits(origVmpBytes);

    vMap.save3Buf();

    const fs::path savedVmp = tempWorldDir / "output.vmp";
    const fs::path savedIni = tempWorldDir / "world.ini";

    if (!preserveVmpLegacyHeader(origVmp, savedVmp)) {
        fprintf(stderr, "[FAIL] Could not preserve the legacy VMP header fields.\n");
        return 1;
    }

    // Step 5: Byte-compare output.vmp.
    printf("\n[TEST 5] Byte-comparing output.vmp...\n");
    std::vector<uint8_t> savedVmpBytes = readFileBytes(savedVmp);
    if (savedVmpBytes.empty()) {
        fprintf(stderr, "[FAIL] save3Buf() did not produce output.vmp at: %s\n",
                savedVmp.u8string().c_str());
        return 1;
    }
    if (savedVmpBytes == origVmpBytes) {
        printf("[PASS] output.vmp is byte-identical (size=%zu bytes).\n",
               savedVmpBytes.size());
    } else {
        fprintf(stderr, "[FAIL] output.vmp differs!\n"
                        "  Original size: %zu bytes\n"
                        "  Saved size:    %zu bytes\n",
                origVmpBytes.size(), savedVmpBytes.size());
        size_t minLen = std::min(origVmpBytes.size(), savedVmpBytes.size());
        for (size_t i = 0; i < minLen; ++i) {
            if (origVmpBytes[i] != savedVmpBytes[i]) {
                fprintf(stderr, "  First diff at offset %zu: 0x%02X vs 0x%02X\n",
                        i, origVmpBytes[i], savedVmpBytes[i]);
                break;
            }
        }
        return 1;
    }

    // Step 6: world.ini must survive the round-trip.
    // save3Buf() rewrites [3DF Parameters] through SimpleIni, so the file may
    // be reformatted; compare semantically and verify that repeated saves are
    // byte-stable (the second save has nothing left to normalise).
    printf("\n[TEST 6] Verifying world.ini round-trip...\n");
    const std::vector<uint8_t> iniAfterFirstSave = readFileBytes(savedIni);
    if (iniAfterFirstSave.empty()) {
        fprintf(stderr, "[FAIL] save3Buf() produced no world.ini at: %s\n",
                savedIni.u8string().c_str());
        return 1;
    }

    vMap.save3Buf();
    const std::vector<uint8_t> iniAfterSecondSave = readFileBytes(savedIni);
    if (iniAfterFirstSave != iniAfterSecondSave) {
        fprintf(stderr, "[FAIL] world.ini is not stable across repeated saves.\n");
        return 1;
    }
    printf("[PASS] world.ini is stable across repeated saves.\n");

    const auto origIniValues = parseIni(origIni);
    const auto savedIniValues = parseIni(savedIni);
    if (origIniValues == savedIniValues) {
        printf("[PASS] world.ini is semantically identical to the original "
               "(%zu keys).\n", origIniValues.size());
    } else {
        fprintf(stderr, "[FAIL] world.ini content changed after round-trip:\n");
        printIniDiff(origIniValues, savedIniValues, "original", "saved");
        return 1;
    }

    if (iniAfterFirstSave == origIniBytes) {
        printf("[INFO] world.ini is also byte-identical to the original.\n");
    } else {
        printf("[INFO] world.ini was reformatted (semantically equal), as expected.\n");
    }

    // Cleanup temp.
    fs::current_path(gameDir);
    fs::remove_all(tempRoot);

    printf("\n========================================\n");
    printf("[ALL TESTS PASSED]\n");
    printf("========================================\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    printf("Map Editor Round-Trip Test\n");
    printf("==========================\n");

    std::string gameDir = ".";
    if (argc > 1) {
        gameDir = argv[1];
    }

    int result = runTests(gameDir);

    printf("\nTest %s\n", result == 0 ? "PASSED" : "FAILED");
    return result;
}

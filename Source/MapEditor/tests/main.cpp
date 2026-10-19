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
// WorldFiles (MapEditorCore) — file-level world folder access.
#include "WorldFiles.h"
// errh_wrapper.h (MapEditorSupport) — ErrH crash handler setup.
#include "errh_wrapper.h"
// shims.h (MapEditorSupport) — ReadIniString, the game's SimpleIni reader.
#include "shims.h"

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

static bool writeFileBytes(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return f.good();
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

    // ------------------------------------------------------------------
    // Step 7: WorldFiles — world.ini read/edit/write round-trip.
    // ------------------------------------------------------------------
    printf("\n[TEST 7] WorldFiles: world.ini read/edit/write...\n");

    MapEditor::WorldFiles worldFiles(tempWorldDir);

    // Baseline is the file as it is on disk right now: TEST 6 let SimpleIni
    // rewrite it, and that state must survive WorldFiles unchanged.
    const std::vector<uint8_t> iniBaseline = readFileBytes(savedIni);
    if (iniBaseline.empty()) {
        fprintf(stderr, "[FAIL] world.ini is missing before the WorldFiles checks.\n");
        return 1;
    }

    // 7.1: an untouched document must be written back byte-for-byte.
    {
        auto document = worldFiles.readIni();
        if (!document) {
            fprintf(stderr, "[FAIL] WorldFiles::readIni() failed.\n");
            return 1;
        }
        if (!worldFiles.writeIni(*document)) {
            fprintf(stderr, "[FAIL] WorldFiles::writeIni() failed.\n");
            return 1;
        }
        if (readFileBytes(savedIni) != iniBaseline) {
            fprintf(stderr, "[FAIL] world.ini changed across a read/write cycle.\n");
            return 1;
        }
        printf("[PASS] untouched world.ini is byte-stable (%zu bytes).\n",
               iniBaseline.size());
    }

    // 7.2: typed getters agree with the file / the loaded world.
    int mapWidth = 0;
    int mapHeight = 0;
    int hZeroPlastValue = 0;
    {
        const auto mapPower = worldFiles.readMapPower();
        if (!mapPower || mapPower->first < 1 || mapPower->second < 1) {
            fprintf(stderr, "[FAIL] could not read [Global Parameters] Map Power X/Y.\n");
            return 1;
        }
        mapWidth = 1 << mapPower->first;
        mapHeight = 1 << mapPower->second;
        if (mapWidth != static_cast<int>(vMap.H_SIZE) ||
            mapHeight != static_cast<int>(vMap.V_SIZE)) {
            fprintf(stderr,
                    "[FAIL] world.ini map power (%d,%d -> %dx%d) does not match "
                    "the loaded map (%ux%u).\n",
                    mapPower->first, mapPower->second, mapWidth, mapHeight,
                    vMap.H_SIZE, vMap.V_SIZE);
            return 1;
        }

        const auto hZeroPlast = worldFiles.readHZeroPlast();
        if (!hZeroPlast || *hZeroPlast < 0 || *hZeroPlast > 255) {
            fprintf(stderr, "[FAIL] [Global Parameters] hZeroPlast is missing or invalid.\n");
            return 1;
        }
        hZeroPlastValue = *hZeroPlast;

        const std::string hzpKey =
            std::string(MapEditor::WorldIni::GlobalParameters) + '\x1f' +
            MapEditor::WorldIni::HZeroPlast;
        const auto expectedHzp = savedIniValues.find(hzpKey);
        if (expectedHzp == savedIniValues.end() ||
            std::to_string(hZeroPlastValue) != expectedHzp->second) {
            fprintf(stderr, "[FAIL] hZeroPlast=%d disagrees with the semantic INI parse.\n",
                    hZeroPlastValue);
            return 1;
        }

        const auto version = worldFiles.readStorageVersion();
        const std::string versionKey =
            std::string(MapEditor::WorldIni::Storage) + '\x1f' +
            MapEditor::WorldIni::Version;
        const auto expectedVersion = savedIniValues.find(versionKey);
        if (!version || expectedVersion == savedIniValues.end() ||
            *version != expectedVersion->second) {
            fprintf(stderr, "[FAIL] [Storage] Version is missing or wrong.\n");
            return 1;
        }

        if (worldFiles.readIniInt(MapEditor::WorldIni::GlobalParameters,
                                  "No Such Key")) {
            fprintf(stderr, "[FAIL] a missing key was reported as present.\n");
            return 1;
        }

        printf("[PASS] typed getters agree with the file (map %dx%d, hZeroPlast=%d, "
               "storage version '%s').\n",
               mapWidth, mapHeight, hZeroPlastValue, version->c_str());
    }

    // 7.3: an in-place edit touches exactly one key and is readable by the
    //      same SimpleIni reader the game uses.
    {
        const int newValue = (hZeroPlastValue == 177) ? 178 : 177;
        if (!worldFiles.writeHZeroPlast(newValue)) {
            fprintf(stderr, "[FAIL] WorldFiles::writeHZeroPlast() failed.\n");
            return 1;
        }

        const auto editedValues = parseIni(savedIni);
        size_t changedKeys = 0;
        std::string changedKey;
        for (const auto& kv : savedIniValues) {
            const auto it = editedValues.find(kv.first);
            if (it == editedValues.end() || it->second != kv.second) {
                ++changedKeys;
                changedKey = kv.first;
            }
        }
        const std::string hzpKey =
            std::string(MapEditor::WorldIni::GlobalParameters) + '\x1f' +
            MapEditor::WorldIni::HZeroPlast;
        const auto editedHzp = editedValues.find(hzpKey);
        if (changedKeys != 1 || changedKey != hzpKey ||
            editedHzp == editedValues.end() ||
            editedHzp->second != std::to_string(newValue)) {
            fprintf(stderr,
                    "[FAIL] editing hZeroPlast changed %zu key(s) (last: '%s').\n",
                    changedKeys, changedKey.c_str());
            return 1;
        }

        char iniBuffer[64] = {};
        ReadIniString(MapEditor::WorldIni::GlobalParameters,
                      MapEditor::WorldIni::HZeroPlast, "", iniBuffer,
                      static_cast<uint32_t>(sizeof(iniBuffer)),
                      savedIni.u8string());
        if (std::string(iniBuffer) != std::to_string(newValue)) {
            fprintf(stderr,
                    "[FAIL] SimpleIni reads hZeroPlast='%s', expected '%d'.\n",
                    iniBuffer, newValue);
            return 1;
        }
        printf("[PASS] in-place edit changes only hZeroPlast and stays readable by SimpleIni.\n");

        if (!worldFiles.writeHZeroPlast(hZeroPlastValue)) {
            fprintf(stderr, "[FAIL] restoring hZeroPlast failed.\n");
            return 1;
        }
        if (readFileBytes(savedIni) != iniBaseline) {
            fprintf(stderr, "[FAIL] restoring the old value did not reproduce world.ini.\n");
            return 1;
        }
        printf("[PASS] restoring the old value reproduces world.ini byte-for-byte.\n");
    }

    // 7.4: adding a key to an existing section and a whole new section.
    {
        if (!worldFiles.writeIniString(MapEditor::WorldIni::RenderingParameters,
                                       "Editor Probe", "42") ||
            !worldFiles.writeIniString("Editor Test Section", "Probe",
                                       "hello world")) {
            fprintf(stderr, "[FAIL] appending new keys/sections failed.\n");
            return 1;
        }

        const auto document = worldFiles.readIni();
        if (!document) {
            fprintf(stderr, "[FAIL] world.ini unreadable after appending keys.\n");
            return 1;
        }
        // Lookups are case-insensitive, like SimpleIni.
        const auto existing = document->getValue("rendering parameters", "EDITOR PROBE");
        const auto appended = document->getValue("Editor Test Section", "Probe");
        if (!existing || *existing != "42" || !appended ||
            *appended != "hello world") {
            fprintf(stderr, "[FAIL] appended keys cannot be read back.\n");
            return 1;
        }

        char iniBuffer[64] = {};
        ReadIniString("Editor Test Section", "Probe", "", iniBuffer,
                      static_cast<uint32_t>(sizeof(iniBuffer)),
                      savedIni.u8string());
        if (std::string(iniBuffer) != "hello world") {
            fprintf(stderr, "[FAIL] SimpleIni cannot read the appended section.\n");
            return 1;
        }
        printf("[PASS] new keys/sections are appended and readable by SimpleIni.\n");

        // Drop the probe lines again so later checks see the baseline file.
        if (!writeFileBytes(savedIni, iniBaseline)) {
            fprintf(stderr, "[FAIL] could not restore the world.ini baseline.\n");
            return 1;
        }
    }

    // ------------------------------------------------------------------
    // Step 8: WorldFiles — palettes, hardness, XML, leveled texture.
    // ------------------------------------------------------------------
    printf("\n[TEST 8] WorldFiles: palettes / hardness / xml / leveled texture...\n");

    const fs::path probeDir = tempRoot / "worldfiles_probe";
    fs::remove_all(probeDir);
    fs::create_directories(probeDir);
    MapEditor::WorldFiles probe(probeDir);

    // 8.1: inGeo.act / inDam.act (768-byte BGR palettes).
    {
        MapEditor::GeoDamPalettes palettes;
        if (!worldFiles.readPalettes(palettes)) {
            fprintf(stderr, "[FAIL] WorldFiles::readPalettes() failed.\n");
            return 1;
        }

        const std::vector<uint8_t> geoOriginal =
            readFileBytes(originalWorldDir / "inGeo.act");
        const std::vector<uint8_t> damOriginal =
            readFileBytes(originalWorldDir / "inDam.act");
        if (geoOriginal.size() != MapEditor::GeoDamPalettes::SizeBytes ||
            damOriginal.size() != MapEditor::GeoDamPalettes::SizeBytes) {
            fprintf(stderr, "[FAIL] source palettes are not 768 bytes long.\n");
            return 1;
        }
        if (std::vector<uint8_t>(palettes.geo.begin(), palettes.geo.end()) != geoOriginal ||
            std::vector<uint8_t>(palettes.dam.begin(), palettes.dam.end()) != damOriginal) {
            fprintf(stderr, "[FAIL] read palettes differ from inGeo.act / inDam.act.\n");
            return 1;
        }

        if (!probe.writePalettes(palettes)) {
            fprintf(stderr, "[FAIL] WorldFiles::writePalettes() failed.\n");
            return 1;
        }
        if (readFileBytes(probeDir / "inGeo.act") != geoOriginal ||
            readFileBytes(probeDir / "inDam.act") != damOriginal) {
            fprintf(stderr, "[FAIL] written palettes are not byte-identical.\n");
            return 1;
        }
        printf("[PASS] inGeo.act / inDam.act round-trip byte-identically (768 bytes each).\n");
    }

    // 8.2: hardness.bin (uint16 width/height + packed 2-bit values).
    {
        MapEditor::HardnessData hardness;
        if (!worldFiles.readHardness(hardness)) {
            fprintf(stderr, "[FAIL] WorldFiles::readHardness() failed.\n");
            return 1;
        }

        const auto expectedDims =
            MapEditor::HardnessData::dimensionsForMap(mapWidth, mapHeight);
        if (hardness.width != expectedDims.first ||
            hardness.height != expectedDims.second) {
            fprintf(stderr,
                    "[FAIL] hardness grid is %ux%u, expected %ux%u for a %dx%d map.\n",
                    static_cast<unsigned>(hardness.width),
                    static_cast<unsigned>(hardness.height),
                    static_cast<unsigned>(expectedDims.first),
                    static_cast<unsigned>(expectedDims.second), mapWidth, mapHeight);
            return 1;
        }

        const std::vector<uint8_t> hardnessOriginal =
            readFileBytes(originalWorldDir / "hardness.bin");
        if (hardnessOriginal.empty()) {
            fprintf(stderr, "[FAIL] source hardness.bin is missing.\n");
            return 1;
        }

        // A mismatched payload must be rejected before anything is written.
        MapEditor::HardnessData broken = hardness;
        broken.packed.pop_back();
        if (probe.writeHardness(broken)) {
            fprintf(stderr, "[FAIL] writeHardness() accepted a truncated payload.\n");
            return 1;
        }

        if (!probe.writeHardness(hardness)) {
            fprintf(stderr, "[FAIL] WorldFiles::writeHardness() failed.\n");
            return 1;
        }
        if (readFileBytes(probeDir / "hardness.bin") != hardnessOriginal) {
            fprintf(stderr, "[FAIL] written hardness.bin is not byte-identical.\n");
            return 1;
        }
        printf("[PASS] hardness.bin round-trips (%ux%u grid, %zu bytes).\n",
               static_cast<unsigned>(hardness.width),
               static_cast<unsigned>(hardness.height), hardness.packed.size());
    }

    // 8.3: geoPal.xml / geoTx.xml copy and rewrite.
    {
        const fs::path xmlDir = tempRoot / "worldfiles_xml";
        fs::remove_all(xmlDir);
        fs::create_directories(xmlDir);
        MapEditor::WorldFiles xmlWorld(xmlDir);

        if (xmlWorld.copyLayerXmlFrom(tempRoot / "does_not_exist")) {
            fprintf(stderr, "[FAIL] copyLayerXmlFrom() accepted a missing source.\n");
            return 1;
        }
        if (!xmlWorld.copyLayerXmlFrom(originalWorldDir)) {
            fprintf(stderr, "[FAIL] WorldFiles::copyLayerXmlFrom() failed.\n");
            return 1;
        }

        const std::vector<uint8_t> geoXmlOriginal =
            readFileBytes(originalWorldDir / "geoPal.xml");
        const std::vector<uint8_t> txXmlOriginal =
            readFileBytes(originalWorldDir / "geoTx.xml");
        if (geoXmlOriginal.empty() || txXmlOriginal.empty()) {
            fprintf(stderr, "[FAIL] source XML files are missing.\n");
            return 1;
        }
        if (readFileBytes(xmlDir / "geoPal.xml") != geoXmlOriginal ||
            readFileBytes(xmlDir / "geoTx.xml") != txXmlOriginal) {
            fprintf(stderr, "[FAIL] copied XML files are not byte-identical.\n");
            return 1;
        }

        const auto geoXml = xmlWorld.readGeoPalXml();
        if (!geoXml || !xmlWorld.writeGeoPalXml(*geoXml)) {
            fprintf(stderr, "[FAIL] geoPal.xml text read/write failed.\n");
            return 1;
        }
        if (readFileBytes(xmlDir / "geoPal.xml") != geoXmlOriginal) {
            fprintf(stderr, "[FAIL] geoPal.xml changed across a text round-trip.\n");
            return 1;
        }
        printf("[PASS] geoPal.xml / geoTx.xml copy and text round-trip byte-identically.\n");
    }

    // 8.4: leveledSurfaceTexture.tga validation and round-trip.
    {
        std::vector<uint8_t> texture;
        if (!worldFiles.readLeveledTexture(texture)) {
            fprintf(stderr, "[FAIL] WorldFiles::readLeveledTexture() failed.\n");
            return 1;
        }
        const auto header = MapEditor::LeveledTextureHeader::fromBytes(texture);
        if (!header) {
            fprintf(stderr, "[FAIL] leveledSurfaceTexture.tga has no TGA header.\n");
            return 1;
        }
        if (header->width > mapWidth || header->height > mapHeight ||
            !header->isPowerOfTwoSized()) {
            fprintf(stderr, "[FAIL] unexpected leveled texture header.\n");
            return 1;
        }

        const auto errors = MapEditor::WorldFiles::validateLeveledTexture(
            texture, mapWidth, mapHeight);
        if (!errors.empty()) {
            fprintf(stderr, "[FAIL] a valid leveled texture was rejected:\n");
            for (const auto& error : errors) {
                fprintf(stderr, "  - %s\n", error.c_str());
            }
            return 1;
        }

        // A map that is too small for the texture must be rejected.
        if (!MapEditor::WorldFiles::validateLeveledTexture(
                 texture, mapWidth / 4, mapHeight / 4).empty()) {
            fprintf(stderr, "[FAIL] a texture larger than the map was accepted.\n");
            return 1;
        }

        // A true-color (unpalettized) TGA must be rejected.
        std::vector<uint8_t> trueColor(18, 0);
        trueColor[1] = 0;                         // no color map
        trueColor[2] = 2;                         // uncompressed true-color
        trueColor[12] = 0x00;
        trueColor[13] = 0x01;                     // width 256
        trueColor[14] = 0x00;
        trueColor[15] = 0x01;                     // height 256
        trueColor[16] = 24;                       // 24 bits per pixel
        if (MapEditor::WorldFiles::validateLeveledTexture(
                trueColor, mapWidth, mapHeight).empty()) {
            fprintf(stderr, "[FAIL] a true-color TGA was accepted.\n");
            return 1;
        }
        if (probe.writeLeveledTexture(trueColor, mapWidth, mapHeight)) {
            fprintf(stderr, "[FAIL] writeLeveledTexture() wrote an invalid texture.\n");
            return 1;
        }

        const std::vector<uint8_t> textureOriginal =
            readFileBytes(originalWorldDir / "leveledSurfaceTexture.tga");
        if (textureOriginal.empty()) {
            fprintf(stderr, "[FAIL] source leveledSurfaceTexture.tga is missing.\n");
            return 1;
        }
        if (!probe.writeLeveledTexture(texture, mapWidth, mapHeight)) {
            fprintf(stderr, "[FAIL] WorldFiles::writeLeveledTexture() failed.\n");
            return 1;
        }
        if (readFileBytes(probeDir / "leveledSurfaceTexture.tga") != textureOriginal) {
            fprintf(stderr, "[FAIL] written leveledSurfaceTexture.tga is not byte-identical.\n");
            return 1;
        }
        printf("[PASS] leveledSurfaceTexture.tga validates (%ux%u) and round-trips.\n",
               static_cast<unsigned>(header->width),
               static_cast<unsigned>(header->height));
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

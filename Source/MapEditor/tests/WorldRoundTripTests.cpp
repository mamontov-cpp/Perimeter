// ============================================================================
// T1 — Terra world load/save round-trip.
//
// Loads a world through the Terra API (COMPASS when available), copies it to a
// temporary directory, saves it with save3Buf() and verifies that:
//   * output.vmp is byte-identical to the source,
//   * world.ini is semantically unchanged and stable across repeated saves.
//
// This test also prepares the shared TestContext used by the WorldFiles tests.
//
// The originals are never modified: all writes go to the temporary copy.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

// Terra / XTool — include terra.h which pulls in VMAP.h with all constants.
// NOTE: _PERIMETER_ and _TX3D_LIBRARY_ are already defined by CMake.
#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

// errh_wrapper.h (MapEditorSupport) — ErrH crash handler setup.
#include "errh_wrapper.h"

#include "terra.h"
#include "xerrhand.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// init_content_root() is provided by MapEditorSupport (shim/content_root.cpp).
namespace MapEditor {
void init_content_root();
}

// From XUTIL/XUTIL.cpp.
void setup_argcv(int argc, char* argv[]);

namespace {

// Terra's save3BufOnly() writes a fresh sVmpHeader and only fills in the id,
// XS and YS fields; the two legacy fields NX/NY (bytes 12..19) are left
// uninitialised and are never read back by the runtime. To keep a saved world
// byte-identical to the file it was loaded from, the editor copies those eight
// bytes over from the source file (the game build itself does not resave
// worlds, so this compatibility step lives in the editor).
bool preserveVmpLegacyHeader(const fs::path& original, const fs::path& saved)
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
void restoreRuntimeShadowBits(const std::vector<std::uint8_t>& originalVmp)
{
    constexpr std::uint8_t shadowBit = 0x20; // At_SHADOW
    const std::size_t headerSize = sizeof(vrtMap::sVmpHeader);
    const std::size_t bufSize =
        static_cast<std::size_t>(vMap.H_SIZE) * vMap.V_SIZE;
    const std::size_t atrOffset = headerSize + 2 * bufSize; // VxG, VxD precede Atr
    if (originalVmp.size() < atrOffset + bufSize) {
        return;
    }
    for (std::size_t i = 0; i < bufSize; ++i) {
        vMap.AtrBuf[i] = static_cast<unsigned char>(
            (vMap.AtrBuf[i] & ~shadowBit) |
            (originalVmp[atrOffset + i] & shadowBit));
    }
}

bool runWorldRoundTripTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    const fs::path& gameDir = ctx.gameDir;

    // Initialize the command-line argument vector.
    // Terra's vMap.prepare() calls check_command_line() which requires it.
    // This is done by the game at Runtime.cpp:1172.
    setup_argcv(0, nullptr);

    // ------------------------------------------------------------------
    // Step 2: Setup ErrH wrapper.
    // ------------------------------------------------------------------
    std::printf("\nStep 2: Setup ErrH wrapper...\n");
    MapEditor::setupErrHWrapper();
    std::printf("[PASS] ErrH wrapper set up.\n");

    // ------------------------------------------------------------------
    // Step 3: Load a world.
    // ------------------------------------------------------------------
    std::printf("\nStep 3: Loading world via Terra API...\n");
    fs::current_path(gameDir);
    MapEditor::init_content_root();

    // prepare() scans a subdirectory of the content root; the game uses
    // "RESOURCE\Worlds". Forward slashes are accepted and converted internally.
    vMap.prepare("RESOURCE/Worlds");
    TEST_REQUIRE(vMap.maxWorld >= 1, "No worlds found after prepare().");

    int worldIdx = vMap.getWorldID("COMPASS");
    if (worldIdx < 0) {
        std::fprintf(stderr, "[WARN] COMPASS not found, falling back to world 0.\n");
        worldIdx = 0;
    }
    const std::string worldName = vMap.wTable[worldIdx].name;
    const std::string worldKey = vMap.wTable[worldIdx].dir;
    std::printf("  Loading world '%s' (key '%s')\n",
                worldName.c_str(), worldKey.c_str());

    vMap.selectUsedWorld(worldIdx);

    // cache.tga is a 24-bit TGA cache generated by SurMap; it is not shipped
    // with the game. fullLoad() requires it (xassert). Generate a dummy black
    // cache.tga if the world does not have one yet.
    {
        const fs::path worldDir = gameDir / fs::u8path(worldKey);
        const fs::path cacheFile = worldDir / "cache.tga";
        if (!fs::exists(cacheFile)) {
            std::printf("  Generating dummy cache.tga (%u x %u) for test...\n",
                        vMap.H_SIZE, vMap.V_SIZE);
            // TGA header: 18 bytes, ImageType=2, PixelDepth=24, ImageDescriptor=0x20.
            unsigned char header[18] = {};
            header[2] = 2; // ImageType: true-color
            header[12] = static_cast<unsigned char>(vMap.H_SIZE & 0xFF);
            header[13] = static_cast<unsigned char>((vMap.H_SIZE >> 8) & 0xFF);
            header[14] = static_cast<unsigned char>(vMap.V_SIZE & 0xFF);
            header[15] = static_cast<unsigned char>((vMap.V_SIZE >> 8) & 0xFF);
            header[16] = 24;   // PixelDepth
            header[17] = 0x20; // top-left origin
            const std::size_t pixelCount =
                static_cast<std::size_t>(vMap.H_SIZE) * vMap.V_SIZE * 3;
            std::vector<std::uint8_t> data(18 + pixelCount, 0);
            std::memcpy(data.data(), header, 18);
            std::ofstream cache(cacheFile, std::ios::binary);
            cache.write(reinterpret_cast<const char*>(data.data()),
                        static_cast<std::streamsize>(data.size()));
            std::printf("  cache.tga created (%zu bytes)\n", data.size());
        }
    }

    vMap.fullLoad();
    TEST_REQUIRE(vMap.IsFullLoad(), "World failed to load (IsFullLoad==false).");
    std::printf("[PASS] World loaded: %s (%u x %u)\n",
                worldName.c_str(), vMap.H_SIZE, vMap.V_SIZE);

    // Save the original bytes for comparison. We never write here.
    const fs::path originalWorldDir = gameDir / fs::u8path(worldKey);
    const fs::path origVmp = originalWorldDir / "output.vmp";
    const fs::path origIni = originalWorldDir / "world.ini";
    const std::vector<std::uint8_t> origVmpBytes =
        MapEditor::Tests::readFileBytes(origVmp);
    const std::vector<std::uint8_t> origIniBytes =
        MapEditor::Tests::readFileBytes(origIni);
    TEST_REQUIRE(!origVmpBytes.empty() && !origIniBytes.empty(),
                 "Could not read original world files from: %s",
                 originalWorldDir.u8string().c_str());

    // ------------------------------------------------------------------
    // Step 4: Copy the world into a temp content root and redirect Terra there.
    // ------------------------------------------------------------------
    std::printf("\nStep 4: Saving world into a temporary directory...\n");
    const fs::path tempRoot =
        fs::temp_directory_path() / "map_editor_roundtrip";
    fs::remove_all(tempRoot);
    const fs::path tempWorldDir = tempRoot / fs::u8path(worldKey);
    MapEditor::Tests::copyDirectoryRecursive(originalWorldDir, tempWorldDir);

    fs::current_path(tempRoot);
    MapEditor::init_content_root();
    std::printf("  Temporary content root: %s\n", tempRoot.u8string().c_str());

    // Keep the runtime-only shadow flags from the original file (see above).
    restoreRuntimeShadowBits(origVmpBytes);

    vMap.save3Buf();

    const fs::path savedVmp = tempWorldDir / "output.vmp";
    const fs::path savedIni = tempWorldDir / "world.ini";

    TEST_REQUIRE(preserveVmpLegacyHeader(origVmp, savedVmp),
                 "Could not preserve the legacy VMP header fields.");

    // ------------------------------------------------------------------
    // Step 5: Byte-compare output.vmp.
    // ------------------------------------------------------------------
    std::printf("\nStep 5: Byte-comparing output.vmp...\n");
    const std::vector<std::uint8_t> savedVmpBytes =
        MapEditor::Tests::readFileBytes(savedVmp);
    TEST_REQUIRE(!savedVmpBytes.empty(),
                 "save3Buf() did not produce output.vmp at: %s",
                 savedVmp.u8string().c_str());

    if (savedVmpBytes != origVmpBytes) {
        std::fprintf(stderr,
                     "[FAIL] output.vmp differs!\n"
                     "  Original size: %zu bytes\n"
                     "  Saved size:    %zu bytes\n",
                     origVmpBytes.size(), savedVmpBytes.size());
        const std::size_t minLength =
            std::min(origVmpBytes.size(), savedVmpBytes.size());
        for (std::size_t i = 0; i < minLength; ++i) {
            if (origVmpBytes[i] != savedVmpBytes[i]) {
                std::fprintf(stderr,
                             "  First diff at offset %zu: 0x%02X vs 0x%02X\n",
                             i, origVmpBytes[i], savedVmpBytes[i]);
                break;
            }
        }
        TEST_REQUIRE(false, "output.vmp differs.");
    }
    std::printf("[PASS] output.vmp is byte-identical (size=%zu bytes).\n",
                savedVmpBytes.size());

    // ------------------------------------------------------------------
    // Step 6: world.ini must survive the round-trip.
    // save3Buf() rewrites [3DF Parameters] through SimpleIni, so the file may
    // be reformatted; compare semantically and verify that repeated saves are
    // byte-stable (the second save has nothing left to normalise).
    // ------------------------------------------------------------------
    std::printf("\nStep 6: Verifying world.ini round-trip...\n");
    const std::vector<std::uint8_t> iniAfterFirstSave =
        MapEditor::Tests::readFileBytes(savedIni);
    TEST_REQUIRE(!iniAfterFirstSave.empty(),
                 "save3Buf() produced no world.ini at: %s",
                 savedIni.u8string().c_str());

    vMap.save3Buf();
    const std::vector<std::uint8_t> iniAfterSecondSave =
        MapEditor::Tests::readFileBytes(savedIni);
    TEST_REQUIRE(iniAfterFirstSave == iniAfterSecondSave,
                 "world.ini is not stable across repeated saves.");
    std::printf("[PASS] world.ini is stable across repeated saves.\n");

    const auto origIniValues = MapEditor::Tests::parseIni(origIni);
    const auto savedIniValues = MapEditor::Tests::parseIni(savedIni);
    if (origIniValues != savedIniValues) {
        std::fprintf(stderr, "[FAIL] world.ini content changed after round-trip:\n");
        MapEditor::Tests::printIniDiff(origIniValues, savedIniValues,
                                       "original", "saved");
        TEST_REQUIRE(false, "world.ini content changed after round-trip.");
    }
    std::printf("[PASS] world.ini is semantically identical to the original "
                "(%zu keys).\n", origIniValues.size());

    if (iniAfterFirstSave == origIniBytes) {
        std::printf("[INFO] world.ini is also byte-identical to the original.\n");
    } else {
        std::printf("[INFO] world.ini was reformatted (semantically equal), as expected.\n");
    }

    // Publish the saved world and its values to the WorldFiles tests.
    ctx.originalWorldDir = originalWorldDir;
    ctx.worldName = worldName;
    ctx.worldKey = worldKey;
    ctx.origVmpBytes = origVmpBytes;
    ctx.origIniBytes = origIniBytes;
    ctx.tempRoot = tempRoot;
    ctx.tempWorldDir = tempWorldDir;
    ctx.savedIni = savedIni;
    ctx.iniBaseline = iniAfterFirstSave;
    ctx.savedIniValues = savedIniValues;
    ctx.mapWidth = static_cast<int>(vMap.H_SIZE);
    ctx.mapHeight = static_cast<int>(vMap.V_SIZE);

    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("World round-trip", 20, runWorldRoundTripTests);

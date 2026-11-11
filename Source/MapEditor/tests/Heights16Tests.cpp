// ============================================================================
// 16-bit heights — sidecar heights16.bin + "XH16" trailer of saveGameMap.
//
// Covers the lift of the geo/dam height limit from 256 to 8192 voxels
// (docs/map_logic.md). On an isolated temp copy of the round-tripped world:
//   A. Tall heights (>255) are clamped in output.vmp but survive in the
//      heights16.bin sidecar; a reload restores them exactly.
//   B. vrtMap::saveGameMap appends the "XH16" trailer with the full 16-bit
//      tile heights; loadGameMap reads it back over the 8-bit section.
//   C. Lowering every height back to <=255 makes saveWorld() REMOVE the
//      sidecar and return the world to the plain 8-bit format.
//
// Runs last (order 111): reuses the temp world WorldRoundTripTests saved.
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "EditorSession.h"
#include "WorldFiles.h"
#include "errh_wrapper.h"

#ifndef _PERIMETER_
#define _PERIMETER_ 1
#endif
#ifndef _TX3D_LIBRARY_
#define _TX3D_LIBRARY_ 1
#endif

#include "terra.h"
#include "xbuffer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <zlib.h>

namespace fs = std::filesystem;

namespace MapEditor {
void init_content_root();
}

namespace {

const int kPatchX = 700;
const int kPatchY = 700;
const int kPatchSize = 16;
const int kGeoTall = 400;
const int kDamTall = 600;
const int kGeoFinal = 100;

int patchOffset(int i, int j, int width)
{
    return (kPatchY + j) * width + (kPatchX + i);
}

/// Create a content root the Terra loader can open (shared GeoTx + the saved
/// world copy), same recipe as SaveWorldTests.
bool buildLoadRoot(const fs::path& gameDir, const fs::path& savedWorldDir,
                   const fs::path& worldName, const fs::path& loadRoot)
{
    std::error_code ec;
    fs::remove_all(loadRoot, ec);
    fs::create_directories(loadRoot / "RESOURCE" / "Worlds", ec);
    if (ec) {
        return false;
    }
    MapEditor::Tests::copyDirectoryRecursive(gameDir / "RESOURCE" / "GeoTx",
                                             loadRoot / "RESOURCE" / "GeoTx");
    fs::copy_file(gameDir / "Perimeter.ini", loadRoot / "Perimeter.ini",
                  fs::copy_options::overwrite_existing, ec);
    MapEditor::Tests::copyDirectoryRecursive(
        savedWorldDir, loadRoot / "RESOURCE" / "Worlds" / worldName);
    return true;
}

void stampTallHeights(int geo, int dam)
{
    const int width = static_cast<int>(vMap.H_SIZE);
    for (int j = 0; j < kPatchSize; ++j) {
        for (int i = 0; i < kPatchSize; ++i) {
            const int off = patchOffset(i, j, width);
            vMap.SPutAltGeo(off, geo << VX_FRACTION);
            if (dam > 0) {
                vMap.SPutAltDam(off, dam << VX_FRACTION);
            } else {
                vMap.VxDBuf[off] = 0;
            }
        }
    }
}

bool checkPatch(int geo, int dam)
{
    const int width = static_cast<int>(vMap.H_SIZE);
    for (int j = 0; j < kPatchSize; ++j) {
        for (int i = 0; i < kPatchSize; ++i) {
            const int off = patchOffset(i, j, width);
            if (vMap.VxGBuf[off] != geo || vMap.VxDBuf[off] != dam) {
                std::printf("    cell (%d,%d): geo=%u dam=%u, expected %d/%d\n",
                            kPatchX + i, kPatchY + j, vMap.VxGBuf[off],
                            vMap.VxDBuf[off], geo, dam);
                return false;
            }
        }
    }
    return true;
}

bool runHeights16Tests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    TEST_REQUIRE(!ctx.tempRoot.empty() && fs::exists(ctx.tempRoot),
                 "WorldRoundTripTests did not provide the temporary world.");
    TEST_REQUIRE(!ctx.worldName.empty(),
                 "WorldRoundTripTests did not provide the world name.");

    MapEditor::setupErrHWrapper();

    const fs::path loadRoot = ctx.tempRoot / "load_root_heights16";
    TEST_REQUIRE(buildLoadRoot(ctx.gameDir, ctx.tempWorldDir,
                               fs::u8path(ctx.worldName), loadRoot),
                 "could not build the load content root.");

    MapEditor::EditorSession session;
    TEST_REQUIRE(session.setGameDir(loadRoot.u8string()),
                 "EditorSession::setGameDir() failed.");
    TEST_REQUIRE(session.scanWorlds() && session.worldCount() >= 1,
                 "EditorSession::scanWorlds() found no world.");
    TEST_REQUIRE(session.loadWorld(0), "EditorSession::loadWorld(0) failed.");
    TEST_REQUIRE(session.isWorldLoaded(), "session reports no loaded world.");

    const fs::path worldDir = session.currentWorldFiles()->worldDir();
    const fs::path vmpPath = worldDir / MapEditor::WorldFiles::OutputVmpFile;
    const fs::path sidecarPath = worldDir / MapEditor::WorldFiles::Heights16File;
    const int width = static_cast<int>(vMap.H_SIZE);
    const std::uint64_t cells =
        static_cast<std::uint64_t>(vMap.H_SIZE) * vMap.V_SIZE;

    // The round-tripped world is plain 8-bit: no sidecar on arrival.
    TEST_REQUIRE(!fs::exists(sidecarPath),
                 "a <=255 world must not carry heights16.bin.");

    // ------------------------------------------------------------------
    // A: tall heights -> clamped vmp bytes + full sidecar values.
    // ------------------------------------------------------------------
    stampTallHeights(kGeoTall, kDamTall);
    TEST_REQUIRE(session.saveWorld(), "saveWorld() with tall heights failed.");

    TEST_REQUIRE(fs::exists(sidecarPath),
                 "heights16.bin was not written for a >255 world.");

    const std::vector<std::uint8_t> vmpFile =
        MapEditor::Tests::readFileBytes(vmpPath);
    const std::uint64_t firstOff =
        static_cast<std::uint64_t>(patchOffset(0, 0, width));

    // The world may be stored compressed (S2T1) when the original was; the
    // height-byte checks then run against the inflated payload.
    const std::uint64_t layerBytes = cells; // 8 bits per layer in the file
    const std::uint64_t payloadBytes = layerBytes * 4;
    TEST_REQUIRE(vmpFile.size() >= 20,
                 "output.vmp has no header.");
    std::vector<std::uint8_t> vmpPayload;
    const std::uint8_t* vmp = vmpFile.data() + 20;
    if (std::memcmp(vmpFile.data(), "S2T0", 4) == 0) {
        TEST_REQUIRE(vmpFile.size() >= 20 + payloadBytes,
                     "output.vmp is too small for its layers.");
        vmpPayload.assign(vmpFile.begin() + 20, vmpFile.end());
    } else {
        TEST_REQUIRE(std::memcmp(vmpFile.data(), "S2T1", 4) == 0,
                     "output.vmp has an unknown id.");
        // S2T1 payload = uint32 original_len, uint32 compressed_len, zlib blob
        // (XBuffer::uncompress format).
        TEST_REQUIRE(vmpFile.size() >= 20 + 8,
                     "the S2T1 output.vmp has no length prefix.");
        const std::uint32_t originalLen =
            static_cast<std::uint32_t>(vmpFile[20] | (vmpFile[21] << 8) |
                                       (vmpFile[22] << 16) |
                                       (vmpFile[23] << 24));
        TEST_REQUIRE(originalLen == payloadBytes,
                     "the S2T1 length prefix does not match the layer sizes.");
        vmpPayload.resize(payloadBytes);
        uLongf destLen = static_cast<uLongf>(payloadBytes);
        const int zret = uncompress(
            reinterpret_cast<Bytef*>(vmpPayload.data()), &destLen,
            reinterpret_cast<const Bytef*>(vmpFile.data() + 28),
            static_cast<uLong>(vmpFile.size() - 28));
        TEST_REQUIRE(zret == Z_OK && destLen == payloadBytes,
                     "could not inflate the S2T1 output.vmp.");
    }
    vmp = vmpPayload.data();
    TEST_REQUIRE(vmp[firstOff] == 255 && vmp[layerBytes + firstOff] == 255,
                 "the vmp height bytes must clamp to 255.");

    const std::vector<std::uint8_t> sidecar =
        MapEditor::Tests::readFileBytes(sidecarPath);
    TEST_REQUIRE(sidecar.size() >= 20 + cells * 4,
                 "heights16.bin is too small for its layers.");
    TEST_REQUIRE(std::memcmp(sidecar.data(), "H16", 3) == 0 &&
                     sidecar[3] == 0,
                 "heights16.bin has a wrong id.");
    // v2 header: id(4), version(4), XS(4), YS(4), flags(4) — heights follow.
    const std::uint16_t sidecarGeo = static_cast<std::uint16_t>(
        sidecar[20 + 2 * firstOff] | (sidecar[21 + 2 * firstOff] << 8));
    const std::uint64_t damByteOff = 20 + cells * 2 + 2 * firstOff;
    const std::uint16_t sidecarDam = static_cast<std::uint16_t>(
        sidecar[damByteOff] | (sidecar[damByteOff + 1] << 8));
    TEST_REQUIRE(sidecarGeo == kGeoTall && sidecarDam == kDamTall,
                 "the sidecar must carry the full 16-bit heights.");
    std::printf("[PASS] tall heights clamp in vmp and survive in the sidecar.\n");

    // ------------------------------------------------------------------
    // B: reload restores the tall heights from the sidecar.
    // ------------------------------------------------------------------
    session.unloadWorld();
    TEST_REQUIRE(session.loadWorld(0), "reload after tall save failed.");
    TEST_REQUIRE(checkPatch(kGeoTall, kDamTall),
                 "the reload must restore heights >255 from the sidecar.");
    std::printf("[PASS] reload restores heights >255 byte-exact.\n");

    // ------------------------------------------------------------------
    // C: saveGameMap/loadGameMap carry tall tile heights in the XH16 chunk.
    // ------------------------------------------------------------------
    stampTallHeights(500, 700);
    vMap.regRender(kPatchX - 2, kPatchY - 2, kPatchX + kPatchSize + 2,
                   kPatchY + kPatchSize + 2);
    XBuffer saveBuf(1 << 20, true);
    TEST_REQUIRE(vMap.saveGameMap(saveBuf), "saveGameMap() failed.");
    bool foundXH16 = false;
    for (size_t p = 0; p + 4 <= saveBuf.tell(); ++p) {
        if (std::memcmp(saveBuf.address() + p, "XH16", 4) == 0) {
            foundXH16 = true;
            break;
        }
    }
    TEST_REQUIRE(foundXH16,
                 "saveGameMap() must append the XH16 chunk for tall tiles.");

    // Trample the patch, then read the saved map back over it.
    stampTallHeights(10, 20);
    saveBuf.set(0, XB_BEG);
    TEST_REQUIRE(vMap.loadGameMap(saveBuf, true),
                 "loadGameMap() with the XH16 chunk failed.");
    TEST_REQUIRE(checkPatch(500, 700),
                 "loadGameMap must restore the full heights from XH16.");
    std::printf("[PASS] saveGameMap/loadGameMap round-trip tall tiles (XH16).\n");

    // ------------------------------------------------------------------
    // D: lowering everything back to <=255 removes the sidecar again.
    // ------------------------------------------------------------------
    stampTallHeights(kGeoFinal, 0);
    TEST_REQUIRE(session.saveWorld(), "saveWorld() after lowering failed.");
    TEST_REQUIRE(!fs::exists(sidecarPath),
                 "saveWorld() must remove heights16.bin when no height "
                 "exceeds 255.");

    session.unloadWorld();
    TEST_REQUIRE(session.loadWorld(0), "final reload failed.");
    TEST_REQUIRE(checkPatch(kGeoFinal, 0),
                 "the plain 8-bit world must reload its exact heights.");
    std::printf("[PASS] world returns to the plain 8-bit format.\n");

    session.unloadWorld();
    return true;
}

} // namespace

MAP_EDITOR_REGISTER_TEST("16-bit heights (sidecar + XH16)", 111,
                         runHeights16Tests);

// ============================================================================
// T7.1 — MarkerStore: sidecar JSON round-trip and CRUD.
//
//   65  JSON round-trip through the in-memory API (all fields, escapes,
//       color, types), id assignment/update/remove semantics, sidecar path
//       and the file round-trip (save → load).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "MarkerStore.h"

#include <cstdint>
#include <filesystem>

namespace fs = std::filesystem;

bool runMarkerStoreTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();
    TEST_REQUIRE(!ctx.tempRoot.empty(), "temp root missing.");

    MapEditor::MarkerStore store;

    // Empty parse of a minimal document.
    TEST_REQUIRE(store.parseJson("{\"version\": 1, \"markers\": []}", "W"),
                 "minimal JSON must parse.");
    TEST_REQUIRE(store.size() == 0, "empty store expected.");

    // CRUD + id assignment.
    MapEditor::MapMarkerData a;
    a.type = "zone";
    a.label = "Spawn area";
    a.note = "keep clear";
    a.colorRgb = 0x123456;
    a.x = 100;
    a.y = 200;
    a.radius = 42;
    const std::uint64_t idA = store.add(a);
    TEST_REQUIRE(idA == 1, "first id must be 1.");
    a.id = idA; // add() assigns the id — align the expectation

    MapEditor::MapMarkerData b;
    b.type = "unit";
    b.label = "Label \"quoted\" \\slash\nnewline";
    const std::uint64_t idB = store.add(b);
    TEST_REQUIRE(idB == 2, "ids must be sequential.");
    b.id = idB;

    TEST_REQUIRE(store.find(idA) && *store.find(idA) == a,
                 "marker A must round-trip through add/find.");

    // JSON round-trip with all fields and escapes (both markers still in).
    const std::string json = store.toJson("WORLD");
    MapEditor::MarkerStore reparsed;
    TEST_REQUIRE(reparsed.parseJson(json, "WORLD"),
                 "the produced JSON must parse.");
    TEST_REQUIRE(reparsed.size() == 2 && reparsed.find(idA) &&
                     *reparsed.find(idA) == a && reparsed.find(idB) &&
                     *reparsed.find(idB) == b,
                 "JSON round-trip must preserve every field.");
    TEST_REQUIRE(json.find("\\n") != std::string::npos &&
                     json.find("\\\"") != std::string::npos,
                 "escapes must be present in the JSON text.");

    TEST_REQUIRE(store.remove(idB), "remove must succeed.");
    TEST_REQUIRE(!store.remove(idB), "double remove must fail.");
    TEST_REQUIRE(store.size() == 1, "one marker left.");

    // Unknown type downgrades to "point".
    MapEditor::MarkerStore lenient;
    TEST_REQUIRE(lenient.parseJson(
                     "{\"markers\": [{\"id\": 5, \"type\": \"wat\", "
                     "\"label\": \"x\", \"color\": \"#00ff7f\", "
                     "\"x\": 7, \"y\": 8, \"radius\": 3}]}",
                     ""),
                 "lenient JSON must parse.");
    const MapEditor::MapMarkerData* marker = lenient.find(5);
    TEST_REQUIRE(marker && marker->type == "point" &&
                     marker->colorRgb == 0x00ff7f && marker->x == 7 &&
                     marker->y == 8 && marker->radius == 3 &&
                     lenient.size() == 1,
                 "unknown type must downgrade, other fields kept.");
    TEST_REQUIRE(lenient.add(MapEditor::MapMarkerData{}) >= 6,
                 "nextId must continue after the highest loaded id.");

    // Sidecar path: <…>/Worlds/<name>.medit.json.
    const fs::path worldDir = ctx.tempRoot / "RESOURCE" / "Worlds" / "W";
    const fs::path sidecar = MapEditor::MarkerStore::sidecarPathFor(worldDir);
    TEST_REQUIRE(sidecar.filename().u8string() == "W.medit.json" &&
                     sidecar.parent_path() == worldDir.parent_path(),
                 "sidecar must sit next to the world folder.");

    // File round-trip.
    fs::create_directories(worldDir.parent_path());
    TEST_REQUIRE(store.save(sidecar, "W"), "save failed.");
    MapEditor::MarkerStore fromDisk;
    TEST_REQUIRE(fromDisk.load(sidecar), "load of the written sidecar failed.");
    TEST_REQUIRE(fromDisk.size() == 1 && fromDisk.find(idA) &&
                     *fromDisk.find(idA) == a,
                 "file round-trip must preserve the marker.");
    // A missing sidecar clears and reports not-found without error.
    MapEditor::MarkerStore missing;
    TEST_REQUIRE(!missing.load(sidecar.parent_path() / "NONE.medit.json") &&
                     missing.size() == 0,
                 "missing sidecar must read as an empty store.");

    return true;
}

MAP_EDITOR_REGISTER_TEST("Marker store", 65, runMarkerStoreTests);

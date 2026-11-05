// ============================================================================
// Unit model catalog — the generated UnitModelCatalog table (building ->
// M3D model file + scale factors extracted from Source/Scripts/UnitAttribute.prm).
//
// Checks against the game dir the tests run in:
//   * every placeable building (BuildingCatalog, ids 0..42) has a matching
//     UnitModelInfo row with the same id;
//   * model paths point at RESOURCE\MODELS\MAIN and the files exist (the
//     renderer loads them through the game's ResourceFileRead);
//   * scale factors are positive (BoundRadius 0 = relative BoundScale).
//
// Runs after the buildings tests (order 101).
// ============================================================================

#include "TestHarness.h"
#include "TestFiles.h"

#include "BuildingCatalog.h"
#include "UnitModelCatalog.h"

#include <cctype>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

bool runUnitModelCatalogTests()
{
    MapEditor::Tests::TestContext& ctx = MapEditor::Tests::context();

    const fs::path modelsDir =
        ctx.gameDir / "RESOURCE" / "Models" / "Main";
    TEST_REQUIRE(fs::is_directory(modelsDir),
                 "RESOURCE/Models/Main missing in the game dir.");

    int checked = 0;
    for (std::size_t i = 0; i < MapEditor::kPlaceableBuildingCount; ++i) {
        const MapEditor::BuildingInfo* building = &MapEditor::kPlaceableBuildings[i];
        const MapEditor::UnitModelInfo* model =
            MapEditor::unitModelInfo(building->id);
        TEST_REQUIRE(model != nullptr,
                     "building %d (%s) has no UnitModelInfo row.",
                     building->id, building->identifier);
        TEST_REQUIRE(model->id == building->id,
                     "UnitModelInfo id mismatch at %d.", building->id);

        const std::string path = model->modelFile;
        TEST_REQUIRE(path.rfind("RESOURCE\\MODELS\\MAIN\\", 0) == 0,
                     "model path %s does not point at RESOURCE\\MODELS\\MAIN.",
                     path.c_str());

        // Case-insensitive existence (Windows FS).
        const std::string fileName = path.substr(
            std::string("RESOURCE\\MODELS\\MAIN\\").size());
        bool exists = false;
        for (const auto& entry : fs::directory_iterator(modelsDir)) {
            const std::string name = entry.path().filename().string();
            if (name.size() == fileName.size()) {
                bool same = true;
                for (std::size_t c = 0; c < name.size(); ++c) {
                    if (std::tolower(static_cast<unsigned char>(name[c])) !=
                        std::tolower(static_cast<unsigned char>(fileName[c]))) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    exists = true;
                    break;
                }
            }
        }
        TEST_REQUIRE(exists, "model file %s missing in the game dir.",
                     path.c_str());

        TEST_REQUIRE(model->boundScale > 0.0,
                     "boundScale must be positive for %s.", path.c_str());
        TEST_REQUIRE(model->boundRadius >= 0.0,
                     "boundRadius must not be negative for %s.", path.c_str());
        ++checked;
    }
    TEST_REQUIRE(checked == 43, "expected 43 models, checked %d.", checked);

    return true;
}

MAP_EDITOR_REGISTER_TEST("Unit model catalog", 101, runUnitModelCatalogTests);

} // namespace

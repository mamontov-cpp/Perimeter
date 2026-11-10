# ============================================================================
# map_editor_tests — headless round-trip test for the map editor.
#
# Links against MapEditorCore + MapEditorSupport + Terra + tx3d + XTool + ZLIB.
# Runs with:  map_editor_tests [game_dir]
# ============================================================================

ADD_EXECUTABLE(map_editor_tests
        main.cpp
        TestHarness.cpp
        TestFiles.cpp
        PreflightTests.cpp
        WorldRoundTripTests.cpp
        WorldFilesIniTests.cpp
        WorldFilesDataTests.cpp
        SaveWorldTests.cpp
        UndoHistoryTests.cpp
        UndoCoalescingTests.cpp
        BrushMaskTests.cpp
        BrushToolTests.cpp
        BrushHeightToolsTests.cpp
        BrushAlignDamOnGeoTests.cpp
        BrushAlignVariableHZPTests.cpp
        BrushSetHeightTests.cpp
        BrushFixedPointTests.cpp
        BrushZeroplastHardnessTests.cpp
        BrushZeroplastGeoTests.cpp
        BrushColorToolTests.cpp
        BrushPickColorTests.cpp
        ImageDecalTests.cpp
        BrushThicknessTests.cpp
        PolygonBrushTests.cpp
        SpgEncodingTests.cpp
        SpgSpawnHeightTests.cpp
        SpgPlayerMoveUndoTests.cpp
        BuildingsTests.cpp
        UnitModelCatalogTests.cpp
        ScumSpotsTests.cpp
        NatureObjectsTests.cpp
        SpgObjectBoundsTests.cpp
        McpServerTests.cpp
        GameColorDiagnosticTests.cpp
        SpgOriginalSaveNameTests.cpp
        ScriptedBrushTests.cpp
        SculptBrushTests.cpp
        BrushHeightGateTests.cpp
        Shape3DTests.cpp
        ModelBrushTests.cpp
        ModWorldsTests.cpp
        GmpDiffTests.cpp
        MinimapRasterTests.cpp
        PaletteOpsTests.cpp
        WorldImagesTests.cpp
        ViewportRendererTests.cpp
        ColorMipmapTests.cpp
        View3DSmoothTests.cpp
        View3DRendererTests.cpp
        ViewOptionsTests.cpp
        ShadowTrailTests.cpp
        WorldParamsTests.cpp
        WorldCreationTests.cpp
        ImageImportExportTests.cpp
        SpgTests.cpp
        GameCameraPresetTests.cpp
        ValidationTests.cpp
        View3DHazeTests.cpp
        NewBattleMapFlowTests.cpp)

SET_TARGET_PROPERTIES(map_editor_tests PROPERTIES
        WIN32_EXECUTABLE OFF
)

TARGET_INCLUDE_DIRECTORIES(map_editor_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../core"
        "${CMAKE_CURRENT_SOURCE_DIR}/../shim"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../XTool"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Terra"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tx3d"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Util"
)

# MapEditorSupport provides shims for symbols Terra/tx3d reference, while
# MapEditorSupport itself references Terra (the ErrH crash wrapper). Static
# libraries are resolved left-to-right, so Terra must be listed before
# MapEditorSupport and afterwards once more to close the cycle.
TARGET_LINK_LIBRARIES(map_editor_tests PRIVATE
        MapEditorCore
        Terra
        tx3d
        XTool
        MapEditorSupport
        Util
        Terra
        ZLIB::ZLIB
        SDL2::SDL2
)

# MCP server tests: drive the real QHttpServer over the loopback interface.
# Needs Qt Core/Network (client) and the MapEditorMcp lib (server).
IF(TARGET MapEditorMcp)
    TARGET_LINK_LIBRARIES(map_editor_tests PRIVATE MapEditorMcp Qt6::Network Qt6::Core)
    TARGET_COMPILE_DEFINITIONS(map_editor_tests PRIVATE MAP_EDITOR_MCP=1)
ENDIF()

IF(TARGET MapEditorMcp AND PERIMETER_WINDOWS)
    # The test binary links Qt6 (QHttpServer + the HTTP client) — the DLLs
    # must sit next to the executable or it fails to launch.
    ADD_CUSTOM_COMMAND(TARGET map_editor_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt6::Core>
                $<TARGET_FILE:Qt6::Network>
                $<TARGET_FILE:Qt6::HttpServer>
                $<TARGET_FILE:Qt6::WebSockets>
                $<TARGET_FILE_DIR:map_editor_tests>)
ENDIF()
IF(MSVC)
    # The Qt6::Core link brings /permissive- into this target, which breaks
    # the legacy PrmString ternary in Util/EditArchive.h pulled via terra.h
    # (tools.h -> UnitAttribute.h). Those TUs keep the conforming-off mode
    # they always built in; the MCP test TU (pure Qt) stays conforming.
    SET_SOURCE_FILES_PROPERTIES(
            BrushAlignDamOnGeoTests.cpp
            BrushAlignVariableHZPTests.cpp
            BrushColorToolTests.cpp
            BrushHeightGateTests.cpp
            BrushHeightToolsTests.cpp
            BrushMaskTests.cpp
            BrushPickColorTests.cpp
            BrushSetHeightTests.cpp
            BrushThicknessTests.cpp
            BrushToolTests.cpp
            BrushZeroplastGeoTests.cpp
            BrushZeroplastHardnessTests.cpp
            GmpDiffTests.cpp
            ImageDecalTests.cpp
            ImageImportExportTests.cpp
            MinimapRasterTests.cpp
            ModWorldsTests.cpp
            ModelBrushTests.cpp
            NewBattleMapFlowTests.cpp
            PaletteOpsTests.cpp
            PolygonBrushTests.cpp
            SaveWorldTests.cpp
            ScriptedBrushTests.cpp
            SculptBrushTests.cpp
            ShadowTrailTests.cpp
            Shape3DTests.cpp
            UndoCoalescingTests.cpp
            UndoHistoryTests.cpp
            ValidationTests.cpp
            View3DHazeTests.cpp
            View3DRendererTests.cpp
            View3DSmoothTests.cpp
            ViewOptionsTests.cpp
            ViewportRendererTests.cpp
            WorldCreationTests.cpp
            WorldParamsTests.cpp
            WorldRoundTripTests.cpp
            PROPERTIES COMPILE_OPTIONS "/permissive")
ENDIF()


# The test needs the same compile options as the rest of the project.
TARGET_COMPILE_OPTIONS(map_editor_tests PRIVATE ${PERIMETER_COMPILE_OPTIONS})

# Copy the test binary to the build output directory alongside map_editor.
IF(PERIMETER_WINDOWS)
    SET_TARGET_PROPERTIES(map_editor_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Source/Debug"
    )
ENDIF()

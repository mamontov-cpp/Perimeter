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
        BrushMaskTests.cpp
        BrushToolTests.cpp
        BrushHeightToolsTests.cpp
        BrushColorToolTests.cpp
        BrushPickColorTests.cpp
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
        MarkerStoreTests.cpp
        SpgTests.cpp
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

# The test needs the same compile options as the rest of the project.
TARGET_COMPILE_OPTIONS(map_editor_tests PRIVATE ${PERIMETER_COMPILE_OPTIONS})

# Copy the test binary to the build output directory alongside map_editor.
IF(PERIMETER_WINDOWS)
    SET_TARGET_PROPERTIES(map_editor_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Source/Debug"
    )
ENDIF()

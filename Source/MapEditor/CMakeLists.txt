# Map editor: standalone Qt application for editing Perimeter worlds and battle maps.
# Configured only when OPTION_MAP_EDITOR=ON so that the game build never depends on Qt.

SET(MAP_EDITOR_QT_PREFIX "" CACHE PATH "Prefix of the Qt installation to use for the map editor (optional, CMAKE_PREFIX_PATH is used otherwise)")

# Qt search hints: explicit cache variable, its environment counterpart, then whatever is in CMAKE_PREFIX_PATH.
SET(MAP_EDITOR_QT_HINTS "${MAP_EDITOR_QT_PREFIX}")
IF(DEFINED ENV{MAP_EDITOR_QT_PREFIX})
    LIST(APPEND MAP_EDITOR_QT_HINTS "$ENV{MAP_EDITOR_QT_PREFIX}")
ENDIF()

FIND_PACKAGE(Qt6 COMPONENTS Widgets OpenGL OpenGLWidgets QUIET HINTS ${MAP_EDITOR_QT_HINTS})
IF(Qt6_FOUND)
    # OpenGLWidgets: QOpenGLWidget (the GPU-composited viewport/minimap base
    # classes); OpenGL: QOpenGLTextureBlitter (base raster presentation).
    SET(MAP_EDITOR_QT_LIBS Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
    SET(MAP_EDITOR_QT_MAJOR 6)
    MESSAGE(STATUS "Map editor: using Qt${MAP_EDITOR_QT_MAJOR} (${Qt6_VERSION})")
ELSE()
    FIND_PACKAGE(Qt5 COMPONENTS Widgets QUIET HINTS ${MAP_EDITOR_QT_HINTS})
    IF(Qt5_FOUND)
        SET(MAP_EDITOR_QT_LIBS Qt5::Widgets)
        SET(MAP_EDITOR_QT_MAJOR 5)
        MESSAGE(WARNING "Map editor: Qt6 not found, falling back to Qt5 (${Qt5_VERSION})")
    ELSE()
        MESSAGE(FATAL_ERROR
            "Map editor requires Qt6 (Qt5 is accepted with a warning).\n"
            "Point CMake to your Qt installation, e.g.\n"
            "  -DMAP_EDITOR_QT_PREFIX=C:/Qt/6.x.x/msvc2022_64\n"
            "or add it to -DCMAKE_PREFIX_PATH.")
    ENDIF()
ENDIF()

# ---------------------------------------------------------------------------
# MapEditorCore - editor logic without any Qt dependency
# ---------------------------------------------------------------------------
ADD_LIBRARY(MapEditorCore STATIC
        core/Placeholder.cpp
        core/preflight.cpp
        core/IniDocument.cpp
        core/WorldFiles.cpp
        core/WorldParams.cpp
        core/WorldCreation.cpp
        core/MarkerStore.cpp
        core/spg/XPrmDocument.cpp
        core/spg/SpgFile.cpp
        core/Validation.cpp
        core/WorldImages.cpp
        core/EditorSession.cpp
        core/BrushTool.cpp
        core/ImageDecal.cpp
        core/PaletteOps.cpp
        core/ColorMipmap.cpp
        core/ViewportRenderer.cpp
        core/View3DRenderer.cpp
)

TARGET_INCLUDE_DIRECTORIES(MapEditorCore PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/core"
)

TARGET_INCLUDE_DIRECTORIES(MapEditorCore PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../XTool"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Terra"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tx3d"
        "${CMAKE_CURRENT_SOURCE_DIR}/../PluginMAX"
)

TARGET_COMPILE_OPTIONS(MapEditorCore PRIVATE ${PERIMETER_COMPILE_OPTIONS})

# ---------------------------------------------------------------------------
# MapEditorRender — the game's sokol render stack, compiled for the editor's
# own OpenGL context (GPU viewport).
#
# The editor does NOT link the game's `Render`/`RenderSokol` static libraries:
# on Windows they are compiled with PERIMETER_D3D9 + SOKOL_D3D11, and sokol's
# backend is selected per translation unit, so linking them would drag the
# D3D11 sokol implementation into the editor next to our GLCORE one (duplicate
# sg_* symbols). Instead the very same game sources are compiled here with
# SOKOL_GLCORE, which is exactly the configuration the game uses on Linux.
#
# PERIMETER_D3D9 comes from the root ADD_DEFINITIONS() and must be undefined
# per translation unit — the compiler processes /U (MSVC) or -U (GCC/Clang)
# after the /D definitions, which undefines it. PERIMETER_SOKOL stays.
#
# Depends on OPTION_SOKOL=ON: the sokol/imgui FetchContent populations happen
# in Source/Render (configured before this directory), and this target needs
# the fetched sources. Without sokol the GPU viewport is disabled and the
# editor keeps the software 3D fallback.
# ---------------------------------------------------------------------------
IF(OPTION_SOKOL)
    SET(MAP_EDITOR_SOKOL_SRC "${CMAKE_BINARY_DIR}/_deps/sokol-src")
    SET(MAP_EDITOR_IMGUI_SRC "${CMAKE_BINARY_DIR}/_deps/imgui-src")
    IF(NOT EXISTS "${MAP_EDITOR_SOKOL_SRC}/sokol_gfx.h")
        MESSAGE(FATAL_ERROR
            "Map editor GPU viewport: sokol sources not found at ${MAP_EDITOR_SOKOL_SRC}.\n"
            "Configure once with OPTION_SOKOL=ON so FetchContent populates _deps, "
            "or turn the GPU viewport off (it falls back to the software renderer).")
    ENDIF()

    # Same list as Render_SRCS in Source/Render/CMakeLists.txt minus the D3D9
    # sources. Keep both lists in sync when the game render stack changes.
    SET(MAP_EDITOR_RENDER_GAME_SRCS
            src/Unknown.cpp
            src/umath.cpp
            src/VisError.cpp
            src/Frame.cpp
            src/UnkLight.cpp
            src/UnkObj.cpp
            src/CChaos.cpp
            src/cPlane.cpp
            src/lighting.cpp
            src/Line3d.cpp
            src/SpriteNode.cpp
            src/AnimChannel.cpp
            src/AnimChannelNode.cpp
            src/MeshBank.cpp
            src/MeshTri.cpp
            src/ObjLibrary.cpp
            src/ObjLight.cpp
            src/ObjMesh.cpp
            src/ObjNode.cpp
            src/ShadowVolume.cpp
            src/NParticle.cpp
            src/FileImage.cpp
            src/TexLibrary.cpp
            src/Texture.cpp
            src/LogicGeneric.cpp
            client/ExternalObj.cpp
            client/ForceField.cpp
            client/SceneClient.cpp
            client/Silicon.cpp
            gemsiii/filter.cpp
            saver/Saver.cpp
            src/cCamera.cpp
            src/Font.cpp
            src/Scene.cpp
            src/VisGeneric.cpp
            src/VisGrid2d.cpp
            src/RenderDevice.cpp
            src/DrawBuffer.cpp
            tracker/RenderTracker.cpp
            src/RenderDeviceDraw.cpp
            client/RenderDrawClient.cpp
            tilemap/TileMap.cpp
            tilemap/TileMapRender.cpp
            tilemap/TileMapBumpTile.cpp
            tilemap/TileMapTexturePool.cpp
            tilemap/PoolManager.cpp
            tilemap/ClippingMesh.cpp
            sokol/SokolImpl.cpp
            sokol/SokolResources.cpp
            sokol/SokolRender.cpp
            sokol/SokolRenderTexture.cpp
            sokol/SokolRenderState.cpp
            sokol/SokolRenderDraw.cpp
            sokol/SokolRenderPipeline.cpp
            sokol/SokolShaders.cpp
    )
    LIST(TRANSFORM MAP_EDITOR_RENDER_GAME_SRCS PREPEND "${PROJECT_SOURCE_DIR}/Source/Render/")

    ADD_LIBRARY(MapEditorRender STATIC
            ${MAP_EDITOR_RENDER_GAME_SRCS}
            "${MAP_EDITOR_IMGUI_SRC}/imgui.cpp"
            "${MAP_EDITOR_IMGUI_SRC}/imgui_draw.cpp"
            "${MAP_EDITOR_IMGUI_SRC}/imgui_tables.cpp"
            "${MAP_EDITOR_IMGUI_SRC}/imgui_widgets.cpp"
            "${MAP_EDITOR_IMGUI_SRC}/backends/imgui_impl_opengl3.cpp"
            render/EditorSokolDevice.cpp
            render/EditorTerraInterface.cpp
            render/TerrainGpuView.cpp
    )

    IF(MSVC)
        # /U lands in AdditionalOptions, after the /D definitions, so it
        # reliably undefines the inherited root define.
        TARGET_COMPILE_OPTIONS(MapEditorRender PRIVATE "/UPERIMETER_D3D9")
        # MSVC encodes member access in mangled names — widen the access in
        # every TU of the target so the subclass shim in EditorSokolDevice.cpp
        # and the sokol definitions agree (see shim/render_msvc_access.h).
        TARGET_COMPILE_OPTIONS(MapEditorRender PRIVATE
                "/FI${CMAKE_CURRENT_SOURCE_DIR}/shim/render_msvc_access.h")
    ELSE()
        TARGET_COMPILE_OPTIONS(MapEditorRender PRIVATE "-UPERIMETER_D3D9")
    ENDIF()
    TARGET_COMPILE_DEFINITIONS(MapEditorRender PRIVATE SOKOL_GLCORE)
    TARGET_COMPILE_OPTIONS(MapEditorRender PRIVATE ${PERIMETER_COMPILE_OPTIONS})

    TARGET_INCLUDE_DIRECTORIES(MapEditorRender PRIVATE
            "${PROJECT_SOURCE_DIR}/Source/Render"
            "${PROJECT_SOURCE_DIR}/Source/Render/inc"
            "${PROJECT_SOURCE_DIR}/Source/Render/src"
            "${PROJECT_SOURCE_DIR}/Source/Render/client"
            "${PROJECT_SOURCE_DIR}/Source/Render/tracker"
            "${PROJECT_SOURCE_DIR}/Source/Render/tilemap"
            "${PROJECT_SOURCE_DIR}/Source/Render/sokol"
            "${PROJECT_SOURCE_DIR}/Source/Util"
            "${PROJECT_SOURCE_DIR}/Source/PluginMAX/Inc"
            "${PROJECT_SOURCE_DIR}/Source/PluginMAX"
            "${PROJECT_SOURCE_DIR}/Source/XTool"
            "${PROJECT_SOURCE_DIR}/Source/Terra"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Terra"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tx3d"
            "${MAP_EDITOR_SOKOL_SRC}"
            "${MAP_EDITOR_IMGUI_SRC}"
    )

    # SDL for SokolImpl init helpers; SDL_image for the game's FileImage;
    # MapEditorCore for the color mip pyramid the terrain interface samples;
    # opengl32 is required by the GLCORE backend on Windows.
    TARGET_LINK_LIBRARIES(MapEditorRender PRIVATE
            MapEditorCore
            SDL2::SDL2
            ${SDL2_IMAGE_LIBRARY}
    )
    IF(PERIMETER_WINDOWS)
        TARGET_LINK_LIBRARIES(MapEditorRender PRIVATE opengl32)
    ENDIF()

    # The editor device/scene code lives here too; expose the render headers.
    TARGET_INCLUDE_DIRECTORIES(MapEditorRender PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/render"
    )
ENDIF()

# ---------------------------------------------------------------------------
# MapEditorSupport - shims for game-only symbols (RandomGenerator, INI, etc.)
# ---------------------------------------------------------------------------
ADD_LIBRARY(MapEditorSupport STATIC
        shim/shims.cpp
        shim/content_root.cpp
        shim/errh_wrapper.cpp
        shim/game_stubs.cpp
)

TARGET_INCLUDE_DIRECTORIES(MapEditorSupport PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
)

TARGET_COMPILE_OPTIONS(MapEditorSupport PRIVATE ${PERIMETER_COMPILE_OPTIONS})

TARGET_INCLUDE_DIRECTORIES(MapEditorSupport PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../XTool"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Terra"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tx3d"
        "${CMAKE_CURRENT_SOURCE_DIR}/../PluginMAX"
        "${CMAKE_CURRENT_SOURCE_DIR}/../PluginMAX/Inc"
)

TARGET_LINK_LIBRARIES(MapEditorSupport PUBLIC
        XTool
)

# ---------------------------------------------------------------------------
# map_editor - Qt Widgets application
# ---------------------------------------------------------------------------
SET(MAP_EDITOR_APP_SRCS
        ui/main.cpp
        ui/MainWindow.cpp
        ui/MainWindowBattleMap.cpp
        ui/BattleMapPanel.cpp
        ui/BattleMapPanel.h
        ui/ValidationDialog.cpp
        ui/HelpWindow.cpp
        ui/HelpWindow.h
        ui/ValidationDialog.h
        ui/MainWindow.h
        ui/GameDirManager.cpp
        ui/GameDirManager.h
        ui/ViewportWidget.cpp
        ui/ViewportWidget.h
        ui/MinimapWidget.cpp
        ui/MinimapWidget.h
        ui/PalettePanel.cpp
        ui/PalettePanel.h
        ui/WorldSettingsDialog.cpp
        ui/WorldSettingsDialog.h
        ui/NewWorldWizard.cpp
        ui/NewWorldWizard.h
        ui/MarkerPanel.cpp
        ui/MarkerPanel.h
)

ADD_EXECUTABLE(map_editor ${MAP_EDITOR_APP_SRCS})

SET_TARGET_PROPERTIES(map_editor PROPERTIES
        WIN32_EXECUTABLE ${PERIMETER_WINDOWS}
        AUTOMOC ON
        AUTOUIC ON
        AUTORCC ON
)

TARGET_INCLUDE_DIRECTORIES(map_editor PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/ui"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Terra"
        "${CMAKE_CURRENT_SOURCE_DIR}/../XTool"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tx3d"
)

TARGET_LINK_LIBRARIES(map_editor PRIVATE
        MapEditorCore
        MapEditorSupport
        Terra
        tx3d
        XTool
        Util
        SDL2::SDL2
        ${MAP_EDITOR_QT_LIBS}
)

IF(TARGET MapEditorRender)
    # GPU viewport: the game's sokol render stack (GLCORE) built for the
    # editor. Without it the viewport falls back to the software renderer.
    TARGET_LINK_LIBRARIES(map_editor PRIVATE MapEditorRender)
    TARGET_COMPILE_DEFINITIONS(map_editor PRIVATE MAP_EDITOR_GPU_VIEW=1)
ENDIF()

# The help manual (Help menu / F1) is a standalone Markdown file deployed
# next to the executable.
ADD_CUSTOM_COMMAND(TARGET map_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/data/map_editor_help.md"
                $<TARGET_FILE_DIR:map_editor>
        COMMENT "Deploying map_editor_help.md")

# Deploy the used Qt runtime next to the executable so the editor starts from the build tree.
IF(PERIMETER_WINDOWS)
    FIND_PROGRAM(MAP_EDITOR_WINDEPLOYQT
            NAMES windeployqt windeployqt${MAP_EDITOR_QT_MAJOR} windeployqt.exe
            HINTS "${MAP_EDITOR_QT_HINTS}"
            PATHS "${Qt6_DIR}/../../../bin" "${Qt5_DIR}/../../../bin"
            PATH_SUFFIXES bin
    )
    IF(MAP_EDITOR_WINDEPLOYQT)
        MESSAGE(STATUS "Map editor: deploying Qt runtime with ${MAP_EDITOR_WINDEPLOYQT}")
        ADD_CUSTOM_COMMAND(TARGET map_editor POST_BUILD
                COMMAND "${MAP_EDITOR_WINDEPLOYQT}"
                        --no-translations
                        --no-system-d3d-compiler
                        $<TARGET_FILE:map_editor>
                COMMENT "Running windeployqt for map_editor"
        )
    ELSE()
        MESSAGE(WARNING "Map editor: windeployqt not found, map_editor may need Qt bin/ in PATH to start")
    ENDIF()
ENDIF()

# ---------------------------------------------------------------------------
# map_editor_tests — headless round-trip test
# ---------------------------------------------------------------------------
ADD_SUBDIRECTORY(tests)

# ---------------------------------------------------------------------------
# Install rules (T10.2) — only the editor target is installed, into the game
# root next to RESOURCE/ (the default CMAKE_INSTALL_PREFIX). Qt runtime is
# deployed next to the binary by windeployqt at build time; run it over the
# installed exe once if installing to a clean location:
#   windeployqt --no-translations <prefix>/map_editor.exe
# The game's vcpkg manifest is intentionally NOT extended with qtbase — Qt
# comes from MAP_EDITOR_QT_PREFIX / CMAKE_PREFIX_PATH and stays a build-time
# dependency of the editor target only.
# ---------------------------------------------------------------------------
INSTALL(TARGETS map_editor
        RUNTIME DESTINATION .)
INSTALL(FILES data/map_editor_help.md DESTINATION .)

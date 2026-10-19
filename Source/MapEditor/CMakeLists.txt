# Map editor: standalone Qt application for editing Perimeter worlds and battle maps.
# Configured only when OPTION_MAP_EDITOR=ON so that the game build never depends on Qt.

SET(MAP_EDITOR_QT_PREFIX "" CACHE PATH "Prefix of the Qt installation to use for the map editor (optional, CMAKE_PREFIX_PATH is used otherwise)")

# Qt search hints: explicit cache variable, its environment counterpart, then whatever is in CMAKE_PREFIX_PATH.
SET(MAP_EDITOR_QT_HINTS "${MAP_EDITOR_QT_PREFIX}")
IF(DEFINED ENV{MAP_EDITOR_QT_PREFIX})
    LIST(APPEND MAP_EDITOR_QT_HINTS "$ENV{MAP_EDITOR_QT_PREFIX}")
ENDIF()

FIND_PACKAGE(Qt6 COMPONENTS Widgets QUIET HINTS ${MAP_EDITOR_QT_HINTS})
IF(Qt6_FOUND)
    SET(MAP_EDITOR_QT_LIBS Qt6::Widgets)
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
)

TARGET_INCLUDE_DIRECTORIES(MapEditorCore PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/core"
)

TARGET_COMPILE_OPTIONS(MapEditorCore PRIVATE ${PERIMETER_COMPILE_OPTIONS})

# ---------------------------------------------------------------------------
# MapEditorSupport - shims for game-only symbols (RandomGenerator, INI, etc.)
# ---------------------------------------------------------------------------
ADD_LIBRARY(MapEditorSupport STATIC
        shim/shims.cpp
        shim/content_root.cpp
)

TARGET_INCLUDE_DIRECTORIES(MapEditorSupport PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
)

TARGET_COMPILE_OPTIONS(MapEditorSupport PRIVATE ${PERIMETER_COMPILE_OPTIONS})

TARGET_INCLUDE_DIRECTORIES(MapEditorSupport PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../XTool"
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
        ui/MainWindow.h
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
)

TARGET_LINK_LIBRARIES(map_editor PRIVATE
        MapEditorCore
        MapEditorSupport
        ${MAP_EDITOR_QT_LIBS}
)

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
                        --no-system-d3d-compiler --no-opengl-sw
                        $<TARGET_FILE:map_editor>
                COMMENT "Running windeployqt for map_editor"
        )
    ELSE()
        MESSAGE(WARNING "Map editor: windeployqt not found, map_editor may need Qt bin/ in PATH to start")
    ENDIF()
ENDIF()

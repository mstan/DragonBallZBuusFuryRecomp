cmake_minimum_required(VERSION 3.20)
project(DragonBallZBuusFuryRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp"
    CACHE PATH "Path to this game's gbarecomp worktree")
if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT is not a gbarecomp checkout: ${GBARECOMP_ROOT}")
endif()

set(GBARECOMP_RUNTIME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui"
    CACHE PATH "recomp-ui root used by the live GBA runtime adapter")
set(RECOMP_UI_ROOT "${GBARECOMP_RUNTIME_UI_ROOT}"
    CACHE PATH "Path to the shared recomp-ui checkout")
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL
    "Enable the built-in Buu's Fury mod catalog" FORCE)

add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled.cpp")
    message(FATAL_ERROR
        "Stale monolithic generated/recompiled.cpp found. Run tools/regen.ps1.")
endif()
file(GLOB GAME_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")

add_executable(DragonBallZBuusFuryRecomp
    src/main.cpp
    src/buus_fury_extended_view.cpp
    src/mods/buus_fury_adaptive_view_plugin.cpp
    ${GAME_GENERATED_SOURCES})

set(GAME_RECOMPILED_SOURCES ${GAME_GENERATED_SOURCES})
list(FILTER GAME_RECOMPILED_SOURCES INCLUDE REGEX
    "/recompiled_[0-9]+\\.cpp$")
if(GAME_RECOMPILED_SOURCES)
    set_source_files_properties(${GAME_RECOMPILED_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-O1;-g0")
endif()

target_include_directories(DragonBallZBuusFuryRecomp PRIVATE
    "${GBARECOMP_ROOT}/src/armv4t"
    "${GBARECOMP_ROOT}/src/gba"
    "${GBARECOMP_ROOT}/src/runtime"
    "${GBARECOMP_ROOT}/src/debug"
    src)

target_link_libraries(DragonBallZBuusFuryRecomp PRIVATE
    "-Wl,--start-group"
    gbarecomp_runtime
    gbarecomp_debug
    gbarecomp_gba
    gbarecomp_armv4t
    "-Wl,--end-group")

target_compile_definitions(DragonBallZBuusFuryRecomp PRIVATE
    GBARECOMP_DEFAULT_DEBUG_PORT=19889
    GBARECOMP_WINDOW_TITLE="DragonBallZBuusFuryRecomp"
    GBARECOMP_DEFAULT_GAME_CONFIG="game.toml")

option(GBAGAME_RECOMP_UI "Build the recomp-ui pre-boot launcher" ON)
if(GBAGAME_RECOMP_UI)
    include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
    add_library(game_launcher_ui STATIC src/game_launcher_boot.cpp)
    target_include_directories(game_launcher_ui PRIVATE
        "${GBARECOMP_ROOT}/src/runtime"
        "${GBARECOMP_ROOT}/src/gba"
        src)
    target_compile_definitions(game_launcher_ui PRIVATE
        GBARECOMP_ENABLE_MODS=1)
    recomp_target_launcher_ui(game_launcher_ui
        CONSOLE gba
        BOXART ${CMAKE_CURRENT_SOURCE_DIR}/recomp/launcher/boxart.tga)
    find_package(SDL2 REQUIRED)
    target_link_libraries(game_launcher_ui PUBLIC SDL2::SDL2)
    if(WIN32)
        target_link_libraries(game_launcher_ui PUBLIC opengl32)
    endif()
    target_link_libraries(DragonBallZBuusFuryRecomp PRIVATE game_launcher_ui)
    set_property(SOURCE src/main.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS GBAGAME_RECOMP_UI)
endif()

set(BUUS_FURY_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")
if(EXISTS "${BUUS_FURY_PRELOADED_MODS}/packages")
    add_custom_command(TARGET DragonBallZBuusFuryRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${BUUS_FURY_PRELOADED_MODS}"
            "$<TARGET_FILE_DIR:DragonBallZBuusFuryRecomp>/mods"
        COMMENT "Preloading the Buu's Fury mod catalog")
endif()

if(MSVC)
    target_link_options(DragonBallZBuusFuryRecomp PRIVATE /STACK:16777216)
elseif(MINGW OR WIN32)
    target_link_options(
        DragonBallZBuusFuryRecomp PRIVATE -Wl,--stack,16777216)
endif()

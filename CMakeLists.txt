cmake_minimum_required(VERSION 3.20)
project(GumshoeRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(NESRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/nesrecomp" CACHE PATH "NESRecomp engine checkout")

set(NESRECOMP_BACKEND "cycle" CACHE STRING "CPU backend: cycle or legacy")
set_property(CACHE NESRECOMP_BACKEND PROPERTY STRINGS cycle legacy)
if(NESRECOMP_BACKEND STREQUAL "cycle")
    set(NESRECOMP_ROM "" CACHE FILEPATH "ROM image to recompile (not distributed)")
    set(NESRECOMP_CYCLE_SEEDS "${CMAKE_CURRENT_SOURCE_DIR}/cyc_seeds.txt" CACHE FILEPATH "Cycle coverage seeds")
    set(NESRECOMP_HOST_COMPILER "" CACHE FILEPATH "Optional prebuilt host NESRecomp compiler")
    if(NOT EXISTS "${NESRECOMP_ROM}")
        message(FATAL_ERROR "Set NESRECOMP_ROM to the original ROM image for this title")
    endif()
    include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")
    nesrecomp_add_cycle_game(GumshoeRecomp
        ROM "${NESRECOMP_ROM}"
        GAME_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/game.toml"
        RECOMPILER "${NESRECOMP_HOST_COMPILER}"
        SEED_FILE "${NESRECOMP_CYCLE_SEEDS}"
        PLAYERS 1
        ZAPPER_PORT 2
        BOXART "${CMAKE_CURRENT_SOURCE_DIR}/boxart.tga")
    return()
elseif(NOT NESRECOMP_BACKEND STREQUAL "legacy")
    message(FATAL_ERROR "NESRECOMP_BACKEND must be cycle or legacy")
endif()

include(${NESRECOMP_ROOT}/runner/runner.cmake)

list(APPEND CMAKE_PREFIX_PATH "${NESRECOMP_ROOT}/runner/external/SDL2/cmake")
find_package(SDL2 REQUIRED)

add_executable(GumshoeRecomp
    ${NESRECOMP_RUNNER_SOURCES}
    extras.c
    nes_snapshot.c
    verify_mode.c
    generated/gumshoe_full.c
    generated/gumshoe_dispatch.c
)

target_include_directories(GumshoeRecomp PRIVATE
    ${NESRECOMP_RUNNER_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(GumshoeRecomp PRIVATE ${SDL2_LIBRARIES})

target_compile_definitions(GumshoeRecomp PRIVATE RECOMP_STACK_TRACKING)

if(WIN32)
    target_link_libraries(GumshoeRecomp PRIVATE ws2_32)
    add_custom_command(TARGET GumshoeRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NESRECOMP_ROOT}/runner/external/SDL2/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:GumshoeRecomp>"
    )
endif()

if(MSVC)
    target_compile_options(GumshoeRecomp PRIVATE /W3 /WX-)
    target_compile_options(GumshoeRecomp PRIVATE /wd4102)
endif()

# ---- Pre-boot GUI launcher (recomp-ui / Dear ImGui) ----
set(NESRECOMP_RECOMP_UI "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui" CACHE PATH "recomp-ui checkout")
include("${NESRECOMP_RECOMP_UI}/recomp_ui.cmake")
recomp_target_launcher_ui(GumshoeRecomp CONSOLE nes BOXART ${CMAKE_SOURCE_DIR}/boxart.tga)
target_compile_definitions(GumshoeRecomp PRIVATE NESRECOMP_GAME_ZAPPER)

# Single-player (one controller) — hide the launcher's Player 2 card.
target_compile_definitions(GumshoeRecomp PRIVATE NESRECOMP_GAME_PLAYERS=1)

cmake_minimum_required(VERSION 3.20)
project(GumshoeRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(NESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/nesrecomp)

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
include(${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake)
recomp_target_launcher_ui(GumshoeRecomp BOXART ${CMAKE_SOURCE_DIR}/boxart.tga)
target_compile_definitions(GumshoeRecomp PRIVATE NESRECOMP_GAME_ZAPPER)

# Single-player (one controller) — hide the launcher's Player 2 card.
target_compile_definitions(GumshoeRecomp PRIVATE NESRECOMP_GAME_PLAYERS=1)

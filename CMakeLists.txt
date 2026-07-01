cmake_minimum_required(VERSION 3.20)
project(GumshoeRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(NESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/nesrecomp)

# ---- Nestopia Oracle (optional) ----
# Set BEFORE including runner.cmake so it picks up the bridge sources.
option(ENABLE_NESTOPIA_ORACLE "Build with Nestopia emulator oracle for verify mode" ON)
if(ENABLE_NESTOPIA_ORACLE)
    set(NESTOPIA_DIR ${CMAKE_SOURCE_DIR}/nestopia-core)
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

if(ENABLE_NESTOPIA_ORACLE)
    target_link_libraries(GumshoeRecomp PRIVATE nestopia_core)
    target_compile_definitions(GumshoeRecomp PRIVATE ENABLE_NESTOPIA_ORACLE)
endif()

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

# ---- Pre-boot GUI launcher (RmlUi) ----
include(${NESRECOMP_ROOT}/runner/launcher.cmake)
nesrecomp_add_launcher(GumshoeRecomp)

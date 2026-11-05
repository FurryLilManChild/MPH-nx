# MPH-nx: Nintendo Switch build of the ndsrecomp runner for Metroid Prime Hunters.
#
# Wraps upstream's runner build (pinned, patched by switch/build.sh) without
# editing its CMakeLists: adds the Switch layer, renames upstream main(),
# wraps libnx pthread internals and packages an NRO.
#
# Configure with the devkitPro Switch toolchain; see switch/build.sh.

cmake_minimum_required(VERSION 3.20)
project(mph_nx LANGUAGES C CXX)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "MPH-nx: configure with the devkitPro Switch toolchain")
endif()

set(MPHNX_VERSION "switch-test5" CACHE STRING "Version string shown in logs and hbmenu")
set(MPHNX_TITLE "MPH-nx" CACHE STRING "Name shown in hbmenu")
option(MPHNX_PIE_LIBS "Compile the runner's static libraries (game banks included) as -fPIE instead of -fPIC" OFF)
set(NDSRECOMP_ROOT "" CACHE PATH "Pinned, patched ndsrecomp checkout")
if(NOT EXISTS "${NDSRECOMP_ROOT}/runner/CMakeLists.txt")
    message(FATAL_ERROR "MPH-nx: NDSRECOMP_ROOT does not point at an ndsrecomp checkout")
endif()

# Runner profile for Switch: portable path (no Dynarmic), SDL2, software 3D.
# Build 1 deliberately leaves the OpenGL compute renderer off; the software
# renderer is a real renderer, and presentation goes through SDL_Renderer.
set(NDS_SDL_BACKEND "SDL2" CACHE STRING "" FORCE)
set(NDS_ENABLE_COMPUTE_RENDERER OFF CACHE BOOL "" FORCE)
set(NDS_BOOTSTRAP_FIRMWARE ON CACHE BOOL "" FORCE)
set(NDS_MPH_MATH_IMPLEMENTATION "HLE" CACHE STRING "" FORCE)
set(NDS_MPH_SUBMIT_IMPLEMENTATION "HLE" CACHE STRING "" FORCE)
set(NDS_GX_COMMAND_CACHE ON CACHE BOOL "" FORCE)
set(NDS_ARM9_DYNARMIC_ROOT "" CACHE PATH "" FORCE)
set(NDSRECOMP_COMPILER_CACHE OFF CACHE STRING "" FORCE)

add_subdirectory("${NDSRECOMP_ROOT}/runner" "${CMAKE_BINARY_DIR}/runner")

set(_runner_dir "${NDSRECOMP_ROOT}/runner")

# Speed experiment (switch-test5 build B). CMake builds static libraries as
# -fPIC, which reaches every global (guest registers, cycle counters, flags)
# through the GOT: one extra load per access, several per guest instruction.
# They only ever link into this PIE executable, so -fPIE is valid and lets the
# compiler address those globals directly.
if(MPHNX_PIE_LIBS)
    foreach(_lib nds_banks nds_support nds_armv4t)
        if(TARGET ${_lib})
            set_target_properties(${_lib} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
            target_compile_options(${_lib} PRIVATE -fPIE)
        endif()
    endforeach()
    message(STATUS "MPH-nx: static libraries built -fPIE")
endif()

# Upstream main() becomes nds_upstream_main(); the Switch entry point calls it.
set_source_files_properties("${_runner_dir}/src/main.cpp"
    DIRECTORY "${_runner_dir}"
    PROPERTIES COMPILE_DEFINITIONS "main=nds_upstream_main")

target_sources(nds_runner PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nx_main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nx_platform.c")
target_compile_definitions(nds_runner PRIVATE MPHNX_VERSION="${MPHNX_VERSION}")
target_include_directories(nds_runner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_options(nds_runner PRIVATE
    "-Wl,--wrap=pthread_create"
    "-Wl,--wrap=pthread_detach")

# Hard rule: never ship without the game code, renderer, sound or input.
# The runner must link the title banks and SDL2; fail the configure if not.
get_target_property(_runner_libs nds_runner LINK_LIBRARIES)
if(NOT _runner_libs MATCHES "SDL2")
    message(FATAL_ERROR "MPH-nx: runner is not linking SDL2 (no video, sound or input)")
endif()
if(NOT _runner_libs MATCHES "nds_banks")
    message(FATAL_ERROR "MPH-nx: runner is not linking the recompiled banks")
endif()

nx_generate_nacp(
    OUTPUT "${CMAKE_BINARY_DIR}/mph-nx.nacp"
    NAME "${MPHNX_TITLE}"
    AUTHOR "FurryLilManChild"
    VERSION "${MPHNX_VERSION}")

nx_create_nro(nds_runner
    OUTPUT "${CMAKE_BINARY_DIR}/mph-nx.nro"
    NACP "${CMAKE_BINARY_DIR}/mph-nx.nacp")

# MPH-nx: Nintendo Switch build of the ndsrecomp runner for Metroid Prime Hunters.
#
# Wraps upstream's runner build (pinned, patched by switch/build.sh) without
# editing its CMakeLists: adds the Switch layer, renames upstream main(),
# wraps libnx pthread internals and packages an NRO.
#
# Configure with the devkitPro Switch toolchain; see switch/build.sh.

cmake_minimum_required(VERSION 3.20)
project(mph_nx LANGUAGES C CXX ASM)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "MPH-nx: configure with the devkitPro Switch toolchain")
endif()

set(MPHNX_VERSION "switch-native4" CACHE STRING "Version string shown in logs and hbmenu")
set(MPHNX_TITLE "MPH-nx" CACHE STRING "Name shown in hbmenu")
option(MPHNX_PIE_LIBS "Compile the runner's static libraries (game banks included) as -fPIE instead of -fPIC" ON)
set(MPHNX_PERF_BANK_DIR "" CACHE PATH "Flat-AOT mph_perf_arm9 bank (nds_recompile --flat-execution); empty = faithful only")
set(MPHNX_DEKO_SHADERS "" CACHE PATH "Generated deko3d shader blob (tools/build_deko_shaders.sh); empty = no GPU 3D renderer")
option(MPHNX_LEAN "Lean ARM9 bank bodies: no per-instruction diagnostics, fixed cached code-fetch cost" ON)
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
# Test10: upstream sampling diagnostics are compiled to armed-only.
set(NDS_LEAN_DIAGNOSTICS ON CACHE BOOL "" FORCE)

add_subdirectory("${NDSRECOMP_ROOT}/runner" "${CMAKE_BINARY_DIR}/runner")

set(_runner_dir "${NDSRECOMP_ROOT}/runner")

# Speed: on since switch-test9 (test5 build B measured no clear gain alone). CMake builds static libraries as
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
    # -fPIE alone still sends extern data through the GOT on AArch64 GCC.
    # Declaring everything hidden (static link, so nothing is preemptible)
    # makes the generated banks address the runtime globals PC-relative.
    target_compile_options(nds_banks PRIVATE
        "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/mphnx_hidden.h")
    message(STATUS "MPH-nx: static libraries built -fPIE")
endif()

if(MPHNX_LEAN)
    # See the MPH-nx lean block at the end of recompiler/armv4t/runtime_arm.h.
    target_compile_definitions(nds_banks PRIVATE MPHNX_LEAN=1)
    message(STATUS "MPH-nx: lean ARM9 bank bodies")
endif()

if(MPHNX_PERF_BANK_DIR)
    # Upstream's portable flat-AOT execution model (built for MKDS) applied to
    # MPH: guest registers and cycles live in locals through a code region,
    # no per-instruction PC/yield/counter work. Selected at launch with
    # --execution-model performance; the faithful banks stay linked.
    file(GLOB _perf_bodies CONFIGURE_DEPENDS "${MPHNX_PERF_BANK_DIR}/mph_perf_arm9_[0-9][0-9].c")
    set(_perf_dispatch "${MPHNX_PERF_BANK_DIR}/mph_perf_arm9_dispatch.c")
    if(NOT _perf_bodies OR NOT EXISTS "${_perf_dispatch}")
        message(FATAL_ERROR "MPH-nx: flat bank missing in ${MPHNX_PERF_BANK_DIR}")
    endif()
    file(READ "${_perf_dispatch}" _perf_text LIMIT 4096)
    if(NOT _perf_text MATCHES "ndsrecomp-flat-aot-v3")
        message(FATAL_ERROR "MPH-nx: ${_perf_dispatch} is not a flat-AOT bank")
    endif()
    add_library(nds_mph_perf_banks STATIC ${_perf_bodies} "${_perf_dispatch}")
    target_include_directories(nds_mph_perf_banks PRIVATE
        "${NDSRECOMP_ROOT}/recompiler/armv4t"
        "${NDSRECOMP_ROOT}/external/arm-recomp-core/common"
        "${NDSRECOMP_ROOT}/external/arm-recomp-core/profiles/armv4t_gba"
        "${MPHNX_PERF_BANK_DIR}")
    target_compile_definitions(nds_mph_perf_banks PRIVATE NDS_STATIC_CPU=0)
    target_compile_options(nds_mph_perf_banks PRIVATE -w -O2 -g0)
    if(MPHNX_PIE_LIBS)
        set_target_properties(nds_mph_perf_banks PROPERTIES POSITION_INDEPENDENT_CODE OFF)
        target_compile_options(nds_mph_perf_banks PRIVATE -fPIE
            "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/mphnx_hidden.h")
    endif()
    target_link_libraries(nds_runner PRIVATE nds_mph_perf_banks)
    target_compile_definitions(nds_runner PRIVATE NDS_MPH_FLAT_PERFORMANCE=1)
    message(STATUS "MPH-nx: flat-AOT ARM9 bank linked from ${MPHNX_PERF_BANK_DIR}")
endif()

set(MPHNX_NATIVE_DIR "" CACHE PATH "Native ARM9 program generated by tools/nrecomp/gen_all.py (USA Rev 1); empty = Rev 0 bank build")
if(MPHNX_NATIVE_DIR)
    # MPH-nx native static recompilation (docs/native-recomp-design.md).
    # Every ARM9 function is a C function with direct calls and returns; the
    # runtime runs it as a coroutine inside the existing scheduler's ARM9 slice.
    file(GLOB _native_units CONFIGURE_DEPENDS "${MPHNX_NATIVE_DIR}/nr_*.c")
    if(NOT _native_units OR NOT EXISTS "${MPHNX_NATIVE_DIR}/nr_tables.c")
        message(FATAL_ERROR "MPH-nx: native program missing in ${MPHNX_NATIVE_DIR}")
    endif()
    if(MPHNX_PERF_BANK_DIR)
        message(FATAL_ERROR "MPH-nx: the native build replaces the Rev 0 flat bank; leave MPHNX_PERF_BANK_DIR empty")
    endif()
    set(_nat "${CMAKE_CURRENT_SOURCE_DIR}/native")
    add_library(mphnx_native_game STATIC ${_native_units})
    target_include_directories(mphnx_native_game PRIVATE "${_nat}" "${MPHNX_NATIVE_DIR}"
        "${NDSRECOMP_ROOT}/external/arm-recomp-core/common")
    target_compile_options(mphnx_native_game PRIVATE -O2 -g0 -w -fno-strict-aliasing)
    if(MPHNX_PIE_LIBS)
        set_target_properties(mphnx_native_game PROPERTIES POSITION_INDEPENDENT_CODE OFF)
        target_compile_options(mphnx_native_game PRIVATE -fPIE
            "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/mphnx_hidden.h")
    endif()
    target_sources(nds_runner PRIVATE "${_nat}/nr_runtime.cpp" "${_nat}/nr_coro.c" "${_nat}/nr_coro_switch.S")
    target_include_directories(nds_runner PRIVATE "${_nat}" "${MPHNX_NATIVE_DIR}")
    target_compile_definitions(nds_runner PRIVATE MPHNX_NATIVE=1)
    target_link_libraries(nds_runner PRIVATE mphnx_native_game)
    message(STATUS "MPH-nx: native ARM9 program from ${MPHNX_NATIVE_DIR} (${CMAKE_MATCH_COUNT})")
endif()

if(MPHNX_DEKO_SHADERS)
    # deko3d GPU 3D renderer ported from RSDuck's melonDS Switch build.
    # Selected at launch from the MPH-nx menu; software 3D stays the default.
    set(_deko "${CMAKE_CURRENT_SOURCE_DIR}/src/deko")
    target_sources(nds_runner PRIVATE
        "${_deko}/nx_deko.cpp" "${_deko}/GpuMemHeap.cpp" "${_deko}/UploadBuffer.cpp"
        "${_deko}/GPU3D_Deko.cpp" "${_deko}/nx_present.cpp" "${MPHNX_DEKO_SHADERS}")
    target_include_directories(nds_runner PRIVATE "${_deko}")
    target_compile_definitions(nds_runner PRIVATE MPHNX_DEKO3D=1)
    target_link_libraries(nds_runner PRIVATE deko3d)
    message(STATUS "MPH-nx: deko3d GPU 3D renderer from ${MPHNX_DEKO_SHADERS}")
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

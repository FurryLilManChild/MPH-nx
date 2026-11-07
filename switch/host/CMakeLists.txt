# MPH-nx host boot test: the native ARM9 program in upstream's headless runner
# on x86-64 Linux. Used in the cloud to boot USA Rev 1 for a fixed number of
# cycles and read the native engine's log, so boot hangs are found and fixed
# before a Switch build goes out. Not shipped.
#
# Configure: see switch/host/build.sh.
cmake_minimum_required(VERSION 3.20)
project(mph_nx_host LANGUAGES C CXX ASM)

set(NDSRECOMP_ROOT "" CACHE PATH "Pinned, patched ndsrecomp checkout")
set(MPHNX_NATIVE_DIR "" CACHE PATH "Native ARM9 program from tools/nrecomp/gen_all.py")
if(NOT EXISTS "${NDSRECOMP_ROOT}/runner/CMakeLists.txt")
    message(FATAL_ERROR "MPH-nx host: NDSRECOMP_ROOT does not point at an ndsrecomp checkout")
endif()
if(NOT EXISTS "${MPHNX_NATIVE_DIR}/nr_tables.c")
    message(FATAL_ERROR "MPH-nx host: native program missing in ${MPHNX_NATIVE_DIR}")
endif()

# Same runner profile as the Switch build, minus presentation.
set(NDS_SDL_BACKEND "NONE" CACHE STRING "" FORCE)
set(NDS_ENABLE_COMPUTE_RENDERER OFF CACHE BOOL "" FORCE)
set(NDS_BOOTSTRAP_FIRMWARE ON CACHE BOOL "" FORCE)
set(NDS_MPH_MATH_IMPLEMENTATION "HLE" CACHE STRING "" FORCE)
set(NDS_MPH_SUBMIT_IMPLEMENTATION "HLE" CACHE STRING "" FORCE)
set(NDS_GX_COMMAND_CACHE ON CACHE BOOL "" FORCE)
set(NDS_ARM9_DYNARMIC_ROOT "" CACHE PATH "" FORCE)
set(NDSRECOMP_COMPILER_CACHE OFF CACHE STRING "" FORCE)
set(NDS_LEAN_DIAGNOSTICS ON CACHE BOOL "" FORCE)

# Turns on the MPH-nx native hooks in the upstream runner (scheduler, bus,
# bank registration) that are otherwise Switch-only.
add_subdirectory("${NDSRECOMP_ROOT}/runner" "${CMAKE_BINARY_DIR}/runner")
set_source_files_properties(
    "${NDSRECOMP_ROOT}/runner/src/scheduler.cpp"
    "${NDSRECOMP_ROOT}/runner/src/bus.cpp"
    "${NDSRECOMP_ROOT}/runner/src/runtime_arm.cpp"
    DIRECTORY "${NDSRECOMP_ROOT}/runner"
    PROPERTIES COMPILE_DEFINITIONS "MPHNX_HOST=1")

set(_nat "${CMAKE_CURRENT_SOURCE_DIR}/../native")
file(GLOB _native_units CONFIGURE_DEPENDS "${MPHNX_NATIVE_DIR}/nr_*.c")
add_library(mphnx_native_game STATIC ${_native_units})
target_include_directories(mphnx_native_game PRIVATE "${_nat}" "${MPHNX_NATIVE_DIR}"
    "${NDSRECOMP_ROOT}/external/arm-recomp-core/common")
target_compile_options(mphnx_native_game PRIVATE -O1 -g0 -w -fno-strict-aliasing)
target_sources(nds_runner PRIVATE "${_nat}/nr_runtime.cpp" "${_nat}/nr_coro.c"
    "${_nat}/nr_coro_switch.S" "${CMAKE_CURRENT_SOURCE_DIR}/host_install.cpp")
target_include_directories(nds_runner PRIVATE "${_nat}" "${MPHNX_NATIVE_DIR}")
target_compile_definitions(nds_runner PRIVATE MPHNX_NATIVE=1)
target_link_libraries(nds_runner PRIVATE mphnx_native_game)

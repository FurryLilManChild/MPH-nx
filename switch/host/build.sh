#!/usr/bin/env bash
# MPH-nx host boot test build (x86-64 Linux, headless). Cloud only.
# Needs the Switch build's generated inputs (BIOS banks in $BUILD_DIR/gen,
# native program in $BUILD_DIR/nrgen) and an ARM7-only title bank folder.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NDSRECOMP_ROOT="${NDSRECOMP_ROOT:-/home/claude/src/ndsrecomp}"
MPH_ROOT="${MPH_ROOT:-/home/claude/src/mph}"
BUILD_DIR="${BUILD_DIR:-/home/claude/build/nx}"
HOST_DIR="${HOST_DIR:-/home/claude/build/host}"
ARM7_BANKS="${ARM7_BANKS:-/home/claude/build/host-arm7banks}"
JOBS="${JOBS:-2}"

# ARM7_BANKS=none: no legacy title banks at all (the release shape).
TITLE_ARGS=()
if [ "$ARM7_BANKS" = none ]; then
  :
elif [ ! -d "$ARM7_BANKS" ]; then
  mkdir -p "$ARM7_BANKS"
  for f in "$MPH_ROOT"/generated/recomp/*arm7*; do ln -s "$f" "$ARM7_BANKS/"; done
  cp "$MPH_ROOT/generated/recomp/registration-order.txt" "$ARM7_BANKS/"
fi
if [ "$ARM7_BANKS" != none ]; then
  TITLE_ARGS=(-DNDS_TITLE_BANK_DIR="$ARM7_BANKS" -DNDS_TITLE_ROM_SHA1=90164d1ac127ee5f9815ea4ae7de798c7b5fc629)
else
  TITLE_ARGS=(-DNDS_TITLE_BANK_DIR= -DNDS_TITLE_ROM_SHA1=)
fi

cmake -S "$HERE" -B "$HOST_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE="-O1 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O1 -DNDEBUG" \
  -DNDSRECOMP_ROOT="$NDSRECOMP_ROOT" \
  -DMPHNX_NATIVE_DIR="$BUILD_DIR/nrgen" \
  -DNDS_GENERATED_DIR="$BUILD_DIR/gen" \
  "${TITLE_ARGS[@]}" \
  -DNDS_RECOMP_UI_ROOT="$MPH_ROOT/third_party/recomp-ui" \
  -DNDSRECOMP_COMPILE_JOBS="$JOBS" -DNDSRECOMP_LINK_JOBS=1
ninja -C "$HOST_DIR" -j "$JOBS" nds_runner

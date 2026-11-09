#!/usr/bin/env bash
# MPH-nx Switch build.
#
# Steps:
#   1. check the upstream checkouts are on their pinned commits
#   2. apply switch/patches to ndsrecomp (idempotent)
#   3. generate the ROM-derived title banks (host tools, needs the ROM)
#   4. generate FreeBIOS banks plus the retail-BIOS stubs
#   5. cross-compile the runner for Switch and package mph-nx.nro
#   6. check the artifact before it goes anywhere
#
# Environment (defaults match the cloud workspace):
#   NDSRECOMP_ROOT  ndsrecomp checkout   (pinned 179ac63)
#   MPH_ROOT        MetroidPrimeHuntersRecomp checkout (pinned 61ac84a)
#   MPH_ROM         verified AMHE rev 0 ROM
#   BUILD_DIR       build output
#   DKRUN           command that runs its arguments inside devkitPro
#   JOBS            parallel compile jobs (2 fits 7 GB of RAM)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NDSRECOMP_ROOT="${NDSRECOMP_ROOT:-/home/claude/src/ndsrecomp}"
MPH_ROOT="${MPH_ROOT:-/home/claude/src/mph}"
MPH_ROM="${MPH_ROM:-$MPH_ROOT/Metroid Prime Hunters.nds}"
BUILD_DIR="${BUILD_DIR:-/home/claude/build/nx}"
DKRUN="${DKRUN:-/home/claude/dk.sh}"
JOBS="${JOBS:-2}"
VERSION="${MPHNX_VERSION:-switch-native14}"
TITLE="${MPHNX_TITLE:-MPH-nx}"
PIE_LIBS="${MPHNX_PIE_LIBS:-ON}"
LEAN="${MPHNX_LEAN:-ON}"
# NATIVE=1 (default): the native ARM9 program for USA Rev 1 (tools/nrecomp).
NATIVE="${NATIVE:-1}"
MPH_ROM_REV1="${MPH_ROM_REV1:-/home/claude/private/mph-usa-rev1.nds}"
ROM_SHA1_REV1=a8679d4f8dcb445cbe946a424241ef44901b0caf

NDSRECOMP_PIN=179ac63d9e4055e1218d61b3fd03c34a94880a6e
MPH_PIN=61ac84aac262645e446ea28e3d4db3567e2dba8c
ROM_SHA1=90164d1ac127ee5f9815ea4ae7de798c7b5fc629

say() { echo "[mph-nx build] $*"; }
die() { echo "[mph-nx build] ERROR: $*" >&2; exit 1; }

STAMP="$(mktemp)"  # build start; the NRO must be newer than this
say "1/6 checking pins"
[ "$(git -C "$NDSRECOMP_ROOT" rev-parse HEAD)" = "$NDSRECOMP_PIN" ] || die "ndsrecomp is not on $NDSRECOMP_PIN"
[ "$(git -C "$MPH_ROOT" rev-parse HEAD)" = "$MPH_PIN" ] || die "MetroidPrimeHuntersRecomp is not on $MPH_PIN"
[ -f "$MPH_ROM" ] || die "ROM not found at $MPH_ROM"
[ "$(sha1sum "$MPH_ROM" | cut -d' ' -f1)" = "$ROM_SHA1" ] || die "ROM SHA-1 does not match AMHE rev 0"

say "2/6 applying patches"
for p in "$HERE"/patches/*.patch; do
  if git -C "$NDSRECOMP_ROOT" apply --reverse --check "$p" 2>/dev/null; then
    say "  already applied: $(basename "$p")"
  else
    git -C "$NDSRECOMP_ROOT" apply "$p" || die "patch failed: $(basename "$p")"
    say "  applied: $(basename "$p")"
  fi
done

say "3/6 title banks"
GEN_HOST="$BUILD_DIR/host-gen"
if [ ! -f "$MPH_ROOT/generated/recomp/mph_arm9_dispatch.c" ]; then
  cmake -S "$MPH_ROOT" -B "$GEN_HOST" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DNDSRECOMP_ROOT="$NDSRECOMP_ROOT" -DMPH_PYTHON="$(command -v python3)" \
    -DMPH_ROM="$MPH_ROM" -DMPH_ALLOW_MISSING_BANKS=ON
  ninja -C "$GEN_HOST" mph_generate_banks
else
  say "  title banks already generated"
fi
NDS_RECOMPILE="${NDS_RECOMPILE:-}"
[ -n "$NDS_RECOMPILE" ] || NDS_RECOMPILE="$(find "$BUILD_DIR/host-gen" "$MPH_ROOT/build-gen" -name nds_recompile -type f 2>/dev/null | head -1 || true)"
[ -n "$NDS_RECOMPILE" ] || die "nds_recompile host tool not found"

PERF_DIR="$BUILD_DIR/perfgen"
if [ ! -f "$PERF_DIR/mph_perf_arm9_dispatch.c" ]; then
  say "  flat-AOT ARM9 bank"
  mkdir -p "$PERF_DIR"
  "$NDS_RECOMPILE" --config "$MPH_ROOT/generated/inputs/arm9.toml" --bin "$MPH_ROOT/generated/inputs/arm9.bin" \
    --out "$PERF_DIR" --bank mph_perf_arm9 --shards 16 --stable-address-shards --max-function-bytes 512 \
    --validate-live-bytes --flat-execution >/dev/null || die "flat bank generation failed"
fi

NATIVE_DIR=""
if [ "$NATIVE" = 1 ]; then
  say "  native ARM9 program (USA Rev 1)"
  [ -f "$MPH_ROM_REV1" ] || die "Rev 1 ROM not found at $MPH_ROM_REV1"
  [ "$(sha1sum "$MPH_ROM_REV1" | cut -c1-40)" = "$ROM_SHA1_REV1" ] || die "Rev 1 ROM SHA-1 mismatch"
  NATIVE_DIR="$BUILD_DIR/nrgen"
  # Runtime roots found by boot tests (derived from the game; never committed).
  export NR_EXTRA_ROOTS="${NR_EXTRA_ROOTS:-$(dirname "$BUILD_DIR")/nrroots.txt}"
  python3 "$HERE/../tools/nrecomp/gen_all.py" "$MPH_ROM_REV1" "$NATIVE_DIR" || die "native generation failed"
  # Native ARM7 (native6). Extra roots: the old ARM7 engine's entry lists
  # (upstream's generated inputs) plus runtime misses from tests.
  A7IN="$MPH_ROOT/generated/inputs"
  export NR7_EXTRA_ROOTS="${NR7_EXTRA_ROOTS:-$A7IN/arm7_wram_alias.toml:$A7IN/arm7_mainram_alias.toml:$(dirname "$BUILD_DIR")/nrroots7.txt}"
  say "  native ARM7 program"
  python3 "$HERE/../tools/nrecomp/gen_all7.py" "$MPH_ROM_REV1" "$NATIVE_DIR" || die "native ARM7 generation failed"
  PERF_DIR=""
fi

DEKO_DIR="$BUILD_DIR/deko-shaders"
say "  deko3d shaders"
DKRUN="$DKRUN" "$HERE/tools/build_deko_shaders.sh" "$HERE/src/deko/shaders" "$DEKO_DIR" >/dev/null || die "deko3d shader build failed"

say "4/6 BIOS banks"
GEN="$BUILD_DIR/gen"
mkdir -p "$GEN"
"$NDS_RECOMPILE" --config "$NDSRECOMP_ROOT/bios/freebios9.toml" \
  --bin "$NDSRECOMP_ROOT/third_party/freebios/drastic_bios_arm9.bin" --out "$GEN" --bank freebios_arm9 >/dev/null
"$NDS_RECOMPILE" --config "$NDSRECOMP_ROOT/bios/freebios7.toml" \
  --bin "$NDSRECOMP_ROOT/third_party/freebios/drastic_bios_arm7.bin" --out "$GEN" --bank freebios_arm7 >/dev/null
cp "$HERE"/stubs/*.c "$GEN"/

say "5/6 Switch configure and build"
COMPAT="$HERE/compat"
INC="$COMPAT;/opt/devkitpro/libnx/include;/opt/devkitpro/portlibs/switch/include"
"$DKRUN" "cmake -S '$HERE' -B '$BUILD_DIR/switch' -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DMPHNX_VERSION='$VERSION' \
  -DMPHNX_TITLE='$TITLE' \
  -DMPHNX_PIE_LIBS=$PIE_LIBS \
  -DMPHNX_LEAN=$LEAN \
  -DMPHNX_PERF_BANK_DIR='$PERF_DIR' \
  -DMPHNX_NATIVE_DIR='$NATIVE_DIR' \
  -DMPHNX_DEKO_SHADERS='$DEKO_DIR/nx_deko_shaders.c' \
  -DNDSRECOMP_ROOT='$NDSRECOMP_ROOT' \
  -DNDS_GENERATED_DIR='$GEN' \
  -DNDS_TITLE_BANK_DIR='$MPH_ROOT/generated/recomp' \
  -DNDS_TITLE_ROM_SHA1=$ROM_SHA1 \
  -DNDS_RECOMP_UI_ROOT='$MPH_ROOT/third_party/recomp-ui' \
  -DNDSRECOMP_COMPILE_JOBS=$JOBS -DNDSRECOMP_LINK_JOBS=1 \
  '-DCMAKE_C_STANDARD_INCLUDE_DIRECTORIES=$INC' \
  '-DCMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES=$INC'"
"$DKRUN" "ninja -C '$BUILD_DIR/switch' -j$JOBS nds_runner_nro"

say "6/6 checking the artifact"
NRO="$BUILD_DIR/switch/mph-nx.nro"
ELF="$BUILD_DIR/switch/runner/nds_runner.elf"
[ -f "$NRO" ] || die "no NRO produced"
[ "$NRO" -nt "$STAMP" ] || die "NRO was not rebuilt by this run (stale file)"
SIZE=$(stat -c %s "$NRO")
say "  mph-nx.nro: $((SIZE / 1048576)) MB"
[ "$SIZE" -gt $((50 * 1048576)) ] || die "NRO is suspiciously small (banks missing?)"
"$DKRUN" "aarch64-none-elf-nm -C '$ELF' > '$BUILD_DIR/switch/nds_runner.syms'"
for sym in SDL_CreateRenderer SDL_OpenAudioDevice SDL_GameControllerOpen SDL_RenderPresent \
           nds_upstream_main g_dispatch_mph_arm9 g_dispatch_freebios_arm9 \
           __wrap_pthread_create __libnx_exception_handler; do
  grep -q "$sym" "$BUILD_DIR/switch/nds_runner.syms" || die "symbol missing from ELF: $sym"
done
if [ "$NATIVE" = 1 ]; then
  for sym in nr_resume_arm9 nr_install nr_tab_static nr_overlays N_02004800_a; do
    grep -q "$sym" "$BUILD_DIR/switch/nds_runner.syms" || die "native program missing from ELF: $sym"
  done
  # The Rev 1 game.toml for the SD card (sha1 gates every Rev 0-only feature off).
  cat > "$BUILD_DIR/switch/game.toml" <<TOML
# MPH-nx native build: Metroid Prime Hunters USA Rev 1
[game]
name = "Metroid Prime Hunters"
id = "AMHE"
region = "USA"
revision = 1
sha1 = "$ROM_SHA1_REV1"

[display]
screen_layout = "separate"
supersampling = 1
internal_resolution = 1
antialiasing = 8
adaptive_widescreen = "none"
adaptive_guest_culling = false

[system]
startup_mode = "automatic"

[cartridge]
save_type = "flash"
save_size = 262144
TOML
  say "  native ARM9 program present; Rev 1 game.toml written"
fi
say "  renderer, audio, input, game banks, FreeBIOS and Switch layer all present"
sha256sum "$NRO"

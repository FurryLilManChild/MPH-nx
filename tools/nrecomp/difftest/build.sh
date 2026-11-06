#!/bin/bash
# Host build of the differential test (x86 Linux). Usage: build.sh <generated dir> <out dir>
set -e
GEN="$1"; OUT="$2"; mkdir -p "$OUT"
HERE="$(cd "$(dirname "$0")" && pwd)"
ARC=/home/claude/src/ndsrecomp/external/arm-recomp-core
I="-DNR_DIFFTEST -I$HERE/../../../switch/native -I$ARC/common"
objs=()
for c in "$GEN"/nr_*.c; do objs+=("$OUT/$(basename "$c" .c).o"); done
for c in "$GEN"/nr_*.c; do
  o="$OUT/$(basename "$c" .c).o"
  [ "$o" -nt "$c" ] && [ "$o" -nt "$HERE/../../../switch/native/nr_runtime.h" ] || echo "gcc -O1 $NRFLAGS -c $I $c -o $o"
done | xargs -P 2 -I{} sh -c "{}"
for c in $ARC/common/interpreter.cpp $ARC/common/arm_decode.cpp $ARC/common/thumb_decode.cpp $ARC/common/arm_ir.cpp $HERE/difftest.cpp; do
  o="$OUT/$(basename "$c" .cpp).o"; objs+=("$o")
  g++ -O1 $NRFLAGS -std=c++20 -c $I "$c" -o "$o"
done
g++ -o "$OUT/difftest" "${objs[@]}"
echo built "$OUT/difftest"

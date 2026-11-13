#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# check.sh [EBOOT]: every check that needs no game window, in the order a change can break them:
#   1. the static build (tools/native/build.sh)
#   2. the C tests (build.sh --test: shader container scan, translator + spirv-val, addresses...)
#   3. the C++ tests (gpu/platform: Gnm command thread and event queues, settings, overlay, PNG)
#   4. the Python tests
#   5. with an eboot.bin (argument, or VW_GAME_DIR/eboot.bin): every game shader translated with
#      tools/native/ns_dump and checked with spirv-val. The SPIR-V goes to a temporary folder that
#      is deleted afterwards: converted game shaders must never be committed or shipped.
# Needs: the build dependencies (README), spirv-val (SPIRV-Tools). Prints "check: ok" at the end.
set -euo pipefail
cd "$(dirname -- "$0")/../.."
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v retcomm | paste -sd:)
unset CC CXX CMAKE_SYSROOT LDFLAGS
step() { printf '\n== %s\n' "$*"; }

step "build"
bash tools/native/build.sh

step "C tests"
bash build.sh --test

step "C++ tests"
tests=(png-decode perf-overlay graphics-settings gpu-platform)
cmake --build out/static --target "${tests[@]/%/-test}" --parallel "$(nproc)" >/dev/null
for t in "${tests[@]}"; do (cd out/static && timeout 60 "./$t-test" >/dev/null) && echo "$t: ok"; done

step "Python tests"
python3 -m unittest discover -s tests -q

eboot=${1:-${VW_GAME_DIR:+$VW_GAME_DIR/eboot.bin}}
if [[ -n ${eboot:-} && -f $eboot ]]; then
    step "game shaders ($eboot)"
    command -v spirv-val >/dev/null || { echo "spirv-val not found (SPIRV-Tools)"; exit 1; }
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    cc -std=c11 -O2 -Isrc tools/native/ns_dump.c src/native_shader.c src/native_shader_spirv.c -o "$tmp/ns_dump"
    "$tmp/ns_dump" "$eboot" --spirv "$tmp/spv" | tail -1
    bad=0
    for f in "$tmp"/spv/*.spv; do spirv-val --target-env vulkan1.3 "$f" >/dev/null 2>&1 || { echo "invalid: $(basename "$f")"; bad=$((bad + 1)); }; done
    echo "$(ls "$tmp"/spv | wc -l) shaders, $bad invalid"
    (( bad == 0 ))
else
    step "game shaders: skipped (pass an eboot.bin or set VW_GAME_DIR)"
fi
printf '\ncheck: ok\n'

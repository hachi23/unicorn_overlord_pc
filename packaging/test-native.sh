#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run after packaging configures the native build. No game dump required.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
case $(uname -s) in
MINGW*|MSYS*) directory=out/gpu-static; suffix=.exe ;;  # build.sh --static (packaging)
*) directory=out/static; suffix= ;;
esac
tests=(png-decode perf-overlay graphics-settings gpu-platform)
targets=()
for name in "${tests[@]}"; do targets+=("$name-test"); done
cmake --build "$directory" --target "${targets[@]}" --parallel 2
for name in "${tests[@]}"; do
    "$directory/$name-test$suffix"
done
if [[ $suffix == .exe ]]; then
    cmake --build "$directory" --target mp3-test videodec-test services-test pad-map-test --parallel 2
    "$directory/mp3-test.exe" tests/data/sine.mp3
    "$directory/videodec-test.exe" tests/data/tiny.h264
    "$directory/services-test.exe"
    "$directory/pad-map-test.exe"
else
    export VW_DEPS=$PWD/out/deps/prefix
    bash build.sh --test
fi

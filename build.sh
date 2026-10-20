#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
# Windows: MSYS2 CLANG64 (packages in packaging/windows/README.md). The GPU library is static
# there and CMake links vwport.exe as well; no PGO (GCC-only flags), LTO off by default.
case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=msys2-env.sh
    source ./msys2-env.sh
    for tool in clang cmake ninja pkg-config; do
        command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (packaging/windows/README.md)." >&2; exit 1; }
    done
    if [[ ! -f gpu/third_party/imgui/imgui.h || ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
        git submodule update --init --recursive
    fi
    # --static (packaging): FFmpeg, SDL3 and the others from packaging/deps.sh, linked in.
    deps=
    # Its own build folder: CMake keeps the pkg-config results of the first configure, so a
    # static build in the development folder linked MSYS2's shared FFmpeg (and its -ldl).
    build=out/gpu
    if [[ ${1:-} == --static ]]; then
        bash packaging/deps.sh
        deps=$(cygpath -m "$PWD/out/deps/prefix")
        # pkg-config must find these, not MSYS2's shared FFmpeg/SDL3 (';' separated).
        export PKG_CONFIG_PATH="$deps/lib/pkgconfig;$deps/share/pkgconfig"
        build=out/gpu-static
    fi
    cmake -S gpu -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVW_PGO=off \
        -DVW_LTO="${VW_LTO:-OFF}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DVW_DEPS="$deps" >/dev/null
    echo "GPU library and loader: LTO ${VW_LTO:-OFF}"
    if ! ninja -C "$build" vwport > "$build-build.log" 2>&1; then
        grep -v '^[[]' "$build-build.log" | tail -40 >&2
        echo "Build failed (full log: $build-build.log)" >&2; exit 1
    fi
    cp "$build/vwport.exe" out/
    echo "Built $PWD/out/vwport.exe"
    exit 0
    ;;
esac
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${VW_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env VW_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3 libavcodec libavutil libswscale)"
read -r -a libraries <<< "$(pkg-config --libs --static vulkan sdl3 libavcodec libavutil libswscale)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libvwgpu.so.
# VW_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. VW_LTO=OFF disables link-time optimization.
pgo=${VW_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Submodules (git clone --recursive, or: git submodule update --init).
if [[ ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
# --static: the release executable, out/static/vwport, with the GPU library and the static
# third-party libraries of packaging/deps.sh linked in (packaging/package.py).
if [[ ${1:-} == --static ]]; then
    bash packaging/deps.sh
    cmake -S gpu -B out/static -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -g' -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -g' \
        -DVW_STATIC=ON -DVW_PGO="$pgo" \
        -DVW_LTO="${VW_LTO:-ON}" -DVW_PGO_DIR="$PWD/pgo" -DCMAKE_C_COMPILER="$CC" \
        -DVW_DEPS="$PWD/out/deps/prefix" >/dev/null
    if ! ninja -C out/static vwport > out/static-build.log 2>&1; then
        grep -v '^[[]' out/static-build.log | tail -40 >&2
        echo 'Static build failed (full log: out/static-build.log)' >&2; exit 1
    fi
    echo "Built $PWD/out/static/vwport"
    exit 0
fi
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVW_PGO="$pgo" \
    -DVW_LTO="${VW_LTO:-ON}" -DVW_PGO_DIR="$PWD/pgo" -DVW_DEPS="${VW_DEPS:-}" >/dev/null
echo "GPU library: PGO $pgo, LTO ${VW_LTO:-ON}"
# A failed GPU build must stop here: an older libvwgpu.so would otherwise be used silently.
if ! ninja -C out/gpu vwgpu > out/gpu-build.log 2>&1; then
    grep -v '^[[]' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lvwgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
runtime=(src/runtime*.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c src/gpu_device.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/vwport
echo "Built $PWD/out/vwport"
if [[ ${1:-} == --test ]]; then
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" -o out/pad-test
    out/pad-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_audio.c "${libraries[@]}" -lm -o out/audio-test
    out/audio-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_save_commit.c -o out/save-commit-test
    out/save-commit-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_mp3.c src/runtime_mp3.c out/libatrac9.a -lm "${libraries[@]}" -o out/mp3-test
    out/mp3-test tests/data/sine.mp3
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_videodec.c "${libraries[@]}" -o out/videodec-test
    out/videodec-test tests/data/tiny.h264
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_rtc.c -o out/rtc-test
    out/rtc-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_json.c -lm -o out/json-test
    out/json-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_services.c -o out/services-test
    out/services-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_button_icons.c -o out/button-icons-test
    out/button-icons-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc tests/test_native_settings.c -o out/native-settings-test
    out/native-settings-test
fi

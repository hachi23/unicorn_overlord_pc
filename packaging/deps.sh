#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Static third-party libraries for release builds (build.sh --static, Windows packaging):
# out/deps/prefix gets zlib, SDL3 and a minimal FFmpeg (the decoders,
# demuxers and parsers the runtime (movies, MP3) and the PNG reader use; nothing else). vwport then only needs
# what every system has: libc, the Vulkan loader and (Linux) X11. Sources are pinned by
# SHA-256; a library is rebuilt only when its version line below changes.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
DEPS=$PWD/out/deps
# pkg-config files and FFmpeg's configure split paths at spaces.
if [[ $PWD == *' '* ]]; then
    echo "deps: the checkout path has a space ($PWD); release builds need a path without spaces." >&2
    exit 1
fi
PREFIX=$DEPS/prefix
JOBS=$(nproc 2>/dev/null || echo 4)
mkdir -p "$DEPS/src" "$DEPS/build" "$PREFIX"
case $(uname -s) in MINGW*|MSYS*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
# NATIVE: the prefix as the compilers, CMake and pkg-config see it (Windows: C:/..., ';' lists);
# PREFIX stays the shell's path for files and PATH.
if [[ $WINDOWS == 1 ]]; then
    NATIVE=$(cygpath -m "$PREFIX")
    export PKG_CONFIG_PATH="$NATIVE/lib/pkgconfig;$NATIVE/share/pkgconfig"
else
    NATIVE=$PREFIX
    export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
fi
export PATH=$PREFIX/bin:$PATH
if [[ $WINDOWS == 1 ]]; then
    # Native CMake and configure want a compiler name, or a path ending in .exe.
    CC=${CC:-clang} CXX=${CXX:-clang++}
else
    CC=${CC:-$(command -v clang || command -v cc)}
    CXX=${CXX:-$(command -v clang++ || command -v c++)}
fi
CMAKE=(cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$NATIVE" -DCMAKE_PREFIX_PATH="$NATIVE"
       -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON
       -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5)

# name version url sha256
SOURCES=(
"zlib 1.3.1 https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
"nasm 2.16.03 https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz 1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148"
"sdl3 3.4.18 https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
"ffmpeg 9.0.2 https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
)

# unpack <name>: the verified source in out/deps/build/<name> (fresh), its directory on stdout.
unpack() {
    local line name version url sha file dir
    for line in "${SOURCES[@]}"; do
        read -r name version url sha <<< "$line"
        [[ $name == "$1" ]] && break
    done
    file=$DEPS/src/$name-$version.tar.${url##*.tar.}
    if [[ ! -f $file ]] || ! echo "$sha  $file" | sha256sum -c --status; then
        curl -fL --retry 3 -o "$file.part" "$url" >&2
        echo "$sha  $file.part" | sha256sum -c --status || { echo "deps: checksum mismatch for $url" >&2; exit 1; }
        mv "$file.part" "$file"
    fi
    dir=$DEPS/build/$name
    rm -rf "$dir" && mkdir -p "$dir"
    rm -f "$DEPS/$1.log"
    tar -xf "$file" -C "$dir" --strip-components=1
    echo "$dir"
}
# quiet <name> <command...>: output to out/deps/<name>.log, shown only when the command fails.
quiet() {
    local name=$1; shift
    if ! "$@" >> "$DEPS/$name.log" 2>&1; then
        tail -30 "$DEPS/$name.log" >&2; echo "deps: $name failed (log: $DEPS/$name.log)" >&2; exit 1
    fi
}
# A library is built once per version line (and build option change).
stamp() { echo "$PREFIX/.$1-$(printf '%s' "${SOURCES[*]} $2" | sha256sum | cut -c1-16)"; }
needs() { [[ ! -f $(stamp "$1" "${2:-}") ]]; }
done_() { rm -f "$PREFIX/.$1-"*; touch "$(stamp "$1" "${2:-}")"; echo "deps: $1 ready"; }

if needs zlib; then
    src=$(unpack zlib)
    quiet zlib "${CMAKE[@]}" -S "$src" -B "$src/build" -DZLIB_BUILD_EXAMPLES=OFF
    quiet zlib ninja -C "$src/build" install
    # Static only: the linker must not find a shared zlib in the prefix.
    rm -f "$PREFIX"/lib/libz.so* "$PREFIX"/lib/libzlib.dll.a "$PREFIX"/bin/zlib*.dll
    [[ $WINDOWS == 1 && -f $PREFIX/lib/libzlibstatic.a ]] && cp "$PREFIX/lib/libzlibstatic.a" "$PREFIX/lib/libz.a"
    done_ zlib
fi
if ! command -v nasm >/dev/null && needs nasm; then
    src=$(unpack nasm)
    quiet nasm bash -c "cd '$src' && ./configure --prefix='$PREFIX' CC='$CC' && make -j$JOBS nasm"
    install -D -m755 "$src/nasm" "$PREFIX/bin/nasm"
    done_ nasm
fi
if needs sdl3; then
    src=$(unpack sdl3)
    quiet sdl3 "${CMAKE[@]}" -S "$src" -B "$src/out" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
        -DSDL_EXAMPLES=OFF -DSDL_TESTS=OFF
    quiet sdl3 ninja -C "$src/out" install
    done_ sdl3
fi
FFMPEG_OPTIONS=(
    --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network
    --disable-avdevice --disable-avfilter --enable-swscale --enable-swresample --enable-zlib
    --enable-decoder=h264,hevc,mp3,mp3float,aac,aac_fixed,png
    --enable-parser=h264,hevc,aac,mpegaudio,png
    --enable-demuxer=mov,aac,mp3,h264,hevc
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc
    --enable-protocol=file
)
if needs ffmpeg "${FFMPEG_OPTIONS[*]}"; then
    src=$(unpack ffmpeg)
    asm=()
    command -v nasm >/dev/null || asm=(--disable-x86asm)
    platform=()
    [[ $WINDOWS == 1 ]] && platform=(--enable-w32threads)
    quiet ffmpeg bash -c 'cd "$1" && shift && ./configure "$@"' _ "$src" --prefix="$NATIVE" --cc="$CC" --cxx="$CXX" \
        --enable-static --disable-shared --enable-pic --pkg-config-flags=--static --extra-cflags="-I$NATIVE/include" \
        --extra-ldflags="-L$NATIVE/lib" "${FFMPEG_OPTIONS[@]}" "${asm[@]}" "${platform[@]}"
    quiet ffmpeg make -C "$src" -j"$JOBS" install
    done_ ffmpeg "${FFMPEG_OPTIONS[*]}"
fi
echo "deps: $PREFIX"

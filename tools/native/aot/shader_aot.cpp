// SPDX-License-Identifier: GPL-2.0-or-later
// shader-aot [--native] EBOOT OUT_DIR: runs gpu/shim/native_convert.cpp from the command line.
// Without --native the output uses the old recompiler's bindings (to compare with its cache).
#include <cstdio>
#include <cstring>
extern "C" int vwgpu_native_convert(const char* eboot, const char* out_dir, int native_renderer);

int main(int argc, char** argv) {
    int native = 0;
    if (argc > 1 && std::strcmp(argv[1], "--native") == 0) {
        native = 1;
        ++argv, --argc;
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: shader-aot [--native] EBOOT OUT_DIR\n");
        return 2;
    }
    const int converted = vwgpu_native_convert(argv[1], argv[2], native);
    return converted > 0 ? 0 : 1;
}

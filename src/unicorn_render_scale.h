/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNICORN_RENDER_SCALE_H
#define UNICORN_RENDER_SCALE_H
#include <stdint.h>
#include <string.h>
#include "unicorn_build.h"

/* Unicorn Overlord CUSA36381 v01.04: render above 1920x1080 (docs/dev/unicorn/HANDOFF.md).
 * On a PS4 Pro (sceKernelIsNeoMode, cached by FUN_00218680) the game draws natively at
 * 3840x2160 with its Font_4K/ fonts. Its screen size getters FUN_0003e2b0 (width) and
 * FUN_0003e2d0 (height), and their two inlined copies in FUN_0003cb60 (the MAIN BUFFER color
 * and depth targets), pick the Pro size with `mov ecx, 0xf00` / `mov ecx, 0x870`: a smaller
 * Pro size (1800p, or below 1080p) is those six imm32 operands. */
static const uint32_t unicorn_width_imm[] = {0x3d9fd, 0x3db77, 0x3e2bd};
static const uint32_t unicorn_height_imm[] = {0x3da19, 0x3db5b, 0x3e2dd};
/* Not patched: 3840.0f and 2160.0f at 0xaf31a4, read only by FUN_00233cd0 at 0x2354bb. They are
 * the rectangle of the World Map's canvas pass (FUN_000393d0, pass 0x1a), whose target is
 * 3840x2160 at every size. At the render size the canvas filled only that target's top-left
 * corner: the explored area was misdrawn and the markers shrank into the top-left of the
 * screen (#69). */

/* The battle map is drawn into targets of its own, min(screen, 2880x1620): the PS4 Pro's limit,
 * kept at any Pro size. Two copies of the code (not a Ghidra function; 0x2377d8 and 0x2379b2)
 * load `mov r32, 2880` and `mov r32, 1620` and take the screen size instead when it is smaller
 * than the floats 2880.0f/1620.0f in rodata (also read by the getters FUN_00337da0/FUN_00337dd0
 * and a pass in FUN_0041a080). At 4K the map was drawn at 2880x1620 and enlarged, so units and
 * their rings looked grainier than on PS5 (#56). The render size there lifts the limit. */
static const uint32_t unicorn_map_width_imm[] = {0x2377da, 0x2379b4};
static const uint32_t unicorn_map_height_imm[] = {0x23780c, 0x2379e0};
static const uint32_t unicorn_map_width_float = 0xaf31b4;
static const uint32_t unicorn_map_height_float = 0xaf31b8;

static inline int unicorn_map_limit_signature_match(const unsigned char *image, uint64_t size) {
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t w = unicorn_address(unicorn_map_width_imm[i]);
        const uint32_t h = unicorn_address(unicorn_map_height_imm[i]);
        uint32_t wv, hv;
        if (!w || !h || w + 4 > size || h + 4 > size) return 0;
        /* mov r14d / r12d (41 be, 41 bc) and mov r15d / ebx (41 bf, bb) */
        if ((image[w - 1] != 0xbe && image[w - 1] != 0xbc) || (image[h - 1] != 0xbf && image[h - 1] != 0xbb))
            return 0;
        memcpy(&wv, image + w, 4);
        memcpy(&hv, image + h, 4);
        if (wv != 2880 || hv != 1620) return 0;
    }
    const uint32_t wf_at = unicorn_address(unicorn_map_width_float), hf_at = unicorn_address(unicorn_map_height_float);
    float wf, hf;
    if (!wf_at || !hf_at || hf_at + 4 > size) return 0;
    memcpy(&wf, image + wf_at, 4);
    memcpy(&hf, image + hf_at, 4);
    return wf == 2880.0f && hf == 1620.0f;
}

/* The RENDERBUFFER heap (FUN_0003cb60, the render targets' pool) is 768 MiB on the Pro path: a
 * u64 in rodata that FUN_00503ec0 copies into the heap sizes (read only at 0x504006; 0x29f19c00
 * replaces it on a base PS4). 3840x2160 peaks at 722 MiB of it. Wider sizes need more than
 * their pixel count says (some targets follow the width alone): 4266x1800 (1800p at 21:9)
 * peaked at 822 MiB and 5120x2160 (4K at 21:9) at 1135 MiB, so the next target's allocation
 * returned null and FUN_00044a90 cleared it (#87). Measured peaks from 2132x900 to 7680x2160
 * (title screen; the prologue battle at 5120x2160 stayed at the same peak) fit
 * 44.75 w*h + 19.6 w*w (per million pixels) + 104 MiB within 40 MiB. */
static const uint32_t unicorn_renderbuffer_size = 0xaf26f0;
#define UNICORN_RENDERBUFFER_PRO UINT64_C(0x30000000)

/* The heap size for width x height: the fitted peak plus a quarter, in 16 MiB steps, at least
 * UNICORN_RENDERBUFFER_PRO. The heap's size is 32-bit: at most 3.75 GiB (render sizes reach
 * 8640x2160, about 3 GiB). */
static inline uint64_t unicorn_renderbuffer_bytes(uint32_t width, uint32_t height) {
    const double w = width, h = height;
    const double mib = (44.75 * w * h + 19.6 * w * w) / 1e6 + 104.0;
    uint64_t bytes = ((uint64_t)(mib * 1.25) + 15) / 16 * 16 << 20;
    if (bytes < UNICORN_RENDERBUFFER_PRO) bytes = UNICORN_RENDERBUFFER_PRO;
    if (bytes > UINT64_C(0xf0000000)) bytes = UINT64_C(0xf0000000);
    return bytes;
}

static inline int unicorn_renderbuffer_signature_match(const unsigned char *image, uint64_t size) {
    const uint32_t at = unicorn_address(unicorn_renderbuffer_size);
    uint64_t value;
    if (!at || at + 8 > size) return 0;
    memcpy(&value, image + at, 8);
    return value == UNICORN_RENDERBUFFER_PRO;
}

/* Each operand follows the `mov ecx` opcode (0xb9) and holds 3840 or 2160. */
static inline int unicorn_render_scale_signature_match(const unsigned char *image, uint64_t size) {
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t w = unicorn_address(unicorn_width_imm[i]), h = unicorn_address(unicorn_height_imm[i]);
        uint32_t wv, hv;
        if (w + 4 > size || h + 4 > size || image[w - 1] != 0xb9 || image[h - 1] != 0xb9) return 0;
        memcpy(&wv, image + w, 4);
        memcpy(&hv, image + h, 4);
        if (wv != 3840 || hv != 2160) return 0;
    }
    return 1;
}

#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNICORN_BUILD_H
#define UNICORN_BUILD_H
#include <stdint.h>

/* Unicorn Overlord CUSA36381 builds the port's code changes know. The addresses in the
 * unicorn_*.h headers are v01.04's (the 1.04 update; the fan English patch keeps its
 * eboot.bin). The Hong Kong base game 01.00 master 1.05 (param.sfo APP_VER 01.00, VERSION
 * 01.05; #33) has the same code moved: unicorn_address() moves a v01.04 address there. Every
 * patch still checks the bytes it changes, so a wrong range turns that feature off.
 *
 * The ranges come from tools/re/map_addresses.py (v01.04 and HK Ghidra ELFs): each covers
 * the patched addresses of one region, all of which moved by the same amount there. */
enum { UNICORN_V0104, UNICORN_HK0105 };
static int unicorn_build = UNICORN_V0104;

static const struct { uint32_t start, end; int32_t shift; } unicorn_hk0105_ranges[] = {
    {0x2b550, 0x2b5d1, 384},     /* FUN_0002b590, the text lookup */
    {0x3d9bd, 0x3e31d, 384},     /* render size getters */
    {0x1fdf80, 0x1fe000, 944},   /* CRILAYLA decompressor */
    {0x233df4, 0x234d1b, 960},   /* text language, 2D ortho */
    {0x2377d0, 0x2379f0, 960},   /* battle map size limit */
    {0x2e0d9a, 0x2e186b, 2544},  /* battle map camera clamp */
    {0x3860e1, 0x38692b, 3008},  /* fade, haze */
    {0x735370, 0x73a01e, -9936}, /* Options screen */
    {0x940e39, 0x9458e8, -9616}, /* weather layer */
    {0xaf1f50, 0xaf1fe0, -8672}, /* rodata: full-screen fill corners */
    {0xaf3164, 0xaf3390, -8624}, /* rodata: screen size floats, 9/16 */
    {0xd7c900, 0xd7cbec, -8384}, /* rodata: Options tables */
    {0x14fcd70, 0x14fcdf0, 0},   /* .data: slider table */
    {0x1600ca8, 0x1602b18, 0},   /* .data: post-processing quad and rectangle */
};

/* A v01.04 image offset in the running build; 0 (no patch matches there) when it is outside
 * the known ranges. */
static inline uint32_t unicorn_address(uint32_t v0104) {
    if (unicorn_build == UNICORN_V0104) return v0104;
    for (unsigned i = 0; i < sizeof(unicorn_hk0105_ranges) / sizeof(*unicorn_hk0105_ranges); ++i)
        if (v0104 >= unicorn_hk0105_ranges[i].start && v0104 < unicorn_hk0105_ranges[i].end)
            return (uint32_t)((int32_t)v0104 + unicorn_hk0105_ranges[i].shift);
    return 0;
}

#endif

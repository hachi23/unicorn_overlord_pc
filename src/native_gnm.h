/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The resource descriptors the game's graphics library (Gnm) passes around, decoded for the
 * native renderer: textures, buffers, samplers, render targets and shader headers. The layouts
 * are the console GPU's resource words; nothing here talks to a GPU. */
#ifndef NATIVE_GNM_H
#define NATIVE_GNM_H
#include <stdint.h>
#include <string.h>

/* Shader stages as the game numbers them. */
enum { GNM_STAGE_CS = 0, GNM_STAGE_PS = 1, GNM_STAGE_VS = 2, GNM_STAGE_COUNT = 7 };
/* Primitive types (VGT_PRIMITIVE_TYPE). */
enum { GNM_PRIM_LIST = 0x4, GNM_PRIM_FAN = 0x5, GNM_PRIM_STRIP = 0x6, GNM_PRIM_RECTS = 0x11, GNM_PRIM_QUADS = 0x13 };

/* Surface formats (the data_format field of textures and buffers). */
enum {
    GNM_FMT_INVALID = 0, GNM_FMT_8 = 1, GNM_FMT_16 = 2, GNM_FMT_8_8 = 3, GNM_FMT_32 = 4,
    GNM_FMT_16_16 = 5, GNM_FMT_10_11_11 = 6, GNM_FMT_11_11_10 = 7, GNM_FMT_10_10_10_2 = 8,
    GNM_FMT_2_10_10_10 = 9, GNM_FMT_8_8_8_8 = 10, GNM_FMT_32_32 = 11, GNM_FMT_16_16_16_16 = 12,
    GNM_FMT_32_32_32 = 13, GNM_FMT_32_32_32_32 = 14, GNM_FMT_5_6_5 = 16, GNM_FMT_1_5_5_5 = 17,
    GNM_FMT_5_5_5_1 = 18, GNM_FMT_4_4_4_4 = 19, GNM_FMT_8_24 = 20, GNM_FMT_24_8 = 21,
    GNM_FMT_X24_8_32 = 22, GNM_FMT_BC1 = 35, GNM_FMT_BC2 = 36, GNM_FMT_BC3 = 37, GNM_FMT_BC4 = 38,
    GNM_FMT_BC5 = 39, GNM_FMT_BC6 = 40, GNM_FMT_BC7 = 41
};
/* Number formats. */
enum { GNM_NUM_UNORM = 0, GNM_NUM_SNORM = 1, GNM_NUM_USCALED = 2, GNM_NUM_SSCALED = 3,
       GNM_NUM_UINT = 4, GNM_NUM_SINT = 5, GNM_NUM_SNORM_NZ = 6, GNM_NUM_FLOAT = 7, GNM_NUM_SRGB = 9 };

/* A texture descriptor: 32 bytes. */
typedef struct {
    uint64_t address;                /* guest address of level 0 */
    uint32_t width, height, depth, pitch;
    uint32_t format, number, tiling, type;
    uint32_t base_level, last_level, base_array, last_array;
    uint8_t swizzle[4];              /* 0 zero, 1 one, 4 x, 5 y, 6 z, 7 w */
} GnmTexture;

static inline void gnm_texture(const void *raw, GnmTexture *t) {
    uint64_t q[4];
    memcpy(q, raw, sizeof(q));
    t->address = (q[0] & ((1ull << 38) - 1)) << 8;
    t->format = (uint32_t)(q[0] >> 52) & 0x3f;
    t->number = (uint32_t)(q[0] >> 58) & 0xf;
    t->width = ((uint32_t)q[1] & 0x3fff) + 1;
    t->height = ((uint32_t)(q[1] >> 14) & 0x3fff) + 1;
    for (int i = 0; i < 4; ++i) t->swizzle[i] = (uint8_t)((q[1] >> (32 + 3 * i)) & 7);
    t->base_level = (uint32_t)(q[1] >> 44) & 0xf;
    t->last_level = (uint32_t)(q[1] >> 48) & 0xf;
    t->tiling = (uint32_t)(q[1] >> 52) & 0x1f;
    t->type = (uint32_t)(q[1] >> 60) & 0xf;
    t->depth = ((uint32_t)q[2] & 0x1fff) + 1;
    t->pitch = ((uint32_t)(q[2] >> 13) & 0x3fff) + 1;
    t->base_array = (uint32_t)(q[2] >> 32) & 0x1fff;
    t->last_array = (uint32_t)(q[2] >> 45) & 0x1fff;
}

/* A buffer descriptor (vertex stream, constant buffer): 16 bytes. */
typedef struct {
    uint64_t address;
    uint32_t stride, count;          /* count is in strides (bytes when the stride is 0 or 1) */
    uint32_t format, number;
    uint8_t swizzle[4];
} GnmBuffer;

static inline void gnm_buffer(const void *raw, GnmBuffer *b) {
    uint64_t q;
    uint32_t w[2];
    memcpy(&q, raw, 8);
    memcpy(w, (const uint8_t *)raw + 8, 8);
    b->address = q & ((1ull << 44) - 1);
    b->stride = (uint32_t)(q >> 48) & 0x3fff;
    b->count = w[0];
    for (int i = 0; i < 4; ++i) b->swizzle[i] = (uint8_t)((w[1] >> (3 * i)) & 7);
    b->number = (w[1] >> 12) & 7;
    b->format = (w[1] >> 15) & 0xf;
}
static inline uint64_t gnm_buffer_bytes(const GnmBuffer *b) {
    return (uint64_t)(b->stride ? b->stride : 1) * b->count;
}

/* A colour render target: the first 64 bytes of the game's structure. */
typedef struct {
    uint64_t address;
    uint32_t width, height;          /* the visible size the game asked for */
    uint32_t pitch;                  /* in pixels */
    uint32_t format, number, swap, tiling;
} GnmTarget;

static inline void gnm_target(const void *raw, GnmTarget *t) {
    uint32_t w[16];
    memcpy(w, raw, sizeof(w));
    t->address = (uint64_t)w[0] << 8;
    t->pitch = ((w[1] & 0x7ff) + 1) * 8;
    t->format = (w[4] >> 2) & 0x1f;
    t->number = (w[4] >> 8) & 7;
    t->swap = (w[4] >> 11) & 3;
    t->tiling = w[5] & 0x1f;
    t->width = w[15] & 0xffff;
    t->height = w[15] >> 16;
}

/* The header shared by the game's vertex, pixel and compute shader objects. */
typedef struct {
    uint64_t code;                   /* guest address of the shader's machine code */
    uint32_t size;                   /* bytes of code */
    uint32_t hash;                   /* the compiler's hash of the shader, 0 when not found */
} GnmShader;

static inline void gnm_shader(const void *raw, GnmShader *s) {
    uint32_t w[4];
    memcpy(w, raw, sizeof(w));
    s->size = w[0] & 0x7fffff;
    s->code = ((uint64_t)w[2] << 8) | ((uint64_t)w[3] << 40);
    s->hash = 0;
    if (!s->code) return;
    /* The code starts with a move whose immediate is the distance, in 8-byte units less one, to
     * the compiler's footer ("OrbShdr", flags, then the hash). */
    const uint8_t *code = (const uint8_t *)(uintptr_t)s->code;
    uint32_t words;
    memcpy(&words, code + 4, 4);
    const uint64_t at = ((uint64_t)words + 1) * 8;
    if (at <= (1u << 20) && !memcmp(code + at, "OrbShdr", 7)) memcpy(&s->hash, code + at + 0x10, 4);
}

/* A fetch shader: the small GCN routine the vertex shader calls first to load its inputs. Each
 * input is one buffer load whose resource is a vertex buffer slot; the inputs are listed in
 * the order of their destination registers, which is the vertex shader's input order. */
typedef struct {
    uint32_t count;
    struct { uint8_t slot, vgpr, components, reserved; uint32_t offset; } input[16];
} GnmFetch;

/* 0 when the code is not understood. */
static inline int gnm_fetch(const uint32_t *code, GnmFetch *f) {
    int8_t sgpr_slot[104];
    memset(sgpr_slot, -1, sizeof(sgpr_slot));
    f->count = 0;
    for (int at = 0, n = 0; n < 128; ++n) {
        const uint32_t w = code[at];
        if ((w >> 27) == 0x18) {                                   /* SMRD */
            const uint32_t op = (w >> 22) & 0x1f, sdst = (w >> 15) & 0x7f;
            if (op == 2 && (w >> 8) & 1 && sdst + 3 < 104)          /* s_load_dwordx4 with an immediate */
                for (int k = 0; k < 4; ++k) sgpr_slot[sdst + k] = (int8_t)((w & 0xff) / 4);
            at += 1;
        } else if ((w >> 26) == 0x38 || (w >> 26) == 0x3a) {       /* MUBUF / MTBUF */
            const uint32_t w1 = code[at + 1];
            const int mubuf = (w >> 26) == 0x38;
            const uint32_t op = mubuf ? (w >> 18) & 0x7f : (w >> 16) & 7;
            uint32_t components = 0;
            if (op <= 3) components = op + 1;                       /* *_load_format_x .. xyzw */
            else if (mubuf && op == 0x0c) components = 1;
            else if (mubuf && op == 0x0d) components = 2;
            else if (mubuf && op == 0x0f) components = 3;
            else if (mubuf && op == 0x0e) components = 4;
            const uint32_t srsrc = ((w1 >> 16) & 0x1f) * 4;
            if (!components || srsrc >= 104 || sgpr_slot[srsrc] < 0 || f->count == 16) return 0;
            f->input[f->count].slot = (uint8_t)sgpr_slot[srsrc];
            f->input[f->count].vgpr = (uint8_t)((w1 >> 8) & 0xff);
            f->input[f->count].components = (uint8_t)components;
            f->input[f->count].reserved = 0;
            f->input[f->count].offset = w & 0xfff;
            ++f->count;
            at += 2;
        } else if ((w >> 23) == 0x17d) {                           /* SOP1 */
            if (((w >> 8) & 0xff) == 0x20) break;                   /* s_setpc_b64: back to the shader */
            at += 1 + ((w & 0xff) == 0xff);
        } else if ((w >> 23) == 0x17f || (w >> 28) == 0xb) {        /* SOPP, SOPK */
            at += 1;
        } else if ((w >> 30) == 2) {                                /* SOP2, SOPC */
            at += 1 + ((w & 0xff) == 0xff || ((w >> 8) & 0xff) == 0xff);
        } else if ((w >> 26) == 0x34) {                             /* VOP3 */
            at += 2;
        } else if ((w >> 31) == 0) {                                /* VOP1, VOP2, VOPC */
            at += 1 + ((w & 0x1ff) == 0xff);
        } else {
            return 0;
        }
    }
    /* Input order is register order. */
    for (uint32_t i = 1; i < f->count; ++i)
        for (uint32_t j = i; j > 0 && f->input[j].vgpr < f->input[j - 1].vgpr; --j) {
            const __typeof__(f->input[0]) t = f->input[j]; f->input[j] = f->input[j - 1]; f->input[j - 1] = t;
        }
    return 1;
}
#endif

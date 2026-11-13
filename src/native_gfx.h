/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The native renderer: the game's calls into its graphics library are taken at the library's
 * entry points and drawn with Vulkan directly, without the console's command buffers.
 *
 * VW_NATIVE (else renderer= in vw.ini, the overlay's "Renderer"):
 *   own, native  the native renderer draws and presents the frame; the game's graphics library
 *                calls are not run (what players switch on)
 *   trace        record the calls beside shadPS4 (frame lists, statistics)
 *   1            as trace, and also draw the recorded frames natively (comparison)
 *   0, shadps4   off (the default) */
#ifndef NATIVE_GFX_H
#define NATIVE_GFX_H
#include <stddef.h>
#include <stdint.h>

enum { NATIVE_OFF = 0, NATIVE_TRACE = 1, NATIVE_DRAW = 2 };

/* Sets the mode from the text above (the probe, before native_gfx_install). */
void native_gfx_select(const char *text);
/* NATIVE_OFF, NATIVE_TRACE or NATIVE_DRAW. */
int native_gfx_mode(void);
/* Hooks the graphics library of the loaded v01.04 image. stubs: zeroed writable memory within
 * rel32 reach of the image, made executable by the caller afterwards. 0 when the image's code
 * is not what the hooks expect (nothing is changed then). */
int native_gfx_install(unsigned char *image, unsigned char *stubs, size_t stub_bytes);

/* ---- A recorded frame: lists of commands in the order the game submitted them. ---- */
enum {
    NC_TARGET = 1,      /* u32 slot, u32 present, raw[64] */
    NC_DEPTH_TARGET,    /* u32 present, raw[48] */
    NC_TARGET_MASK,     /* u32 mask */
    NC_VIEWPORT,        /* i32 l, t, r, b; f32 zscale, zoffset */
    NC_SCISSOR,         /* i32 l, t, r, b */
    NC_BLEND,           /* u32 slot, u32 bits */
    NC_CB_CONTROL,      /* u32 mode, u32 rop */
    NC_DEPTH_CONTROL,   /* u32 bits */
    NC_DB_RENDER,       /* u32 bits */
    NC_DEPTH_CLEAR,     /* f32 value */
    NC_PRIM_SETUP,      /* u32 bits */
    NC_CLIP,            /* u32 bits */
    NC_PRIM_TYPE,       /* u32 type */
    NC_INDEX_SIZE,      /* u32 size */
    NC_VS,              /* u64 object, u64 code, u32 size, u32 hash */
    NC_PS,              /* same */
    NC_CS,              /* same */
    NC_TEXTURE,         /* u32 stage, u32 slot, u32 present, u32 rw, raw[32] */
    NC_SAMPLER,         /* u32 stage, u32 slot, u32 present, u32 0, raw[16] */
    NC_CONSTANTS,       /* u32 stage, u32 slot, u32 bytes, u32 0, u64 address, data[] */
    NC_RW_BUFFER,       /* u32 stage, u32 slot, u32 present, u32 0, raw[16] */
    NC_DRAW,            /* NativeDraw, then streams, ranges, index data, vertex data */
    NC_DISPATCH,        /* u32 x, y, z */
    NC_FILL,            /* u64 address, u32 words, u32 values, u32 value[2] */
    NC_COUNT
};

typedef struct { uint32_t op, bytes; } NativeCommand; /* bytes: the payload, a multiple of 8 */

typedef struct {
    uint32_t index_count, index_bytes;   /* index_bytes: 2 or 4 per index */
    uint32_t vertex_count;               /* highest index + 1 */
    uint32_t stream_count, range_count;
    uint32_t reserved;
    uint64_t index_address;              /* where the game kept the indices */
} NativeDraw;
/* One vertex input of the bound vertex shader. */
typedef struct {
    uint32_t slot, range;                /* range: index into the draw's ranges */
    uint32_t offset, stride;             /* offset of vertex 0 inside the range */
    uint32_t format, number;
    uint8_t swizzle[4];
    uint32_t location;                   /* the vertex shader input it feeds; NATIVE_NO_LOCATION when unknown */
} NativeStream;
#define NATIVE_NO_LOCATION 0xffu
typedef struct { uint64_t address; uint32_t bytes, data; } NativeRange; /* data: offset from the payload */

typedef struct NativeList {
    struct NativeList *next;
    uint8_t *data;
    size_t used, capacity;
    uint64_t context;                    /* the game's context the list was recorded on */
    uint32_t draws, flip;                /* flip: the list ended a frame */
    /* Recording state: the vertex streams bound for the vertex stage. */
    uint8_t stream_raw[32][16];
    uint32_t stream_mask, index_bytes;
    /* The bound vertex shader's inputs (from its fetch shader): slot and byte offset of each,
     * in input order. fetch_known is 0 when the fetch shader was not understood. */
    uint8_t fetch_slot[16];
    uint32_t fetch_offset[16], fetch_count, fetch_known;
} NativeList;

/* ---- The renderer's side (native_vk.c). ---- */
/* A frame's lists, oldest first; the renderer owns them and frees them with native_list_free. */
void native_render_frame(NativeList *lists);
int native_render_init(void);
/* The CPU reads a render target the native renderer drew (VW_NATIVE=own: the game detiles it
 * itself, e.g. for save thumbnails): copies columns left..right-1 of rows top..bottom-1 to out
 * (pitch pixels per row) in the target's byte order, as of the last replayed frame. Returns 0
 * when address is not such a target. Any thread. */
int native_target_read(uint64_t address, void *out, uint32_t left, uint32_t top, uint32_t right,
                       uint32_t bottom, uint32_t pitch);
void native_list_free(NativeList *list);
/* Writes a frame's lists as text (the recorder's dump format). */
void native_dump_frame(NativeList *lists, const char *path);
#endif

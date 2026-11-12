/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Records the game's calls into its statically linked graphics library (Unicorn Overlord
 * v01.04, image offsets 0x214000-0x21d000) as lists of commands, one list per graphics
 * context, handed over in the order the game submits them. See native_gfx.h. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "native_gfx.h"
#include "native_gnm.h"
#include "runtime.h"

static int mode;
/* VW_NATIVE=own: the native renderer owns the frame. The hooks record and do not run the game's
 * own graphics code, so the command buffers it submits keep only what the hooks do not cover
 * (synchronisation, labels, flips); the presenter shows the native image. */
static int own;

void native_gfx_select(const char *text) {
    own = 0;
    if (!text || !*text || !strcmp(text, "0") || !strcmp(text, "shadps4")) mode = NATIVE_OFF;
    else if (!strcmp(text, "trace")) mode = NATIVE_TRACE;
    else if (!strcmp(text, "own") || !strcmp(text, "native")) mode = NATIVE_DRAW, own = 1;
    else mode = NATIVE_DRAW;
}

int native_gfx_mode(void) {
    return mode;
}

/* ---- Lists ---- */
enum { CONTEXTS = 64, LCUE_OFFSET = 0x1c8 };
static struct { uint64_t context; NativeList *list; } contexts[CONTEXTS];
static volatile int contexts_lock;
static NativeList *frame_head, *frame_tail;       /* submitted lists of the frame being built */
static uint64_t frames, stat_draws, stat_bytes, stat_lists, stat_commands;
static volatile uint64_t stat_ns;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void native_list_free(NativeList *list) {
    if (!list) return;
    free(list->data);
    free(list);
}

static NativeList *list_of(uint64_t context) {
    for (int i = 0; i < CONTEXTS; ++i)
        if (contexts[i].context == context) {
            if (!contexts[i].list) {
                NativeList *list = calloc(1, sizeof(*list));
                if (!list) abort();
                list->context = context;
                list->index_bytes = 2;
                contexts[i].list = list;
            }
            return contexts[i].list;
        }
    while (__atomic_exchange_n(&contexts_lock, 1, __ATOMIC_ACQUIRE)) { }
    int slot = -1;
    for (int i = 0; i < CONTEXTS && slot < 0; ++i)
        if (!contexts[i].context) slot = i;
    if (slot < 0) { fprintf(stderr, "Native: more than %d graphics contexts\n", CONTEXTS); abort(); }
    contexts[slot].list = NULL;
    __atomic_store_n(&contexts[slot].context, context, __ATOMIC_RELEASE);
    __atomic_store_n(&contexts_lock, 0, __ATOMIC_RELEASE);
    return list_of(context);
}

/* Room for a command with `bytes` of payload; returns the payload. */
static void *emit(NativeList *list, uint32_t op, size_t bytes) {
    bytes = (bytes + 7) & ~(size_t)7;
    const size_t need = list->used + sizeof(NativeCommand) + bytes;
    if (need > list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : (size_t)1 << 16;
        while (capacity < need) capacity *= 2;
        list->data = realloc(list->data, capacity);
        if (!list->data) abort();
        list->capacity = capacity;
    }
    NativeCommand *command = (NativeCommand *)(list->data + list->used);
    command->op = op;
    command->bytes = (uint32_t)bytes;
    list->used = need;
    return command + 1;
}
static void emit_words(NativeList *list, uint32_t op, const uint32_t *words, unsigned count) {
    memcpy(emit(list, op, count * 4u), words, count * 4u);
}

/* ---- The library's functions (trampolines to the game's own code) ---- */
#define GFX_FUNCTIONS(X) \
    X(0x2146d0, target_mask,   void, (void *c, uint32_t mask), (c, mask)) \
    X(0x214730, blend,         void, (void *c, int slot, uint32_t bits), (c, slot, bits)) \
    X(0x2147a0, cb_control,    void, (void *c, uint32_t m, uint32_t rop), (c, m, rop)) \
    X(0x214810, depth_control, void, (void *c, uint32_t bits), (c, bits)) \
    X(0x214870, depth_clear,   void, (void *c, float value), (c, value)) \
    X(0x215a50, db_render,     void, (void *c, uint32_t bits), (c, bits)) \
    X(0x214ac0, depth_target,  void, (void *c, const void *target), (c, target)) \
    X(0x218ae0, target,        void, (void *c, uint32_t slot, const void *target), (c, slot, target)) \
    X(0x214de0, prim_type,     void, (void *c, uint32_t type), (c, type)) \
    X(0x2150b0, index_size,    void, (void *c, uint32_t size, uint32_t policy), (c, size, policy)) \
    X(0x214e50, draw_index,    void, (void *c, uint32_t count, const void *indices, int flags), (c, count, indices, flags)) \
    X(0x214ed0, dispatch,      void, (void *c, uint32_t x, uint32_t y, uint32_t z, int flags), (c, x, y, z, flags)) \
    X(0x215960, scissor,       void, (void *c, uint32_t l, int t, uint32_t r, int b), (c, l, t, r, b)) \
    X(0x215ab0, clip,          void, (void *c, uint32_t bits), (c, bits)) \
    X(0x215b10, prim_setup,    void, (void *c, uint32_t bits), (c, bits)) \
    X(0x21c1f0, viewport,      void, (void *c, uint32_t l, uint32_t t, int r, int b, float zs, float zo), (c, l, t, r, b, zs, zo)) \
    X(0x218bf0, textures,      void, (void *l, uint32_t stage, uint32_t slot, const void *t), (l, stage, slot, t)) \
    X(0x218df0, buffers,       void, (void *l, uint32_t stage, uint32_t slot, const void *t), (l, stage, slot, t)) \
    X(0x218fe0, samplers,      void, (void *l, uint64_t stage, uint32_t slot, const void *s), (l, stage, slot, s)) \
    X(0x2191b0, constants,     void, (void *l, uint32_t stage, uint32_t slot, const void *b), (l, stage, slot, b)) \
    X(0x2193a0, rw_buffers,    void, (void *l, uint64_t stage, uint32_t slot, const void *b), (l, stage, slot, b)) \
    X(0x219580, vertex_buffers,void, (void *l, uint32_t stage, uint32_t slot, int count, const void *b), (l, stage, slot, count, b)) \
    X(0x2199b0, vs,            void, (void *l, const void *shader, uint64_t modifier, uint64_t fetch, void *table), (l, shader, modifier, fetch, table)) \
    X(0x219b50, ps,            void, (void *l, const void *shader, void *table), (l, shader, table)) \
    X(0x219d00, cs,            void, (void *l, const void *shader, void *table), (l, shader, table)) \
    X(0x21be90, submit,        int,  (void *c, uint64_t workload), (c, workload)) \
    X(0x21c050, submit_flip,   int,  (uint64_t workload, uint32_t handle, uint32_t buffer, uint32_t flip, uint64_t arg, void *c), (workload, handle, buffer, flip, arg, c)) \
    X(0x3ede0,  fill,          void, (uint64_t unused, void *c, uint64_t address, int words, const uint32_t *value, int values), (unused, c, address, words, value, values)) \
    X(0x20df20, detile,        int,  (void *out, const void *in, const void *tiling, const uint32_t *region, uint32_t pitch, uint32_t slice_pitch), (out, in, tiling, region, pitch, slice_pitch))

#define X(at, name, ret, params, args) static ret (ABI *game_##name) params;
GFX_FUNCTIONS(X)
#undef X

static NativeList *of_lcue(void *lcue) { return list_of((uint64_t)(uintptr_t)lcue - LCUE_OFFSET); }
static NativeList *of_context(void *context) { return list_of((uint64_t)(uintptr_t)context); }

static ABI void hook_target_mask(void *c, uint32_t mask) {
    emit_words(of_context(c), NC_TARGET_MASK, &mask, 1);
    if (!own) game_target_mask(c, mask);
}
static ABI void hook_blend(void *c, int slot, uint32_t bits) {
    const uint32_t w[2] = {(uint32_t)slot, bits};
    emit_words(of_context(c), NC_BLEND, w, 2);
    if (!own) game_blend(c, slot, bits);
}
static ABI void hook_cb_control(void *c, uint32_t m, uint32_t rop) {
    const uint32_t w[2] = {m, rop};
    emit_words(of_context(c), NC_CB_CONTROL, w, 2);
    if (!own) game_cb_control(c, m, rop);
}
static ABI void hook_depth_control(void *c, uint32_t bits) {
    emit_words(of_context(c), NC_DEPTH_CONTROL, &bits, 1);
    if (!own) game_depth_control(c, bits);
}
static ABI void hook_depth_clear(void *c, float value) {
    uint32_t w;
    memcpy(&w, &value, 4);
    emit_words(of_context(c), NC_DEPTH_CLEAR, &w, 1);
    if (!own) game_depth_clear(c, value);
}
static ABI void hook_db_render(void *c, uint32_t bits) {
    emit_words(of_context(c), NC_DB_RENDER, &bits, 1);
    if (!own) game_db_render(c, bits);
}
static ABI void hook_depth_target(void *c, const void *target) {
    uint32_t *p = emit(of_context(c), NC_DEPTH_TARGET, 8 + 48);
    p[0] = target != NULL; p[1] = 0;
    if (target) memcpy(p + 2, target, 48); else memset(p + 2, 0, 48);
    if (!own) game_depth_target(c, target);
}
static ABI void hook_target(void *c, uint32_t slot, const void *target) {
    uint32_t *p = emit(of_context(c), NC_TARGET, 8 + 64);
    p[0] = slot; p[1] = target != NULL;
    if (target) memcpy(p + 2, target, 64); else memset(p + 2, 0, 64);
    if (!own) game_target(c, slot, target);
}
static ABI void hook_prim_type(void *c, uint32_t type) {
    emit_words(of_context(c), NC_PRIM_TYPE, &type, 1);
    if (!own) game_prim_type(c, type);
}
static ABI void hook_index_size(void *c, uint32_t size, uint32_t policy) {
    NativeList *list = of_context(c);
    list->index_bytes = size ? 4 : 2;
    emit_words(list, NC_INDEX_SIZE, &size, 1);
    if (!own) game_index_size(c, size, policy);
}
static ABI void hook_scissor(void *c, uint32_t l, int t, uint32_t r, int b) {
    const uint32_t w[4] = {l, (uint32_t)t, r, (uint32_t)b};
    emit_words(of_context(c), NC_SCISSOR, w, 4);
    if (!own) game_scissor(c, l, t, r, b);
}
static ABI void hook_clip(void *c, uint32_t bits) {
    emit_words(of_context(c), NC_CLIP, &bits, 1);
    if (!own) game_clip(c, bits);
}
static ABI void hook_prim_setup(void *c, uint32_t bits) {
    emit_words(of_context(c), NC_PRIM_SETUP, &bits, 1);
    if (!own) game_prim_setup(c, bits);
}
static ABI void hook_viewport(void *c, uint32_t l, uint32_t t, int r, int b, float zs, float zo) {
    uint32_t w[6] = {l, t, (uint32_t)r, (uint32_t)b, 0, 0};
    memcpy(w + 4, &zs, 4);
    memcpy(w + 5, &zo, 4);
    emit_words(of_context(c), NC_VIEWPORT, w, 6);
    if (!own) game_viewport(c, l, t, r, b, zs, zo);
}
static void record_texture(void *l, uint32_t stage, uint32_t slot, const void *t, uint32_t rw) {
    uint32_t *p = emit(of_lcue(l), NC_TEXTURE, 16 + 32);
    p[0] = stage; p[1] = slot; p[2] = t != NULL; p[3] = rw;
    if (t) memcpy(p + 4, t, 32); else memset(p + 4, 0, 32);
}
static ABI void hook_textures(void *l, uint32_t stage, uint32_t slot, const void *t) {
    record_texture(l, stage, slot, t, 0);
    if (!own) game_textures(l, stage, slot, t);
}
static ABI void hook_buffers(void *l, uint32_t stage, uint32_t slot, const void *b) {
    uint32_t *p = emit(of_lcue(l), NC_RW_BUFFER, 16 + 16);
    p[0] = stage; p[1] = slot; p[2] = b != NULL; p[3] = 1; /* read only */
    if (b) memcpy(p + 4, b, 16); else memset(p + 4, 0, 16);
    if (!own) game_buffers(l, stage, slot, b);
}
static ABI void hook_samplers(void *l, uint64_t stage, uint32_t slot, const void *s) {
    uint32_t *p = emit(of_lcue(l), NC_SAMPLER, 16 + 16);
    p[0] = (uint32_t)stage; p[1] = slot; p[2] = s != NULL; p[3] = 0;
    if (s) memcpy(p + 4, s, 16); else memset(p + 4, 0, 16);
    if (!own) game_samplers(l, stage, slot, s);
}
static ABI void hook_constants(void *l, uint32_t stage, uint32_t slot, const void *b) {
    NativeList *list = of_lcue(l);
    GnmBuffer buffer = {0};
    if (b) gnm_buffer(b, &buffer);
    uint64_t bytes = b ? gnm_buffer_bytes(&buffer) : 0;
    if (bytes > (1u << 20)) bytes = 0;
    uint32_t *p = emit(list, NC_CONSTANTS, 24 + bytes);
    p[0] = stage; p[1] = slot; p[2] = (uint32_t)bytes; p[3] = 0;
    memcpy(p + 4, &buffer.address, 8);
    if (bytes) memcpy(p + 6, (const void *)(uintptr_t)buffer.address, bytes);
    __atomic_add_fetch(&stat_bytes, bytes, __ATOMIC_RELAXED);
    if (!own) game_constants(l, stage, slot, b);
}
static ABI void hook_rw_buffers(void *l, uint64_t stage, uint32_t slot, const void *b) {
    uint32_t *p = emit(of_lcue(l), NC_RW_BUFFER, 16 + 16);
    p[0] = (uint32_t)stage; p[1] = slot; p[2] = b != NULL; p[3] = 0;
    if (b) memcpy(p + 4, b, 16); else memset(p + 4, 0, 16);
    if (!own) game_rw_buffers(l, stage, slot, b);
}
static ABI void hook_vertex_buffers(void *l, uint32_t stage, uint32_t slot, int count, const void *b) {
    if (stage == GNM_STAGE_VS) {
        NativeList *list = of_lcue(l);
        for (int i = 0; i < count && slot + (uint32_t)i < 32; ++i) {
            if (b) {
                memcpy(list->stream_raw[slot + i], (const uint8_t *)b + 16 * i, 16);
                list->stream_mask |= 1u << (slot + i);
            } else {
                list->stream_mask &= ~(1u << (slot + i));
            }
        }
    }
    if (!own) game_vertex_buffers(l, stage, slot, count, b);
}
static void record_shader(void *l, uint32_t op, const void *shader) {
    uint64_t *p = emit(of_lcue(l), op, 24);
    GnmShader s = {0};
    if (shader) gnm_shader(shader, &s);
    p[0] = (uint64_t)(uintptr_t)shader;
    p[1] = s.code;
    p[2] = (uint64_t)s.size | ((uint64_t)s.hash << 32);
}
static ABI void hook_vs(void *l, const void *shader, uint64_t modifier, uint64_t fetch, void *table) {
    record_shader(l, NC_VS, shader);
    NativeList *list = of_lcue(l);
    GnmFetch f;
    list->fetch_known = fetch && gnm_fetch((const uint32_t *)(uintptr_t)fetch, &f);
    list->fetch_count = 0;
    if (list->fetch_known) {
        list->fetch_count = f.count;
        for (uint32_t i = 0; i < f.count; ++i) { list->fetch_slot[i] = f.input[i].slot; list->fetch_offset[i] = f.input[i].offset; }
    }
    /* Each fetch shader is described once in the log. */
    static uint64_t seen[256];
    static uint32_t seen_count;
    uint32_t k = 0;
    while (k < __atomic_load_n(&seen_count, __ATOMIC_ACQUIRE) && seen[k] != fetch) ++k;
    if (k == seen_count && k < 256) {
        seen[seen_count] = fetch;
        __atomic_store_n(&seen_count, k + 1, __ATOMIC_RELEASE);
        GnmShader s = {0};
        if (shader) gnm_shader(shader, &s);
        char text[256];
        int n = snprintf(text, sizeof(text), "Native: vs %08x fetch %" PRIx64 ":", s.hash, fetch);
        if (!list->fetch_known) n += snprintf(text + n, sizeof(text) - n, " not understood");
        for (uint32_t i = 0; i < f.count && list->fetch_known && n < 200; ++i)
            n += snprintf(text + n, sizeof(text) - n, " in%u=slot%u+%u/v%u x%u", i, f.input[i].slot, f.input[i].offset,
                          f.input[i].vgpr, f.input[i].components);
        puts(text);
    }
    if (!own) game_vs(l, shader, modifier, fetch, table);
}
static ABI void hook_ps(void *l, const void *shader, void *table) {
    record_shader(l, NC_PS, shader);
    if (!own) game_ps(l, shader, table);
}
static ABI void hook_cs(void *l, const void *shader, void *table) {
    record_shader(l, NC_CS, shader);
    if (!own) game_cs(l, shader, table);
}

/* A draw: the indices and the part of every bound vertex stream they reach are copied, since
 * the game reuses that memory before the renderer gets to the list. */
static ABI void hook_draw_index(void *c, uint32_t count, const void *indices, int flags) {
    const uint64_t start = now_ns();
    NativeList *list = of_context(c);
    const uint32_t index_bytes = list->index_bytes;
    uint32_t highest = 0;
    if (index_bytes == 2) {
        const uint16_t *p = indices;
        for (uint32_t i = 0; i < count; ++i) if (p[i] > highest) highest = p[i];
    } else {
        const uint32_t *p = indices;
        for (uint32_t i = 0; i < count; ++i) if (p[i] > highest) highest = p[i];
    }
    const uint32_t vertices = count ? highest + 1 : 0;
    NativeStream streams[32];
    struct { uint64_t start, end; } ranges[32];
    uint32_t stream_count = 0, range_count = 0;
    /* Only the streams the vertex shader reads, in its input order, when its fetch shader is
     * known; otherwise every bound stream. */
    const uint32_t wanted = list->fetch_known ? list->fetch_count : 32;
    for (uint32_t i = 0; i < wanted; ++i) {
        const uint32_t slot = list->fetch_known ? list->fetch_slot[i] : i;
        if (!(list->stream_mask & (1u << slot))) continue;
        GnmBuffer buffer;
        gnm_buffer(list->stream_raw[slot], &buffer);
        if (!buffer.address) continue;
        buffer.address += list->fetch_known ? list->fetch_offset[i] : 0;
        const uint32_t stride = buffer.stride ? buffer.stride : 1;
        uint32_t reach = vertices < buffer.count ? vertices : buffer.count;
        /* The last vertex's element can be shorter than the stride; 16 bytes covers every format. */
        const uint64_t begin = buffer.address;
        const uint64_t end = begin + (reach ? (uint64_t)(reach - 1) * stride + 16 : 0);
        NativeStream *s = &streams[stream_count++];
        memset(s, 0, sizeof(*s));
        s->slot = slot; s->stride = stride; s->format = buffer.format; s->number = buffer.number;
        s->location = list->fetch_known ? i : NATIVE_NO_LOCATION;
        memcpy(s->swizzle, buffer.swizzle, 4);
        /* Interleaved inputs share one range. */
        uint32_t r = 0;
        for (; r < range_count; ++r)
            if (begin <= ranges[r].end && end >= ranges[r].start) break;
        if (r == range_count) { ranges[range_count].start = begin; ranges[range_count].end = end; ++range_count; }
        else {
            if (begin < ranges[r].start) ranges[r].start = begin;
            if (end > ranges[r].end) ranges[r].end = end;
        }
        s->range = r;
        s->offset = 0;
    }
    size_t bytes = sizeof(NativeDraw) + stream_count * sizeof(NativeStream) + range_count * sizeof(NativeRange);
    const size_t index_at = bytes;
    bytes += ((size_t)count * index_bytes + 7) & ~(size_t)7;
    size_t range_at[32];
    for (uint32_t r = 0; r < range_count; ++r) {
        range_at[r] = bytes;
        bytes += ((size_t)(ranges[r].end - ranges[r].start) + 7) & ~(size_t)7;
    }
    uint8_t *payload = emit(list, NC_DRAW, bytes);
    NativeDraw *draw = (NativeDraw *)payload;
    draw->index_count = count; draw->index_bytes = index_bytes; draw->vertex_count = vertices;
    draw->stream_count = stream_count; draw->range_count = range_count; draw->reserved = 0;
    draw->index_address = (uint64_t)(uintptr_t)indices;
    NativeStream *out_streams = (NativeStream *)(draw + 1);
    NativeRange *out_ranges = (NativeRange *)(out_streams + stream_count);
    for (uint32_t i = 0; i < stream_count; ++i) {
        GnmBuffer buffer;
        gnm_buffer(list->stream_raw[streams[i].slot], &buffer);
        if (streams[i].location != NATIVE_NO_LOCATION) buffer.address += list->fetch_offset[streams[i].location];
        streams[i].offset = (uint32_t)(buffer.address - ranges[streams[i].range].start);
        out_streams[i] = streams[i];
    }
    for (uint32_t r = 0; r < range_count; ++r) {
        out_ranges[r].address = ranges[r].start;
        out_ranges[r].bytes = (uint32_t)(ranges[r].end - ranges[r].start);
        out_ranges[r].data = (uint32_t)range_at[r];
        memcpy(payload + range_at[r], (const void *)(uintptr_t)ranges[r].start, out_ranges[r].bytes);
    }
    memcpy(payload + index_at, indices, (size_t)count * index_bytes);
    ++list->draws;
    __atomic_add_fetch(&stat_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stat_ns, now_ns() - start, __ATOMIC_RELAXED);
    if (!own) game_draw_index(c, count, indices, flags);
}
static ABI void hook_dispatch(void *c, uint32_t x, uint32_t y, uint32_t z, int flags) {
    const uint32_t w[3] = {x, y, z};
    emit_words(of_context(c), NC_DISPATCH, w, 3);
    if (!own) game_dispatch(c, x, y, z, flags);
}
/* The game clears memory (render targets, mostly) with a compute shader that writes a value. */
static ABI void hook_fill(uint64_t unused, void *c, uint64_t address, int words, const uint32_t *value, int values) {
    uint32_t *p = emit(of_context(c), NC_FILL, 24);
    memcpy(p, &address, 8);
    p[2] = (uint32_t)words; p[3] = (uint32_t)values;
    p[4] = value ? value[0] : 0; p[5] = value && values > 1 ? value[1] : 0;
    if (!own) game_fill(unused, c, address, words, value, values);
}

/* ---- Frames ---- */
static const char *const command_names[NC_COUNT] = {
    "?", "target", "depth_target", "target_mask", "viewport", "scissor", "blend", "cb_control",
    "depth_control", "db_render", "depth_clear", "prim_setup", "clip", "prim_type", "index_size",
    "vs", "ps", "cs", "texture", "sampler", "constants", "rw_buffer", "draw", "dispatch", "fill"};

void native_dump_frame(NativeList *lists, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    int n = 0;
    for (NativeList *list = lists; list; list = list->next, ++n) {
        fprintf(f, "== list %d context %" PRIx64 " bytes %zu draws %u%s\n", n, list->context, list->used,
                list->draws, list->flip ? " FLIP" : "");
        for (size_t at = 0; at < list->used;) {
            const NativeCommand *command = (const NativeCommand *)(list->data + at);
            const uint32_t *w = (const uint32_t *)(command + 1);
            const uint8_t *payload = (const uint8_t *)w;
            fprintf(f, "%s", command->op < NC_COUNT ? command_names[command->op] : "?");
            switch (command->op) {
            case NC_TARGET: {
                GnmTarget t;
                gnm_target(w + 2, &t);
                if (w[1]) fprintf(f, " slot %u addr %" PRIx64 " %ux%u pitch %u fmt %u num %u swap %u tile %u", w[0],
                                  t.address, t.width, t.height, t.pitch, t.format, t.number, t.swap, t.tiling);
                else fprintf(f, " slot %u none", w[0]);
                break;
            }
            case NC_DEPTH_TARGET:
                if (w[0]) fprintf(f, " zinfo %x zread %x zwrite %x size %x", w[2], w[4], w[6], w[8]);
                else fprintf(f, " none");
                break;
            case NC_VIEWPORT: {
                float z[2];
                memcpy(z, w + 4, 8);
                fprintf(f, " %d %d %d %d z %g %g", (int)w[0], (int)w[1], (int)w[2], (int)w[3], z[0], z[1]);
                break;
            }
            case NC_SCISSOR: fprintf(f, " %d %d %d %d", (int)w[0], (int)w[1], (int)w[2], (int)w[3]); break;
            case NC_VS: case NC_PS: case NC_CS: {
                uint64_t q[3];
                memcpy(q, w, 24);
                fprintf(f, " hash %08x size %u code %" PRIx64, (uint32_t)(q[2] >> 32), (uint32_t)q[2], q[1]);
                break;
            }
            case NC_TEXTURE: {
                GnmTexture t;
                gnm_texture(w + 4, &t);
                if (w[2]) fprintf(f, " stage %u slot %u%s addr %" PRIx64 " %ux%u pitch %u fmt %u num %u tile %u type %u levels %u-%u swz %u%u%u%u",
                                  w[0], w[1], w[3] ? " rw" : "", t.address, t.width, t.height, t.pitch, t.format,
                                  t.number, t.tiling, t.type, t.base_level, t.last_level, t.swizzle[0],
                                  t.swizzle[1], t.swizzle[2], t.swizzle[3]);
                else fprintf(f, " stage %u slot %u none", w[0], w[1]);
                break;
            }
            case NC_SAMPLER: fprintf(f, " stage %u slot %u %08x %08x %08x %08x", w[0], w[1], w[4], w[5], w[6], w[7]); break;
            case NC_CONSTANTS: {
                fprintf(f, " stage %u slot %u bytes %u :", w[0], w[1], w[2]);
                const uint32_t shown = w[2] < 128 ? w[2] : 128;
                for (uint32_t i = 0; i + 4 <= shown; i += 4) {
                    float v;
                    memcpy(&v, payload + 24 + i, 4);
                    fprintf(f, " %g", v);
                }
                break;
            }
            case NC_DRAW: {
                const NativeDraw *draw = (const NativeDraw *)payload;
                const NativeStream *streams = (const NativeStream *)(draw + 1);
                const NativeRange *ranges = (const NativeRange *)(streams + draw->stream_count);
                fprintf(f, " indices %u x%u vertices %u |", draw->index_count, draw->index_bytes, draw->vertex_count);
                for (uint32_t i = 0; i < draw->stream_count; ++i)
                    fprintf(f, " s%u(r%u+%u stride %u fmt %u/%u)", streams[i].slot, streams[i].range,
                            streams[i].offset, streams[i].stride, streams[i].format, streams[i].number);
                for (uint32_t r = 0; r < draw->range_count; ++r) fprintf(f, " r%u=%u", r, ranges[r].bytes);
                break;
            }
            case NC_FILL: {
                uint64_t address;
                memcpy(&address, w, 8);
                fprintf(f, " addr %" PRIx64 " words %u values %u %08x %08x", address, w[2], w[3], w[4], w[5]);
                break;
            }
            default:
                for (uint32_t i = 0; i < command->bytes / 4 && i < 6; ++i) fprintf(f, " %x", w[i]);
            }
            fputc('\n', f);
            at += sizeof(*command) + command->bytes;
        }
    }
    fclose(f);
}

/* The context's list is finished: it joins the frame. On a flip the frame goes to the renderer
 * (or is counted and dropped). */
static void submit_list(void *c, int flip) {
    const uint64_t context = (uint64_t)(uintptr_t)c;
    NativeList *list = NULL;
    for (int i = 0; i < CONTEXTS; ++i)
        if (contexts[i].context == context) { list = contexts[i].list; contexts[i].list = NULL; break; }
    if (!list) {
        list = calloc(1, sizeof(*list));
        if (!list) abort();
        list->context = context;
    }
    list->flip = (uint32_t)flip;
    list->next = NULL;
    if (frame_tail) frame_tail->next = list; else frame_head = list;
    frame_tail = list;
    stat_draws += list->draws;
    ++stat_lists;
    for (size_t at = 0; at < list->used; at += sizeof(NativeCommand) + ((NativeCommand *)(list->data + at))->bytes)
        ++stat_commands;
    if (!flip) return;
    ++frames;
    const char *trigger = getenv("VW_NATIVE_DUMP_TRIGGER");
    if (trigger && *trigger) {
        FILE *f = fopen(trigger, "r");
        if (f) {
            char path[4096];
            fclose(f);
            remove(trigger);
            const char *dir = getenv("VW_NATIVE_DUMP_DIR");
            snprintf(path, sizeof(path), "%s/native_frame_%06" PRIu64 ".txt", dir && *dir ? dir : ".", frames);
            native_dump_frame(frame_head, path);
            printf("Native: frame %" PRIu64 " written to %s\n", frames, path);
            fflush(stdout);
        }
    }
    if (frames % 600 == 0) {
        printf("Native: %" PRIu64 " frames; per frame %.1f lists, %.0f draws, %.0f commands, %.0f KiB copied, %.2f ms recording draws\n",
               frames, stat_lists / 600.0, stat_draws / 600.0, stat_commands / 600.0,
               (double)__atomic_exchange_n(&stat_bytes, 0, __ATOMIC_RELAXED) / 600.0 / 1024.0,
               (double)__atomic_exchange_n(&stat_ns, 0, __ATOMIC_RELAXED) / 600.0 / 1e6);
        fflush(stdout);
        stat_lists = stat_draws = stat_commands = 0;
    }
    NativeList *lists = frame_head;
    frame_head = frame_tail = NULL;
    if (mode == NATIVE_DRAW) {
        native_render_frame(lists);
    } else {
        while (lists) { NativeList *next = lists->next; native_list_free(lists); lists = next; }
    }
}
static ABI int hook_submit(void *c, uint64_t workload) {
    submit_list(c, 0);
    return game_submit(c, workload);
}
/* GpuAddress::detileSurfaceRegion(out, tiled, tiling parameters, region {left, top, front, right,
 * bottom, back}, pitch, slice pitch): the CPU reading a GPU-rendered surface, e.g. the save
 * thumbnail (FnThumbCapture). In own mode that memory was never drawn: read the native image. */
static ABI int hook_detile(void *out, const void *in, const void *tiling, const uint32_t *region, uint32_t pitch, uint32_t slice_pitch) {
    if (own && region && region[2] == 0 && region[5] == 1 &&
        native_target_read((uint64_t)(uintptr_t)in, out, region[0], region[1], region[3], region[4], pitch))
        return 0;
    return game_detile(out, in, tiling, region, pitch, slice_pitch);
}

static ABI int hook_submit_flip(uint64_t workload, uint32_t handle, uint32_t buffer, uint32_t flip, uint64_t arg, void *c) {
    submit_list(c, 1);
    return game_submit_flip(workload, handle, buffer, flip, arg, c);
}

/* ---- Installation ---- */
/* Every hooked function starts with push rbp; mov rbp, rsp; push <register>. */
static unsigned prologue_bytes(const unsigned char *code) {
    static const unsigned char frame[4] = {0x55, 0x48, 0x89, 0xe5};
    if (memcmp(code, frame, 4)) return 0;
    if (code[4] == 0x41 && code[5] >= 0x50 && code[5] <= 0x57) return 6;
    if (code[4] >= 0x50 && code[4] <= 0x57) return 5;
    return 0;
}

int native_gfx_install(unsigned char *image, unsigned char *stubs, size_t stub_bytes) {
    static const struct { uint32_t at; void *hook; void **game; } table[] = {
#define X(at, name, ret, params, args) {at, (void *)hook_##name, (void **)&game_##name},
        GFX_FUNCTIONS(X)
#undef X
    };
    enum { COUNT = sizeof(table) / sizeof(*table), SLOT = 48 };
    if (!native_gfx_mode()) return 0;
    if (stub_bytes < (size_t)COUNT * SLOT) return 0;
    for (int i = 0; i < COUNT; ++i) {
        const int64_t reach = (int64_t)(uintptr_t)(stubs + i * SLOT) - (int64_t)(uintptr_t)(image + table[i].at + 5);
        if (!prologue_bytes(image + table[i].at) || reach < INT32_MIN || reach > INT32_MAX) {
            printf("Native: the graphics library is not where v01.04 has it (0x%x); native renderer off\n", table[i].at);
            mode = NATIVE_OFF;
            return 0;
        }
    }
    if (mode == NATIVE_DRAW && !native_render_init()) {
        printf("Native: the renderer did not start; shadPS4 draws instead\n");
        mode = NATIVE_OFF;
        own = 0;
        return 0;
    }
    for (int i = 0; i < COUNT; ++i) {
        unsigned char *code = image + table[i].at, *stub = stubs + i * SLOT, *trampoline = stub + 16;
        const unsigned length = prologue_bytes(code);
        const uintptr_t hook = (uintptr_t)table[i].hook, resume = (uintptr_t)(code + length);
        /* stub: movabs r11, hook; jmp r11 */
        stub[0] = 0x49; stub[1] = 0xbb; memcpy(stub + 2, &hook, 8);
        stub[10] = 0x41; stub[11] = 0xff; stub[12] = 0xe3;
        /* trampoline: the replaced instructions; movabs r11, resume; jmp r11 */
        memcpy(trampoline, code, length);
        trampoline[length] = 0x49; trampoline[length + 1] = 0xbb; memcpy(trampoline + length + 2, &resume, 8);
        trampoline[length + 10] = 0x41; trampoline[length + 11] = 0xff; trampoline[length + 12] = 0xe3;
        *table[i].game = trampoline;
        const int32_t relative = (int32_t)((int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(code + 5));
        code[0] = 0xe9;
        memcpy(code + 1, &relative, 4);
        memset(code + 5, 0x90, length - 5);
    }
    printf("Native: %d graphics library entries hooked (%s)\n", COUNT,
           own ? "drawing, owning the frame" : mode == NATIVE_DRAW ? "drawing" : "recording");
    return 1;
}

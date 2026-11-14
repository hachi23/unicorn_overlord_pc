/* Frame interpolation (docs/dev/FRAME_INTERPOLATION.md): a frame between two recorded frames.
 *
 * native_interp_plan(old, new) pairs the new frame's draws with the old frame's once;
 * native_interp_apply(plan, alpha) then writes, in place, the matrix of every paired draw as
 * old + alpha * (new - old): floats 0..15 of the constants bound at slot 0 (the engine binds the
 * same block to the VS and the PS). Everything else is the new frame's. Alpha 1 restores it.
 *
 * Partners: a draw's key is its render target, VS, PS, texture 0 and index count (the N64 ports'
 * "key and occurrence"); within a key, the closest old draw by where its matrix puts the origin.
 * Snaps (the new matrix): no old draw of the key within NATIVE_INTERP_JUMP in normalised device
 * units (new sprites, teleports); the whole frame when most changed draws have none (a cut). */
#ifndef NATIVE_INTERP_H
#define NATIVE_INTERP_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "native_gfx.h"

#define NATIVE_INTERP_JUMP 0.35f
#define NATIVE_INTERP_NEAR 0.05f /* an own-order partner this close is taken without a search */

typedef struct {
    uint64_t key;
    uint32_t ordinal, group;       /* group: draws with this key in the frame */
    float *vs, *ps;                /* the slot 0 constants' first 16 floats, or NULL */
    float x, y;                    /* where the matrix puts the origin (normalised device units) */
} InterpDraw;

typedef struct { InterpDraw *draws; size_t count; } InterpFrame;

static uint64_t interp_hash(const void *data, size_t bytes, uint64_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static void interp_origin(const float *m, float out[2]);

static int interp_order(const void *a, const void *b) {
    const InterpDraw *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->ordinal < y->ordinal ? -1 : x->ordinal > y->ordinal;
}

/* The frame's draws, sorted by key and ordinal. The float pointers point into the lists. */
static InterpFrame interp_scan(const NativeList *lists) {
    InterpFrame f = {0};
    size_t cap = 0;
    for (const NativeList *list = lists; list; list = list->next) {
        uint64_t target = 0, tex0 = 0;
        uint32_t vs = 0, ps = 0;
        float *vs_m = NULL, *ps_m = NULL;
        for (size_t at = 0; at < list->used;) {
            const NativeCommand *c = (const NativeCommand *)(list->data + at);
            uint32_t *w = (uint32_t *)(list->data + at + sizeof(*c));
            switch (c->op) {
            case NC_TARGET: if (w[0] == 0) target = w[1] ? interp_hash(w + 2, 16, 0xcbf29ce484222325ull) : 0; break;
            case NC_VS: vs = w[5]; break;
            case NC_PS: ps = w[5]; break;
            case NC_TEXTURE: if (w[0] == 1 && w[1] == 0) tex0 = w[2] ? interp_hash(w + 4, 8, 7) : 0; break;
            case NC_CONSTANTS:
                if (w[1] == 0 && (w[0] == 1 || w[0] == 2)) {
                    float *m = w[2] >= 64 ? (float *)(w + 6) : NULL;
                    if (w[0] == 2) vs_m = m; else ps_m = m;
                }
                break;
            case NC_DRAW: {
                if (f.count == cap) {
                    cap = cap ? cap * 2 : 4096;
                    f.draws = realloc(f.draws, cap * sizeof(*f.draws));
                }
                const NativeDraw *d = (const NativeDraw *)w;
                uint64_t k = interp_hash(&target, 8, 0xcbf29ce484222325ull);
                k = interp_hash(&vs, 4, k), k = interp_hash(&ps, 4, k), k = interp_hash(&tex0, 8, k);
                k = interp_hash(&d->index_count, 4, k);
                InterpDraw *nd = &f.draws[f.count++];
                *nd = (InterpDraw){.key = k, .vs = vs_m, .ps = ps_m};
                const float *m = vs_m ? vs_m : ps_m;
                if (m) {
                    float at2[2];
                    interp_origin(m, at2);
                    nd->x = at2[0], nd->y = at2[1];
                }
                break;
            }
            default: break;
            }
            at += sizeof(*c) + c->bytes;
        }
    }
    /* Ordinals in submission order: a stable sort by key keeps it, then number each run. */
    for (size_t i = 0; i < f.count; ++i) f.draws[i].ordinal = (uint32_t)i;
    qsort(f.draws, f.count, sizeof(*f.draws), interp_order);
    for (size_t i = 0; i < f.count;) {
        size_t j = i;
        while (j < f.count && f.draws[j].key == f.draws[i].key) ++j;
        for (size_t k = i; k < j; ++k) f.draws[k].ordinal = (uint32_t)(k - i), f.draws[k].group = (uint32_t)(j - i);
        i = j;
    }
    return f;
}

/* Where the translation puts the origin, in normalised device units. */
static void interp_origin(const float *m, float out[2]) {
    const float w = fabsf(m[15]) > 1e-6f ? m[15] : 1.0f;
    out[0] = m[12] / w, out[1] = m[13] / w;
}

typedef struct { uint32_t pairs, snapped, blended, cut; } InterpStats;

/* One constants block of the new frame to blend: dst points into the new frame's lists. */
typedef struct { float *dst; float from[16], to[16]; } InterpBlock;
typedef struct { InterpBlock *blocks; size_t count; InterpStats stats; } InterpPlan;

static float interp_distance(const InterpDraw *a, const InterpDraw *b) {
    return fmaxf(fabsf(a->x - b->x), fabsf(a->y - b->y));
}

/* Old draws in key order, then by x: a window of x is a range. */
static const InterpDraw *interp_sort_base;
static int interp_by_key_x(const void *a, const void *b) {
    const InterpDraw *x = &interp_sort_base[*(const uint32_t *)a], *y = &interp_sort_base[*(const uint32_t *)b];
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->x < y->x ? -1 : x->x > y->x;
}

static int interp_by_dst(const void *a, const void *b) {
    const InterpBlock *x = a, *y = b;
    return x->dst < y->dst ? -1 : x->dst > y->dst;
}

/* What to blend from old to new (once per new frame). Every new draw takes the closest unused
 * old draw of its key (its own ordinal on a tie), so sprites that enter or leave the view do not
 * shift the others' partners; a partner further than NATIVE_INTERP_JUMP is none. The plan points
 * into new_lists; old can be freed afterwards. */
static InterpPlan native_interp_plan_from(const InterpFrame *old_scan, NativeList *new_lists, InterpFrame *keep);

static InterpPlan native_interp_plan(const NativeList *old, NativeList *new_lists) {
    if (!old) return (InterpPlan){0};
    InterpFrame o = interp_scan(old);
    InterpPlan plan = native_interp_plan_from(&o, new_lists, NULL);
    free(o.draws);
    return plan;
}

/* The same from the old frame's scan (kept from when it was new); the new frame's scan goes to
 * keep (the caller frees keep->draws) or is freed when keep is NULL. */
static InterpPlan native_interp_plan_from(const InterpFrame *old_scan, NativeList *new_lists, InterpFrame *keep) {
    InterpPlan plan = {0};
    const InterpFrame o = *old_scan;
    InterpFrame n = interp_scan(new_lists);
    uint8_t *used = calloc(o.count + 1, 1);
    /* by_x[start..end) are the old run's indices sorted by x (runs keep their place). */
    uint32_t *by_x = malloc((o.count + 1) * sizeof(*by_x));
    for (size_t i = 0; i < o.count; ++i) by_x[i] = (uint32_t)i;
    interp_sort_base = o.draws;
    qsort(by_x, o.count, sizeof(*by_x), interp_by_key_x);
    plan.blocks = malloc((n.count * 2 + 1) * sizeof(*plan.blocks));
    size_t changed = 0, jumped = 0;
    for (size_t i = 0; i < n.count; ++i) {
        const InterpDraw *d = &n.draws[i];
        const InterpDraw first = {.key = d->key, .ordinal = 0};
        const InterpDraw *run = bsearch(&first, o.draws, o.count, sizeof(*d), interp_order);
        if (!run) continue;
        const size_t start = (size_t)(run - o.draws), end = start + run->group;
        size_t best = SIZE_MAX;
        float best_distance = NATIVE_INTERP_JUMP;
        if (d->ordinal < run->group && !used[start + d->ordinal]) {
            const float t = interp_distance(d, &o.draws[start + d->ordinal]);
            if (t < best_distance) best = start + d->ordinal, best_distance = t;
        }
        /* Same number of draws and its own partner close by: no search. */
        const int settled = run->group == d->group && best != SIZE_MAX && best_distance < NATIVE_INTERP_NEAR;
        if (!settled && best_distance > 1e-5f) {
            /* Only old draws with |x - d.x| < best can be closer. */
            size_t lo = start, hi = end;
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                if (o.draws[by_x[mid]].x < d->x - best_distance) lo = mid + 1; else hi = mid;
            }
            for (size_t j = lo; j < end && o.draws[by_x[j]].x <= d->x + best_distance; ++j) {
                const size_t k = by_x[j];
                if (used[k]) continue;
                const float t = interp_distance(d, &o.draws[k]);
                if (t + 1e-5f < best_distance) best = k, best_distance = t;
            }
        }
        if (best == SIZE_MAX) { ++plan.stats.snapped, ++jumped, ++changed; continue; }
        used[best] = 1;
        ++plan.stats.pairs;
        const InterpDraw *p = &o.draws[best];
        const float *pairs[2][2] = {{d->vs, p->vs}, {d->ps, p->ps}};
        for (int k = 0; k < 2; ++k) {
            float *dst = (float *)pairs[k][0];
            const float *from = pairs[k][1];
            if (!dst || !from || !memcmp(dst, from, 64)) continue;
            ++changed;
            InterpBlock *b = &plan.blocks[plan.count++];
            b->dst = dst;
            memcpy(b->from, from, 64);
            memcpy(b->to, dst, 64);
        }
    }
    /* Most draws losing their partner is a cut: show the new frame as it is. */
    plan.stats.cut = changed >= 8 && jumped * 2 > changed;
    if (plan.stats.cut) plan.count = 0;
    /* Several draws can share one constants block: keep the first pairing of each. */
    qsort(plan.blocks, plan.count, sizeof(*plan.blocks), interp_by_dst);
    size_t kept = 0;
    for (size_t i = 0; i < plan.count; ++i)
        if (!kept || plan.blocks[kept - 1].dst != plan.blocks[i].dst) plan.blocks[kept++] = plan.blocks[i];
    plan.count = kept;
    plan.stats.blended = (uint32_t)kept;
    free(used);
    free(by_x);
    if (keep) *keep = n; else free(n.draws);
    return plan;
}

/* Writes the blend at alpha into the new frame's lists; alpha 1 puts the new frame back. */
static void native_interp_apply(const InterpPlan *plan, float alpha) {
    for (size_t i = 0; i < plan->count; ++i) {
        const InterpBlock *b = &plan->blocks[i];
        if (alpha >= 1.0f) { memcpy(b->dst, b->to, 64); continue; }
        for (int e = 0; e < 16; ++e) b->dst[e] = b->from[e] + alpha * (b->to[e] - b->from[e]);
    }
}

static void native_interp_plan_free(InterpPlan *plan) {
    free(plan->blocks);
    *plan = (InterpPlan){0};
}
#endif

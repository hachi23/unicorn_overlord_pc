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
#define NATIVE_INTERP_MAX_FPS 240 /* presents per second, at most */
#define NATIVE_INTERP_NEAR 0.05f /* an own-order partner this close is taken without a search */

typedef struct {
    uint64_t key, target;
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
                *nd = (InterpDraw){.key = k, .target = target, .vs = vs_m, .ps = ps_m};
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

static float interp_distance_to(const InterpDraw *a, float x, float y) {
    return fmaxf(fabsf(a->x - x), fabsf(a->y - y));
}

/* The old draws of one key (o->draws[start..end), by_x sorted by x within it) not yet paired. */
typedef struct {
    const InterpFrame *o;
    const uint32_t *by_x;
    const uint8_t *used;
    size_t start, end;
} InterpWindow;

/* The closest unpaired old draw to (x, y) nearer than limit, or SIZE_MAX. */
static size_t interp_nearest(const InterpWindow *w, float x, float y, float limit, float *distance) {
    size_t lo = w->start, hi = w->end, best = SIZE_MAX;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (w->o->draws[w->by_x[mid]].x < x - limit) lo = mid + 1; else hi = mid;
    }
    for (size_t j = lo; j < w->end && w->o->draws[w->by_x[j]].x <= x + limit; ++j) {
        const size_t k = w->by_x[j];
        if (w->used[k]) continue;
        const float t = interp_distance_to(&w->o->draws[k], x, y);
        if (t + 1e-5f < limit) best = k, limit = t;
    }
    if (best != SIZE_MAX) *distance = limit;
    return best;
}

/* How well a key's new draws fit the old ones moved by (sx, sy): the sum of each one's distance
 * to the closest old draw, NATIVE_INTERP_JUMP for none. Lower is better. */
static float interp_fit(const InterpWindow *w, const InterpDraw *draws, size_t count, float sx, float sy) {
    float sum = 0;
    for (size_t i = 0; i < count; ++i) {
        float t = NATIVE_INTERP_JUMP;
        interp_nearest(w, draws[i].x - sx, draws[i].y - sy, t, &t);
        sum += t;
    }
    return sum;
}

/* The camera's motion per render target: the median move of the draws whose partner is certain
 * (a key with the same number of draws in both frames, paired in order) and that moved. A fast
 * pan moves the scene further than sprites are apart (about 80 px a frame on the battle map at
 * cursor speed), so the nearest old sprite of a key is often another one; searching where the
 * camera says a sprite was finds its own. */
typedef struct { uint64_t target; float dx, dy; } InterpShift;

static int interp_by_float(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static int interp_by_target(const void *a, const void *b) {
    const InterpShift *x = a, *y = b;
    return x->target < y->target ? -1 : x->target > y->target;
}

static size_t interp_shifts(const InterpFrame *o, const InterpFrame *n, InterpShift **out) {
    InterpShift *moves = malloc((n->count + 1) * sizeof(*moves));
    size_t count = 0;
    for (size_t i = 0, start = 0; i < n->count; ++i) {
        const InterpDraw *d = &n->draws[i];
        if (d->ordinal == 0) {
            const InterpDraw probe = {.key = d->key, .ordinal = 0};
            const InterpDraw *run = bsearch(&probe, o->draws, o->count, sizeof(probe), interp_order);
            start = run && run->group == d->group ? (size_t)(run - o->draws) : SIZE_MAX;
        }
        if (start == SIZE_MAX || !d->vs) continue;
        const InterpDraw *p = &o->draws[start + d->ordinal];
        const float dx = d->x - p->x, dy = d->y - p->y;
        if (fabsf(dx) + fabsf(dy) > 1e-5f && fmaxf(fabsf(dx), fabsf(dy)) < NATIVE_INTERP_JUMP)
            moves[count++] = (InterpShift){d->target, dx, dy};
    }
    qsort(moves, count, sizeof(*moves), interp_by_target);
    float *xs = malloc((count + 1) * sizeof(*xs)), *ys = malloc((count + 1) * sizeof(*ys));
    size_t kept = 0;
    for (size_t i = 0; i < count;) {
        size_t j = i;
        while (j < count && moves[j].target == moves[i].target) xs[j - i] = moves[j].dx, ys[j - i] = moves[j].dy, ++j;
        if (j - i >= 4) { /* a few moved draws are animation, not a camera */
            qsort(xs, j - i, sizeof(*xs), interp_by_float);
            qsort(ys, j - i, sizeof(*ys), interp_by_float);
            moves[kept++] = (InterpShift){moves[i].target, xs[(j - i) / 2], ys[(j - i) / 2]};
        }
        i = j;
    }
    free(xs);
    free(ys);
    *out = moves;
    return kept;
}

static void interp_shift_of(const InterpShift *shifts, size_t count, uint64_t target, float *dx, float *dy) {
    const InterpShift probe = {.target = target};
    const InterpShift *s = bsearch(&probe, shifts, count, sizeof(probe), interp_by_target);
    if (s) *dx = s->dx, *dy = s->dy;
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
    InterpShift *shifts = NULL;
    const size_t shift_count = interp_shifts(&o, &n, &shifts);
    size_t changed = 0, jumped = 0;
    for (size_t first = 0; first < n.count;) {
        size_t last = first;
        while (last < n.count && n.draws[last].key == n.draws[first].key) ++last;
        const InterpDraw probe = {.key = n.draws[first].key, .ordinal = 0};
        const InterpDraw *run = bsearch(&probe, o.draws, o.count, sizeof(probe), interp_order);
        if (!run) { first = last; continue; }
        const size_t start = (size_t)(run - o.draws), end = start + run->group;
        /* Where the old frame had this key's draws: in place (HUD) or moved with the camera. */
        float sx = 0, sy = 0;
        interp_shift_of(shifts, shift_count, n.draws[first].target, &sx, &sy);
        if (sx != 0 || sy != 0) {
            const InterpWindow w = {&o, by_x, used, start, end};
            if (interp_fit(&w, n.draws + first, last - first, 0, 0) <= interp_fit(&w, n.draws + first, last - first, sx, sy))
                sx = sy = 0;
        }
        for (size_t i = first; i < last; ++i) {
            const InterpDraw *d = &n.draws[i];
            const float px = d->x - sx, py = d->y - sy; /* where it was, if it moved with its key */
            size_t best = SIZE_MAX;
            float best_distance = NATIVE_INTERP_JUMP;
            if (d->ordinal < run->group && !used[start + d->ordinal]) {
                const float t = interp_distance_to(&o.draws[start + d->ordinal], px, py);
                if (t < best_distance) best = start + d->ordinal, best_distance = t;
            }
            /* Same number of draws and its own partner close by: no search. */
            const int settled = run->group == d->group && best != SIZE_MAX && best_distance < NATIVE_INTERP_NEAR;
            if (!settled && best_distance > 1e-5f) {
                const InterpWindow w = {&o, by_x, used, start, end};
                const size_t k = interp_nearest(&w, px, py, best_distance, &best_distance);
                if (k != SIZE_MAX) best = k;
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
        first = last;
    }
    free(shifts);
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

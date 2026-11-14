/* src/native_interp.h on made-up frames: matched matrices blend, a key whose number of draws
 * changed snaps, a jump snaps, a cut snaps everything, and no old frame gives the new one. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_interp.h"

void native_list_free(NativeList *list) {
    if (!list) return;
    free(list->data);
    free(list);
}

static void emit(NativeList *l, uint32_t op, const void *payload, uint32_t bytes) {
    const uint32_t padded = (bytes + 7) & ~7u;
    if (l->used + 8 + padded > l->capacity) {
        l->capacity = (l->used + 8 + padded) * 2;
        l->data = realloc(l->data, l->capacity);
    }
    const NativeCommand c = {op, padded};
    memcpy(l->data + l->used, &c, 8);
    memset(l->data + l->used + 8, 0, padded);
    memcpy(l->data + l->used + 8, payload, bytes);
    l->used += 8 + padded;
}

/* A sprite at (x, y): its matrix (ortho scale, translation x, y) bound to the VS and PS, then a
 * draw with texture `tex`. */
static void sprite(NativeList *l, uint32_t tex, float x, float y) {
    uint32_t t[12] = {1, 0, 1, 0, tex, 0};
    emit(l, NC_TEXTURE, t, sizeof(t));
    for (uint32_t stage = 1; stage <= 2; ++stage) {
        uint8_t c[24 + 80] = {0};
        const uint32_t head[4] = {stage, 0, 80, 0};
        memcpy(c, head, 16);
        float m[20] = {0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, 1, 0, x, y, 0, 1, 0.5f};
        memcpy(c + 24, m, sizeof(m));
        emit(l, NC_CONSTANTS, c, sizeof(c));
    }
    const NativeDraw d = {.index_count = 6, .index_bytes = 2, .vertex_count = 4};
    emit(l, NC_DRAW, &d, sizeof(d));
}

static NativeList *frame(const float (*at)[3], int count) {
    NativeList *l = calloc(1, sizeof(*l));
    const uint32_t target[18] = {0, 1, 0x1234};
    emit(l, NC_TARGET, target, sizeof(target));
    const uint32_t vs[6] = {0, 0, 0, 0, 0, 0xabcd}, ps[6] = {0, 0, 0, 0, 0, 0xef01};
    emit(l, NC_VS, vs, sizeof(vs));
    emit(l, NC_PS, ps, sizeof(ps));
    for (int i = 0; i < count; ++i) sprite(l, (uint32_t)at[i][0], at[i][1], at[i][2]);
    return l;
}

/* The translation of the n-th draw's VS matrix and its float 16. */
static void nth(const NativeList *l, int n, float out[3]) {
    const float *vs = NULL;
    for (size_t at = 0; at < l->used;) {
        const NativeCommand *c = (const NativeCommand *)(l->data + at);
        const uint32_t *w = (const uint32_t *)(c + 1);
        if (c->op == NC_CONSTANTS && w[0] == 2) vs = (const float *)(w + 6);
        if (c->op == NC_DRAW && n-- == 0) { out[0] = vs[12], out[1] = vs[13], out[2] = vs[16]; return; }
        at += sizeof(*c) + c->bytes;
    }
    assert(0);
}

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

/* Plans old -> new, writes the blend at alpha into new and returns the plan's stats. */
static InterpStats blend(const NativeList *o, NativeList *n, float alpha, InterpPlan *plan) {
    *plan = native_interp_plan(o, n);
    native_interp_apply(plan, alpha);
    return plan->stats;
}

static void restore(InterpPlan *plan) {
    native_interp_apply(plan, 1.0f);
    native_interp_plan_free(plan);
}

int main(void) {
    InterpPlan plan;
    InterpStats s;
    float v[3];

    /* Two sprites of texture 1 and one of texture 2; two move (a frame's worth) and blend halfway. */
    const float a0[][3] = {{1, 0.0f, 0.0f}, {1, 0.1f, 0.1f}, {2, -0.2f, 0.2f}};
    const float a1[][3] = {{1, 0.02f, 0.0f}, {1, 0.1f, 0.14f}, {2, -0.2f, 0.2f}};
    NativeList *o = frame(a0, 3), *n = frame(a1, 3);
    NativeList *pristine = frame(a1, 3);
    s = blend(o, n, 0.5f, &plan);
    nth(n, 0, v), assert(near(v[0], 0.01f) && near(v[1], 0.0f) && near(v[2], 0.5f)); /* float 16 kept */
    nth(n, 1, v), assert(near(v[0], 0.1f) && near(v[1], 0.12f));
    nth(n, 2, v), assert(near(v[0], -0.2f) && near(v[1], 0.2f));
    assert(s.pairs == 3 && s.snapped == 0 && s.blended == 4 && !s.cut); /* VS and PS of two draws */
    native_interp_apply(&plan, 0.0f); /* alpha 0: the old matrices */
    nth(n, 1, v), assert(near(v[1], 0.1f));
    restore(&plan); /* alpha 1: the new frame, byte for byte */
    assert(n->used == pristine->used && !memcmp(n->data, pristine->data, n->used));

    /* A texture-1 sprite comes into view before the others: they keep their partners. */
    const float a2[][3] = {{1, 0.6f, 0.6f}, {1, 0.02f, 0.0f}, {1, 0.1f, 0.14f}, {2, -0.1f, 0.2f}};
    NativeList *n2 = frame(a2, 4);
    s = blend(o, n2, 0.5f, &plan);
    nth(n2, 0, v), assert(near(v[0], 0.6f) && near(v[1], 0.6f)); /* new: as it is */
    nth(n2, 1, v), assert(near(v[0], 0.01f) && near(v[1], 0.0f));
    nth(n2, 2, v), assert(near(v[0], 0.1f) && near(v[1], 0.12f));
    nth(n2, 3, v), assert(near(v[0], -0.15f));
    assert(s.pairs == 3 && s.snapped == 1 && !s.cut);
    restore(&plan);

    /* One leaves and another enters in the same frame (same count): pairs by place, not order. */
    const float b0[][3] = {{3, -0.5f, 0.0f}, {3, 0.0f, 0.0f}, {3, 0.5f, 0.0f}};
    const float b1[][3] = {{3, 0.02f, 0.0f}, {3, 0.52f, 0.0f}, {3, 0.9f, 0.0f}};
    NativeList *p0 = frame(b0, 3), *p1 = frame(b1, 3);
    s = blend(p0, p1, 0.5f, &plan);
    nth(p1, 0, v), assert(near(v[0], 0.01f));
    nth(p1, 1, v), assert(near(v[0], 0.51f));
    nth(p1, 2, v), assert(near(v[0], 0.9f)); /* 0.4 from its nearest: new */
    assert(s.pairs == 2 && s.snapped == 1);
    restore(&plan);

    /* One sprite jumps across the screen: it snaps, the others blend. */
    const float a3[][3] = {{1, 0.9f, 0.0f}, {1, 0.1f, 0.14f}, {2, -0.1f, 0.2f}};
    NativeList *n3 = frame(a3, 3);
    s = blend(o, n3, 0.5f, &plan);
    nth(n3, 0, v), assert(near(v[0], 0.9f));
    nth(n3, 1, v), assert(near(v[1], 0.12f));
    assert(!s.cut && s.snapped == 1);
    restore(&plan);

    /* Everything jumps (a camera cut): the new frame as it is. */
    float many0[12][3], many1[12][3];
    for (int i = 0; i < 12; ++i) {
        many0[i][0] = many1[i][0] = (float)(10 + i);
        many0[i][1] = 0, many0[i][2] = 0;
        many1[i][1] = 1.5f, many1[i][2] = 0;
    }
    NativeList *c0 = frame(many0, 12), *c1 = frame(many1, 12);
    s = blend(c0, c1, 0.5f, &plan);
    assert(s.cut && s.blended == 0);
    nth(c1, 5, v), assert(near(v[0], 1.5f));
    restore(&plan);

    /* A fast pan: the camera moves 0.08 a frame, trees are 0.05 apart and one comes into view.
     * The nearest old tree is often the neighbour; with the camera's move (from the single
     * terrain pieces) each tree finds its own, and the HUD sprite of the same kind stays put. */
    float pan0[13][3], pan1[14][3];
    int m0 = 0, m1 = 0;
    for (int i = 0; i < 6; ++i) { /* terrain: one draw per texture, so certain partners */
        pan0[m0][0] = pan1[m1][0] = (float)(20 + i);
        pan0[m0][1] = -0.5f + 0.2f * i, pan0[m0++][2] = -0.3f;
        pan1[m1][1] = -0.5f + 0.2f * i - 0.08f, pan1[m1++][2] = -0.3f;
    }
    for (int i = 5; i >= 0; --i) { /* trees, texture 4, drawn right to left */
        pan0[m0][0] = pan1[m1][0] = 4;
        pan0[m0][1] = 0.05f * i, pan0[m0++][2] = 0.2f;
        pan1[m1][1] = 0.05f * i - 0.08f, pan1[m1++][2] = 0.2f;
    }
    pan1[m1][0] = 4, pan1[m1][1] = 0.30f - 0.08f + 0.05f, pan1[m1++][2] = 0.2f; /* enters at the right */
    pan0[m0][0] = pan1[m1][0] = 4; /* HUD icon drawn like a tree: stays */
    pan0[m0][1] = pan1[m1][1] = 0.9f, pan0[m0++][2] = pan1[m1++][2] = 0.9f;
    NativeList *f0 = frame((const float (*)[3])pan0, m0), *f1 = frame((const float (*)[3])pan1, m1);
    s = blend(f0, f1, 0.5f, &plan);
    for (int i = 0; i < 6; ++i) nth(f1, 11 - i, v), assert(near(v[0], 0.05f * i - 0.04f));
    nth(f1, 12, v), assert(near(v[0], 0.27f)); /* new: as it is */
    nth(f1, 13, v), assert(near(v[0], 0.9f) && near(v[1], 0.9f));
    assert(!s.cut && s.snapped == 1);
    restore(&plan);

    /* No old frame: nothing to do. */
    s = blend(NULL, n, 0.5f, &plan);
    assert(s.pairs == 0 && plan.count == 0 && !memcmp(n->data, pristine->data, n->used));
    restore(&plan);

    NativeList *all[] = {o, n, pristine, n2, p0, p1, n3, c0, c1, f0, f1};
    for (size_t i = 0; i < sizeof(all) / sizeof(*all); ++i) native_list_free(all[i]);
    printf("native interp: ok\n");
    return 0;
}

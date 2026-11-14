/* Measurement only (VW_INTERP_PROBE=1): can a frame's draws be matched to the previous frame's,
 * and what changes between them? Key = render target, VS, PS, texture 0, index count; ordinal
 * within the key. For matched draws: are the positions (location 0), the VS constants at slot 0
 * (matrix) or the other constants different, and by how much do positions move. */
#ifndef NATIVE_INTERP_PROBE_H
#define NATIVE_INTERP_PROBE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_gfx.h"
#include "native_gnm.h"

typedef struct {
    uint64_t key, pos, cb0, cbs;
    uint32_t ordinal, vertices, stride, format;
    float first[4]; /* the first vertex's position (as f32) */
    float cb0f[16];
} ProbeDraw;

static uint64_t probe_hash(const void *data, size_t bytes, uint64_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static int probe_cmp(const void *a, const void *b) {
    const ProbeDraw *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->ordinal < y->ordinal ? -1 : x->ordinal > y->ordinal;
}

static ProbeDraw *probe_prev;
static size_t probe_prev_count;
static uint64_t probe_frames;
static double probe_sum[12];

static void native_interp_probe(const NativeList *lists) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("VW_INTERP_PROBE") != NULL;
    if (!enabled) return;
    static uint64_t known[512]; static size_t known_n; /* every target address ever drawn into */
    uint64_t written[512]; size_t written_n = 0;
    static uint64_t carried[64]; static size_t carried_n;
    size_t count = 0, cap = 4096;
    ProbeDraw *draws = malloc(cap * sizeof(*draws));
    for (const NativeList *list = lists; list; list = list->next) {
        uint64_t target = 0, tex0 = 0, cbs = 0, cb0 = 0;
        uint32_t vs = 0, ps = 0;
        float cb0f[16] = {0};
        for (size_t at = 0; at < list->used;) {
            const NativeCommand *command = (const NativeCommand *)(list->data + at);
            const uint32_t *w = (const uint32_t *)(command + 1);
            const uint8_t *payload = (const uint8_t *)w;
            switch (command->op) {
            case NC_TARGET:
                if (w[0] == 0) {
                    target = w[1] ? probe_hash(w + 2, 16, 1469598103934665603ull) : 0;
                    if (w[1]) {
                        GnmTarget t; gnm_target(w + 2, &t);
                        int seen = 0;
                        for (size_t i = 0; i < written_n; ++i) seen |= written[i] == t.address;
                        if (!seen && written_n < 512) written[written_n++] = t.address;
                        seen = 0;
                        for (size_t i = 0; i < known_n; ++i) seen |= known[i] == t.address;
                        if (!seen && known_n < 512) known[known_n++] = t.address;
                    }
                }
                break;
            case NC_VS: vs = w[5]; break;
            case NC_PS: ps = w[5]; break;
            case NC_TEXTURE:
                if (w[0] == 1 && w[1] == 0) tex0 = w[2] ? probe_hash(w + 4, 8, 7) : 0;
                if (w[0] == 1 && w[2]) {
                    GnmTexture t; gnm_texture(w + 4, &t);
                    int is_target = 0, now = 0, logged = 0;
                    for (size_t i = 0; i < known_n; ++i) is_target |= known[i] == t.address;
                    for (size_t i = 0; i < written_n; ++i) now |= written[i] == t.address;
                    for (size_t i = 0; i < carried_n; ++i) logged |= carried[i] == t.address;
                    if (is_target && !now && !logged && carried_n < 64) {
                        carried[carried_n++] = t.address;
                        printf("Interp probe: target %llx is read before it is drawn in a frame (carried over)\n",
                               (unsigned long long)t.address);
                    }
                }
                break;
            case NC_CONSTANTS:
                if (w[0] == 2 && w[1] == 0) {
                    cb0 = probe_hash(payload + 24, w[2], 11);
                    memset(cb0f, 0, sizeof(cb0f));
                    memcpy(cb0f, payload + 24, w[2] < 64 ? w[2] : 64);
                } else {
                    cbs = probe_hash(payload + 24, w[2], cbs ^ ((uint64_t)w[0] << 8 | w[1]));
                }
                break;
            case NC_DRAW: {
                NativeDraw d;
                memcpy(&d, payload, sizeof(d));
                const NativeStream *streams = (const NativeStream *)(payload + sizeof(d));
                const NativeRange *ranges = (const NativeRange *)(streams + d.stream_count);
                if (count == cap) draws = realloc(draws, (cap *= 2) * sizeof(*draws));
                ProbeDraw *p = &draws[count++];
                memset(p, 0, sizeof(*p));
                uint64_t k = probe_hash(&target, 8, 1469598103934665603ull);
                k = probe_hash(&vs, 4, k), k = probe_hash(&ps, 4, k), k = probe_hash(&tex0, 8, k);
                k = probe_hash(&d.index_count, 4, k);
                p->key = k, p->cb0 = cb0, p->cbs = cbs, p->vertices = d.vertex_count;
                memcpy(p->cb0f, cb0f, sizeof(cb0f));
                const NativeStream *s = NULL;
                for (uint32_t i = 0; i < d.stream_count; ++i)
                    if (streams[i].location == 0) s = &streams[i];
                if (!s && d.stream_count) s = &streams[0];
                if (s && s->range < d.range_count) {
                    const NativeRange *r = &ranges[s->range];
                    size_t bytes = (size_t)s->stride * d.vertex_count;
                    if (s->offset + bytes > r->bytes) bytes = r->bytes > s->offset ? r->bytes - s->offset : 0;
                    p->pos = probe_hash(payload + r->data + s->offset, bytes, 13);
                    p->stride = s->stride, p->format = s->format;
                    if (bytes >= 16) memcpy(p->first, payload + r->data + s->offset, 16);
                }
                break;
            }
            default: break;
            }
            at += sizeof(*command) + command->bytes;
        }
    }
    /* ordinals by key in submission order */
    for (size_t i = 0; i < count; ++i) {
        uint32_t n = 0;
        for (size_t j = i; j-- > 0;) if (draws[j].key == draws[i].key) { n = draws[j].ordinal + 1; break; }
        draws[i].ordinal = n;
    }
    qsort(draws, count, sizeof(*draws), probe_cmp);
    if (probe_prev) {
        double matched = 0, same = 0, pos_only = 0, cb0_only = 0, both = 0, other = 0, moved_far = 0;
        double vertex_diff = 0, cb0_eps = 0;
        int samples = 0;
        for (size_t i = 0; i < count; ++i) {
            const ProbeDraw *n = &draws[i];
            const ProbeDraw *o = bsearch(n, probe_prev, probe_prev_count, sizeof(*n), probe_cmp);
            if (!o) continue;
            ++matched;
            if (o->vertices != n->vertices) { ++vertex_diff; continue; }
            const int dp = o->pos != n->pos, dc = o->cb0 != n->cb0;
            if (!dp && !dc) { if (o->cbs != n->cbs) ++other; else ++same; }
            else if (dp && dc) ++both;
            else if (dp) ++pos_only;
            else ++cb0_only;
            if (dp && probe_frames % 120 == 119 && samples < 8) {
                ++samples;
                printf("  sample: vs %08x? verts %u stride %u fmt %u first (%.2f %.2f %.2f %.2f) -> (%.2f %.2f %.2f %.2f) "
                       "matrix %s row0 %.3f %.3f %.3f %.3f row3 %.3f %.3f %.3f %.3f\n",
                       (unsigned)(n->key & 0xffffffff), n->vertices, n->stride, n->format, o->first[0], o->first[1],
                       o->first[2], o->first[3], n->first[0], n->first[1], n->first[2], n->first[3],
                       dc ? "changed" : "same", n->cb0f[0], n->cb0f[1], n->cb0f[2], n->cb0f[3], n->cb0f[12],
                       n->cb0f[13], n->cb0f[14], n->cb0f[15]);
            }
            if (dp) {
                const float dx = n->first[0] - o->first[0], dy = n->first[1] - o->first[1];
                if (!(fabsf(dx) < 64 && fabsf(dy) < 64)) ++moved_far;
            }
            if (dc) {
                float m = 0;
                for (int e = 0; e < 16; ++e) m = fmaxf(m, fabsf(n->cb0f[e] - o->cb0f[e]));
                if (m > 0.05f) ++cb0_eps;
            }
        }
        const double v[12] = {(double)count, matched, same, pos_only, cb0_only, both, other, moved_far, vertex_diff, cb0_eps, (double)probe_prev_count, 0};
        for (int e = 0; e < 12; ++e) probe_sum[e] += v[e];
        if (++probe_frames % 120 == 0) {
            const double f = (double)probe_frames;
            printf("Interp probe (per frame, %llu frames): %.0f draws (prev %.0f), matched %.0f: same %.0f, "
                   "positions only %.0f, matrix only %.0f, both %.0f, other constants only %.0f, vertex count "
                   "differs %.0f; moved >64 units %.0f; matrix changed >0.05 %.0f\n",
                   (unsigned long long)probe_frames, probe_sum[0] / f, probe_sum[10] / f, probe_sum[1] / f,
                   probe_sum[2] / f, probe_sum[3] / f, probe_sum[4] / f, probe_sum[5] / f, probe_sum[6] / f,
                   probe_sum[8] / f, probe_sum[7] / f, probe_sum[9] / f);
            fflush(stdout);
            memset(probe_sum, 0, sizeof(probe_sum));
            probe_frames = 0;
        }
    }
    free(probe_prev);
    probe_prev = draws, probe_prev_count = count;
}
#endif

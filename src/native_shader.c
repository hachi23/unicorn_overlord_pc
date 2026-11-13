/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The game's shader containers and the GCN instruction decoder (src/native_shader.h). The
 * translation to SPIR-V is in native_shader_spirv.c. */
#include "native_shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Containers ----
 * Each shader sits in the executable as a "Shdr" header (the stage's registers, input-usage
 * slots, semantics), then the GCN code, which starts with s_mov_b32 vcc_hi, n where (n + 1) * 8
 * bytes reach the "OrbShdr" footer after it. Footer: "OrbShdr", then type (0 PS, 1 VS, ...),
 * length in bytes, hash (offsets as AmdGpu::BinaryInfo). */
typedef struct {
    uint8_t signature[7];
    uint8_t version;
    uint32_t pssl_or_cg : 1, cached : 1, type : 4, source_type : 2, length : 24;
    uint8_t chunk_usage_base_offset_in_dw;
    uint8_t num_input_usage_slots;
    uint8_t is_srt : 1, is_srt_used_info_valid : 1, is_extended_usage_info : 1, reserved2 : 5;
    uint8_t reserved3;
    uint64_t shader_hash;
    uint32_t crc32;
} Footer;

int ns_find_shaders(const uint32_t *words, size_t size, void (*found)(const NsShader *, void *), void *user,
                    int *failed) {
    const uint8_t *d = (const uint8_t *)words;
    uint32_t *seen = NULL;
    size_t seen_count = 0, seen_cap = 0;
    int count = 0;
    *failed = 0;
    for (size_t o = 8; o + sizeof(Footer) <= size; o += 4) {
        if (memcmp(d + o, "OrbShdr", 7) != 0) continue;
        Footer footer;
        memcpy(&footer, d + o, sizeof(footer));
        if (footer.type != 0 && footer.type != 1) continue;   /* compute and others */
        const uint32_t hash = (uint32_t)footer.shader_hash;
        int duplicate = 0;
        for (size_t i = 0; i < seen_count && !duplicate; ++i) duplicate = seen[i] == hash;
        if (duplicate) continue;
        if (seen_count == seen_cap) {
            uint32_t *grown = realloc(seen, (seen_cap ? seen_cap * 2 : 1024) * sizeof(*seen));
            if (!grown) { ++*failed; break; }
            seen = grown;
            seen_cap = seen_cap ? seen_cap * 2 : 1024;
        }
        seen[seen_count++] = hash;
        size_t start = 0, header = 0;
        for (size_t c = o - 8; c > 0 && c + 0x10000 > o; c -= 4) {
            const uint32_t *w = (const uint32_t *)(d + c);
            if (w[0] == 0xbeeb03ffu && c + ((size_t)w[1] + 1) * 8 == o) { start = c; break; }
        }
        for (size_t c = start; start && c >= 4 && c + 0x400 > start; c -= 4)
            if (memcmp(d + c, "Shdr", 4) == 0 && memcmp(d + c - 3, "Orb", 3) != 0) { header = c; break; }
        if (!start || !header) {
            fprintf(stderr, "%08x: code or header not found\n", hash);
            ++*failed;
            continue;
        }
        NsShader s;
        memset(&s, 0, sizeof(s));
        s.hash = hash;
        s.vertex = footer.type == 1;
        s.code = (const uint32_t *)(d + start);
        s.code_words = footer.length / 4;
        if ((size_t)s.code_words * 4 > o - start) {   /* the decoder would read past the code */
            fprintf(stderr, "%08x: code length %u past the footer\n", hash, footer.length);
            ++*failed;
            continue;
        }
        uint32_t common;
        memcpy(&common, d + header + 0x10, 4);
        const uint32_t slots = common >> 24, regs = s.vertex ? 7 : 12;
        memcpy(s.regs, d + header + 0x18, regs * 4);
        const uint8_t *counts = d + header + 0x18 + regs * 4, *at = counts + 4;
        int bad = slots > NS_MAX_SLOTS || counts[0] > NS_MAX_SEMANTICS || (s.vertex && counts[1] > NS_MAX_SEMANTICS);
        for (uint32_t i = 0; !bad && i < slots; ++i, at += 4) s.slots[s.slot_count++] = (NsSlot){at[0], at[1], at[2], at[3]};
        if (!bad && s.vertex) {
            for (uint32_t i = 0; i < counts[0]; ++i, at += 4) s.inputs[s.input_count++] = at[0] | at[1] << 8 | at[2] << 16;
            for (uint32_t i = 0; i < counts[1]; ++i, at += 2) s.exports[s.export_count++] = (uint16_t)(at[0] | at[1] << 8);
        } else if (!bad) {
            for (uint32_t i = 0; i < counts[0]; ++i, at += 2) s.inputs[s.input_count++] = at[0] | at[1] << 8;
        }
        if (bad || at > d + start) {
            fprintf(stderr, "%08x: header not understood\n", hash);
            ++*failed;
            continue;
        }
        found(&s, user);
        ++count;
    }
    free(seen);
    return count;
}

/* ---- Decoder ---- */

static const struct { const char *name; uint8_t encoding; uint16_t number; } operations[] = {
    {"?", 0, 0},
#define X(name, encoding, number) {#name, encoding, number},
    NS_OPERATIONS(X)
#undef X
};

const char *ns_operation_name(int operation) {
    return operation > 0 && operation < NS_OPERATION_COUNT ? operations[operation].name : "?";
}

/* The operation of (encoding, number); VOP3 also carries the VOPC (0-255), VOP2 (256-319) and
 * VOP1 (384-511) operations. */
static uint16_t operation_of(int encoding, uint32_t number) {
    if (encoding == NS_VOP3) {
        if (number < 256) encoding = NS_VOPC;
        else if (number < 320) encoding = NS_VOP2, number -= 256;
        else if (number >= 384) encoding = NS_VOP1, number -= 384;
    }
    for (int i = 1; i < NS_OPERATION_COUNT; ++i)
        if (operations[i].encoding == encoding && operations[i].number == number) return (uint16_t)i;
    return NS_UNKNOWN;
}

static int literal_used(const NsInst *n) {
    for (int i = 0; i < 3; ++i) if (n->src[i] == NS_LITERAL) return 1;
    return 0;
}

int ns_decode(const uint32_t *code, uint32_t words, uint32_t at, NsInst *n) {
    if (at >= words) return 0;
    memset(n, 0, sizeof(*n));
    const uint32_t w = code[at], w2 = at + 1 < words ? code[at + 1] : 0;
    n->words = 1;
    n->src[0] = n->src[1] = n->src[2] = 0xffff;
    if (w >> 25 == 0x3f) {                             /* VOP1 (bit 31 clear, like VOP2) */
        n->encoding = NS_VOP1; n->number = w >> 9 & 0xff;
        n->src[0] = w & 0x1ff; n->dst = NS_VGPR + (w >> 17 & 0xff);
    } else if (w >> 25 == 0x3e) {                      /* VOPC */
        n->encoding = NS_VOPC; n->number = w >> 17 & 0xff;
        n->src[0] = w & 0x1ff; n->src[1] = NS_VGPR + (w >> 9 & 0xff); n->sdst = NS_VCC_LO;
    } else if (!(w >> 31)) {                           /* VOP2 */
        n->encoding = NS_VOP2; n->number = w >> 25 & 0x3f;
        n->src[0] = w & 0x1ff; n->src[1] = NS_VGPR + (w >> 9 & 0xff); n->dst = NS_VGPR + (w >> 17 & 0xff);
        if (n->number == 32 || n->number == 33) n->literal = w2, n->words = 2;   /* madmk, madak */
    } else if (w >> 23 == 0x17d) {                     /* SOP1 */
        n->encoding = NS_SOP1; n->number = w >> 8 & 0xff;
        n->src[0] = w & 0xff; n->dst = w >> 16 & 0x7f;
    } else if (w >> 23 == 0x17e) {                     /* SOPC */
        n->encoding = NS_SOPC; n->number = w >> 16 & 0x7f;
        n->src[0] = w & 0xff; n->src[1] = w >> 8 & 0xff;
    } else if (w >> 23 == 0x17f) {                     /* SOPP */
        n->encoding = NS_SOPP; n->number = w >> 16 & 0x7f; n->imm = (int16_t)(w & 0xffff);
    } else if (w >> 28 == 0xb) {                       /* SOPK */
        n->encoding = NS_SOPK; n->number = w >> 23 & 0x1f; n->dst = w >> 16 & 0x7f; n->imm = (int16_t)(w & 0xffff);
    } else if (w >> 30 == 2) {                         /* SOP2 */
        n->encoding = NS_SOP2; n->number = w >> 23 & 0x7f;
        n->src[0] = w & 0xff; n->src[1] = w >> 8 & 0xff; n->dst = w >> 16 & 0x7f;
    } else if (w >> 27 == 0x18) {                      /* SMRD */
        n->encoding = NS_SMRD; n->number = w >> 22 & 0x1f;
        n->dst = w >> 15 & 0x7f; n->src[0] = (w >> 9 & 0x3f) * 2;
        n->smrd_imm = w >> 8 & 1;
        if (n->smrd_imm) n->imm = (int32_t)(w & 0xff) * 4;
        else if ((w & 0xff) == NS_LITERAL) n->smrd_imm = 1, n->imm = (int32_t)(w2 * 4), n->words = 2;   /* in dwords */
        else n->imm = (int32_t)(w & 0xff);
    } else {
        n->words = 2;
        switch (w >> 26) {
        case 0x34:                                     /* VOP3 */
            n->encoding = NS_VOP3; n->number = w >> 17 & 0x1ff;
            n->dst = w & 0xff;
            n->src[0] = w2 & 0x1ff; n->src[1] = w2 >> 9 & 0x1ff; n->src[2] = w2 >> 18 & 0x1ff;
            n->omod = w2 >> 27 & 3; n->neg = w2 >> 29 & 7;
            if (n->number < 256) {                     /* VOPC in VOP3: a scalar mask destination */
                n->sdst = n->dst; n->dst = 0; n->abs = w >> 8 & 7; n->clamp = w >> 11 & 1;
            } else {
                n->dst += NS_VGPR; n->abs = w >> 8 & 7; n->clamp = w >> 11 & 1;
            }
            break;
        case 0x32:                                     /* VINTRP */
            n->words = 1;
            n->encoding = NS_VINTRP; n->number = w >> 16 & 3;
            n->src[0] = NS_VGPR + (w & 0xff); n->chan = w >> 8 & 3; n->attr = w >> 10 & 0x3f;
            n->dst = NS_VGPR + (w >> 18 & 0xff);
            break;
        case 0x36: n->encoding = NS_DS; n->number = w >> 18 & 0xff; break;
        case 0x38: case 0x3a:                          /* MUBUF, MTBUF */
            n->encoding = (w >> 26) == 0x38 ? NS_MUBUF : NS_MTBUF;
            n->number = n->encoding == NS_MUBUF ? (w >> 18 & 0x7f) : (w >> 16 & 7);
            n->offset = w & 0xfff; n->offen = w >> 12 & 1; n->idxen = w >> 13 & 1; n->glc = w >> 14 & 1;
            n->addr64 = w >> 15 & 1;
            if (n->encoding == NS_MTBUF) n->dfmt = w >> 19 & 0xf, n->nfmt = w >> 23 & 7;
            n->vaddr = w2 & 0xff; n->vdata = w2 >> 8 & 0xff; n->srsrc = (w2 >> 16 & 0x1f) * 4;
            n->slc = w2 >> 22 & 1; n->tfe = w2 >> 23 & 1; n->soffset = w2 >> 24 & 0xff;
            break;
        case 0x3c:                                     /* MIMG */
            n->encoding = NS_MIMG; n->number = w >> 18 & 0x7f;
            n->dmask = w >> 8 & 0xf; n->unorm = w >> 12 & 1; n->glc = w >> 13 & 1; n->da = w >> 14 & 1;
            n->r128 = w >> 15 & 1; n->tfe = w >> 16 & 1; n->lwe = w >> 17 & 1; n->slc = w >> 25 & 1;
            n->vaddr = w2 & 0xff; n->vdata = w2 >> 8 & 0xff; n->srsrc = (w2 >> 16 & 0x1f) * 4;
            n->ssamp = (w2 >> 21 & 0x1f) * 4;
            break;
        case 0x3e:                                     /* EXP */
            n->encoding = NS_EXP;
            n->en = w & 0xf; n->target = w >> 4 & 0x3f; n->compr = w >> 10 & 1; n->done = w >> 11 & 1;
            n->vm = w >> 12 & 1;
            for (int i = 0; i < 3; ++i) n->src[i] = NS_VGPR + (w2 >> (8 * i) & 0xff);
            n->literal = w2 >> 24;   /* the fourth source VGPR */
            break;
        default:
            return 0;
        }
    }
    if (n->encoding != NS_EXP && n->encoding != NS_SMRD && n->encoding != NS_MIMG && n->encoding != NS_MTBUF &&
        n->encoding != NS_MUBUF && n->words < 3 && literal_used(n)) {
        if (n->encoding == NS_VOP3) return 0;          /* VOP3 cannot take a literal */
        n->literal = at + n->words < words ? code[at + n->words] : 0;
        n->words += 1;
    }
    if (at + n->words > words) return 0;
    n->operation = operation_of(n->encoding, n->number);
    return 1;
}

/* ---- Disassembly ---- */

static int operand(char *out, size_t size, uint16_t o, const NsInst *n) {
    if (o < 104) return snprintf(out, size, "s%u", o);
    if (o >= NS_VGPR) return snprintf(out, size, "v%u", o - NS_VGPR);
    if (o >= 129 && o <= 192) return snprintf(out, size, "%d", o - 128);
    if (o >= 193 && o <= 208) return snprintf(out, size, "%d", 192 - o);
    static const float floats[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
    if (o >= 240 && o <= 247) return snprintf(out, size, "%g", floats[o - 240]);
    switch (o) {
    case 128: return snprintf(out, size, "0");
    case NS_VCC_LO: return snprintf(out, size, "vcc_lo");
    case NS_VCC_HI: return snprintf(out, size, "vcc_hi");
    case NS_M0: return snprintf(out, size, "m0");
    case NS_EXEC_LO: return snprintf(out, size, "exec_lo");
    case NS_EXEC_HI: return snprintf(out, size, "exec_hi");
    case NS_VCCZ: return snprintf(out, size, "vccz");
    case NS_EXECZ: return snprintf(out, size, "execz");
    case NS_SCC: return snprintf(out, size, "scc");
    case NS_LITERAL: return snprintf(out, size, "0x%x", n->literal);
    default: return snprintf(out, size, "?%u", o);
    }
}

void ns_print(const NsInst *n, char *out, size_t size) {
    size_t used = (size_t)snprintf(out, size, "%-24s", ns_operation_name(n->operation));
    char a[32];
#define PUT(...) do { if (used < size) used += (size_t)snprintf(out + used, size - used, __VA_ARGS__); } while (0)
    switch (n->encoding) {
    case NS_SMRD:
        PUT("s%u, s[%u:%u], %s0x%x", n->dst, n->src[0], n->src[0] + 1, n->smrd_imm ? "" : "s", (unsigned)n->imm);
        break;
    case NS_SOPP:
        PUT("%d", n->imm);
        break;
    case NS_MIMG:
        PUT("v%u, v%u, s[%u], s[%u] dmask 0x%x", n->vdata, n->vaddr, n->srsrc, n->ssamp, n->dmask);
        break;
    case NS_MTBUF: case NS_MUBUF:
        PUT("v%u, v%u, s[%u] +%u%s%s soffset %u fmt %u/%u", n->vdata, n->vaddr, n->srsrc, n->offset,
            n->idxen ? " idxen" : "", n->offen ? " offen" : "", n->soffset, n->dfmt, n->nfmt);
        break;
    case NS_EXP:
        PUT("target %u en 0x%x%s%s v%u v%u v%u v%u", n->target, n->en, n->compr ? " compr" : "", n->done ? " done" : "",
            n->src[0] - NS_VGPR, n->src[1] - NS_VGPR, n->src[2] - NS_VGPR, n->literal);
        break;
    case NS_VINTRP:
        PUT("v%u, v%u, attr%u.%c", n->dst - NS_VGPR, n->src[0] - NS_VGPR, n->attr, "xyzw"[n->chan]);
        break;
    default:
        if (n->encoding == NS_VOPC || (n->encoding == NS_VOP3 && n->number < 256)) {
            operand(a, sizeof(a), n->sdst, n);
            PUT("%s", a);
        } else if (n->encoding != NS_SOPC) {
            operand(a, sizeof(a), n->dst, n);
            PUT("%s", a);
        }
        for (int i = 0; i < 3; ++i) {
            if (n->src[i] == 0xffff) continue;
            operand(a, sizeof(a), n->src[i], n);
            PUT("%s%s%s%s", (i || n->encoding != NS_SOPC) ? ", " : "", (n->neg >> i & 1) ? "-" : "",
                (n->abs >> i & 1) ? "|" : "", a);
            if (n->abs >> i & 1) PUT("|");
        }
        if (n->operation == NS_V_MADMK_F32 || n->operation == NS_V_MADAK_F32) PUT(" k=0x%x", n->literal);
        if (n->clamp) PUT(" clamp");
        if (n->omod) PUT(" omod%u", n->omod);
        if (n->encoding == NS_SOPK) PUT(", %d", n->imm);
        break;
    }
#undef PUT
}

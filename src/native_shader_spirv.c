/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GCN to SPIR-V for the native renderer (src/native_shader.h).
 *
 * The model, one shader invocation per GCN lane:
 *   - Each VGPR is a u32 Function variable; float operations bitcast. Drivers fold the
 *     variables into registers.
 *   - Scalar registers are tracked while translating: a user register or a register loaded from
 *     a resource table holds a descriptor or a table pointer, known by its game slot, so the
 *     resource it names is known here. Anything else is a u32 value in a Function variable.
 *   - 64-bit lane masks (EXEC, VCC, saved masks) are one bool per invocation: this lane's bit.
 *     Vector writes keep the old value where EXEC is off, once EXEC can be partial; a pixel
 *     whose EXEC is off at its export is demoted (alpha kill).
 *   - Branches: "skip when no lane is active" jumps fall through (inactive lanes write nothing);
 *     while and do-while loops become structured loops; anything else fails the shader.
 *
 * The interface (src/native_vk.c): push constant {u64 records}; record = records + index * 544,
 * the index from gl_InstanceIndex (VS) passed flat at location 15; record.slot[8] (texture
 * index | sampler index << 20) at 0, record.block[2][16] (constant buffer addresses, VS then PS)
 * at 32, record.stream[16] {address lo, hi, stride, format} at 288; textures and samplers in set
 * 0, bindings 0 and 1, as unbounded arrays. VS parameter export k goes to location
 * semantic - 15; PS input i reads location semantic - 15; colour target 0 is location 0. */
#include "native_shader.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

enum { RECORD_SIZE = 544, SLOT_OFFSET = 0, BLOCK_OFFSET = 32, STREAM_OFFSET = 288, BLOCKS_PER_STAGE = 16,
       RECORD_SLOTS = 8, RECORD_STREAMS = 16, INDEX_LOCATION = 15, CONSTANT_BUFFER_STRIDE = 16 };

/* ---- SPIR-V words ---- */

typedef struct { uint32_t *w; size_t n, cap; } Buf;

/* A shader is a few KiB: running out of memory here means the process cannot go on. */
static void *need(void *p) {
    if (!p) {
        fputs("Native: out of memory translating shaders\n", stderr);
        abort();
    }
    return p;
}

static void put(Buf *b, uint32_t word) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->w = need(realloc(b->w, b->cap * 4));
    }
    b->w[b->n++] = word;
}
static void op_n(Buf *b, uint32_t code, const uint32_t *operands, size_t count) {
    put(b, (uint32_t)(count + 1) << 16 | code);
    for (size_t i = 0; i < count; ++i) put(b, operands[i]);
}
#define OP(b, code, ...) op_n(b, code, (const uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / 4)
#define OP0(b, code) op_n(b, code, NULL, 0)
/* An instruction with a string operand after `before` words of operands. */
static void op_string(Buf *b, uint32_t code, const uint32_t *before, size_t count, const char *text,
                      const uint32_t *after, size_t after_count) {
    const size_t length = strlen(text), string_words = length / 4 + 1;
    put(b, (uint32_t)(1 + count + string_words + after_count) << 16 | code);
    for (size_t i = 0; i < count; ++i) put(b, before[i]);
    for (size_t i = 0; i < string_words; ++i) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; ++k) {
            const size_t at = i * 4 + k;
            if (at < length) w |= (uint32_t)(uint8_t)text[at] << (8 * k);
        }
        put(b, w);
    }
    for (size_t i = 0; i < after_count; ++i) put(b, after[i]);
}

enum {   /* opcodes */
    OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeRuntimeArray = 29, OpTypeStruct = 30,
    OpTypePointer = 32, OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43,
    OpConstantComposite = 44, OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62,
    OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpSampledImage = 86, OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88,
    OpConvertSToF = 111, OpUConvert = 113, OpConvertUToPtr = 120, OpBitcast = 124, OpIAdd = 128, OpFAdd = 129,
    OpISub = 130, OpFSub = 131, OpIMul = 132, OpFMul = 133, OpFDiv = 136, OpLogicalOr = 166,
    OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169, OpUGreaterThan = 172, OpSLessThan = 177,
    OpFOrdEqual = 180, OpFOrdNotEqual = 182, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186,
    OpFOrdLessThanEqual = 188, OpFOrdGreaterThanEqual = 190, OpShiftRightLogical = 194,
    OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196, OpBitwiseAnd = 199, OpLoopMerge = 246,
    OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251,
    OpReturn = 253, OpGroupNonUniformAny = 335, OpGroupNonUniformBroadcastFirst = 338,
    OpDemoteToHelperInvocation = 5380,
};
enum {   /* GLSL.std.450 */
    GlslFloor = 8, GlslFract = 10, GlslSin = 13, GlslCos = 14, GlslExp2 = 29, GlslLog2 = 30, GlslSqrt = 31,
    GlslFAbs = 4, GlslFClamp = 43, GlslFma = 50, GlslPackHalf2x16 = 58, GlslUnpackHalf2x16 = 62,
    GlslUnpackUnorm4x8 = 64, GlslNMin = 79, GlslNMax = 80,
};
enum {
    StorageUniformConstant = 0, StorageInput = 1, StorageOutput = 3, StorageFunction = 7, StoragePushConstant = 9,
    StoragePhysical = 5349, DecorationBlock = 2, DecorationBuiltIn = 11, DecorationFlat = 14,
    DecorationNoContraction = 42, DecorationLocation = 30, DecorationBinding = 33, DecorationDescriptorSet = 34,
    DecorationOffset = 35, BuiltInPosition = 0, BuiltInVertexIndex = 42, BuiltInInstanceIndex = 43,
    MemoryAligned = 2, ScopeSubgroup = 3,
};

/* ---- Translation state ---- */

enum { K_NONE, K_VALUE, K_DESC, K_PTR, K_MASK };
enum { T_RESOURCE, T_SAMPLER, T_CONST, T_VERTEX, T_FETCH, T_USER_DATA };   /* descriptor / table types */
typedef struct { uint8_t kind, type, word; uint16_t slot; } Scalar;

typedef struct {
    const NsShader *s;
    char *error;
    size_t error_size;
    int failed;
    uint32_t next_id;
    Buf caps, header, decorations, types, vars, prologue, body;
    uint32_t interface[64];
    int interface_count;
    /* types */
    uint32_t t_void, t_bool, t_u32, t_f32, t_u64, t_v2u32, t_v4u32, t_v2f32, t_v4f32, t_fn;
    uint32_t t_image, t_sampler, t_sampled, p_fn_u32, p_fn_bool, p_psb_u32, p_psb_u64, p_psb_v4u32;
    uint32_t glsl, c_true, c_false;
    uint32_t constants_u[1024], constants_v[1024], constants_f[1024], constants_fv[1024];
    int constant_count, constant_f_count;
    uint32_t pointers[32][3];      /* storage class, pointee, pointer type */
    int pointer_count;
    /* globals */
    uint32_t push, textures, samplers, index_var, vertex_index, position, outputs[32], inputs[32], color;
    /* prologue values */
    uint32_t record, block_address[BLOCKS_PER_STAGE], slot_word[RECORD_SLOTS];
    /* registers */
    uint32_t vgpr[256], sgpr[128], mask[64], scc;
    Scalar sc[128];
    Scalar user[256];             /* the user data registers, extended user data from 16 */
    int mask_true[64];            /* the mask is known all-ones (every lane) */
    int exec_partial;             /* EXEC may be off in some lane */
    int scc_valid;
} Ctx;

static void fail(Ctx *c, const char *format, ...) {
    if (c->failed) return;
    c->failed = 1;
    va_list args;
    va_start(args, format);
    vsnprintf(c->error, c->error_size, format, args);
    va_end(args);
}

static uint32_t id(Ctx *c) { return c->next_id++; }

/* Pointer types are declared once each (duplicate type declarations are invalid). */
static uint32_t type_pointer(Ctx *c, uint32_t storage, uint32_t type) {
    for (int i = 0; i < c->pointer_count; ++i)
        if (c->pointers[i][0] == storage && c->pointers[i][1] == type) return c->pointers[i][2];
    const uint32_t r = id(c);
    OP(&c->types, OpTypePointer, r, storage, type);
    if (c->pointer_count < 32) {
        c->pointers[c->pointer_count][0] = storage;
        c->pointers[c->pointer_count][1] = type;
        c->pointers[c->pointer_count++][2] = r;
    }
    return r;
}

static uint32_t const_u32(Ctx *c, uint32_t value) {
    for (int i = 0; i < c->constant_count; ++i) if (c->constants_v[i] == value) return c->constants_u[i];
    const uint32_t r = id(c);
    OP(&c->types, OpConstant, c->t_u32, r, value);
    if (c->constant_count < 1024) c->constants_v[c->constant_count] = value, c->constants_u[c->constant_count++] = r;
    return r;
}
static uint32_t const_f32(Ctx *c, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    for (int i = 0; i < c->constant_f_count; ++i) if (c->constants_fv[i] == bits) return c->constants_f[i];
    const uint32_t r = id(c);
    OP(&c->types, OpConstant, c->t_f32, r, bits);
    if (c->constant_f_count < 1024) c->constants_fv[c->constant_f_count] = bits, c->constants_f[c->constant_f_count++] = r;
    return r;
}
static uint32_t const_u64(Ctx *c, uint64_t value) {
    const uint32_t r = id(c);
    OP(&c->types, OpConstant, c->t_u64, r, (uint32_t)value, (uint32_t)(value >> 32));
    return r;
}

static void decorate(Ctx *c, uint32_t target, uint32_t decoration) { OP(&c->decorations, OpDecorate, target, decoration); }
static void decorate1(Ctx *c, uint32_t target, uint32_t decoration, uint32_t value) {
    OP(&c->decorations, OpDecorate, target, decoration, value);
}

static uint32_t global(Ctx *c, uint32_t pointer_type, uint32_t storage) {
    const uint32_t r = id(c);
    OP(&c->types, OpVariable, pointer_type, r, storage);
    if (c->interface_count < 64) c->interface[c->interface_count++] = r;
    return r;
}

/* ---- Body helpers (b: the buffer to emit into) ---- */

static uint32_t emit1(Ctx *c, Buf *b, uint32_t code, uint32_t type, uint32_t a) {
    const uint32_t r = id(c);
    OP(b, code, type, r, a);
    return r;
}
static uint32_t emit2(Ctx *c, Buf *b, uint32_t code, uint32_t type, uint32_t a, uint32_t x) {
    const uint32_t r = id(c);
    OP(b, code, type, r, a, x);
    return r;
}
static uint32_t emit3(Ctx *c, Buf *b, uint32_t code, uint32_t type, uint32_t a, uint32_t x, uint32_t y) {
    const uint32_t r = id(c);
    OP(b, code, type, r, a, x, y);
    return r;
}
static uint32_t ext1(Ctx *c, uint32_t type, uint32_t fn, uint32_t a) {
    const uint32_t r = id(c);
    OP(&c->body, OpExtInst, type, r, c->glsl, fn, a);
    return r;
}
static uint32_t ext2(Ctx *c, uint32_t type, uint32_t fn, uint32_t a, uint32_t x) {
    const uint32_t r = id(c);
    OP(&c->body, OpExtInst, type, r, c->glsl, fn, a, x);
    return r;
}
static uint32_t ext3(Ctx *c, uint32_t type, uint32_t fn, uint32_t a, uint32_t x, uint32_t y) {
    const uint32_t r = id(c);
    OP(&c->body, OpExtInst, type, r, c->glsl, fn, a, x, y);
    return r;
}
static uint32_t load(Ctx *c, Buf *b, uint32_t type, uint32_t pointer) { return emit1(c, b, OpLoad, type, pointer); }
static uint32_t bitcast(Ctx *c, uint32_t type, uint32_t value) { return emit1(c, &c->body, OpBitcast, type, value); }
static uint32_t label(Ctx *c, uint32_t l) { OP(&c->body, OpLabel, l); return l; }

/* A load through a physical address (u64) at byte offset `offset` from `base`. */
static uint32_t load_psb(Ctx *c, Buf *b, uint32_t type, uint32_t pointer_type, uint32_t base, uint64_t offset,
                         uint32_t align) {
    const uint32_t address = offset ? emit2(c, b, OpIAdd, c->t_u64, base, const_u64(c, offset)) : base;
    const uint32_t pointer = emit1(c, b, OpConvertUToPtr, pointer_type, address);
    const uint32_t r = id(c);
    OP(b, OpLoad, type, r, pointer, MemoryAligned, align);
    return r;
}

/* ---- Registers ---- */

static uint32_t function_var(Ctx *c, uint32_t pointer_type, uint32_t initial) {
    const uint32_t r = id(c);
    OP(&c->vars, OpVariable, pointer_type, r, StorageFunction, initial);
    return r;
}
static uint32_t vgpr_var(Ctx *c, unsigned v) {
    if (!c->vgpr[v]) c->vgpr[v] = function_var(c, c->p_fn_u32, const_u32(c, 0));
    return c->vgpr[v];
}
static uint32_t sgpr_var(Ctx *c, unsigned s) {
    if (s >= 128) { fail(c, "scalar register %u", s); s = 0; }
    if (!c->sgpr[s]) c->sgpr[s] = function_var(c, c->p_fn_u32, const_u32(c, 0));
    return c->sgpr[s];
}
static uint32_t mask_var(Ctx *c, unsigned s) {
    if (!c->mask[s / 2]) c->mask[s / 2] = function_var(c, c->p_fn_bool, c->c_true);
    return c->mask[s / 2];
}
static uint32_t read_vgpr(Ctx *c, unsigned v) { return load(c, &c->body, c->t_u32, vgpr_var(c, v)); }

static uint32_t exec_value(Ctx *c) {
    return c->mask_true[NS_EXEC_LO / 2] ? c->c_true : load(c, &c->body, c->t_bool, mask_var(c, NS_EXEC_LO));
}

/* VGPR v = value, where EXEC is on. */
static void write_vgpr(Ctx *c, unsigned v, uint32_t value) {
    const uint32_t var = vgpr_var(c, v);
    if (c->exec_partial) {
        const uint32_t old = load(c, &c->body, c->t_u32, var);
        value = emit3(c, &c->body, OpSelect, c->t_u32, exec_value(c), value, old);
    }
    OP(&c->body, OpStore, var, value);
}
static void write_sgpr(Ctx *c, unsigned s, uint32_t value) {
    if (s >= 128) { fail(c, "write to scalar register %u", s); return; }
    c->sc[s] = (Scalar){K_VALUE, 0, 0, 0};
    OP(&c->body, OpStore, sgpr_var(c, s), value);
}

static int inline_value(unsigned o, uint32_t *value) {
    static const float floats[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
    if (o == 128) { *value = 0; return 1; }
    if (o >= 129 && o <= 192) { *value = o - 128; return 1; }
    if (o >= 193 && o <= 208) { *value = (uint32_t)(192 - (int)o); return 1; }
    if (o >= 240 && o <= 247) { memcpy(value, &floats[o - 240], 4); return 1; }
    return 0;
}

/* A 32-bit source operand as u32. */
static uint32_t read_src(Ctx *c, const NsInst *n, unsigned o) {
    uint32_t value;
    if (o >= NS_VGPR) return read_vgpr(c, o - NS_VGPR);
    if (o == NS_LITERAL) return const_u32(c, n->literal);
    if (inline_value(o, &value)) return const_u32(c, value);
    if (o < 104 || o == NS_VCC_LO || o == NS_VCC_HI || o == NS_M0) {
        const Scalar *k = &c->sc[o];
        if (k->kind == K_DESC || k->kind == K_PTR) fail(c, "descriptor register s%u read as a value", o);
        if (k->kind == K_MASK) fail(c, "mask register s%u read as a value", o);
        return load(c, &c->body, c->t_u32, sgpr_var(c, o));
    }
    fail(c, "operand %u not handled", o);
    return const_u32(c, 0);
}

static uint32_t float_src(Ctx *c, const NsInst *n, int i) {
    uint32_t v = bitcast(c, c->t_f32, read_src(c, n, n->src[i]));
    if (n->abs >> i & 1) v = ext1(c, c->t_f32, GlslFAbs, v);
    if (n->neg >> i & 1) v = emit1(c, &c->body, 127 /* OpFNegate */, c->t_f32, v);
    return v;
}

/* Output modifiers and clamp of a VOP3 float result, then the store. */
static void write_float(Ctx *c, const NsInst *n, uint32_t value) {
    if (n->omod) {
        static const float scale[] = {1.0f, 2.0f, 4.0f, 0.5f};
        value = emit2(c, &c->body, OpFMul, c->t_f32, value, const_f32(c, scale[n->omod]));
    }
    if (n->clamp) value = ext3(c, c->t_f32, GlslFClamp, value, const_f32(c, 0.0f), const_f32(c, 1.0f));
    write_vgpr(c, n->dst - NS_VGPR, bitcast(c, c->t_u32, value));
}

static uint32_t precise(Ctx *c, uint32_t code, uint32_t a, uint32_t b) {
    const uint32_t r = emit2(c, &c->body, code, c->t_f32, a, b);
    decorate(c, r, DecorationNoContraction);
    return r;
}

/* A lane mask operand (a 64-bit scalar source) as this lane's bool. */
static uint32_t read_mask(Ctx *c, unsigned o) {
    if (o == NS_EXEC_LO) return exec_value(c);
    if (o == 128) return c->c_false;
    if (o == 193) return c->c_true;   /* -1: every lane */
    if ((o < 104 || o == NS_VCC_LO) && !(o & 1)) {
        if (c->sc[o].kind != K_MASK) fail(c, "s%u used as a lane mask", o);
        return c->mask_true[o / 2] ? c->c_true : load(c, &c->body, c->t_bool, mask_var(c, o));
    }
    fail(c, "mask operand %u not handled", o);
    return c->c_false;
}
static int mask_known_true(Ctx *c, unsigned o) {
    if (o == 193) return 1;
    if (o == NS_EXEC_LO || ((o < 104 || o == NS_VCC_LO) && !(o & 1))) return c->mask_true[o / 2];
    return 0;
}
static void write_mask(Ctx *c, unsigned dst, uint32_t value, int known_true) {
    if (dst & 1 || (dst >= 104 && dst != NS_VCC_LO && dst != NS_EXEC_LO)) { fail(c, "mask written to %u", dst); return; }
    OP(&c->body, OpStore, mask_var(c, dst), value);
    c->mask_true[dst / 2] = known_true;
    if (dst != NS_EXEC_LO) c->sc[dst] = c->sc[dst + 1] = (Scalar){K_MASK, 0, 0, 0};
    else if (!known_true) c->exec_partial = 1;
}
/* SCC of a 64-bit mask result: some lane of the wave has its bit set. */
static void set_scc_any(Ctx *c, uint32_t lane) {
    const uint32_t any = emit2(c, &c->body, OpGroupNonUniformAny, c->t_bool, const_u32(c, ScopeSubgroup), lane);
    OP(&c->body, OpStore, c->scc, any);
    c->scc_valid = 1;
}

/* ---- Records ---- */

static uint32_t block_address(Ctx *c, unsigned slot) {
    if (slot >= BLOCKS_PER_STAGE) { fail(c, "constant buffer slot %u", slot); slot = 0; }
    if (!c->block_address[slot]) {
        const unsigned block = slot + (c->s->vertex ? 0 : BLOCKS_PER_STAGE);
        c->block_address[slot] = load_psb(c, &c->prologue, c->t_u64, c->p_psb_u64, c->record, BLOCK_OFFSET + 8 * block, 8);
    }
    return c->block_address[slot];
}
static uint32_t slot_word(Ctx *c, unsigned slot) {
    if (slot >= RECORD_SLOTS) { fail(c, "texture slot %u", slot); slot = 0; }
    if (!c->slot_word[slot])
        c->slot_word[slot] = load_psb(c, &c->prologue, c->t_u32, c->p_psb_u32, c->record, SLOT_OFFSET + 4 * slot, 4);
    return c->slot_word[slot];
}

/* ---- Scalar memory ---- */

static void scalar_load(Ctx *c, const NsInst *n, unsigned count) {
    const Scalar *base = &c->sc[n->src[0]];
    if (n->dst + count > 104 && !(n->dst == NS_VCC_LO && count == 2)) { fail(c, "load into s%u", n->dst); return; }
    if (n->operation >= NS_S_BUFFER_LOAD_DWORD) {   /* a constant buffer */
        if (base->kind != K_DESC || base->type != T_CONST || base->word != 0) {
            fail(c, "s_buffer_load from s%u, not a constant buffer", n->src[0]);
            return;
        }
        uint32_t address = block_address(c, base->slot), offset_value = 0;
        if (!n->smrd_imm) {
            offset_value = emit1(c, &c->body, OpUConvert, c->t_u64, read_src(c, n, (unsigned)n->imm));
            address = emit2(c, &c->body, OpIAdd, c->t_u64, address, offset_value);
        }
        for (unsigned i = 0; i < count; ++i)
            write_sgpr(c, n->dst + i,
                       load_psb(c, &c->body, c->t_u32, c->p_psb_u32, address, (uint64_t)(n->smrd_imm ? n->imm : 0) + 4 * i, 4));
        return;
    }
    /* s_load from a table: descriptors, known by slot. */
    if (base->kind != K_PTR || !n->smrd_imm) { fail(c, "s_load from s%u, not a table", n->src[0]); return; }
    for (unsigned i = 0; i < count; ++i) {
        const unsigned byte = (unsigned)n->imm + 4 * i;
        Scalar k;
        if (base->type == T_USER_DATA) {
            k = c->user[16 + byte / 4 < 256 ? 16 + byte / 4 : 255];
        } else {
            const unsigned entry = base->type == T_RESOURCE ? 32 : 16;
            k = (Scalar){K_DESC, base->type, (uint8_t)(byte % entry / 4), (uint16_t)(byte / entry)};
            if (base->type == T_FETCH) k.kind = K_NONE;
        }
        c->sc[n->dst + i] = k;
    }
}

/* ---- Vertex inputs: stream i of the record feeds input i ---- */

static void pull_inputs(Ctx *c) {
    const NsShader *s = c->s;
    const uint32_t vertex = load(c, &c->body, c->t_u32, c->vertex_index);
    for (uint32_t i = 0; i < s->input_count && i < RECORD_STREAMS; ++i) {
        const unsigned dest = s->inputs[i] >> 8 & 0xff, elements = s->inputs[i] >> 16 & 0xff;
        const uint32_t stream = load_psb(c, &c->body, c->t_v4u32, c->p_psb_v4u32, c->record, STREAM_OFFSET + 16 * i, 16);
        const uint32_t lo = emit2(c, &c->body, OpCompositeExtract, c->t_u32, stream, 0);
        const uint32_t hi = emit2(c, &c->body, OpCompositeExtract, c->t_u32, stream, 1);
        const uint32_t stride = emit2(c, &c->body, OpCompositeExtract, c->t_u32, stream, 2);
        const uint32_t format = emit2(c, &c->body, OpCompositeExtract, c->t_u32, stream, 3);
        const uint32_t base = bitcast(c, c->t_u64, emit2(c, &c->body, OpCompositeConstruct, c->t_v2u32, lo, hi));
        const uint32_t offset = emit1(c, &c->body, OpUConvert, c->t_u64, emit2(c, &c->body, OpIMul, c->t_u32, vertex, stride));
        const uint32_t address = emit2(c, &c->body, OpIAdd, c->t_u64, base, offset);
        /* Defaults (0, 0, 0, 1); format 1-4: that many 32-bit words, 5: four unorm8 as floats,
         * 6: four uint8 as integers. */
        const uint32_t zero = const_u32(c, 0), one = const_u32(c, 0x3f800000u);
        for (unsigned k = 0; k < elements; ++k) OP(&c->body, OpStore, vgpr_var(c, dest + k), k == 3 ? one : zero);
        uint32_t cases[6], merge = id(c);
        for (int k = 0; k < 6; ++k) cases[k] = id(c);
        OP(&c->body, OpSelectionMerge, merge, 0);
        OP(&c->body, OpSwitch, format, merge, 1, cases[0], 2, cases[1], 3, cases[2], 4, cases[3], 5, cases[4], 6, cases[5]);
        for (unsigned words = 1; words <= 4; ++words) {
            label(c, cases[words - 1]);
            for (unsigned k = 0; k < words && k < elements; ++k)
                OP(&c->body, OpStore, vgpr_var(c, dest + k), load_psb(c, &c->body, c->t_u32, c->p_psb_u32, address, 4 * k, 4));
            OP(&c->body, OpBranch, merge);
        }
        label(c, cases[4]);
        {
            const uint32_t packed = load_psb(c, &c->body, c->t_u32, c->p_psb_u32, address, 0, 4);
            const uint32_t v = ext1(c, c->t_v4f32, GlslUnpackUnorm4x8, packed);
            for (unsigned k = 0; k < elements && k < 4; ++k)
                OP(&c->body, OpStore, vgpr_var(c, dest + k), bitcast(c, c->t_u32, emit2(c, &c->body, OpCompositeExtract, c->t_f32, v, k)));
            OP(&c->body, OpBranch, merge);
        }
        label(c, cases[5]);
        {
            const uint32_t packed = load_psb(c, &c->body, c->t_u32, c->p_psb_u32, address, 0, 4);
            for (unsigned k = 0; k < elements && k < 4; ++k) {
                const uint32_t shifted = emit2(c, &c->body, OpShiftRightLogical, c->t_u32, packed, const_u32(c, 8 * k));
                OP(&c->body, OpStore, vgpr_var(c, dest + k), emit2(c, &c->body, OpBitwiseAnd, c->t_u32, shifted, const_u32(c, 0xff)));
            }
            OP(&c->body, OpBranch, merge);
        }
        label(c, merge);
    }
}

/* ---- Vector memory ---- */

static void buffer_load(Ctx *c, const NsInst *n) {
    const Scalar *k = &c->sc[n->srsrc];
    if (k->kind != K_DESC || k->type != T_CONST || k->word != 0) { fail(c, "tbuffer_load from s%u, not a constant buffer", n->srsrc); return; }
    if (n->dfmt != 14 || n->nfmt != 7 || n->addr64 || n->tfe) { fail(c, "tbuffer_load format %u/%u", n->dfmt, n->nfmt); return; }
    /* address = buffer + index * stride + offset + immediate offset + soffset */
    uint32_t bytes = const_u32(c, n->offset);
    unsigned v = n->vaddr;
    if (n->idxen) {
        const uint32_t index = read_vgpr(c, v++);
        bytes = emit2(c, &c->body, OpIAdd, c->t_u32, bytes, emit2(c, &c->body, OpIMul, c->t_u32, index, const_u32(c, CONSTANT_BUFFER_STRIDE)));
    }
    if (n->offen) bytes = emit2(c, &c->body, OpIAdd, c->t_u32, bytes, read_vgpr(c, v));
    if (n->soffset != 128) bytes = emit2(c, &c->body, OpIAdd, c->t_u32, bytes, read_src(c, n, n->soffset));
    const uint32_t address = emit2(c, &c->body, OpIAdd, c->t_u64, block_address(c, k->slot), emit1(c, &c->body, OpUConvert, c->t_u64, bytes));
    const unsigned count = n->number + 1u;
    for (unsigned i = 0; i < count; ++i) write_vgpr(c, n->vdata + i, load_psb(c, &c->body, c->t_u32, c->p_psb_u32, address, 4 * i, 4));
}

static void image_sample(Ctx *c, const NsInst *n) {
    const Scalar *t = &c->sc[n->srsrc], *sm = &c->sc[n->ssamp];
    if (t->kind != K_DESC || t->type != T_RESOURCE || t->word != 0) { fail(c, "image_sample texture s%u not a known slot", n->srsrc); return; }
    if (sm->kind != K_DESC || sm->type != T_SAMPLER || sm->word != 0) { fail(c, "image_sample sampler s%u not a known slot", n->ssamp); return; }
    if (sm->slot != t->slot) { fail(c, "texture slot %u with sampler slot %u", t->slot, sm->slot); return; }
    if (n->unorm || n->da || n->r128 || n->tfe || n->lwe) { fail(c, "image_sample flags"); return; }
    const uint32_t word = slot_word(c, t->slot);
    const uint32_t texture_index = emit2(c, &c->body, OpBitwiseAnd, c->t_u32, word, const_u32(c, 0xfffff));
    const uint32_t sampler_index = emit2(c, &c->body, OpShiftRightLogical, c->t_u32, word, const_u32(c, 20));
    const uint32_t texture = load(c, &c->body, c->t_image,
                                  emit2(c, &c->body, OpAccessChain, type_pointer(c, StorageUniformConstant, c->t_image), c->textures, texture_index));
    const uint32_t sampler = load(c, &c->body, c->t_sampler,
                                  emit2(c, &c->body, OpAccessChain, type_pointer(c, StorageUniformConstant, c->t_sampler), c->samplers, sampler_index));
    const uint32_t sampled = emit2(c, &c->body, OpSampledImage, c->t_sampled, texture, sampler);
    const uint32_t u = bitcast(c, c->t_f32, read_vgpr(c, n->vaddr)), v = bitcast(c, c->t_f32, read_vgpr(c, n->vaddr + 1u));
    const uint32_t uv = emit2(c, &c->body, OpCompositeConstruct, c->t_v2f32, u, v);
    const uint32_t r = id(c);
    if (c->s->vertex) OP(&c->body, OpImageSampleExplicitLod, c->t_v4f32, r, sampled, uv, 2 /* Lod */, const_f32(c, 0.0f));
    else OP(&c->body, OpImageSampleImplicitLod, c->t_v4f32, r, sampled, uv);
    unsigned dst = n->vdata;
    for (unsigned k = 0; k < 4; ++k)
        if (n->dmask >> k & 1) write_vgpr(c, dst++, bitcast(c, c->t_u32, emit2(c, &c->body, OpCompositeExtract, c->t_f32, r, k)));
}

/* ---- Exports ---- */

static uint32_t output_var(Ctx *c, unsigned location) {
    if (location >= 32) { fail(c, "output location %u", location); location = 0; }
    if (!c->outputs[location]) {
        c->outputs[location] = global(c, type_pointer(c, StorageOutput, c->t_v4f32), StorageOutput);
        decorate1(c, c->outputs[location], DecorationLocation, location);
    }
    return c->outputs[location];
}

static void export(Ctx *c, const NsInst *n) {
    uint32_t v[4];
    if (n->compr) {
        for (int h = 0; h < 2; ++h) {
            const uint32_t pair = ext1(c, c->t_v2f32, GlslUnpackHalf2x16, read_vgpr(c, n->src[h] - NS_VGPR));
            v[2 * h] = emit2(c, &c->body, OpCompositeExtract, c->t_f32, pair, 0);
            v[2 * h + 1] = emit2(c, &c->body, OpCompositeExtract, c->t_f32, pair, 1);
        }
    } else {
        const unsigned regs[4] = {n->src[0] - NS_VGPR, n->src[1] - NS_VGPR, n->src[2] - NS_VGPR, n->literal};
        for (int k = 0; k < 4; ++k)
            v[k] = (n->en >> k & 1) ? bitcast(c, c->t_f32, read_vgpr(c, regs[k])) : const_f32(c, k == 3 ? 1.0f : 0.0f);
    }
    const uint32_t vec = id(c);
    OP(&c->body, OpCompositeConstruct, c->t_v4f32, vec, v[0], v[1], v[2], v[3]);
    if (c->s->vertex) {
        if (n->target == 12) { OP(&c->body, OpStore, c->position, vec); return; }
        if (n->target >= 32 && n->target < 64) {
            const unsigned k = n->target - 32u;
            if (k >= c->s->export_count) return;   /* no semantic: nothing reads it (full-screen helpers) */
            const unsigned semantic = c->s->exports[k] & 0xff;
            if (semantic < 15) { fail(c, "parameter semantic %u", semantic); return; }
            OP(&c->body, OpStore, output_var(c, semantic - 15), vec);
            return;
        }
        fail(c, "vertex export target %u", n->target);
        return;
    }
    if (n->target == 9) return;   /* null export */
    if (n->target != 0) { fail(c, "pixel export target %u", n->target); return; }
    if (c->exec_partial) {   /* lanes whose EXEC is off export nothing: demote them */
        const uint32_t merge = id(c), kill = id(c), on = exec_value(c);
        OP(&c->body, OpSelectionMerge, merge, 0);
        OP(&c->body, OpBranchConditional, on, merge, kill);
        label(c, kill);
        OP0(&c->body, OpDemoteToHelperInvocation);
        OP(&c->body, OpBranch, merge);
        label(c, merge);
    }
    if (!c->color) {
        c->color = global(c, type_pointer(c, StorageOutput, c->t_v4f32), StorageOutput);
        decorate1(c, c->color, DecorationLocation, 0);
    }
    OP(&c->body, OpStore, c->color, vec);
}

static uint32_t input_var(Ctx *c, unsigned attr) {
    if (attr >= c->s->input_count) { fail(c, "interpolated attribute %u of %u", attr, c->s->input_count); attr = 0; }
    if (!c->inputs[attr]) {
        const unsigned semantic = c->s->inputs[attr] & 0xff;
        if (semantic < 15) fail(c, "input semantic %u", semantic);
        c->inputs[attr] = global(c, type_pointer(c, StorageInput, c->t_v4f32), StorageInput);
        decorate1(c, c->inputs[attr], DecorationLocation, semantic - 15);
        if (c->s->inputs[attr] >> 10 & 1) decorate(c, c->inputs[attr], DecorationFlat);
    }
    return c->inputs[attr];
}

/* ---- Instructions ---- */

static uint32_t f_compare(Ctx *c, int operation, uint32_t a, uint32_t b) {
    uint32_t code = OpFOrdLessThan;
    switch (operation) {
    case NS_V_CMP_LT_F32: code = OpFOrdLessThan; break;
    case NS_V_CMP_EQ_F32: code = OpFOrdEqual; break;
    case NS_V_CMP_LE_F32: code = OpFOrdLessThanEqual; break;
    case NS_V_CMP_GT_F32: code = OpFOrdGreaterThan; break;
    case NS_V_CMP_LG_F32: code = OpFOrdNotEqual; break;
    case NS_V_CMP_GE_F32: code = OpFOrdGreaterThanEqual; break;
    }
    return emit2(c, &c->body, code, c->t_bool, a, b);
}

static void vector_alu(Ctx *c, const NsInst *n) {
    const int op = n->operation;
    const unsigned d = n->dst - NS_VGPR;
    uint32_t a, b, r;
    switch (op) {
    case NS_V_MOV_B32: write_vgpr(c, d, read_src(c, n, n->src[0])); return;
    case NS_V_ADD_F32: write_float(c, n, precise(c, OpFAdd, float_src(c, n, 0), float_src(c, n, 1))); return;
    case NS_V_SUB_F32: write_float(c, n, precise(c, OpFSub, float_src(c, n, 0), float_src(c, n, 1))); return;
    case NS_V_SUBREV_F32: write_float(c, n, precise(c, OpFSub, float_src(c, n, 1), float_src(c, n, 0))); return;
    case NS_V_MUL_F32: write_float(c, n, precise(c, OpFMul, float_src(c, n, 0), float_src(c, n, 1))); return;
    case NS_V_MIN_F32: write_float(c, n, ext2(c, c->t_f32, GlslNMin, float_src(c, n, 0), float_src(c, n, 1))); return;
    case NS_V_MAX_F32: write_float(c, n, ext2(c, c->t_f32, GlslNMax, float_src(c, n, 0), float_src(c, n, 1))); return;
    case NS_V_MAC_F32:
        r = precise(c, OpFMul, float_src(c, n, 0), float_src(c, n, 1));
        write_float(c, n, precise(c, OpFAdd, r, bitcast(c, c->t_f32, read_vgpr(c, d))));
        return;
    case NS_V_MADMK_F32:   /* d = s0 * K + s1 */
        a = bitcast(c, c->t_f32, const_u32(c, n->literal));
        r = precise(c, OpFMul, float_src(c, n, 0), a);
        write_float(c, n, precise(c, OpFAdd, r, float_src(c, n, 1)));
        return;
    case NS_V_MADAK_F32:   /* d = s0 * s1 + K */
        r = precise(c, OpFMul, float_src(c, n, 0), float_src(c, n, 1));
        write_float(c, n, precise(c, OpFAdd, r, bitcast(c, c->t_f32, const_u32(c, n->literal))));
        return;
    case NS_V_MAD_F32:
        r = precise(c, OpFMul, float_src(c, n, 0), float_src(c, n, 1));
        write_float(c, n, precise(c, OpFAdd, r, float_src(c, n, 2)));
        return;
    case NS_V_FMA_F32: write_float(c, n, ext3(c, c->t_f32, GlslFma, float_src(c, n, 0), float_src(c, n, 1), float_src(c, n, 2))); return;
    case NS_V_FLOOR_F32: write_float(c, n, ext1(c, c->t_f32, GlslFloor, float_src(c, n, 0))); return;
    case NS_V_FRACT_F32: write_float(c, n, ext1(c, c->t_f32, GlslFract, float_src(c, n, 0))); return;
    case NS_V_EXP_F32: write_float(c, n, ext1(c, c->t_f32, GlslExp2, float_src(c, n, 0))); return;
    case NS_V_LOG_F32: write_float(c, n, ext1(c, c->t_f32, GlslLog2, float_src(c, n, 0))); return;
    case NS_V_SQRT_F32: write_float(c, n, ext1(c, c->t_f32, GlslSqrt, float_src(c, n, 0))); return;
    case NS_V_RCP_F32: write_float(c, n, emit2(c, &c->body, OpFDiv, c->t_f32, const_f32(c, 1.0f), float_src(c, n, 0))); return;
    case NS_V_SIN_F32: case NS_V_COS_F32:   /* the input is in revolutions */
        r = precise(c, OpFMul, float_src(c, n, 0), const_f32(c, 6.28318530717958647692f));
        write_float(c, n, ext1(c, c->t_f32, op == NS_V_SIN_F32 ? GlslSin : GlslCos, r));
        return;
    case NS_V_CVT_F32_I32: write_float(c, n, emit1(c, &c->body, OpConvertSToF, c->t_f32, read_src(c, n, n->src[0]))); return;
    case NS_V_CVT_PKRTZ_F16_F32:
        a = emit2(c, &c->body, OpCompositeConstruct, c->t_v2f32, float_src(c, n, 0), float_src(c, n, 1));
        write_vgpr(c, d, ext1(c, c->t_u32, GlslPackHalf2x16, a));
        return;
    case NS_V_CNDMASK_B32: {   /* d = mask ? s1 : s0; the mask is VCC (VOP2) or src2 (VOP3) */
        const uint32_t m = read_mask(c, n->encoding == NS_VOP3 ? n->src[2] : NS_VCC_LO);
        a = read_src(c, n, n->src[0]);
        b = read_src(c, n, n->src[1]);
        if (n->abs || n->neg) fail(c, "v_cndmask with modifiers");
        write_vgpr(c, d, emit3(c, &c->body, OpSelect, c->t_u32, m, b, a));
        return;
    }
    case NS_V_ADD_I32:   /* the carry out (VCC) is not modelled */
        write_vgpr(c, d, emit2(c, &c->body, OpIAdd, c->t_u32, read_src(c, n, n->src[0]), read_src(c, n, n->src[1])));
        if (n->encoding == NS_VOP2) c->sc[NS_VCC_LO] = c->sc[NS_VCC_HI] = (Scalar){K_NONE, 0, 0, 0};
        return;
    case NS_V_AND_B32:
        write_vgpr(c, d, emit2(c, &c->body, OpBitwiseAnd, c->t_u32, read_src(c, n, n->src[0]), read_src(c, n, n->src[1])));
        return;
    case NS_V_LSHLREV_B32: case NS_V_LSHRREV_B32: case NS_V_ASHRREV_I32: {   /* d = s1 shifted by s0 */
        const uint32_t amount = emit2(c, &c->body, OpBitwiseAnd, c->t_u32, read_src(c, n, n->src[0]), const_u32(c, 31));
        const uint32_t code = op == NS_V_LSHLREV_B32 ? OpShiftLeftLogical : op == NS_V_LSHRREV_B32 ? OpShiftRightLogical : OpShiftRightArithmetic;
        write_vgpr(c, d, emit2(c, &c->body, code, c->t_u32, read_src(c, n, n->src[1]), amount));
        return;
    }
    case NS_V_SAD_U32: {   /* |s0 - s1| + s2, unsigned */
        a = read_src(c, n, n->src[0]);
        b = read_src(c, n, n->src[1]);
        const uint32_t greater = emit2(c, &c->body, OpUGreaterThan, c->t_bool, a, b);
        const uint32_t diff = emit3(c, &c->body, OpSelect, c->t_u32, greater, emit2(c, &c->body, OpISub, c->t_u32, a, b),
                                    emit2(c, &c->body, OpISub, c->t_u32, b, a));
        write_vgpr(c, d, emit2(c, &c->body, OpIAdd, c->t_u32, diff, read_src(c, n, n->src[2])));
        return;
    }
    case NS_V_CMP_LT_F32: case NS_V_CMP_EQ_F32: case NS_V_CMP_LE_F32: case NS_V_CMP_GT_F32:
    case NS_V_CMP_LG_F32: case NS_V_CMP_GE_F32: {
        const uint32_t result = f_compare(c, op, float_src(c, n, 0), float_src(c, n, 1));
        const uint32_t lane = c->exec_partial ? emit2(c, &c->body, OpLogicalAnd, c->t_bool, result, exec_value(c)) : result;
        write_mask(c, n->sdst, lane, 0);
        return;
    }
    default:
        fail(c, "%s not handled", ns_operation_name(op));
    }
}

static void scalar_alu(Ctx *c, const NsInst *n) {
    const int op = n->operation;
    switch (op) {
    case NS_S_MOV_B32: case NS_S_MOVK_I32: {
        if (op == NS_S_MOVK_I32) { write_sgpr(c, n->dst, const_u32(c, (uint32_t)n->imm)); return; }
        const unsigned s = n->src[0];
        if (s < 104 && (c->sc[s].kind == K_DESC || c->sc[s].kind == K_PTR)) { c->sc[n->dst] = c->sc[s]; return; }
        write_sgpr(c, n->dst, read_src(c, n, s));
        return;
    }
    case NS_S_MOV_B64: case NS_S_WQM_B64: {
        const unsigned s = n->src[0];
        if (s < 104 && (c->sc[s].kind == K_DESC || c->sc[s].kind == K_PTR) && op == NS_S_MOV_B64) {
            c->sc[n->dst] = c->sc[s];
            c->sc[n->dst + 1] = c->sc[s + 1];
            return;
        }
        write_mask(c, n->dst, read_mask(c, s), mask_known_true(c, s));
        return;
    }
    case NS_S_AND_B64: case NS_S_OR_B64: case NS_S_ANDN2_B64: case NS_S_NOR_B64: {
        const uint32_t a = read_mask(c, n->src[0]), b = read_mask(c, n->src[1]);
        uint32_t r;
        int known = 0;
        if (op == NS_S_AND_B64) r = emit2(c, &c->body, OpLogicalAnd, c->t_bool, a, b), known = mask_known_true(c, n->src[0]) && mask_known_true(c, n->src[1]);
        else if (op == NS_S_OR_B64) r = emit2(c, &c->body, OpLogicalOr, c->t_bool, a, b), known = mask_known_true(c, n->src[0]) || mask_known_true(c, n->src[1]);
        else if (op == NS_S_ANDN2_B64) r = emit2(c, &c->body, OpLogicalAnd, c->t_bool, a, emit1(c, &c->body, OpLogicalNot, c->t_bool, b));
        else r = emit1(c, &c->body, OpLogicalNot, c->t_bool, emit2(c, &c->body, OpLogicalOr, c->t_bool, a, b));
        write_mask(c, n->dst, r, known);
        set_scc_any(c, r);
        return;
    }
    case NS_S_AND_SAVEEXEC_B64: {
        const uint32_t e = exec_value(c), m = read_mask(c, n->src[0]);
        const int known = c->mask_true[NS_EXEC_LO / 2];
        const uint32_t r = emit2(c, &c->body, OpLogicalAnd, c->t_bool, m, e);
        const int r_known = known && mask_known_true(c, n->src[0]);
        write_mask(c, n->dst, e, known);
        write_mask(c, NS_EXEC_LO, r, r_known);
        set_scc_any(c, r);
        return;
    }
    case NS_S_ADD_I32:
        write_sgpr(c, n->dst, emit2(c, &c->body, OpIAdd, c->t_u32, read_src(c, n, n->src[0]), read_src(c, n, n->src[1])));
        c->scc_valid = 0;   /* overflow: not modelled */
        return;
    case NS_S_MUL_I32:
        write_sgpr(c, n->dst, emit2(c, &c->body, OpIMul, c->t_u32, read_src(c, n, n->src[0]), read_src(c, n, n->src[1])));
        return;
    case NS_S_CMP_LT_I32:
        OP(&c->body, OpStore, c->scc, emit2(c, &c->body, OpSLessThan, c->t_bool, read_src(c, n, n->src[0]), read_src(c, n, n->src[1])));
        c->scc_valid = 1;
        return;
    default:
        fail(c, "%s not handled", ns_operation_name(op));
    }
}

/* ---- Control flow ---- */

typedef struct { uint32_t at; NsInst n; } Line;
enum { BR_NONE, BR_SKIP, BR_WHILE_EXIT, BR_WHILE_BACK, BR_DO_WHILE };
typedef struct { int kind; uint32_t partner; } Branch;   /* partner: line index of the loop's other end */

static uint32_t find_line(const Line *lines, uint32_t count, uint32_t at) {
    for (uint32_t i = 0; i < count; ++i) if (lines[i].at == at) return i;
    return UINT32_MAX;
}
static int is_mask_op(int op) {
    return op == NS_S_AND_B64 || op == NS_S_OR_B64 || op == NS_S_ANDN2_B64 || op == NS_S_NOR_B64 || op == NS_S_AND_SAVEEXEC_B64;
}

/* The branch plan; 0 with an error when a branch is not one of the shapes handled. */
static int plan_branches(Ctx *c, const Line *lines, uint32_t count, Branch *plan) {
    int last_scc_writer = NS_UNKNOWN;
    for (uint32_t i = 0; i < count; ++i) {
        const NsInst *n = &lines[i].n;
        if (n->encoding == NS_SOP2 || n->encoding == NS_SOPC || n->operation == NS_S_AND_SAVEEXEC_B64) last_scc_writer = n->operation;
        if (n->encoding != NS_SOPP || n->operation < NS_S_BRANCH || n->operation > NS_S_CBRANCH_EXECNZ) continue;
        if (plan[i].kind != BR_NONE) continue;   /* the back edge of a while loop, already planned */
        const uint32_t target_at = lines[i].at + 1 + (uint32_t)n->imm;
        const int forward = n->imm >= 0;
        if (n->operation == NS_S_CBRANCH_EXECZ || (n->operation == NS_S_CBRANCH_SCC0 && is_mask_op(last_scc_writer) && forward)) {
            plan[i].kind = BR_SKIP;
            continue;
        }
        if (n->operation == NS_S_BRANCH || n->operation == NS_S_CBRANCH_EXECNZ) {
            fail(c, "branch at %u not handled", lines[i].at);
            return 0;
        }
        const uint32_t target = find_line(lines, count, target_at);
        if (!forward) {   /* do-while: the loop runs from target to here */
            if (target == UINT32_MAX) { fail(c, "loop at %u: target %u", lines[i].at, target_at); return 0; }
            plan[i] = (Branch){BR_DO_WHILE, target};
            continue;
        }
        /* while: the exit jumps past an s_branch back to the loop header */
        if (target == UINT32_MAX || target == 0 || lines[target - 1].n.operation != NS_S_BRANCH || lines[target - 1].n.imm >= 0) {
            fail(c, "forward branch at %u not handled", lines[i].at);
            return 0;
        }
        const uint32_t back = target - 1;
        const uint32_t header = find_line(lines, count, lines[back].at + 1 + (uint32_t)lines[back].n.imm);
        if (header == UINT32_MAX || header > i) { fail(c, "loop at %u: header", lines[i].at); return 0; }
        plan[i] = (Branch){BR_WHILE_EXIT, header};
        plan[back] = (Branch){BR_WHILE_BACK, i};
    }
    return 1;
}

/* The branch condition as a bool: is the branch taken. */
static uint32_t branch_taken(Ctx *c, const NsInst *n) {
    switch (n->operation) {
    case NS_S_CBRANCH_SCC0: case NS_S_CBRANCH_SCC1: {
        if (!c->scc_valid) fail(c, "branch on an SCC that is not modelled");
        const uint32_t scc = load(c, &c->body, c->t_bool, c->scc);
        return n->operation == NS_S_CBRANCH_SCC1 ? scc : emit1(c, &c->body, OpLogicalNot, c->t_bool, scc);
    }
    case NS_S_CBRANCH_VCCZ: case NS_S_CBRANCH_VCCNZ: {
        const uint32_t any = emit2(c, &c->body, OpGroupNonUniformAny, c->t_bool, const_u32(c, ScopeSubgroup), read_mask(c, NS_VCC_LO));
        return n->operation == NS_S_CBRANCH_VCCNZ ? any : emit1(c, &c->body, OpLogicalNot, c->t_bool, any);
    }
    default:
        fail(c, "branch %s not handled", ns_operation_name(n->operation));
        return c->c_false;
    }
}

/* ---- The module ---- */

static void define_types(Ctx *c) {
    c->t_void = id(c); OP(&c->types, OpTypeVoid, c->t_void);
    c->t_bool = id(c); OP(&c->types, OpTypeBool, c->t_bool);
    c->t_u32 = id(c); OP(&c->types, OpTypeInt, c->t_u32, 32, 0);
    c->t_f32 = id(c); OP(&c->types, OpTypeFloat, c->t_f32, 32);
    c->t_u64 = id(c); OP(&c->types, OpTypeInt, c->t_u64, 64, 0);
    c->t_v2u32 = id(c); OP(&c->types, OpTypeVector, c->t_v2u32, c->t_u32, 2);
    c->t_v4u32 = id(c); OP(&c->types, OpTypeVector, c->t_v4u32, c->t_u32, 4);
    c->t_v2f32 = id(c); OP(&c->types, OpTypeVector, c->t_v2f32, c->t_f32, 2);
    c->t_v4f32 = id(c); OP(&c->types, OpTypeVector, c->t_v4f32, c->t_f32, 4);
    c->t_fn = id(c); OP(&c->types, OpTypeFunction, c->t_fn, c->t_void);
    c->t_image = id(c); OP(&c->types, OpTypeImage, c->t_image, c->t_f32, 1 /* 2D */, 0, 0, 0, 1, 0);
    c->t_sampler = id(c); OP(&c->types, OpTypeSampler, c->t_sampler);
    c->t_sampled = id(c); OP(&c->types, OpTypeSampledImage, c->t_sampled, c->t_image);
    c->p_fn_u32 = type_pointer(c, StorageFunction, c->t_u32);
    c->p_fn_bool = type_pointer(c, StorageFunction, c->t_bool);
    c->p_psb_u32 = type_pointer(c, StoragePhysical, c->t_u32);
    c->p_psb_u64 = type_pointer(c, StoragePhysical, c->t_u64);
    c->p_psb_v4u32 = type_pointer(c, StoragePhysical, c->t_v4u32);
    c->c_true = id(c); OP(&c->types, OpConstantTrue, c->t_bool, c->c_true);
    c->c_false = id(c); OP(&c->types, OpConstantFalse, c->t_bool, c->c_false);
}

static void define_globals(Ctx *c) {
    /* push constant { u64 records } */
    const uint32_t block = id(c);
    OP(&c->types, OpTypeStruct, block, c->t_u64);
    decorate(c, block, DecorationBlock);
    OP(&c->decorations, OpMemberDecorate, block, 0, DecorationOffset, 0);
    c->push = global(c, type_pointer(c, StoragePushConstant, block), StoragePushConstant);
    /* bindless tables */
    const uint32_t images = id(c), samplers = id(c);
    OP(&c->types, OpTypeRuntimeArray, images, c->t_image);
    OP(&c->types, OpTypeRuntimeArray, samplers, c->t_sampler);
    c->textures = global(c, type_pointer(c, StorageUniformConstant, images), StorageUniformConstant);
    c->samplers = global(c, type_pointer(c, StorageUniformConstant, samplers), StorageUniformConstant);
    decorate1(c, c->textures, DecorationDescriptorSet, 0);
    decorate1(c, c->textures, DecorationBinding, 0);
    decorate1(c, c->samplers, DecorationDescriptorSet, 0);
    decorate1(c, c->samplers, DecorationBinding, 1);
    if (c->s->vertex) {
        c->index_var = global(c, type_pointer(c, StorageOutput, c->t_u32), StorageOutput);
        c->vertex_index = global(c, type_pointer(c, StorageInput, c->t_u32), StorageInput);
        decorate1(c, c->vertex_index, DecorationBuiltIn, BuiltInVertexIndex);
        c->position = global(c, type_pointer(c, StorageOutput, c->t_v4f32), StorageOutput);
        decorate1(c, c->position, DecorationBuiltIn, BuiltInPosition);
    } else {
        c->index_var = global(c, type_pointer(c, StorageInput, c->t_u32), StorageInput);
    }
    decorate1(c, c->index_var, DecorationLocation, INDEX_LOCATION);
    decorate(c, c->index_var, DecorationFlat);
}

/* record = records + broadcastFirst(index) * 544; the VS passes its index on. */
static void emit_prologue(Ctx *c) {
    Buf *p = &c->prologue;
    const uint32_t member = emit2(c, p, OpAccessChain, type_pointer(c, StoragePushConstant, c->t_u64), c->push, const_u32(c, 0));
    const uint32_t records = load(c, p, c->t_u64, member);
    uint32_t index;
    if (c->s->vertex) {
        const uint32_t instance = global(c, type_pointer(c, StorageInput, c->t_u32), StorageInput);
        decorate1(c, instance, DecorationBuiltIn, BuiltInInstanceIndex);
        index = load(c, p, c->t_u32, instance);
        OP(p, OpStore, c->index_var, index);
    } else {
        index = load(c, p, c->t_u32, c->index_var);
    }
    index = emit2(c, p, OpGroupNonUniformBroadcastFirst, c->t_u32, const_u32(c, ScopeSubgroup), index);
    const uint32_t offset = emit2(c, p, OpIMul, c->t_u64, emit1(c, p, OpUConvert, c->t_u64, index), const_u64(c, RECORD_SIZE));
    c->record = emit2(c, p, OpIAdd, c->t_u64, records, offset);
    if (c->s->vertex) OP(p, OpStore, vgpr_var(c, 0), load(c, p, c->t_u32, c->vertex_index));   /* v0: vertex index */
}

/* The user data registers from the header's input-usage slots. */
static void init_user_data(Ctx *c) {
    for (uint32_t i = 0; i < c->s->slot_count; ++i) {
        const NsSlot *s = &c->s->slots[i];
        Scalar k = {K_DESC, 0, 0, s->api};
        unsigned words = 0;
        switch (s->type) {
        case NS_IMM_RESOURCE: case NS_IMM_RW_RESOURCE: k.type = T_RESOURCE; words = 8; break;
        case NS_IMM_SAMPLER: k.type = T_SAMPLER; words = 4; break;
        case NS_IMM_CONST_BUFFER: k.type = T_CONST; words = 4; break;
        case NS_IMM_VERTEX_BUFFER: k.type = T_VERTEX; words = 4; break;
        case NS_SUB_PTR_FETCH_SHADER: k = (Scalar){K_PTR, T_FETCH, 0, 0}; words = 2; break;
        case NS_PTR_RESOURCE_TABLE: case NS_PTR_RW_RESOURCE_TABLE: k = (Scalar){K_PTR, T_RESOURCE, 0, 0}; words = 2; break;
        case NS_PTR_SAMPLER_TABLE: k = (Scalar){K_PTR, T_SAMPLER, 0, 0}; words = 2; break;
        case NS_PTR_CONST_BUFFER_TABLE: k = (Scalar){K_PTR, T_CONST, 0, 0}; words = 2; break;
        case NS_PTR_VERTEX_BUFFER_TABLE: k = (Scalar){K_PTR, T_VERTEX, 0, 0}; words = 2; break;
        case NS_PTR_EXTENDED_USER_DATA: k = (Scalar){K_PTR, T_USER_DATA, 0, 0}; words = 2; break;
        default: fail(c, "input usage type 0x%x", s->type); return;
        }
        for (unsigned w = 0; w < words && s->reg + w < 256; ++w) {
            Scalar e = k;
            if (k.kind == K_DESC) e.word = (uint8_t)w;
            c->user[s->reg + w] = e;
        }
    }
    for (unsigned r = 0; r < 16; ++r) c->sc[r] = c->user[r];
}

static void assemble(Ctx *c, Buf *out) {
    put(out, 0x07230203u);
    put(out, 0x00010600u);
    put(out, 0);
    put(out, c->next_id + 1);
    put(out, 0);
    for (size_t i = 0; i < c->caps.n; ++i) put(out, c->caps.w[i]);
    for (size_t i = 0; i < c->header.n; ++i) put(out, c->header.w[i]);
    for (size_t i = 0; i < c->decorations.n; ++i) put(out, c->decorations.w[i]);
    for (size_t i = 0; i < c->types.n; ++i) put(out, c->types.w[i]);
}

uint32_t *ns_translate(const NsShader *s, uint32_t *word_count, char *error, size_t error_size) {
    Ctx *c = need(calloc(1, sizeof(Ctx)));
    c->s = s;
    c->error = error;
    c->error_size = error_size;
    c->next_id = 1;
    error[0] = 0;
    *word_count = 0;
    define_types(c);
    c->glsl = id(c);
    define_globals(c);
    c->scc = function_var(c, c->p_fn_bool, c->c_false);
    init_user_data(c);
    c->mask_true[NS_EXEC_LO / 2] = 1;
    emit_prologue(c);

    /* Decode the whole program (code after the first s_endpgm can be a branch target). */
    Line *lines = need(calloc(s->code_words + 1, sizeof(Line)));
    Branch *plan = need(calloc(s->code_words + 1, sizeof(Branch)));
    uint32_t count = 0;
    for (uint32_t at = 0; at < s->code_words; ) {
        if (!ns_decode(s->code, s->code_words, at, &lines[count].n)) break;
        lines[count].at = at;
        at += lines[count].n.words;
        if (lines[count].n.operation == NS_UNKNOWN) {
            fail(c, "instruction at %u (encoding %u, number %u) not handled", lines[count].at, lines[count].n.encoding,
                 lines[count].n.number);
            break;
        }
        ++count;
    }
    if (!c->failed) plan_branches(c, lines, count, plan);

    /* Loops open at their header line. */
    uint32_t open_header = UINT32_MAX, loop_header = 0, loop_continue = 0, loop_merge = 0;
    int ended = 0;
    for (uint32_t i = 0; i < count && !c->failed && !ended; ++i) {
        /* a loop starting here? */
        for (uint32_t j = i; j < count; ++j) {
            if ((plan[j].kind == BR_DO_WHILE || plan[j].kind == BR_WHILE_EXIT) && plan[j].partner == i) {
                if (open_header != UINT32_MAX) { fail(c, "nested loops"); break; }
                open_header = i;
                loop_header = id(c), loop_continue = id(c), loop_merge = id(c);
                OP(&c->body, OpBranch, loop_header);
                label(c, loop_header);
                if (plan[j].kind == BR_DO_WHILE) {
                    const uint32_t body = id(c);
                    OP(&c->body, OpLoopMerge, loop_merge, loop_continue, 0);
                    OP(&c->body, OpBranch, body);
                    label(c, body);
                }
                break;
            }
        }
        const NsInst *n = &lines[i].n;
        switch (plan[i].kind) {
        case BR_SKIP: continue;
        case BR_WHILE_EXIT: {   /* the header ends here: leave when taken */
            const uint32_t taken = branch_taken(c, n), body = id(c);
            OP(&c->body, OpLoopMerge, loop_merge, loop_continue, 0);
            OP(&c->body, OpBranchConditional, taken, loop_merge, body);
            label(c, body);
            continue;
        }
        case BR_WHILE_BACK:
            OP(&c->body, OpBranch, loop_continue);
            label(c, loop_continue);
            OP(&c->body, OpBranch, loop_header);
            label(c, loop_merge);
            open_header = UINT32_MAX;
            continue;
        case BR_DO_WHILE: {   /* taken: run the loop again */
            const uint32_t taken = branch_taken(c, n);
            OP(&c->body, OpBranch, loop_continue);
            label(c, loop_continue);
            OP(&c->body, OpBranchConditional, taken, loop_header, loop_merge);
            label(c, loop_merge);
            open_header = UINT32_MAX;
            continue;
        }
        }
        switch (n->encoding) {
        case NS_SOPP:
            if (n->operation == NS_S_ENDPGM) {
                if (open_header != UINT32_MAX) fail(c, "s_endpgm inside a loop");
                ended = 1;
            } else if (n->operation != NS_S_WAITCNT) {
                fail(c, "%s at %u not handled", ns_operation_name(n->operation), lines[i].at);
            }
            break;
        case NS_SOP1:
            if (n->operation == NS_S_SWAPPC_B64) {   /* the fetch shader call */
                if (!s->vertex || c->sc[n->src[0]].kind != K_PTR || c->sc[n->src[0]].type != T_FETCH) fail(c, "s_swappc_b64 not a fetch shader call");
                else pull_inputs(c);
                break;
            }
            scalar_alu(c, n);
            break;
        case NS_SOP2: case NS_SOPC: case NS_SOPK: scalar_alu(c, n); break;
        case NS_SMRD: {
            static const unsigned counts[] = {1, 2, 4, 8, 16};
            const unsigned which = n->number >= 8 ? n->number - 8 : n->number;
            scalar_load(c, n, which < 5 ? counts[which] : 1);
            break;
        }
        case NS_VOP1: case NS_VOP2: case NS_VOPC: case NS_VOP3: vector_alu(c, n); break;
        case NS_VINTRP: {   /* p1 and p2 both give the interpolated value: the hardware's i/j steps */
            const uint32_t value = load(c, &c->body, c->t_v4f32, input_var(c, n->attr));
            write_vgpr(c, n->dst - NS_VGPR, bitcast(c, c->t_u32, emit2(c, &c->body, OpCompositeExtract, c->t_f32, value, n->chan)));
            break;
        }
        case NS_MTBUF: buffer_load(c, n); break;
        case NS_MIMG: image_sample(c, n); break;
        case NS_EXP: export(c, n); break;
        default: fail(c, "%s not handled", ns_operation_name(n->operation));
        }
    }
    if (!ended && !c->failed) fail(c, "no s_endpgm");
    free(lines);
    free(plan);

    uint32_t *result = NULL;
    if (!c->failed) {
        /* capabilities, the instruction import, the memory model, the entry point */
        static const uint32_t capabilities[] = {1 /* Shader */, 11 /* Int64 */, 61 /* GroupNonUniform */,
                                                62 /* Vote */, 64 /* Ballot */, 5302 /* RuntimeDescriptorArray */,
                                                5347 /* PhysicalStorageBufferAddresses */, 5379 /* Demote */};
        for (size_t i = 0; i < sizeof(capabilities) / 4; ++i) OP(&c->caps, OpCapability, capabilities[i]);
        op_string(&c->header, OpExtInstImport, (const uint32_t[]){c->glsl}, 1, "GLSL.std.450", NULL, 0);
        OP(&c->header, OpMemoryModel, 5348 /* PhysicalStorageBuffer64 */, 1 /* GLSL450 */);
        const uint32_t main = id(c);
        op_string(&c->header, OpEntryPoint, (const uint32_t[]){s->vertex ? 0u : 4u, main}, 2, "main",
                  (const uint32_t *)c->interface, (size_t)c->interface_count);
        if (!s->vertex) OP(&c->header, OpExecutionMode, main, 7 /* OriginUpperLeft */);
        /* the function: variables, prologue, body */
        Buf fn = {0};
        OP(&fn, OpFunction, c->t_void, main, 0, c->t_fn);
        OP(&fn, OpLabel, id(c));
        for (size_t i = 0; i < c->vars.n; ++i) put(&fn, c->vars.w[i]);
        for (size_t i = 0; i < c->prologue.n; ++i) put(&fn, c->prologue.w[i]);
        for (size_t i = 0; i < c->body.n; ++i) put(&fn, c->body.w[i]);
        OP0(&fn, OpReturn);
        OP0(&fn, OpFunctionEnd);
        Buf out = {0};
        assemble(c, &out);
        for (size_t i = 0; i < fn.n; ++i) put(&out, fn.w[i]);
        out.w[3] = c->next_id;   /* the id bound */
        free(fn.w);
        result = out.w;
        *word_count = (uint32_t)out.n;
    }
    free(c->caps.w); free(c->header.w); free(c->decorations.w); free(c->types.w);
    free(c->vars.w); free(c->prologue.w); free(c->body.w);
    free(c);
    return result;
}

/* ---- Every shader of the executable ---- */

typedef struct { const char *out_dir; int ok, failed; } Convert;

static void convert_one(const NsShader *s, void *user) {
    Convert *cv = user;
    char error[256];
    uint32_t words;
    uint32_t *spirv = ns_translate(s, &words, error, sizeof(error));
    if (!spirv) {
        fprintf(stderr, "%08x (%s): %s\n", s->hash, s->vertex ? "vs" : "ps", error);
        ++cv->failed;
        return;
    }
    char path[1200];
    snprintf(path, sizeof(path), "%s/%08x.spv", cv->out_dir, s->hash);
    FILE *f = fopen(path, "wb");
    if (f && fwrite(spirv, 4, words, f) == words) ++cv->ok;
    else ++cv->failed;
    if (f) fclose(f);
    free(spirv);
}

int ns_convert_all(const char *eboot, const char *out_dir) {
    FILE *f = fopen(eboot, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *words = size > 0 ? calloc((size_t)size / 4 + 1, 4) : NULL;
    const int read_ok = words && fread(words, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read_ok) { free(words); return -1; }
#ifdef _WIN32
    _mkdir(out_dir);
#else
    mkdir(out_dir, 0755);
#endif
    Convert cv = {out_dir, 0, 0};
    int unreadable;
    ns_find_shaders(words, (size_t)size, convert_one, &cv, &unreadable);
    free(words);
    printf("Native: %d shaders translated, %d failed\n", cv.ok, cv.failed + unreadable);
    return cv.ok;
}

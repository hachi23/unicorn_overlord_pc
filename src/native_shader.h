/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The native renderer's shader translator: the game's GCN (PS4, Sea Islands) vertex and pixel
 * shaders to SPIR-V for src/native_vk.c, on the player's machine from their own eboot.bin.
 *
 * It covers what Unicorn Overlord's 1726 shaders use (63 instructions, docs/dev/NATIVE_RENDERER.md
 * "Own shader translator") and fails a shader, by name, on anything else. */
#ifndef NATIVE_SHADER_H
#define NATIVE_SHADER_H
#include <stddef.h>
#include <stdint.h>

/* ---- The shader containers in the executable ---- */

/* Input-usage slot of a shader header: which game slot (api) sits in which user SGPR (reg). */
typedef struct { uint8_t type, api, reg, flags; } NsSlot;
enum {
    NS_IMM_RESOURCE = 0x00, NS_IMM_SAMPLER = 0x01, NS_IMM_CONST_BUFFER = 0x02, NS_IMM_VERTEX_BUFFER = 0x03,
    NS_IMM_RW_RESOURCE = 0x04, NS_SUB_PTR_FETCH_SHADER = 0x12, NS_PTR_RESOURCE_TABLE = 0x13,
    NS_PTR_SAMPLER_TABLE = 0x15, NS_PTR_CONST_BUFFER_TABLE = 0x16, NS_PTR_VERTEX_BUFFER_TABLE = 0x17,
    NS_PTR_RW_RESOURCE_TABLE = 0x19, NS_PTR_EXTENDED_USER_DATA = 0x1b,
};

enum { NS_MAX_SLOTS = 48, NS_MAX_SEMANTICS = 32 };
typedef struct {
    uint32_t hash;                  /* low 32 bits of the OrbShdr hash: the .spv file name */
    int vertex;                     /* 1 vertex shader, 0 pixel shader */
    const uint32_t *code;
    uint32_t code_words;
    uint32_t regs[12];              /* the stage's registers from the header (VS 7, PS 12) */
    NsSlot slots[NS_MAX_SLOTS];
    uint32_t slot_count;
    /* VS: semantic | dest VGPR << 8 | elements << 16, in fetch order (stream i feeds input i).
     * PS: 16-bit entries: semantic (low 8 bits), flat (bit 10). */
    uint32_t inputs[NS_MAX_SEMANTICS];
    uint32_t input_count;
    uint16_t exports[NS_MAX_SEMANTICS];   /* VS parameter exports: semantic in the low 8 bits */
    uint32_t export_count;
} NsShader;

/* Calls found() for every vertex and pixel shader in the executable image (words: the file,
 * 4-byte aligned), once per hash. Returns the number found; failed counts containers that could
 * not be read (their hash is printed). */
int ns_find_shaders(const uint32_t *words, size_t bytes, void (*found)(const NsShader *, void *), void *user,
                    int *failed);

/* ---- Instructions ---- */

enum {
    NS_SOP2, NS_SOPK, NS_SOP1, NS_SOPC, NS_SOPP, NS_SMRD, NS_VOP2, NS_VOP1, NS_VOPC, NS_VOP3,
    NS_VINTRP, NS_DS, NS_MUBUF, NS_MTBUF, NS_MIMG, NS_EXP, NS_ENCODINGS
};

/* Operations by name, whatever encoding carries them (a VOP2 add and its VOP3 form are both
 * NS_V_ADD_F32). X(name, encoding of its short form, number there). */
#define NS_OPERATIONS(X) \
    X(S_ADD_I32, NS_SOP2, 2) X(S_AND_B64, NS_SOP2, 15) X(S_OR_B64, NS_SOP2, 17) X(S_ANDN2_B64, NS_SOP2, 21) \
    X(S_NOR_B64, NS_SOP2, 27) X(S_MUL_I32, NS_SOP2, 38) \
    X(S_MOVK_I32, NS_SOPK, 0) \
    X(S_MOV_B32, NS_SOP1, 3) X(S_MOV_B64, NS_SOP1, 4) X(S_WQM_B64, NS_SOP1, 10) X(S_SWAPPC_B64, NS_SOP1, 33) \
    X(S_AND_SAVEEXEC_B64, NS_SOP1, 36) \
    X(S_CMP_LT_I32, NS_SOPC, 4) \
    X(S_ENDPGM, NS_SOPP, 1) X(S_BRANCH, NS_SOPP, 2) X(S_CBRANCH_SCC0, NS_SOPP, 4) X(S_CBRANCH_SCC1, NS_SOPP, 5) \
    X(S_CBRANCH_VCCZ, NS_SOPP, 6) X(S_CBRANCH_VCCNZ, NS_SOPP, 7) X(S_CBRANCH_EXECZ, NS_SOPP, 8) \
    X(S_CBRANCH_EXECNZ, NS_SOPP, 9) X(S_WAITCNT, NS_SOPP, 12) \
    X(S_LOAD_DWORDX4, NS_SMRD, 2) X(S_LOAD_DWORDX8, NS_SMRD, 3) X(S_BUFFER_LOAD_DWORD, NS_SMRD, 8) \
    X(S_BUFFER_LOAD_DWORDX2, NS_SMRD, 9) X(S_BUFFER_LOAD_DWORDX4, NS_SMRD, 10) \
    X(S_BUFFER_LOAD_DWORDX8, NS_SMRD, 11) X(S_BUFFER_LOAD_DWORDX16, NS_SMRD, 12) \
    X(V_CNDMASK_B32, NS_VOP2, 0) X(V_ADD_F32, NS_VOP2, 3) X(V_SUB_F32, NS_VOP2, 4) X(V_SUBREV_F32, NS_VOP2, 5) \
    X(V_MUL_F32, NS_VOP2, 8) X(V_MIN_F32, NS_VOP2, 15) X(V_MAX_F32, NS_VOP2, 16) X(V_LSHRREV_B32, NS_VOP2, 22) \
    X(V_ASHRREV_I32, NS_VOP2, 24) X(V_LSHLREV_B32, NS_VOP2, 26) X(V_AND_B32, NS_VOP2, 27) X(V_MAC_F32, NS_VOP2, 31) \
    X(V_MADMK_F32, NS_VOP2, 32) X(V_MADAK_F32, NS_VOP2, 33) X(V_ADD_I32, NS_VOP2, 37) \
    X(V_CVT_PKRTZ_F16_F32, NS_VOP2, 47) \
    X(V_MOV_B32, NS_VOP1, 1) X(V_CVT_F32_I32, NS_VOP1, 5) X(V_FRACT_F32, NS_VOP1, 32) X(V_FLOOR_F32, NS_VOP1, 36) \
    X(V_EXP_F32, NS_VOP1, 37) X(V_LOG_F32, NS_VOP1, 39) X(V_RCP_F32, NS_VOP1, 42) X(V_SQRT_F32, NS_VOP1, 51) \
    X(V_SIN_F32, NS_VOP1, 53) X(V_COS_F32, NS_VOP1, 54) \
    X(V_CMP_LT_F32, NS_VOPC, 1) X(V_CMP_EQ_F32, NS_VOPC, 2) X(V_CMP_LE_F32, NS_VOPC, 3) X(V_CMP_GT_F32, NS_VOPC, 4) \
    X(V_CMP_LG_F32, NS_VOPC, 5) X(V_CMP_GE_F32, NS_VOPC, 6) \
    X(V_MAD_F32, NS_VOP3, 321) X(V_FMA_F32, NS_VOP3, 331) X(V_SAD_U32, NS_VOP3, 349) \
    X(V_INTERP_P1_F32, NS_VINTRP, 0) X(V_INTERP_P2_F32, NS_VINTRP, 1) \
    X(TBUFFER_LOAD_FORMAT_X, NS_MTBUF, 0) X(TBUFFER_LOAD_FORMAT_XY, NS_MTBUF, 1) \
    X(TBUFFER_LOAD_FORMAT_XYZ, NS_MTBUF, 2) X(TBUFFER_LOAD_FORMAT_XYZW, NS_MTBUF, 3) \
    X(IMAGE_SAMPLE, NS_MIMG, 32) \
    X(EXPORT, NS_EXP, 0)

enum {
    NS_UNKNOWN = 0,
#define X(name, encoding, number) NS_##name,
    NS_OPERATIONS(X)
#undef X
    NS_OPERATION_COUNT
};

/* Operand numbers (9-bit source fields): 0-103 SGPRs, 106/107 VCC, 124 M0, 126/127 EXEC,
 * 128-208 inline integers, 240-247 inline floats, 251 VCCZ, 252 EXECZ, 253 SCC, 255 the
 * literal, 256-511 VGPRs. */
enum {
    NS_VCC_LO = 106, NS_VCC_HI = 107, NS_M0 = 124, NS_EXEC_LO = 126, NS_EXEC_HI = 127,
    NS_VCCZ = 251, NS_EXECZ = 252, NS_SCC = 253, NS_LITERAL = 255, NS_VGPR = 256,
};

typedef struct {
    uint16_t operation;     /* NS_* (NS_UNKNOWN: not in the table above) */
    uint8_t encoding;       /* NS_SOP2 ... */
    uint8_t words;          /* length, literal included */
    uint16_t number;        /* opcode number within the encoding */
    uint16_t src[3];        /* sources as operand numbers */
    uint16_t dst;           /* destination: operand number (VGPRs from 256) */
    uint16_t sdst;          /* VOP3b scalar destination / VOPC-in-VOP3 mask destination */
    uint8_t abs, neg, clamp, omod;
    uint32_t literal;
    int32_t imm;            /* SOPP/SOPK simm16, SMRD offset in bytes */
    uint8_t smrd_imm;       /* SMRD: offset is an immediate (else an SGPR, imm holds its number) */
    /* Memory and export fields. */
    uint8_t dmask, unorm, da, glc, slc, tfe, lwe, r128;
    uint8_t srsrc, ssamp;   /* first SGPR of the resource / sampler descriptor */
    uint8_t vaddr, vdata;   /* VGPR numbers */
    uint16_t offset;        /* MTBUF/MUBUF immediate offset */
    uint8_t offen, idxen, addr64, dfmt, nfmt, soffset;
    uint8_t target, en, compr, done, vm;   /* EXP */
    uint8_t attr, chan;     /* VINTRP */
} NsInst;

/* Decodes the instruction at code[at]; 0 when the words do not form one (past the end). */
int ns_decode(const uint32_t *code, uint32_t words, uint32_t at, NsInst *inst);
const char *ns_operation_name(int operation);
/* One line of disassembly (no newline). */
void ns_print(const NsInst *inst, char *out, size_t size);

/* ---- Translation ---- */

/* SPIR-V for the native renderer (the interface in src/native_vk.c: records, streams, bindless
 * tables). Returns the module (malloc'd, *words set) or NULL with a reason in error. */
uint32_t *ns_translate(const NsShader *shader, uint32_t *words, char *error, size_t error_size);

/* Converts every shader of eboot into out_dir/<hash>.spv. Returns the number converted, -1 when
 * the file or folder cannot be used. */
int ns_convert_all(const char *eboot, const char *out_dir);

#endif

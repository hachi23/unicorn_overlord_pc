/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The shader container scan (src/native_shader.h ns_find_shaders) on hand-built containers:
 * header "Shdr", code starting with s_mov_b32 vcc_hi, n, then the "OrbShdr" footer; and the
 * translation of a hand-assembled shader, checked with spirv-val. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_shader.h"

enum { CONTAINER = 0xc0, HEADER = 0x10, CODE = 0x80, FOOTER = 0xa0 };

/* A pixel shader container at `at` with no slots or inputs and 8 code words; `length` is the
 * footer's code length in bytes (32 for these 8 words). */
static void container(uint8_t *image, size_t at, uint64_t hash, uint32_t length) {
    uint8_t *c = image + at;
    memset(c, 0, CONTAINER);
    memcpy(c + HEADER, "Shdr", 4);
    const uint32_t code[2] = {0xbeeb03ffu, (FOOTER - CODE) / 8 - 1};
    memcpy(c + CODE, code, sizeof(code));
    memcpy(c + FOOTER, "OrbShdr", 7);
    const uint32_t bits = 0u << 2 | length << 8;   /* type 0: pixel shader */
    memcpy(c + FOOTER + 8, &bits, 4);
    memcpy(c + FOOTER + 16, &hash, 8);
}

typedef struct { int calls; uint32_t last_hash, last_words; } Seen;
static void found(const NsShader *shader, void *user) {
    Seen *seen = user;
    ++seen->calls;
    seen->last_hash = shader->hash;
    seen->last_words = shader->code_words;
}

static Seen scan(const uint8_t *image, size_t size, int *failed) {
    Seen seen = {0};
    const int count = ns_find_shaders((const uint32_t *)image, size, found, &seen, failed);
    assert(count == seen.calls);
    return seen;
}

static void a_good_container_is_found(void) {
    static uint8_t image[CONTAINER] __attribute__((aligned(4)));
    container(image, 0, 0x1234abcd, 32);
    int failed = -1;
    const Seen seen = scan(image, sizeof(image), &failed);
    assert(seen.calls == 1 && failed == 0);
    assert(seen.last_hash == 0x1234abcd && seen.last_words == 8);
}

static void a_length_past_the_footer_is_a_failure(void) {
    static uint8_t image[CONTAINER] __attribute__((aligned(4)));
    container(image, 0, 0x1234abcd, 0x1000);   /* 4 KiB of code claimed, 32 bytes there */
    int failed = -1;
    const Seen seen = scan(image, sizeof(image), &failed);
    assert(seen.calls == 0 && failed == 1);
}

/* Each hash once, also after thousands of shaders (the game has 1731). */
static void a_repeated_hash_is_found_once(void) {
    enum { DISTINCT = 5000 };
    const size_t size = (size_t)(DISTINCT + 1) * CONTAINER;
    uint8_t *image = aligned_alloc(4, size);
    for (uint32_t i = 0; i < DISTINCT; ++i) container(image, (size_t)i * CONTAINER, 0x10000 + i, 32);
    container(image, (size_t)DISTINCT * CONTAINER, 0x10000 + DISTINCT - 1, 32);   /* the last again */
    int failed = -1;
    const Seen seen = scan(image, size, &failed);
    assert(seen.calls == DISTINCT && failed == 0);
    free(image);
}

/* A pixel shader that writes 1.0 to colour target 0, assembled by hand:
 *   s_mov_b32 vcc_hi, 3 (the length marker every game shader starts with)
 *   v_mov_b32 v0, 1.0
 *   exp mrt0 v0, v0, v0, v0 done vm
 *   s_endpgm */
static const uint32_t white_ps[] = {0xbeeb03ffu, 3, 0x7e0002f2u, 0xf800180fu, 0x00000000u, 0xbf810000u};

/* 1 when spirv-val accepts the module (Vulkan 1.3 rules, as the renderer loads it). */
static int valid_spirv(const uint32_t *module, uint32_t words) {
    char path[] = "/tmp/native-shader-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "wb");
    fwrite(module, 4, words, f);
    fclose(f);
    char command[256];
    snprintf(command, sizeof(command), "spirv-val --target-env vulkan1.3 %s", path);
    const int status = system(command);
    remove(path);
    if (status == 127 << 8) {
        fputs("spirv-val not found (SPIRV-Tools): needed to check the translator\n", stderr);
        exit(1);
    }
    return status == 0;
}

static void a_pixel_shader_translates_to_valid_spirv(void) {
    NsShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.hash = 0xfeedf00d;
    shader.code = white_ps;
    shader.code_words = sizeof(white_ps) / 4;
    uint32_t words = 0;
    char error[256];
    uint32_t *module = ns_translate(&shader, &words, error, sizeof(error));
    if (!module) fprintf(stderr, "translation failed: %s\n", error);
    assert(module && words > 5 && module[0] == 0x07230203u);
    assert(valid_spirv(module, words));
    free(module);
}

/* An instruction the translator does not know is reported at its own word offset. */
static void an_unknown_instruction_is_reported_where_it_is(void) {
    static const uint32_t code[] = {0xbeeb03ffu, 3, 0x7e0002f2u, 0xc400180fu, 0, 0xbf810000u};   /* word 3: not GCN */
    NsShader shader;
    memset(&shader, 0, sizeof(shader));
    shader.code = code;
    shader.code_words = sizeof(code) / 4;
    uint32_t words = 0;
    char error[256];
    uint32_t *module = ns_translate(&shader, &words, error, sizeof(error));
    assert(!module && words == 0);
    assert(strstr(error, "instruction at 3 "));
}

/* Disassembly: a scalar compare has no destination, so its sources come first. */
static void a_compare_disassembles_without_a_leading_comma(void) {
    static const uint32_t code[] = {0xbf040100u};   /* s_cmp_lt_i32 s0, s1 */
    NsInst inst;
    assert(ns_decode(code, 1, 0, &inst));
    char text[128];
    ns_print(&inst, text, sizeof(text));
    assert(strstr(text, "S_CMP_LT_I32") == text);
    const char *operands = text + strlen("S_CMP_LT_I32");
    while (*operands == ' ') ++operands;
    assert(strcmp(operands, "s0, s1") == 0);
}

int main(void) {
    a_good_container_is_found();
    a_length_past_the_footer_is_a_failure();
    a_repeated_hash_is_found_once();
    a_pixel_shader_translates_to_valid_spirv();
    an_unknown_instruction_is_reported_where_it_is();
    a_compare_disassembles_without_a_leading_comma();
    puts("native shader containers: ok");
    return 0;
}

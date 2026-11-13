/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ns_dump EBOOT                 every instruction of every shader: "hash vs|ps OPERATION"
 * ns_dump EBOOT HASH            the disassembly of one shader, with its header
 * ns_dump EBOOT --spirv OUT_DIR translate every shader (src/native_shader.h) into OUT_DIR
 * Build: cc -std=c11 -O2 -Isrc tools/native/ns_dump.c src/native_shader*.c -o out/ns_dump */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_shader.h"

static uint32_t wanted;

static void listing(const NsShader *s, void *user) {
    (void)user;
    if (getenv("NS_HEADERS")) {   /* one line per shader: hash stage regs */
        printf("%08x %s", s->hash, s->vertex ? "vs" : "ps");
        for (int i = 0; i < (s->vertex ? 7 : 12); ++i) printf(" %08x", s->regs[i]);
        printf("\n");
        return;
    }
    if (wanted && s->hash != wanted) return;
    if (wanted) {
        printf("%08x %s, %u words, %u slots, %u inputs, %u exports\n", s->hash, s->vertex ? "vs" : "ps",
               s->code_words, s->slot_count, s->input_count, s->export_count);
        printf("  regs:");
        for (int i = 0; i < (s->vertex ? 7 : 12); ++i) printf(" %08x", s->regs[i]);
        printf("\n");
        for (uint32_t i = 0; i < s->slot_count; ++i)
            printf("  slot type 0x%02x api %u reg s%u flags 0x%x\n", s->slots[i].type, s->slots[i].api, s->slots[i].reg,
                   s->slots[i].flags);
        for (uint32_t i = 0; i < s->input_count; ++i)
            printf(s->vertex ? "  input %u: semantic %u -> v%u x%u\n" : "  input %u: 0x%04x\n", i,
                   s->vertex ? s->inputs[i] & 0xff : s->inputs[i], s->inputs[i] >> 8 & 0xff, s->inputs[i] >> 16);
        for (uint32_t i = 0; i < s->export_count; ++i) printf("  export %u: 0x%04x\n", i, s->exports[i]);
    }
    NsInst n;
    for (uint32_t at = 0; ns_decode(s->code, s->code_words, at, &n); at += n.words) {
        if (wanted) {
            char text[160];
            ns_print(&n, text, sizeof(text));
            printf("  %4u: %s\n", at, text);
        } else {
            printf("%08x %s %s\n", s->hash, s->vertex ? "vs" : "ps", ns_operation_name(n.operation));
        }
        if (n.operation == NS_UNKNOWN) {
            printf("%08x ?? encoding %u number %u word %08x\n", s->hash, n.encoding, n.number, s->code[at]);
            break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ns_dump EBOOT [HASH | --spirv OUT_DIR]\n"); return 2; }
    if (argc > 3 && !strcmp(argv[2], "--spirv")) {
        const int n = ns_convert_all(argv[1], argv[3]);
        return n > 0 ? 0 : 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *words = calloc((size_t)size / 4 + 1, 4);
    if (fread(words, 1, (size_t)size, f) != (size_t)size) return 1;
    fclose(f);
    if (argc > 2) wanted = (uint32_t)strtoul(argv[2], NULL, 16);
    int failed;
    const int count = ns_find_shaders(words, (size_t)size, listing, NULL, &failed);
    fprintf(stderr, "%d shaders, %d not readable\n", count, failed);
    return 0;
}

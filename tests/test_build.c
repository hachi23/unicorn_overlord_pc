/* src/unicorn_build.h: v01.04 addresses stay; in the Hong Kong 1.05 build they move to where
 * tools/re/map_addresses.py found the same bytes; outside the known ranges there is none. */
#include <assert.h>
#include <stdio.h>
#include "unicorn_build.h"

int main(void) {
    assert(unicorn_address(0x233e34) == 0x233e34);
    unicorn_build = UNICORN_HK0105;
    assert(unicorn_address(0x233e34) == 0x2341f4); /* text language */
    assert(unicorn_address(0x1fdfc0) == 0x1fe370); /* CRILAYLA decompressor */
    assert(unicorn_address(0x735490) == 0x732dc0); /* Options: open */
    assert(unicorn_address(0x2b590) == 0x2b710);   /* text lookup */
    assert(unicorn_address(0xd7c9f0) == 0xd7a930); /* Options rows table */
    assert(unicorn_address(0xaf3350) == 0xaf11a0); /* 9/16 */
    assert(unicorn_address(0xaf26f0) == 0xaf0510); /* RENDERBUFFER heap size */
    /* the graphics library entries the native renderer hooks (src/native_rec.c) */
    assert(unicorn_address(0x2146d0) == 0x214a80); /* Gnmx: target mask */
    assert(unicorn_address(0x215b10) == 0x215ec0); /* Gnmx: primitive setup */
    assert(unicorn_address(0x218ae0) == 0x218ea0); /* Gnmx: colour target */
    assert(unicorn_address(0x21c050) == 0x21c410); /* Gnmx: submit and flip */
    assert(unicorn_address(0x21c1f0) == 0x21c5b0); /* Gnmx: viewport */
    assert(unicorn_address(0x3ede0) == 0x3ef70);   /* engine: fill memory */
    assert(unicorn_address(0x20df20) == 0x20e2d0); /* detile a surface region */
    assert(unicorn_address(0x1602ad8) == 0x1602ad8);
    assert(unicorn_address(0x500000) == 0);
    puts("build: ok");
    return 0;
}

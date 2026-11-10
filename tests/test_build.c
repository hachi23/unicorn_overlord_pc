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
    assert(unicorn_address(0x1602ad8) == 0x1602ad8);
    assert(unicorn_address(0x500000) == 0);
    puts("build: ok");
    return 0;
}

/* src/unicorn_render_scale.h: the RENDERBUFFER heap holds the peak each render size measured
 * (#87) with room to spare, never less than the game's 768 MiB, and fits its 32-bit size. */
#include <assert.h>
#include <stdio.h>
#include "unicorn_render_scale.h"

static uint64_t mib(uint32_t w, uint32_t h) { return unicorn_renderbuffer_bytes(w, h) >> 20; }

int main(void) {
    static const struct { uint32_t w, h, peak; } measured[] = {
        {2132, 900, 288},  {2560, 1080, 360},  {3840, 1080, 557},  {3200, 1800, 571},
        {4266, 1800, 822}, {3840, 2160, 722},  {5120, 2160, 1135}, {7680, 2160, 1997},
    };
    for (unsigned i = 0; i < sizeof(measured) / sizeof(*measured); ++i)
        assert(mib(measured[i].w, measured[i].h) >= measured[i].peak * 115 / 100);
    assert(mib(1280, 720) == 768);
    assert(mib(5120, 2160) == 1392);
    assert(mib(8640, 2160) < 4096);
    assert(unicorn_renderbuffer_bytes(5120, 2160) % (UINT64_C(16) << 20) == 0);
    puts("render targets: ok");
    return 0;
}

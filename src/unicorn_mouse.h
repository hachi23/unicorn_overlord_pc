/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The mouse on the battle map: moving the mouse moves the game's own map cursor (the glove,
 * class UcStCursor) the same way on screen. Mouse buttons are in src/runtime_pad.c.
 *
 * What the game has (v01.04, found with VW_RE_WATCH, src/re_watch.h):
 *   UcStCursor: vtable 0x15012d0, 0xd90 bytes. World position x, y, z floats at +0xa0 and the
 *     last frame's at +0xb0. Screen right is world +x, screen down +z.
 *   Vtable slot 4 (0x4569b0) runs once per frame while the cursor exists.
 *   The battle manager (global pointer 0x234eec0) lists the windows open over the map at +0xb98:
 *     zero while the cursor moves freely.
 *
 * Slot 4 is replaced by unicorn_cursor_stub: it calls the game's method with every register as
 * the game passed it, then unicorn_cursor_after(object). VW_MOUSE=0 turns this off,
 * VW_MOUSE_DEBUG=1 logs each move. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

/* Mouse motion in window heights since the last call (src/runtime_pad.c). */
void runtime_mouse_take(float *dx, float *dy);
/* Tilts the left stick toward (dx, dy) for a moment (src/runtime_pad.c). */
void runtime_mouse_nudge(float dx, float dy);

enum { CURSOR_VTABLE = 0x15012d0, CURSOR_UPDATE_SLOT = 4, CURSOR_UPDATE = 0x4569b0, BATTLE_MANAGER = 0x234eec0 };
__attribute__((used)) void *unicorn_cursor_original;   /* read by the stub */
static int cursor_debug;
static unsigned char *cursor_image;
static int held;                         /* frames to keep checking the last mouse move */
static float held_at[2], held_dir[2];    /* where it put the cursor, and the mouse's direction */

__attribute__((used)) void ABI unicorn_cursor_after(unsigned char *object) {
    float dx, dy;
    runtime_mouse_take(&dx, &dy);
    /* A window over the map (the deploy menu, a unit's orders...): the cursor stays put, and
     * motion meanwhile is dropped, not saved for later. */
    const unsigned char *manager = *(unsigned char **)(cursor_image + BATTLE_MANAGER);
    if (!manager || *(const uint64_t *)(manager + 0xb98)) { held = 0; return; }
    float *p = (float *)(object + 0xa0), *previous = (float *)(object + 0xb0);
    /* After a facility menu the game walks the cursor back to the facility until the stick
     * moves it. Seen as the cursor leaving where the mouse put it: put it back and tilt the
     * stick the mouse's way, which ends that. */
    if (held > 0) {
        --held;
        if (SDL_fabsf(p[0] - held_at[0]) + SDL_fabsf(p[2] - held_at[1]) > 1.0f) {
            p[0] = previous[0] = held_at[0], p[2] = previous[2] = held_at[1];
            runtime_mouse_nudge(held_dir[0], held_dir[1]);
            if (cursor_debug) printf("Mouse: the game pulled the cursor back: nudging the stick\n");
        }
    }
    if (dx == 0 && dy == 0) return;
    /* World units per window height, measured on the first battle map at the default zoom (the
     * camera's tilt shortens screen-vertical distances). */
    const float wx = dx * 925.0f, wz = dy * 1630.0f;
    p[0] += wx, p[2] += wz;
    previous[0] += wx, previous[2] += wz;
    held = 4, held_at[0] = p[0], held_at[1] = p[2], held_dir[0] = dx, held_dir[1] = dy;
    if (cursor_debug) printf("Mouse: moved %.3f %.3f (window heights) -> cursor %.1f %.1f\n", dx, dy, p[0], p[2]);
}

/* Entry: rsp = 8 (mod 16). Keeps rdi (the object), calls the game's method, then the C side
 * with the object, keeping the method's rax and xmm0. */
__asm__(".text\n.globl unicorn_cursor_stub\nunicorn_cursor_stub:\n"
        "push %rdi\n"
        "call *unicorn_cursor_original(%rip)\n"
        "pop %rdi\n"
        "push %rax\n"
        "sub $16, %rsp\n"
        "movdqu %xmm0, (%rsp)\n"
        "call unicorn_cursor_after\n"
        "movdqu (%rsp), %xmm0\n"
        "add $16, %rsp\n"
        "pop %rax\n"
        "ret\n");
void unicorn_cursor_stub(void);

/* v01.04 only: the vtable must hold the method it had when this was written. image: the eboot
 * of a known Unicorn Overlord build, image_bytes how much of it is mapped. */
static void install_unicorn_mouse(unsigned char *image, uint64_t image_bytes) {
    if (getenv("VW_MOUSE") && !strcmp(getenv("VW_MOUSE"), "0")) return;
    if (image_bytes < (uint64_t)BATTLE_MANAGER + 8) return;
    cursor_debug = getenv("VW_MOUSE_DEBUG") != NULL;
    cursor_image = image;
    uint64_t *slot = (uint64_t *)(image + CURSOR_VTABLE) + CURSOR_UPDATE_SLOT;
    if (*slot != (uint64_t)(uintptr_t)(image + CURSOR_UPDATE)) {
        printf("Mouse: the battle map cursor is not where v01.04 has it: the mouse only clicks\n");
        return;
    }
    unicorn_cursor_original = (void *)(uintptr_t)*slot;
    *slot = (uint64_t)(uintptr_t)unicorn_cursor_stub;
    printf("Mouse: the battle map cursor follows the mouse\n");
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The mouse on the battle map: moving the mouse moves the game's own map cursor (the glove,
 * class UcStCursor) the same way on screen. Mouse buttons are in src/runtime_pad.c.
 *
 * What the game has (v01.04, found with VW_RE_WATCH, src/re_watch.h):
 *   UcStCursor: vtable 0x15012d0, 0xd90 bytes. World position x, y, z floats at +0xa0 and the
 *     last frame's at +0xb0. Screen right is world +x, screen down +z.
 *   Vtable slot 4 (0x4569b0) runs once per frame while the cursor exists.
 *   The battle manager (global pointer 0x234eec0; 1.05: see cursor_builds) lists the windows open over the map at +0xb98:
 *     zero while the cursor moves freely.
 *   The camera: field manager (global pointer 0x234eed0) +0xae38. At +0x470, +0x480, +0x490,
 *     +0x4a0 the corners of its view rectangle in the world (top left, top right, bottom right,
 *     bottom left: x, y, z, 1), at +0x4b0..+0x4e0 the normals of its top, right, bottom and left
 *     planes, which meet at the eye. The camera keeps the cursor in the middle of the screen,
 *     so the mouse moves the cursor relative to where it is on screen, not to an absolute
 *     point (the map would run away under a fixed pointer).
 *
 * Slot 4 is replaced by unicorn_cursor_stub: it calls the game's method with every register as
 * the game passed it, then unicorn_cursor_after(object). VW_MOUSE=0 turns this off,
 * VW_MOUSE_DEBUG=1 logs each move. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

/* Mouse motion in window points since the last call, and the window size (src/runtime_pad.c). */
void runtime_mouse_take(float *dx, float *dy, int *window_w, int *window_h);
/* Tilts the left stick toward (dx, dy) for a moment (src/runtime_pad.c). */
void runtime_mouse_nudge(float dx, float dy);
/* Hides the system pointer for a moment while the mouse steers the cursor (gpu/vwgpu.h). */
void vwgpu_mouse_map_active(void);

enum { CURSOR_VTABLE = 0x15012d0, CURSOR_UPDATE_SLOT = 4 };
/* The update method and the battle manager's global per build (Hong Kong 1.05 found with
 * tools/re/map_addresses.py; the vtable did not move). */
static const struct { const char *build; uint32_t update, manager, field; } cursor_builds[] = {
    {"v01.04", 0x4569b0, 0x234eec0, 0x234eed0},
    {"Hong Kong 1.05", 0x4575e0, 0x234eee0, 0x234eef0},
};
static uint32_t battle_manager, field_manager;
static unsigned char *cursor_image;

typedef struct { float x, y, z; } Vec3;
static Vec3 v_sub(Vec3 a, Vec3 b) { return (Vec3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 v_add(Vec3 a, Vec3 b) { return (Vec3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 v_scale(Vec3 a, float s) { return (Vec3){a.x * s, a.y * s, a.z * s}; }
static float v_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 v_cross(Vec3 a, Vec3 b) { return (Vec3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 v_at(const unsigned char *p) { const float *f = (const float *)p; return (Vec3){f[0], f[1], f[2]}; }

/* The camera's view: the rectangle's corner a, its right and down edges, the eye. */
typedef struct { Vec3 a, right, down, eye; } View;
static int camera_view(View *v) {
    const unsigned char *field = *(unsigned char **)(cursor_image + field_manager);
    const unsigned char *c = field ? *(unsigned char **)(field + 0xae38) : NULL;
    if (!c) return 0;
    const Vec3 a = v_at(c + 0x470), b = v_at(c + 0x480), d = v_at(c + 0x4a0);
    const Vec3 top = v_at(c + 0x4b0), right = v_at(c + 0x4c0), left = v_at(c + 0x4e0);
    /* The eye: on the top and left planes (through a) and the right one (through b). */
    const float k[3] = {v_dot(top, a), v_dot(right, b), v_dot(left, a)};
    const Vec3 r0 = top, r1 = right, r2 = left;
    const float det = v_dot(r0, v_cross(r1, r2));
    if (!(SDL_fabsf(det) > 1e-4f)) return 0;
    const Vec3 eye = v_scale(v_add(v_add(v_scale(v_cross(r1, r2), k[0]), v_scale(v_cross(r2, r0), k[1])),
                                   v_scale(v_cross(r0, r1), k[2])), 1.0f / det);
    v->a = a, v->right = v_sub(b, a), v->down = v_sub(d, a), v->eye = eye;
    return v_dot(v->right, v->right) > 1.0f && v_dot(v->down, v->down) > 1.0f;
}
/* Where a world point is on screen (0..1 across the game image). */
static int view_project(const View *v, Vec3 p, float *screen_x, float *screen_y) {
    const Vec3 n = v_cross(v->right, v->down), ray = v_sub(p, v->eye);
    const float along = v_dot(n, ray);
    if (SDL_fabsf(along) < 1e-6f) return 0;
    const Vec3 q = v_sub(v_add(v->eye, v_scale(ray, v_dot(n, v_sub(v->a, v->eye)) / along)), v->a);
    *screen_x = v_dot(q, v->right) / v_dot(v->right, v->right);
    *screen_y = v_dot(q, v->down) / v_dot(v->down, v->down);
    return 1;
}
/* The ground point at height y seen at (screen_x, screen_y) on screen. */
static int view_ground(const View *v, float screen_x, float screen_y, float y, Vec3 *out) {
    const Vec3 through = v_add(v->a, v_add(v_scale(v->right, screen_x), v_scale(v->down, screen_y)));
    const Vec3 ray = v_sub(through, v->eye);
    if (!(ray.y < -1e-4f) || !(v->eye.y > y)) return 0;
    *out = v_add(v->eye, v_scale(ray, (y - v->eye.y) / ray.y));
    return 1;
}
__attribute__((used)) void *unicorn_cursor_original;   /* read by the stub */
static int cursor_debug;
/* After a mouse move: how many more frames to watch for the game pulling the cursor back,
 * where the mouse put it, and which way the mouse went. */
static int watch_frames;
static float placed_at[2], moved_dir[2];
static uint64_t last_frame_ns;

__attribute__((used)) void ABI unicorn_cursor_after(unsigned char *object) {
    float dx, dy;
    int window_w, window_h;
    runtime_mouse_take(&dx, &dy, &window_w, &window_h);
    /* The first frame of the map after menus, scenes or a battle: the motion summed up since
     * the map was last shown is not this frame's, so it is dropped (else the cursor jumps). */
    const uint64_t now = SDL_GetTicksNS();
    if (now - last_frame_ns > 200000000ull) dx = dy = 0;
    last_frame_ns = now;
    /* A window over the map (the deploy menu, a unit's orders...): the cursor stays put, and
     * motion meanwhile is dropped, not saved for later. */
    const unsigned char *manager = *(unsigned char **)(cursor_image + battle_manager);
    if (!manager || *(const uint64_t *)(manager + 0xb98)) { watch_frames = 0; return; }
    vwgpu_mouse_map_active();   /* the mouse steers the cursor: no system pointer over the map */
    float *p = (float *)(object + 0xa0), *previous = (float *)(object + 0xb0);
    /* After a facility menu the game walks the cursor back to the facility until the stick
     * moves it. Seen as the cursor leaving where the mouse put it: put it back and tilt the
     * stick the mouse's way, which ends that. */
    if (watch_frames > 0) {
        --watch_frames;
        if (SDL_fabsf(p[0] - placed_at[0]) + SDL_fabsf(p[2] - placed_at[1]) > 1.0f) {
            p[0] = previous[0] = placed_at[0], p[2] = previous[2] = placed_at[1];
            runtime_mouse_nudge(moved_dir[0], moved_dir[1]);
            if (cursor_debug) printf("Mouse: the game pulled the cursor back: nudging the stick\n");
        }
    }
    if (dx == 0 && dy == 0) return;
    /* The cursor goes where the camera shows it moved by the mouse's motion on screen. The game
     * image is letterboxed in the window at the camera's aspect ratio. */
    float wx = 0, wz = 0;
    View view;
    float screen_x, screen_y;
    Vec3 to;
    int exact = camera_view(&view);
    if (exact) {
        const float aspect = SDL_sqrtf(v_dot(view.right, view.right) / v_dot(view.down, view.down));
        float image_h = (float)window_h;
        if ((float)window_w / aspect < image_h) image_h = (float)window_w / aspect;
        const Vec3 at = {p[0], p[1], p[2]};
        exact = view_project(&view, at, &screen_x, &screen_y) &&
                view_ground(&view, screen_x + dx / (image_h * aspect), screen_y + dy / image_h, p[1], &to);
        if (exact) wx = to.x - p[0], wz = to.z - p[2];
    }
    if (!exact) {
        /* World units per window height at the first battle map's default zoom. */
        wx = dx / (float)window_h * 925.0f, wz = dy / (float)window_h * 1630.0f;
    }
    p[0] += wx, p[2] += wz;
    previous[0] += wx, previous[2] += wz;
    watch_frames = 4, placed_at[0] = p[0], placed_at[1] = p[2], moved_dir[0] = dx, moved_dir[1] = dy;
    if (cursor_debug)
        printf("Mouse: moved %.0f %.0f points (%s) -> cursor %.1f %.1f\n", dx, dy, exact ? "camera" : "estimate", p[0], p[2]);
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

/* The build is told by which method the vtable holds. image: the eboot of a known Unicorn
 * Overlord build, image_bytes how much of it is mapped. */
static void install_unicorn_mouse(unsigned char *image, uint64_t image_bytes) {
    if (getenv("VW_MOUSE") && !strcmp(getenv("VW_MOUSE"), "0")) return;
    if (image_bytes < 0x234eef8) return;   /* every build's globals are inside */
    cursor_debug = getenv("VW_MOUSE_DEBUG") != NULL;
    cursor_image = image;
    uint64_t *slot = (uint64_t *)(image + CURSOR_VTABLE) + CURSOR_UPDATE_SLOT;
    for (size_t i = 0; i < sizeof(cursor_builds) / sizeof(*cursor_builds); ++i) {
        if (*slot != (uint64_t)(uintptr_t)(image + cursor_builds[i].update)) continue;
        battle_manager = cursor_builds[i].manager;
        field_manager = cursor_builds[i].field;
        unicorn_cursor_original = (void *)(uintptr_t)*slot;
        *slot = (uint64_t)(uintptr_t)unicorn_cursor_stub;
        printf("Mouse: the battle map cursor follows the mouse (%s)\n", cursor_builds[i].build);
        return;
    }
    printf("Mouse: the battle map cursor is not where known builds have it: the mouse only clicks\n");
}

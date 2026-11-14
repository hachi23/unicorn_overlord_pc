/* C interface of the GPU library (gpu/platform/): window, Vulkan device, presenter and overlay,
 * and the PS4 GnmDriver, VideoOut and kernel event queue libraries. */
#ifndef VWGPU_H
#define VWGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} VwGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void vwgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int vwgpu_init(const VwGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t vwgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int vwgpu_handle_fault(void *ucontext, void *address);
/* VW_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void vwgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int vwgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int vwgpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int vwgpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int vwgpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int vwgpu_overlay_captures_input(void);
/* The battle map cursor follows the mouse this frame: the window hides the system pointer
 * over it for a moment (src/unicorn_mouse.h). Any thread. */
void vwgpu_mouse_map_active(void);
/* A text setting of vw.ini ("key_map", "pad_map"): 1 and the value in out when it is set. */
int vwgpu_setting_text(const char *key, char *out, uint64_t size);
/* The sticks' dead zone in percent of their travel (vw.ini stick_dead_zone). */
int vwgpu_stick_dead_zone(void);
/* The port's settings as rows of the game's own Options screen (src/unicorn_native_settings.h).
 * kind: 0 section heading, 1 list of choices, 2 off/on, 3 slider from min to max in steps.
 * Values are floats as the game keeps them: a choice index, 0 or 1, or the slider value.
 * fallback: the value "Restore Defaults" sets. format: the slider's number (printf, a double). */
typedef struct {
    const char *label, *help;
    int kind, count;
    const char *const *choices;
    float fallback, min, max, step;
    const char *format;
} VwgpuSettingRow;
enum { VWGPU_ROW_SECTION = 0, VWGPU_ROW_CHOICE = 1, VWGPU_ROW_SWITCH = 2, VWGPU_ROW_SLIDER = 3 };
/* The rows (rows may be NULL): their number. */
int vwgpu_setting_rows(const VwgpuSettingRow **rows);
float vwgpu_setting_value(int row);
/* Applies a row's value at once and writes vw.ini. */
void vwgpu_setting_set(int row, float value);
/* Saves vw.ini and closes the window, as closing it does (the game's Exit Game, the Insert
 * menu's Quit game). Any thread. */
void vwgpu_quit(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned vwgpu_symbol_count(void);
/* The native renderer (src/native_vk.c) shares the presenter's Vulkan device: its handles
 * (VkInstance, VkPhysicalDevice, VkDevice, VkQueue as pointers) and graphics queue family.
 * 0 before the presenter exists. Submits to the queue hold vwgpu_queue_lock. */
int vwgpu_vulkan_handles(void **instance, void **physical, void **device, void **queue, uint32_t *family);
void vwgpu_queue_lock(void);
void vwgpu_queue_unlock(void);
/* Implemented by the native renderer: the image it rendered for the display buffer at that
 * address (VkImage as a pointer, R8G8B8A8, created with mutable format), ready for a
 * fragment shader to sample in SHADER_READ_ONLY_OPTIMAL once earlier queue work completes;
 * 0 when it has none (the presenter then uses its own). */
int native_present_image(uint64_t address, void **image, uint32_t *width, uint32_t *height);
#ifdef __cplusplus
}
#endif
#endif

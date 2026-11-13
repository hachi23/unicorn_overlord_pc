// SPDX-License-Identifier: GPL-2.0-or-later
// The port's graphics platform (gpu/platform): the game window, the Vulkan device the native
// renderer (src/native_vk.c) draws with, the presenter, and the PS4 system libraries the game
// calls around its drawing: GnmDriver (command buffer submission and GPU events), VideoOut
// (display buffers and flips) and the kernel event queues they signal.
//
// Drawing itself is not here: the game's graphics library calls are recorded and replayed by
// src/native_rec.c and src/native_vk.c. What reaches GnmDriver as command buffers is only the
// synchronisation around it (memory labels, end-of-pipe events, waits) and the flip marker.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vulkan/vulkan.h>

#define PS4_ABI __attribute__((sysv_abi))

namespace Frontend {
class WindowSDL;
}

namespace Platform {

using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// ---- Errors the game sees ----
constexpr s32 OK = 0;
constexpr s32 KERNEL_EBADF = s32(0x80020009), KERNEL_EFAULT = s32(0x8002000e),
              KERNEL_EINVAL = s32(0x80020016), KERNEL_ENAMETOOLONG = s32(0x8002003f),
              KERNEL_ETIMEDOUT = s32(0x8002003c);

// ---- vulkan.cpp ----
struct Vulkan {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    u32 family{};
    u32 api_version{};
    VkSurfaceKHR surface{};
    bool memory_budget{};   // VK_EXT_memory_budget
    std::string gpu_name;
};
extern Vulkan vk;
/// Submissions and presents to vk.queue hold it (the renderer submits from the game's thread).
extern std::mutex queue_mutex;
/// The window exists (window thread); creates the instance, surface and device.
bool CreateVulkan(Frontend::WindowSDL& window);
u32 MemoryType(u32 bits, VkMemoryPropertyFlags flags);
/// Device memory in use and the budget, in bytes (0 without VK_EXT_memory_budget).
void MemoryUsage(u64& used, u64& budget);

extern Frontend::WindowSDL* window;
/// The game's title and serial (param.sfo), for the overlay.
extern std::string game_title, game_serial;

// ---- presenter.cpp ----
/// Display formats of VideoOut buffers the presenter distinguishes.
enum class DisplayFormat { Srgb, Unorm, Unorm10, Srgb10 };
void PresenterInit();
/// Shows the image (or black when image is null), scaled into the window, with the overlay on
/// top. Flip thread only. The image is sampled in SHADER_READ_ONLY_OPTIMAL.
void Present(VkImage image, u32 width, u32 height, DisplayFormat format);
/// The last frame again (overlay changes while the game shows none).
void PresentAgain();

// ---- equeue.cpp ----
namespace Equeue {
enum Filter : s16 { Timer = -7, User = -11, VideoOut = -13, GraphicsCore = -14, HrTimer = -15 };
enum Flags : u16 { Add = 1, Delete = 2, OneShot = 0x10, Clear = 0x20 };
struct Event {
    u64 ident;
    s16 filter;
    u16 flags;
    u32 fflags;
    u64 data;
    void* udata;
};
static_assert(sizeof(Event) == 32);
/// Adds (or updates) an event; false when there is no such queue.
bool AddEvent(s64 queue, u64 ident, s16 filter, void* udata);
bool RemoveEvent(s64 queue, u64 ident, s16 filter);
/// Triggers the event on every queue that has it. data: the event's data (VideoOut: the flip
/// or vblank hint, see video_out.cpp).
void Trigger(u64 ident, s16 filter, u64 data);
} // namespace Equeue

// ---- gnm.cpp / video_out.cpp ----
/// The graphics end-of-pipe interrupt (sceGnmAddEqEvent id) and its event type.
constexpr u64 GfxEop = 0x40;
namespace VideoOut {
/// The address of the buffer labels (one u64 per display buffer): 1 from the flip request in a
/// command buffer until the buffer stops being shown.
u64* BufferLabels();
/// A flip from a command buffer (sceGnmSubmitAndFlipCommandBuffers): the command thread calls it
/// when it reaches the flip marker. false when the queue is full.
bool SubmitEopFlip(s32 handle, s32 index, s64 arg);
/// Counts a flip request at submission time (FlipStatus gc_queue_num/flip_pending_num).
bool ReserveFlip(s32 handle);
} // namespace VideoOut

namespace Gnm {
/// Runs call on the command thread after the work submitted before it.
void QueueCall(std::function<void()> call);
} // namespace Gnm

// ---- symbols (vwgpu.cpp) ----
void AddSymbol(const char* nid, void* function);
void RegisterEqueue();
void RegisterGnm();
void RegisterVideoOut();
void RegisterAvPlayer();

} // namespace Platform

// SPDX-License-Identifier: GPL-2.0-or-later
// VideoOut: the display buffers the game registers, its flips and their timing.
//
// One port (handle 1). A flip request names a display buffer; the flip thread takes one per
// vblank (every flip_rate + 1 vblanks), shows the image the native renderer drew for that
// buffer's address (native_present_image), then updates the flip status, triggers the flip
// events and clears the label of the buffer shown before it (sceGnmInsertWaitFlipDone waits
// for that). The vblank runs at VW_VBLANK_HZ (60; 0 for uncapped presets: 480 Hz with a
// frame limit at the display rate, at most 120 FPS, so a finished frame is shown at once).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../vwgpu.h"
#include "platform.h"
#include "vw_overlay.h"

namespace Platform::VideoOut {
namespace {

constexpr s32 ErrorInvalidValue = s32(0x80290001), ErrorInvalidAddress = s32(0x80290002),
              ErrorInvalidPitch = s32(0x80290004), ErrorInvalidTiling = s32(0x80290007),
              ErrorBusy = s32(0x80290009), ErrorInvalidHandle = s32(0x8029000b),
              ErrorInvalidEqueue = s32(0x8029000c), ErrorNoSlot = s32(0x8029000f),
              ErrorSlotOccupied = s32(0x80290010), ErrorFull = s32(0x80290012);

struct BufferAttribute {
    u32 pixel_format;
    s32 tiling_mode;
    s32 aspect_ratio;
    u32 width, height, pitch_in_pixel;
    u32 option, reserved0;
    u64 reserved1;
};
static_assert(sizeof(BufferAttribute) == 40);

struct FlipStatus {
    u64 count;
    u64 process_time;
    u64 tsc;
    s64 flip_arg;
    u64 submit_tsc;
    u64 reserved0;
    s32 gc_queue_num;
    s32 flip_pending_num;
    s32 current_buffer;
    u32 reserved1;
};
static_assert(sizeof(FlipStatus) == 64);

constexpr int Buffers = 16, Groups = 4;
constexpr u64 FlipEventId = 6, VblankEventId = 7;

struct Request {
    s32 index;
    s64 arg;
    bool eop;
};

struct Port {
    std::mutex mutex;
    bool open{};
    s32 flip_rate{};
    BufferAttribute groups[Groups]{};
    bool group_used[Groups]{};
    struct {
        s32 group = -1;
        u64 address = 0;
    } buffers[Buffers];
    alignas(8) u64 labels[Buffers]{};
    FlipStatus status{.flip_arg = -1, .current_buffer = -1};
    s32 shown = -1;
    std::deque<Request> requests;
    std::condition_variable requests_cv;
    std::vector<s64> flip_queues, vblank_queues;
    std::condition_variable vblank_cv;
} port;

std::atomic<u64> vblank_count;

extern "C" uint64_t runtime_process_time_us(void);
u64 Tsc() {
    return u64(std::chrono::steady_clock::now().time_since_epoch().count());
}

u32 Number(const char* name, long fallback) {
    const char* v = std::getenv(name);
    return v && *v ? u32(std::strtol(v, nullptr, 10)) : u32(fallback);
}
u32 DisplayHz() {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    return std::max<u32>(mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60, 60);
}

DisplayFormat FormatOf(u32 pixel_format) {
    switch (pixel_format) {
    case 0x80000000u: case 0x80002200u: return DisplayFormat::Srgb;
    case 0x88000000u: return DisplayFormat::Srgb10;
    case 0x88060000u: case 0x88740000u: return DisplayFormat::Unorm10;
    default: return DisplayFormat::Unorm;
    }
}

// ---- Frame statistics (VW_FRAME_STATS=1): flip rate and the worst interval every 5 s ----
void NoteFlip() {
    static const bool enabled = std::getenv("VW_FRAME_STATS") != nullptr;
    if (!enabled) return;
    static auto window = std::chrono::steady_clock::now(), last = window;
    static u32 frames;
    static double worst;
    const auto now = std::chrono::steady_clock::now();
    worst = std::max(worst, std::chrono::duration<double, std::milli>(now - last).count());
    last = now;
    ++frames;
    const double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds >= 5.0) {
        static const u32 hz = Number("VW_VBLANK_HZ", 60);
        std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz)\n", frames / seconds, worst,
                    hz ? hz : 480);
        std::fflush(stdout);
        window = now;
        frames = 0;
        worst = 0;
    }
}

void Flip(const Request& r) {
    void* image = nullptr;
    u32 width = 0, height = 0;
    DisplayFormat format = DisplayFormat::Srgb;
    if (r.index >= 0 && r.index < Buffers) {
        u64 address;
        {
            std::scoped_lock lock{port.mutex};
            address = port.buffers[r.index].address;
            const s32 group = port.buffers[r.index].group;
            if (group >= 0) format = FormatOf(port.groups[group].pixel_format);
        }
        if (!native_present_image(address, &image, &width, &height)) image = nullptr;
    }
    Present(static_cast<VkImage>(image), width, height, format);
    NoteFlip();
    std::vector<s64> queues;
    {
        std::scoped_lock lock{port.mutex};
        FlipStatus& s = port.status;
        ++s.count;
        s.process_time = runtime_process_time_us();
        s.tsc = Tsc();
        s.flip_arg = r.arg;
        s.current_buffer = r.index;
        if (r.eop) --s.gc_queue_num;
        --s.flip_pending_num;
        if (port.shown >= 0) std::atomic_ref<u64>(port.labels[port.shown]).store(0, std::memory_order_release);
        port.shown = r.index;
    }
    Equeue::Trigger(FlipEventId, Equeue::VideoOut, FlipEventId | u64(r.arg) << 16);
}

void FlipThread() {
    const u32 vblank_hz = [] {
        const u32 hz = Number("VW_VBLANK_HZ", 60);
        return hz ? hz : 480u;
    }();
    // Uncapped presets (VW_VBLANK_HZ=0): limited to the display rate, at most 120 FPS (faster
    // breaks the game's movement timing). VW_FPS_LIMIT overrides.
    const char* limit_env = std::getenv("VW_FPS_LIMIT");
    const u32 frame_limit = limit_env && *limit_env ? Number("VW_FPS_LIMIT", 0)
                            : Number("VW_VBLANK_HZ", 60) ? 0u
                                                         : std::min<u32>(DisplayHz(), 120);
    const auto period = std::chrono::nanoseconds(1000000000 / vblank_hz);
    const auto frame_period = frame_limit ? std::chrono::nanoseconds(1000000000 / frame_limit)
                                          : std::chrono::nanoseconds(0);
    std::printf("VideoOut: vblank %u Hz, frame limit %u FPS\n", vblank_hz, frame_limit);
    auto next_tick = std::chrono::steady_clock::now();
    auto next_flip = next_tick;
    auto last_present = next_tick;
    const auto take = [](Request& r) {
        std::scoped_lock lock{port.mutex};
        if (port.requests.empty()) return false;
        r = port.requests.front();
        port.requests.pop_front();
        return true;
    };
    for (;;) {
        next_tick += period;
        const auto now = std::chrono::steady_clock::now();
        if (next_tick < now - period * 4) next_tick = now + period;   // fell behind: no burst
        s32 rate;
        {
            std::scoped_lock lock{port.mutex};
            rate = port.flip_rate;
        }
        Request r;
        const bool slot = !frame_limit || now >= next_flip;
        if (slot && vblank_count % u64(rate + 1) == 0 && take(r)) {
            if (frame_limit) next_flip = std::max(next_flip + frame_period, now - frame_period);
            Flip(r);
            last_present = std::chrono::steady_clock::now();
        } else if (now - last_present > std::chrono::milliseconds(100) && VwOverlay::Visible()) {
            // No game frames (loading, a dialog waiting): the menu still updates.
            PresentAgain();
            last_present = std::chrono::steady_clock::now();
        }
        {
            std::scoped_lock lock{port.mutex};
            ++vblank_count;
        }
        port.vblank_cv.notify_all();
        Equeue::Trigger(VblankEventId, Equeue::VideoOut, VblankEventId | (vblank_count.load() << 16));
        if (frame_limit && rate == 0) {
            // Between vblanks, a flip is shown as soon as it arrives and the limit allows.
            for (;;) {
                {
                    std::unique_lock lock{port.mutex};
                    if (!port.requests_cv.wait_until(lock, next_tick, [] { return !port.requests.empty(); })) break;
                }
                if (std::chrono::steady_clock::now() < next_flip) {
                    if (next_flip >= next_tick) break;
                    std::this_thread::sleep_until(next_flip);
                }
                if (!take(r)) continue;
                const auto t = std::chrono::steady_clock::now();
                next_flip = std::max(next_flip + frame_period, t - frame_period);
                Flip(r);
                last_present = std::chrono::steady_clock::now();
            }
        }
        std::this_thread::sleep_until(next_tick);
    }
}

void Queue(s32 index, s64 arg, bool eop) {
    {
        std::scoped_lock lock{port.mutex};
        port.requests.push_back({index, arg, eop});
    }
    port.requests_cv.notify_one();
}

// ---- The game's calls ----
s32 PS4_ABI sceVideoOutOpen(s32 /*user*/, s32 bus, s32 index, const void*) {
    if (bus != 0 || index != 0) return ErrorInvalidValue;
    std::scoped_lock lock{port.mutex};
    if (port.open) return ErrorBusy;
    port.open = true;
    static std::once_flag started;
    std::call_once(started, [] { std::thread(FlipThread).detach(); });
    return 1;
}
s32 PS4_ABI sceVideoOutClose(s32 handle) {
    if (handle != 1) return ErrorInvalidHandle;
    std::scoped_lock lock{port.mutex};
    port.open = false;
    for (int g = 0; g < Groups; ++g) port.group_used[g] = false;
    for (auto& b : port.buffers) b = {};
    for (s64 q : port.flip_queues) Equeue::RemoveEvent(q, FlipEventId, Equeue::VideoOut);
    for (s64 q : port.vblank_queues) Equeue::RemoveEvent(q, VblankEventId, Equeue::VideoOut);
    port.flip_queues.clear();
    port.vblank_queues.clear();
    return OK;
}
void PS4_ABI sceVideoOutSetBufferAttribute(BufferAttribute* a, u32 pixel_format, u32 tiling, u32 aspect,
                                          u32 width, u32 height, u32 pitch) {
    std::memset(a, 0, sizeof(*a));
    *a = {pixel_format, s32(tiling), s32(aspect), width, height, pitch, 0, 0, 0};
}
s32 PS4_ABI sceVideoOutRegisterBuffers(s32 handle, s32 start, void* const* addresses, s32 count,
                                      const BufferAttribute* a) {
    if (!addresses || !a) return ErrorInvalidAddress;
    if (handle != 1) return ErrorInvalidHandle;
    if (start < 0 || count < 0 || start + count > Buffers) return ErrorInvalidValue;
    if (a->reserved0 || a->reserved1) return ErrorInvalidValue;
    if (a->width > a->pitch_in_pixel) return ErrorInvalidPitch;
    if (a->tiling_mode < 0 || a->tiling_mode > 1) return ErrorInvalidTiling;
    std::scoped_lock lock{port.mutex};
    if (!port.open) return ErrorInvalidHandle;
    s32 group = 0;
    while (group < Groups && port.group_used[group]) ++group;
    if (group == Groups) return ErrorNoSlot;
    for (s32 i = start; i < start + count; ++i)
        if (port.buffers[i].group >= 0) return ErrorSlotOccupied;
    port.groups[group] = *a;
    port.group_used[group] = true;
    for (s32 i = 0; i < count; ++i) {
        port.buffers[start + i] = {group, reinterpret_cast<u64>(addresses[i])};
        port.labels[start + i] = 0;
    }
    std::printf("VideoOut: %d display buffers %ux%u, format %08x\n", count, a->width, a->height, a->pixel_format);
    return group;
}
s32 PS4_ABI sceVideoOutSetFlipRate(s32 handle, s32 rate) {
    if (handle != 1) return ErrorInvalidHandle;
    std::scoped_lock lock{port.mutex};
    port.flip_rate = std::clamp(rate, 0, 2);
    return OK;
}
s32 PS4_ABI sceVideoOutAddFlipEvent(s64 queue, s32 handle, void* udata) {
    if (handle != 1) return ErrorInvalidHandle;
    if (!Equeue::AddEvent(queue, FlipEventId, Equeue::VideoOut, udata)) return ErrorInvalidEqueue;
    std::scoped_lock lock{port.mutex};
    port.flip_queues.push_back(queue);
    return OK;
}
struct ResolutionStatus {
    s32 full_width, full_height, pane_width, pane_height;
    u64 refresh_rate;
    float screen_size_in_inch;
    u16 flags, reserved0;
    u32 reserved1[3];
};
static_assert(sizeof(ResolutionStatus) == 48);
s32 PS4_ABI sceVideoOutGetResolutionStatus(s32 handle, ResolutionStatus* status) {
    if (handle != 1) return ErrorInvalidHandle;
    std::scoped_lock lock{port.mutex};
    if (!port.open) return ErrorInvalidHandle;
    // The game's output size: 1920x1080 at 59.94 Hz (SCE_VIDEO_OUT_REFRESH_RATE_59_94HZ = 3).
    *status = {1920, 1080, 1920, 1080, 3, 50.0f, 0, 0, {}};
    return OK;
}
s32 PS4_ABI sceVideoOutAddVblankEvent(s64 queue, s32 handle, void* udata) {
    if (handle != 1) return ErrorInvalidHandle;
    if (!Equeue::AddEvent(queue, VblankEventId, Equeue::VideoOut, udata)) return ErrorInvalidEqueue;
    std::scoped_lock lock{port.mutex};
    port.vblank_queues.push_back(queue);
    return OK;
}
s32 PS4_ABI sceVideoOutWaitVblank(s32 handle) {
    if (handle != 1) return ErrorInvalidHandle;
    std::unique_lock lock{port.mutex};
    const u64 before = vblank_count;
    port.vblank_cv.wait(lock, [&] { return vblank_count != before; });
    return OK;
}
s32 PS4_ABI sceVideoOutGetFlipStatus(s32 handle, FlipStatus* status) {
    if (!status) return ErrorInvalidAddress;
    if (handle != 1) return ErrorInvalidHandle;
    std::scoped_lock lock{port.mutex};
    *status = port.status;
    return OK;
}
s32 PS4_ABI sceVideoOutSubmitFlip(s32 handle, s32 index, s32 /*mode*/, s64 arg) {
    if (handle != 1) return ErrorInvalidHandle;
    if (index < -1 || index >= Buffers) return ErrorInvalidValue;
    {
        std::scoped_lock lock{port.mutex};
        if (index >= 0 && port.status.flip_pending_num > 16) return ErrorFull;
        ++port.status.flip_pending_num;
        port.status.submit_tsc = Tsc();
    }
    // After the command buffers submitted before it (their flips and labels).
    Gnm::QueueCall([index, arg] { Queue(index, arg, false); });
    return OK;
}

} // namespace

u64* BufferLabels() {
    return port.labels;
}
bool ReserveFlip(s32 handle) {
    if (handle != 1) return false;
    std::scoped_lock lock{port.mutex};
    if (port.status.flip_pending_num > 16) return false;
    ++port.status.gc_queue_num;
    ++port.status.flip_pending_num;
    port.status.submit_tsc = Tsc();
    return true;
}
bool SubmitEopFlip(s32 handle, s32 index, s64 arg) {
    if (handle != 1) return false;
    Queue(index, arg, true);
    return true;
}

} // namespace Platform::VideoOut

namespace Platform {
void RegisterVideoOut() {
    using namespace VideoOut;
    AddSymbol("Up36PTk687E", (void*)sceVideoOutOpen);
    AddSymbol("uquVH4-Du78", (void*)sceVideoOutClose);
    AddSymbol("i6-sR91Wt-4", (void*)sceVideoOutSetBufferAttribute);
    AddSymbol("w3BY+tAEiQY", (void*)sceVideoOutRegisterBuffers);
    AddSymbol("CBiu4mCE1DA", (void*)sceVideoOutSetFlipRate);
    AddSymbol("HXzjK9yI30k", (void*)sceVideoOutAddFlipEvent);
    AddSymbol("SbU3dwp80lQ", (void*)sceVideoOutGetFlipStatus);
    AddSymbol("U46NwOiJpys", (void*)sceVideoOutSubmitFlip);
    AddSymbol("Xru92wHJRmg", (void*)sceVideoOutAddVblankEvent);
    AddSymbol("6kPnj51T62Y", (void*)sceVideoOutGetResolutionStatus);
    AddSymbol("j6RaAUlaLv0", (void*)sceVideoOutWaitVblank);
}
} // namespace Platform

// SPDX-License-Identifier: GPL-2.0-or-later
// The C interface of the graphics platform (gpu/vwgpu.h): the window thread, the Vulkan device
// and presenter, the table of system functions the loader resolves the game's imports against,
// and the window's text entry and choice list.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../vwgpu.h"
#include "platform.h"
#include "sdl_window.h"
#include "vw_overlay.h"
#include "vw_png.h"
#include "vw_settings.h"

namespace Platform {

std::string game_title, game_serial;

namespace {
struct Symbol {
    std::string nid;
    void* function;
};
std::vector<Symbol> symbols;

std::mutex window_mutex;
std::condition_variable window_cv;
bool window_ready;
} // namespace

void AddSymbol(const char* nid, void* function) {
    symbols.push_back({nid, function});
}

// ---- AvPlayer: the game links it but plays its movies through videodec (runtime_videodec.c) ----
namespace {
s32 PS4_ABI AvZero() {
    return OK;
}
bool PS4_ABI AvFalse() {
    return false;
}
void* PS4_ABI AvInit(void*) {
    static u64 player;
    return &player;
}
} // namespace

void RegisterAvPlayer() {
    AddSymbol("aS66RI0gGgo", (void*)AvInit);   // Init
    AddSymbol("UbQoYawOsfY", (void*)AvFalse);  // IsActive
    AddSymbol("JdksQu8pNdQ", (void*)AvFalse);  // GetVideoDataEx
    AddSymbol("Wnp1OVcrZgk", (void*)AvFalse);  // GetAudioData
    // PostInit, AddSource, SetLooping, Close, Start, Stop, Pause, Resume, CurrentTime,
    // StreamCount (no streams), GetStreamInfo, EnableStream.
    for (const char* nid : {"HD1YKVU26-M", "KMcEa+rHsIo", "OVths0xGfho", "NkJwDzKmIlw", "ET4Gr-Uu07s",
                            "ZC17w3vB5Lo", "9y5v+fGN4Wk", "w5moABNwnRY", "wwM99gjFf1Y", "hdTyRzCXQeQ",
                            "d8FcbzfAdQw", "ODJK2sn9w4A"}) {
        AddSymbol(nid, (void*)AvZero);
    }
}

} // namespace Platform

#ifdef VW_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (VW_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int vwgpu_init(const VwGpuConfig* config) {
    using namespace Platform;
    VwSettings::Load();
#ifdef VW_PGO_GENERATE
    StartProfileWriter();
#endif
    game_title = config->title ? config->title : "";
    game_serial = config->serial ? config->serial : "UNKNOWN";
    const std::string title = config->title ? config->title : "vwport";
    const s32 width = config->width, height = config->height;
    // SDL video is driven from one thread: the window lives on its own.
    std::thread([title, width, height] {
        auto* w = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{window_mutex};
            window = w;
            window_ready = true;
        }
        window_cv.notify_all();
        while (w->PollEvents()) {
            SDL_Delay(2);
        }
        std::printf("Window closed by user\n");
        std::fflush(stdout);
        std::_Exit(0);
    }).detach();
    {
        std::unique_lock lock{window_mutex};
        window_cv.wait(lock, [] { return window_ready; });
    }
    if (!CreateVulkan(*window)) return 1;
    PresenterInit();
    RegisterGnm();
    RegisterVideoOut();
    return 0;
}

extern "C" void vwgpu_register_kernel(void) {
    Platform::RegisterEqueue();
    Platform::RegisterAvPlayer();
}

extern "C" uintptr_t vwgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : Platform::symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return reinterpret_cast<uintptr_t>(symbol.function);
        }
    }
    return 0;
}

extern "C" int vwgpu_handle_fault(void*, void*) {
    return 0;
}

extern "C" void vwgpu_dump_guest_writes(void*) {}

extern "C" unsigned vwgpu_symbol_count(void) {
    return unsigned(Platform::symbols.size());
}

extern "C" int vwgpu_setting_text(const char* key, char* out, uint64_t size) {
    const std::string value = VwSettings::Text(key);
    if (value.empty() || !size) {
        return 0;
    }
    std::snprintf(out, size, "%s", value.c_str());
    return 1;
}

extern "C" int vwgpu_overlay_captures_input(void) {
    return VwOverlay::CapturesInput() ? 1 : 0;
}

extern "C" void vwgpu_mouse_map_active(void) {
    Frontend::MouseMapActive();
}

extern "C" int vwgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!Platform::window) return 0;
    Platform::window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int vwgpu_text_input_poll(char* out, uint64_t size) {
    if (!Platform::window) return 2;
    std::string text;
    const int state = Platform::window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}

extern "C" int vwgpu_choice_begin(const char* title, const char* const* items, const char* const* images,
                                  int count, int focus) {
    if (!Platform::window || count <= 0) return 0;
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<VwPng::Image> pictures(count);
    for (int i = 0; images && i < count; ++i) {
        if (images[i] && !VwPng::LoadRgba(images[i], pictures[i])) {
            std::printf("Save list: no picture from %s\n", images[i]);
        }
    }
    VwOverlay::BeginChoice(title ? title : "", std::vector<std::string>(items, items + count),
                           std::move(pictures), focus);
    return 1;
}

extern "C" int vwgpu_choice_poll(void) {
    return VwOverlay::PollChoice();
}

extern "C" int vwgpu_vulkan_handles(void** instance, void** physical, void** device, void** queue,
                                    uint32_t* family) {
    using Platform::vk;
    if (!vk.device) return 0;
    *instance = vk.instance;
    *physical = vk.physical;
    *device = vk.device;
    *queue = vk.queue;
    *family = vk.family;
    return 1;
}
extern "C" void vwgpu_queue_lock(void) {
    Platform::queue_mutex.lock();
}
extern "C" void vwgpu_queue_unlock(void) {
    Platform::queue_mutex.unlock();
}

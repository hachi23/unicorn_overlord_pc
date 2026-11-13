// SPDX-License-Identifier: GPL-2.0-or-later
// The game window. vwgpu_init creates it on the window thread, which pumps its events.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <cstdint>

struct SDL_Window;

namespace Frontend {

using s32 = int32_t;

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    bool pointer_hidden = false;   // window thread only
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int display_mode = -1; ///< applied VwSettings display mode (window thread)
    void ApplyDisplayMode(int mode);
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    SDL_Window* window{};
};

// The battle map cursor followed the mouse (any thread): PollEvents hides the system pointer
// over the window while that keeps happening.
void MouseMapActive();

} // namespace Frontend

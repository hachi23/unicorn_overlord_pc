// SPDX-License-Identifier: GPL-2.0-or-later
// vw: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or R3+L2 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in vw_settings.h.

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "vw_png.h"
#include <vulkan/vulkan.h>

union SDL_Event;
struct SDL_Window;

namespace VwOverlay {

using u32 = uint32_t;
using u64 = uint64_t;
using u8 = uint8_t;
using s32 = int32_t;

/// Present thread, once: the ImGui context and its Vulkan backend (on Platform::vk).
void Init(VkFormat format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread, once per presented frame (FPS readout and VW_FPS_LOG).
void NotePresent();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(VkCommandBuffer cmdbuf, VkImageView view, VkExtent2D extent);

/// The menu is open or a text entry is shown: the game's input is held neutral.
bool CapturesInput();

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

/// The game's save list: shown until the player chooses (arrows/D-pad, Enter/Cross) or
/// cancels (Esc/Circle). `pictures`: one per item (empty = none), drawn beside its text.
/// PollChoice: -1 while open, -2 cancelled, else the chosen index.
void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<VwPng::Image> pictures, int focus);
int PollChoice();

} // namespace VwOverlay

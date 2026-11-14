// SPDX-License-Identifier: GPL-2.0-or-later
// vw: user settings changed at run time from the in-game menu (vw_overlay.h) and kept
// in vw.ini (VW_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <string>

namespace VwSettings {

struct Values {
    std::atomic<bool> show_fps{false};
    /// The start program looks for a newer release (scripts/updates.py); it reads vw.ini itself.
    std::atomic<bool> check_updates{true};
    /// Performance overlay (vw_perf_stats.h): items shown, font size and background opacity
    /// in percent, corner (0 top left, 1 top right, 2 bottom left, 3 bottom right), layout
    /// (0 one line, 1 one item per line). menu_tab: the menu's last tab.
    std::atomic<unsigned> overlay_items{7};
    std::atomic<int> overlay_scale{100}, overlay_corner{1}, overlay_opacity{50}, overlay_layout{0};
    std::atomic<int> menu_tab{0};
    // Graphics (vw_graphics.h). Live: applied on the next frame.
    std::atomic<int> display_mode{0};
    std::atomic<bool> vsync{true};
    /// Frame interpolation (docs/dev/FRAME_INTERPOLATION.md): frames blended between the game's
    /// 60, shown at the display rate. VW_INTERP overrides.
    std::atomic<bool> frame_interpolation{false};
    std::atomic<bool> fsr1{false};
    /// Anime4K preset (VwGraphics::Anime4k), before FSR 1.
    std::atomic<int> anime4k{0};
    std::atomic<bool> rcas{true};
    std::atomic<int> rcas_strength{75};
    /// Set by the presenter: sharpening ran on the last frame (it needs FSR 1 when the game's
    /// image is smaller than the window).
    std::atomic<bool> rcas_applied{false};
    /// Set by the presenter: the game's image and the window it is shown in, in pixels.
    std::atomic<int> image_width{0}, image_height{0}, window_width{0}, window_height{0};
    /// Set by the presenter: Anime4K doubled the image on the last frame.
    std::atomic<bool> anime4k_upscaled{false};
    /// Controls: the sticks' dead zone in percent of their travel (src/runtime_pad.c). Worn or
    /// non-PlayStation sticks rest off centre, and the game reads that as a held direction.
    std::atomic<int> stick_dead_zone{15};
    // Startup: run.py turns them into patches and environment variables; "Apply and restart".
    std::atomic<int> render_resolution{0};
    std::atomic<int> aspect_ratio{0};
    /// Index into VwGraphics::TextLanguages; the probe reads text_language= itself.
    std::atomic<int> text_language{0};
    /// Set before an "Apply and restart"; cleared once the new launch shows the game. run.py
    /// restores the previous startup options when a launch never cleared it.
    std::atomic<bool> restart_unconfirmed{false};
    int startup_render_resolution = 0, startup_aspect_ratio = 0, startup_text_language = 0;
};

Values& Get();

constexpr int StickDeadZoneMax = 40;

/// Text settings: "key_map" and "pad_map" (button remapping, src/runtime_pad.c format).
std::string Text(const std::string& key);
void SetText(const std::string& key, const std::string& value);

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// A startup option (graphics, text language) differs from the one this launch started with.
bool GraphicsNeedRestart();
/// Writes the file (menu changes).
void Save();

} // namespace VwSettings

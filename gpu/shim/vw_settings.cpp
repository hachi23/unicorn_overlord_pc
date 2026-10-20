// SPDX-License-Identifier: GPL-2.0-or-later
#include "vw_settings.h"
#include "vw_graphics.h"
#include "../vwgpu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace VwSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("VW_CONFIG");
    return env && env[0] ? env : "vw.ini";
}

std::mutex text_mutex;
std::map<std::string, std::string> texts; // key_map, pad_map, button_icons
constexpr const char* TextKeys[] = {"key_map", "pad_map", "button_icons"};

void Set(Values& v, const std::string& key, const std::string& value) {
    for (const char* text_key : TextKeys) {
        if (key == text_key) {
            std::scoped_lock lock{text_mutex};
            texts[key] = value;
            return;
        }
    }
    const int i = std::atoi(value.c_str());
    static const char* const overlay_keys[] = {"overlay_fps", "overlay_frametime", "overlay_resolution",
                                               "overlay_gpu", "overlay_cpu", "overlay_ram", "overlay_vram"};
    for (unsigned item = 0; item < 7; ++item) {
        if (key == overlay_keys[item]) {
            const unsigned bit = 1u << item;
            v.overlay_items = i ? (v.overlay_items | bit) : (v.overlay_items & ~bit);
            return;
        }
    }
    if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(i, 50, 300);
        return;
    }
    if (key == "overlay_corner") {
        v.overlay_corner = i >= 0 && i <= 3 ? i : 1;
        return;
    }
    if (key == "overlay_opacity") {
        v.overlay_opacity = std::clamp(i, 0, 100);
        return;
    }
    if (key == "overlay_layout") {
        v.overlay_layout = i == 1 ? 1 : 0;
        return;
    }
    if (key == "menu_tab") {
        v.menu_tab = i >= 0 && i <= 3 ? i : 0;
        return;
    }
    if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "check_updates") {
        v.check_updates = i != 0;
    } else if (key == "display_mode") {
        v.display_mode = VwGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = i != 0;
    } else if (key == "async_shaders") {
        v.async_shaders = i != 0;
    } else if (key == "fsr1") {
        v.fsr1 = i != 0;
    } else if (key == "rcas") {
        v.rcas = i != 0;
    } else if (key == "rcas_strength") {
        v.rcas_strength = VwGraphics::ParseRcasStrength(value.empty() ? -1 : i);
    } else if (key == "frames_queued") {
        v.frames_queued = VwGraphics::ParseFramesQueued(value.empty() ? -1 : i);
    } else if (key == "render_resolution") {
        v.render_resolution = VwGraphics::ParseResolution(value);
    } else if (key == "deferred_readback") {
        v.deferred_readback = i != 0;
    } else if (key == "stale_readback") {
        v.stale_readback = i != 0;
    } else if (key == "text_language") {
        v.text_language = VwGraphics::ParseTextLanguage(value);
    } else if (key == "restart_unconfirmed") {
        v.restart_unconfirmed = i != 0;
    }
}

} // namespace

std::string Text(const std::string& key) {
    std::scoped_lock lock{text_mutex};
    const auto it = texts.find(key);
    return it == texts.end() ? std::string{} : it->second;
}

void SetText(const std::string& key, const std::string& value) {
    std::scoped_lock lock{text_mutex};
    texts[key] = value;
}

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[1280];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    const std::pair<const char*, const char*> env_keys[] = {
        {"VW_ASYNC_SHADERS", "async_shaders"}, {"VW_DEFERRED_READBACK", "deferred_readback"},
        {"VW_STALE_READBACK", "stale_readback"}, {"VW_FSR1", "fsr1"},
        {"VW_RCAS", "rcas"},                   {"VW_FRAMES_AHEAD", "frames_queued"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_render_resolution = v.render_resolution;
    v.startup_deferred_readback = v.deferred_readback;
    v.startup_stale_readback = v.stale_readback;
    v.startup_text_language = v.text_language;
}

bool GraphicsNeedRestart() {
    const auto& v = Get();
    return v.render_resolution != v.startup_render_resolution ||
           v.deferred_readback != v.startup_deferred_readback ||
           v.stale_readback != v.startup_stale_readback || v.text_language != v.startup_text_language;
}

// Written beside vw.ini, then renamed over it: a crash or a suspend while writing kept an empty
// or cut-off vw.ini, and every setting went back to its default.
void Save() {
    static std::mutex save_mutex; // the overlay and the game's Options screen both save
    std::scoped_lock save_lock{save_mutex};
    const auto& v = Get();
    const std::string temp = std::string(Path()) + ".tmp";
    FILE* file = std::fopen(temp.c_str(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", temp.c_str());
        return;
    }
    std::fprintf(file, "# vw settings (in-game menu: Insert / R3+L2)\nshow_fps=%d\ncheck_updates=%d\n",
                 int(v.show_fps.load()), int(v.check_updates.load()));
    std::fprintf(file,
                 "overlay_fps=%d\noverlay_frametime=%d\noverlay_resolution=%d\noverlay_gpu=%d\n"
                 "overlay_cpu=%d\noverlay_ram=%d\noverlay_vram=%d\noverlay_scale=%d\noverlay_corner=%d\n"
                 "overlay_opacity=%d\noverlay_layout=%d\nmenu_tab=%d\n",
                 int(v.overlay_items >> 0 & 1), int(v.overlay_items >> 1 & 1), int(v.overlay_items >> 2 & 1),
                 int(v.overlay_items >> 3 & 1), int(v.overlay_items >> 4 & 1), int(v.overlay_items >> 5 & 1),
                 int(v.overlay_items >> 6 & 1), v.overlay_scale.load(), v.overlay_corner.load(),
                 v.overlay_opacity.load(), v.overlay_layout.load(), v.menu_tab.load());
    std::fprintf(file,
                 "display_mode=%s\nvsync=%d\nasync_shaders=%d\n"
                 "fsr1=%d\nrcas=%d\nrcas_strength=%d\nframes_queued=%d\n"
                 "render_resolution=%s\ndeferred_readback=%d\nstale_readback=%d\n"
                 "text_language=%s\nrestart_unconfirmed=%d\n",
                 VwGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)].data(),
                 int(v.vsync.load()), int(v.async_shaders.load()),
                 int(v.fsr1.load()), int(v.rcas.load()), v.rcas_strength.load(), v.frames_queued.load(),
                 VwGraphics::Resolutions[std::clamp(v.render_resolution.load(), 0,
                                                      int(VwGraphics::Resolutions.size()) - 1)]
                     .data(),
                 int(v.deferred_readback.load()), int(v.stale_readback.load()),
                 VwGraphics::TextLanguages[std::clamp(v.text_language.load(), 0,
                                                        int(VwGraphics::TextLanguages.size()) - 1)]
                     .data(),
                 int(v.restart_unconfirmed.load()));
    {
        std::scoped_lock lock{text_mutex};
        for (const char* key : TextKeys) {
            if (const auto it = texts.find(key); it != texts.end() && !it->second.empty()) {
                std::fprintf(file, "%s=%s\n", key, it->second.c_str());
            }
        }
    }
    const bool written = !std::ferror(file);
    if (std::fclose(file) != 0 || !written) {
        std::printf("Settings: cannot write %s\n", temp.c_str());
        std::remove(temp.c_str());
        return;
    }
    std::error_code error;
    std::filesystem::rename(temp, Path(), error);
    if (error) {
        std::printf("Settings: cannot replace %s (%s)\n", Path(), error.message().c_str());
        std::remove(temp.c_str());
    }
}

} // namespace VwSettings

// The rows the game's Options screen shows (vwgpu.h, src/unicorn_native_settings.h).
namespace {

namespace G = VwGraphics;

enum Row {
    Heading, DisplayMode, Vsync, Resolution, Fsr1, Sharpening, SharpeningStrength, ButtonIcons,
    TextLanguage, Overlay, CheckUpdates, RowCount
};

constexpr const char* DisplayModes[] = {"Windowed", "Borderless", "Fullscreen"};
constexpr const char* ResolutionNames[] = {"1080p (native)", "720p", "900p", "1800p", "4K"};
constexpr const char* IconKeys[] = {"auto", "playstation", "xbox", "nintendo"};
constexpr const char* IconNames[] = {"Auto", "PlayStation", "Xbox", "Nintendo"};
// The game's fonts have Latin letters but not every script: each name in its own script would
// show as blanks in some languages.
constexpr const char* LanguageNames[] = {"Game Default", "Japanese", "Traditional Chinese",
                                         "Simplified Chinese", "Korean"};
static_assert(std::size(ResolutionNames) == G::Resolutions.size());
static_assert(std::size(LanguageNames) == G::TextLanguages.size());

const VwgpuSettingRow Rows[RowCount] = {
    {"PC", nullptr, VWGPU_ROW_SECTION, 0, nullptr, 0, 0, 0, 0, nullptr},
    {"Display Mode", "Plays in a window, a borderless window or exclusive fullscreen.", VWGPU_ROW_CHOICE,
     int(std::size(DisplayModes)), DisplayModes, float(G::Windowed), 0, 0, 0, nullptr},
    {"VSync", "Waits for the monitor: no tearing, slightly more input delay.", VWGPU_ROW_SWITCH, 0, nullptr, 1, 0,
     0, 0, nullptr},
    {"Render Resolution",
     "The size the game is drawn at: lower is faster, higher is sharper.\nTakes effect the next time the game "
     "starts.",
     VWGPU_ROW_CHOICE, int(std::size(ResolutionNames)), ResolutionNames, 0, 0, 0, 0, nullptr},
    {"FSR 1 Upscaling", "Upscales a render resolution smaller than the window with AMD FSR 1.", VWGPU_ROW_SWITCH,
     0, nullptr, 0, 0, 0, 0, nullptr},
    {"Sharpening", "Sharpens the final image.", VWGPU_ROW_SWITCH, 0, nullptr, 1, 0, 0, 0, nullptr},
    {"Sharpening Strength", "How strong the sharpening is.\n(Range: 0% to 100%)", VWGPU_ROW_SLIDER, 0, nullptr, 75,
     0, 100, 1, "%.0f%%"},
    {"Button Icons", "The button icons in prompts.\nAuto follows the controller in use.", VWGPU_ROW_CHOICE,
     int(std::size(IconNames)), IconNames, 0, 0, 0, 0, nullptr},
    {"Text Language",
     "The language of the game's text. Korean needs Unicorn_KO.CPK.\nTakes effect the next time the game starts.",
     VWGPU_ROW_CHOICE, int(std::size(LanguageNames)), LanguageNames, 0, 0, 0, 0, nullptr},
    {"Performance Overlay",
     "Shows the frame rate and other measurements in a corner.\nMore settings: Insert or R3+L2.", VWGPU_ROW_SWITCH, 0,
     nullptr, 0, 0, 0, 0, nullptr},
    {"Check for Updates", "At start, looks for a newer version of the PC port.\nIt asks before downloading anything.",
     VWGPU_ROW_SWITCH, 0, nullptr, 1, 0, 0, 0, nullptr},
};

int IconIndex() {
    const std::string key = VwSettings::Text("button_icons");
    for (int i = 1; i < int(std::size(IconKeys)); ++i) {
        if (key == IconKeys[i]) return i;
    }
    return 0;
}

} // namespace

extern "C" int vwgpu_setting_rows(const VwgpuSettingRow** rows) {
    if (rows) *rows = Rows;
    return RowCount;
}

extern "C" float vwgpu_setting_value(int row) {
    const auto& v = VwSettings::Get();
    switch (row) {
    case DisplayMode: return float(v.display_mode.load());
    case Vsync: return v.vsync ? 1.0f : 0.0f;
    case Resolution: return float(v.render_resolution.load());
    case Fsr1: return v.fsr1 ? 1.0f : 0.0f;
    case Sharpening: return v.rcas ? 1.0f : 0.0f;
    case SharpeningStrength: return float(v.rcas_strength.load());
    case ButtonIcons: return float(IconIndex());
    case TextLanguage: return float(v.text_language.load());
    case Overlay: return v.show_fps ? 1.0f : 0.0f;
    case CheckUpdates: return v.check_updates ? 1.0f : 0.0f;
    default: return 0.0f;
    }
}

extern "C" void vwgpu_setting_set(int row, float value) {
    if (row <= Heading || row >= RowCount || value != value) return;
    auto& v = VwSettings::Get();
    const auto& r = Rows[row];
    // The game moves sliders in float steps: round to the nearest whole value.
    const int i = int(value + (value < 0 ? -0.5f : 0.5f));
    const int choice = std::clamp(i, 0, std::max(r.count - 1, 0));
    switch (row) {
    case DisplayMode: v.display_mode = choice; break;
    case Vsync: v.vsync = i != 0; break;
    case Resolution: v.render_resolution = choice; break;
    case Fsr1: v.fsr1 = i != 0; break;
    case Sharpening: v.rcas = i != 0; break;
    case SharpeningStrength: v.rcas_strength = std::clamp(i, int(r.min), int(r.max)); break;
    case ButtonIcons: VwSettings::SetText("button_icons", choice ? IconKeys[choice] : ""); break;
    case TextLanguage: v.text_language = choice; break;
    case Overlay: v.show_fps = i != 0; break;
    case CheckUpdates: v.check_updates = i != 0; break;
    }
    VwSettings::Save();
}

// SPDX-License-Identifier: GPL-2.0-or-later
// vw: graphics options of the in-game menu (Insert / R3+L2), kept in vw.ini. The live ones
// apply on the next frame; the startup ones (render resolution, GPU readback modes) are read at
// launch and apply through "Apply and restart". The launcher reads and writes the same keys.
#pragma once

#include <array>
#include <string_view>

namespace VwGraphics {

enum DisplayMode : int { Windowed = 0, Borderless = 1, Fullscreen = 2, DisplayModeCount };
inline constexpr std::array<std::string_view, DisplayModeCount> DisplayModeKeys = {
    "windowed", "borderless", "fullscreen"};
inline constexpr std::array<const char*, DisplayModeCount> DisplayModeNames = {
    "Windowed", "Borderless", "Fullscreen"};

/// Render resolutions: the game's 1920x1080 frame drawn at another size (the probe's render
/// scale patch, src/unicorn_render_scale.h) and scaled to the window. Index 0 is the default.
/// 1440p was removed: the battle map's foreground blur broke into blocks at that size.
inline constexpr std::array<std::string_view, 5> Resolutions = {"native", "720p", "900p", "1800p",
                                                                "4K"};

/// Height of each entry of Resolutions.
inline constexpr std::array<int, 5> ResolutionHeights = {1080, 720, 900, 1800, 2160};

struct Size {
    int w, h;
};

/// First size of the window in Windowed mode: the game's image at the render resolution and
/// aspect ratio (at most 1080 tall), shrunk to fit the display's usable area so that no side
/// falls off the screen (issue #70: a 1920x1080 window on a 1280x1024 monitor).
inline Size WindowedSize(int resolution, int aspect_w, int aspect_h, int usable_w, int usable_h) {
    const int height = resolution >= 0 && resolution < int(ResolutionHeights.size())
                           ? ResolutionHeights[resolution] : 1080;
    Size s{0, height < 1080 ? height : 1080};
    s.w = s.h * aspect_w / aspect_h;
    if (usable_w > 0 && usable_h > 0 && (s.w > usable_w || s.h > usable_h)) {
        if (s.w * usable_h > usable_w * s.h) {  // width is the tighter side
            s.h = s.h * usable_w / s.w;
            s.w = usable_w;
        } else {
            s.w = s.w * usable_h / s.h;
            s.h = usable_h;
        }
    }
    return s;
}

/// Text languages (the probe's text language patch, src/probe.c). "auto" leaves the game's
/// choice: Traditional Chinese for the English system language the port reports, English with the
/// English patch. Korean needs Unicorn_KO.CPK, which the Asia version does not ship.
inline constexpr std::array<std::string_view, 5> TextLanguages = {"auto", "japanese", "traditional_chinese",
                                                                  "simplified_chinese", "korean"};

/// Aspect ratios of the game's image (the probe's ultrawide patch, src/unicorn_ultrawide.h):
/// wider than 16:9 shows more of the scene at the sides; menus stay 16:9 in the middle. 21:9
/// is 64:27 (2560x1080). Index 0 is the default.
inline constexpr std::array<std::string_view, 3> AspectRatios = {"16:9", "21:9", "32:9"};

template <typename T, std::size_t N>
constexpr int IndexOf(const std::array<T, N>& values, const T& value, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (values[i] == value) return int(i);
    }
    return fallback;
}

inline int ParseDisplayMode(std::string_view text) {
    return IndexOf(DisplayModeKeys, text, Windowed);
}
/// Index into Resolutions; unknown text is native.
inline int ParseResolution(std::string_view text) {
    return IndexOf(Resolutions, text, 0);
}
/// Index into TextLanguages; unknown text is auto.
inline int ParseTextLanguage(std::string_view text) {
    return IndexOf(TextLanguages, text, 0);
}

/// Index into AspectRatios; unknown text is 16:9.
inline int ParseAspectRatio(std::string_view text) {
    return IndexOf(AspectRatios, text, 0);
}

/// Frames the GPU command thread may queue ahead of the GPU; 0 = unbounded.
inline constexpr std::array<int, 3> FramesQueued = {1, 2, 0};
inline int ParseFramesQueued(int value) {
    return IndexOf(FramesQueued, value, -1) >= 0 ? value : 1;
}
/// RCAS strength in percent; invalid values fall back to 75.
inline int ParseRcasStrength(int value) {
    return value >= 0 && value <= 100 ? value : 75;
}
/// FSR RCAS attenuation for a strength: 0 is the sharpest, 2 the softest.
inline float RcasAttenuation(int strength) {
    return 2.0f * (1.0f - float(ParseRcasStrength(strength)) / 100.0f);
}
/// Whether the FSR pass runs: FSR 1 when the window is larger than the game's image, RCAS
/// alone when they are the same size. A smaller window is left to the regular scaling.
inline bool FsrPassRuns(bool fsr1, bool rcas, int in_w, int in_h, int out_w, int out_h) {
    const bool upscale = in_w < out_w && in_h < out_h;
    const bool same = in_w == out_w && in_h == out_h;
    return (fsr1 && upscale) || (rcas && same);
}

/// Anime4K (host_passes/anime4k_pass.cpp), a line-art upscaler: off, or its restore and x2
/// upscale models in the small (Fast) or medium (Quality) size.
enum Anime4k : int { Anime4kOff = 0, Anime4kFast = 1, Anime4kQuality = 2, Anime4kCount };
inline constexpr std::array<const char*, Anime4kCount> Anime4kNames = {"Off", "Fast", "Quality"};
inline int ParseAnime4k(int value) {
    return value >= 0 && value < Anime4kCount ? value : Anime4kOff;
}
/// Anime4K's x2 upscale runs when the window is over 1.2 times the image both ways (its mpv
/// condition); otherwise only the restore model runs and FSR 1 or the scaling does the rest.
inline bool Anime4kUpscales(int in_w, int in_h, int out_w, int out_h) {
    return out_w * 5 > in_w * 6 && out_h * 5 > in_h * 6;
}

} // namespace VwGraphics

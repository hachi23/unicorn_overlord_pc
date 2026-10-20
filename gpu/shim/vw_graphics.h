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

/// Text languages (the probe's text language patch, src/probe.c). "auto" leaves the game's
/// choice: Traditional Chinese for the English system language the port reports, English with the
/// English patch. Korean needs Unicorn_KO.CPK, which the Asia version does not ship.
inline constexpr std::array<std::string_view, 5> TextLanguages = {"auto", "japanese", "traditional_chinese",
                                                                  "simplified_chinese", "korean"};

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

} // namespace VwGraphics

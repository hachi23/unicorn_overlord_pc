// vw.ini graphics keys (vw_settings.cpp, vw_graphics.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "vw_graphics.h"
#include "vw_settings.h"
#include "../vwgpu.h"
#ifndef _WIN32
static int _putenv_s(const char* name, const char* value) {
    return value[0] ? setenv(name, value, 1) : unsetenv(name);
}
#endif

namespace G = VwGraphics;

static std::string Read(const char* path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

int main() {
    assert(G::ParseDisplayMode("borderless") == G::Borderless && G::ParseDisplayMode("bogus") == G::Windowed);
    assert(G::ParseResolution("720p") == 1 && G::ParseResolution("900p") == 2);
    assert(G::ParseResolution("1800p") == 3 && G::ParseResolution("4K") == 4 && G::ParseResolution("8K") == 0);
    assert(G::ParseResolution("1440p") == 0);  // removed: a vw.ini that still has it starts native
    assert(G::ParseTextLanguage("japanese") == 1 && G::ParseTextLanguage("korean") == 4 &&
           G::ParseTextLanguage("english") == 5 && G::ParseTextLanguage("italian") == 9 &&
           G::ParseTextLanguage("klingon") == 0);
    assert(G::ParseRcasStrength(100) == 100 && G::ParseRcasStrength(101) == 75 && G::ParseRcasStrength(-1) == 75);
    assert(G::RcasAttenuation(100) == 0.0f && G::RcasAttenuation(0) == 2.0f && G::RcasAttenuation(75) == 0.5f);
    assert(G::FsrPassRuns(true, false, 1280, 720, 2560, 1440) && !G::FsrPassRuns(false, false, 1280, 720, 2560, 1440));
    assert(G::FsrPassRuns(false, true, 1920, 1080, 1920, 1080) && !G::FsrPassRuns(true, false, 1920, 1080, 1920, 1080));
    assert(!G::FsrPassRuns(true, true, 2560, 1440, 1920, 1080));  // larger image: regular scaling
    // Windowed: the Window size, shrunk to the usable area (issue #70); "auto" fills it.
    {
        const G::Size a = G::WindowedSize(1, 16, 9, 2560, 1400), b = G::WindowedSize(5, 16, 9, 3840, 2100);
        const G::Size c = G::WindowedSize(3, 16, 9, 1280, 984), d = G::WindowedSize(3, 64, 27, 1920, 1040);
        const G::Size e = G::WindowedSize(0, 16, 9, 0, 0), f = G::WindowedSize(0, 16, 9, 1824, 936);
        const G::Size g = G::WindowedSize(0, 16, 9, 2432, 1305), h = G::WindowedSize(4, 16, 9, 1824, 936);
        assert(a.w == 1280 && a.h == 720 && b.w == 3733 && b.h == 2100);
        assert(c.w == 1280 && c.h == 720 && d.w == 1920 && d.h == 810 && e.w == 1920 && e.h == 1080);
        assert(f.w == 1664 && f.h == 936 && g.w == 2320 && g.h == 1305 && h.w == 1664 && h.h == 936);
        assert(G::ParseWindowSize("1440p") == 4 && G::ParseWindowSize("auto") == 0 && G::ParseWindowSize("8K") == 0);
    }
    assert(G::ParseFpsLimit(30) == 30 && G::ParseFpsLimit(40) == 40 && G::ParseFpsLimit(0) == 0);
    assert(G::ParseFpsLimit(240) == 0 && G::ParseFpsLimit(-5) == 0 && G::ParseFpsLimit(35) == 0);
    assert(G::ParseScreenFit("fit") == G::ScreenFitFit &&
           G::ParseScreenFit("stretch") == G::ScreenFitStretch && G::ParseScreenFit("bogus") == G::ScreenFitFit);
    {
        const G::Size fit = G::FrameSize(1920, 1200, 16.0f / 9.0f, G::ScreenFitFit);
        const G::Size stretch = G::FrameSize(1920, 1200, 16.0f / 9.0f, G::ScreenFitStretch);
        assert(fit.w == 1920 && fit.h == 1080);
        assert(stretch.w == 1920 && stretch.h == 1200);
    }
    {
        // PresentAgain can still hold a 1920x1200 Stretch frame when the mode changes to Fit.
        const float game_ratio = 16.0f / 9.0f;
        const auto stretch = G::PresentRect(1920, 1200, game_ratio, G::ScreenFitStretch);
        const auto fit = G::PresentRect(1920, 1200, game_ratio, G::ScreenFitFit);
        assert(stretch.x == 0 && stretch.y == 0 && stretch.width == 1920 && stretch.height == 1200);
        assert(fit.x == 0 && fit.y == 60 && fit.width == 1920 && fit.height == 1080);
        const auto wide = G::PresentRect(1600, 720, game_ratio, G::ScreenFitFit);
        assert(wide.x == 160 && wide.y == 0 && wide.width == 1280 && wide.height == 720);
    }
    {
        // Vulkan dimensions are physical pixels; SDL reports logical window points at high DPI.
        const auto fit = vw_image_coverage(1920, 1080, 1920, 1200);
        const auto stretch = vw_image_coverage(1920, 1200, 1920, 1200);
        const auto wide = vw_image_coverage(1920, 1080, 2560, 1080);
        const auto empty = vw_image_coverage(0, 0, 1920, 1200);
        const auto fit_points = vw_image_logical_size(fit, 1280, 800);
        const auto stretch_points = vw_image_logical_size(stretch, 1280, 800);
        const auto wide_points = vw_image_logical_size(wide, 2560, 1080);
        const auto resized = vw_image_logical_size(fit, 1600, 1000);
        assert(fit_points.width == 1280 && fit_points.height == 720);
        assert(stretch_points.width == 1280 && stretch_points.height == 800);
        assert(wide_points.width == 1920 && wide_points.height == 1080);
        assert(resized.width == 1600 && resized.height == 900);
        assert(empty.width == 0 && empty.height == 0);
    }
    assert(G::ParseAnime4k(2) == G::Anime4kQuality && G::ParseAnime4k(3) == G::Anime4kOff && G::ParseAnime4k(-1) == 0);
    // Anime4K's x2 runs above 1.2 times both ways: 1080p to 1440p, not 1080p to 1200p or 1080p.
    assert(G::Anime4kUpscales(1920, 1080, 2560, 1440) && G::Anime4kUpscales(1280, 720, 1920, 1080));
    assert(!G::Anime4kUpscales(1920, 1080, 1920, 1200) && !G::Anime4kUpscales(1920, 1080, 1920, 1080));
    assert(!G::Anime4kUpscales(1920, 1080, 2304, 1296));  // exactly 1.2: no

    const char* path = "graphics-settings-test.ini";
    {
        std::ofstream out(path);
        // renderer= picks the game executable; the shadPS4 renderer's own keys (async_shaders,
        // deferred_readback, ...) are kept unchanged for it.
        out << "display_mode=borderless\nscreen_fit=stretch\nvsync=0\nfps_limit=240\nasync_shaders=0\nrenderer=shadps4\n"
               "render_resolution=1800p\naspect_ratio=21:9\ndeferred_readback=0\nstale_readback=1\n"
               "fsr1=1\nanime4k=1\nrcas=0\nrcas_strength=40\nframes_queued=2\ntext_language=simplified_chinese\n";
    }
    _putenv_s("VW_CONFIG", path);
    _putenv_s("VW_VSYNC", "");
    _putenv_s("VW_SCREEN_FIT", "");
    auto& v = VwSettings::Get();
    VwSettings::Load();
    assert(v.display_mode == G::Borderless && v.screen_fit == G::ScreenFitStretch && !v.vsync);
    assert(v.render_resolution == 3 && v.aspect_ratio == 1);
    assert(v.fsr1 && !v.rcas && v.rcas_strength == 40);
    assert(v.anime4k == G::Anime4kFast);
    assert(v.fps_limit == 0);  // 240 is not one of the choices
    assert(v.text_language == 3 && v.renderer == 1 && !VwSettings::GraphicsNeedRestart());
    v.renderer = 0;  // run.py starts the other game executable: a restart
    assert(VwSettings::GraphicsNeedRestart());
    v.renderer = 1;
    v.text_language = 1;  // applied by the probe at start
    assert(VwSettings::GraphicsNeedRestart());
    v.text_language = 3;
    v.render_resolution = 1;  // 720p
    assert(VwSettings::GraphicsNeedRestart());
    v.vsync = false;  // live: no restart because of it
    v.render_resolution = 3;
    assert(!VwSettings::GraphicsNeedRestart());
    v.aspect_ratio = 2;  // 32:9
    assert(VwSettings::GraphicsNeedRestart());
    v.aspect_ratio = 1;

    VwSettings::Save();
    const std::string saved = Read(path);
    for (const char* line : {"display_mode=borderless\n", "screen_fit=stretch\n", "vsync=0\n", "render_resolution=1800p\n",
                             "aspect_ratio=21:9\n", "fsr1=1\n", "anime4k=1\n", "rcas=0\n", "rcas_strength=40\n",
                             "text_language=simplified_chinese\n", "renderer=shadps4\n", "async_shaders=0\n",
                             "deferred_readback=0\n", "stale_readback=1\n", "frames_queued=2\n"}) {
        assert(saved.find(line) != std::string::npos);
    }
    assert(saved.find("renderer=", saved.find("renderer=") + 1) == std::string::npos);  // once
    assert(saved.find("fps_limit=0\n") != std::string::npos);

    {
        std::ofstream out(path);
        out << "display_mode=huge\nfps_limit=7\nrender_resolution=8K\naspect_ratio=5:4\n"
               "rcas_strength=250\nanime4k=7\nvsync=0\n";
    }
    {
        std::ofstream out(path);
        out << "fps_limit=40\n";
    }
    VwSettings::Load();
    assert(v.fps_limit == 40);
    VwSettings::Save();
    assert(Read(path).find("fps_limit=40\n") != std::string::npos);
    {
        std::ofstream out(path);
        out << "display_mode=huge\nfps_limit=7\nrender_resolution=8K\naspect_ratio=5:4\n"
               "rcas_strength=250\nanime4k=7\nvsync=0\n";
    }
    _putenv_s("VW_VSYNC", "1");  // the environment still wins at start
    _putenv_s("VW_SCREEN_FIT", "stretch");
    VwSettings::Load();
    assert(v.display_mode == G::Windowed && v.render_resolution == 0 && v.aspect_ratio == 0);
    assert(v.screen_fit == G::ScreenFitStretch);
    assert(v.rcas_strength == 75 && v.anime4k == G::Anime4kOff);
    assert(v.fps_limit == 0);  // 7 is not one of the choices
    assert(v.vsync);

    // The rows of the game's Options screen (vwgpu.h).
    const VwgpuSettingRow* rows = nullptr;
    const int count = vwgpu_setting_rows(&rows);
    assert(count == 11 && rows[0].kind == VWGPU_ROW_SECTION && !std::strcmp(rows[0].label, "PC"));
    int display = -1, resolution = -1, aspect = -1, strength = -1, icons = -1, overlay = -1;
    for (int i = 0; i < count; ++i) {
        const std::string label = rows[i].label;
        display = label == "Display Mode" ? i : display;
        resolution = label == "Render Resolution" ? i : resolution;
        aspect = label == "Aspect Ratio" ? i : aspect;
        assert(label != "Frame Rate Limit");
        strength = label == "Sharpening Strength" ? i : strength;
        icons = label == "Button Icons" ? i : icons;
        assert(label != "Text Language");  // the Insert menu only: the screen has 11 rows
        overlay = label == "Performance Overlay" ? i : overlay;
        if (rows[i].kind == VWGPU_ROW_CHOICE) assert(rows[i].count > 1 && rows[i].choices[rows[i].count - 1]);
    }
    assert(display > 0 && resolution > 0 && aspect > 0 && strength > 0 && icons > 0 && overlay > 0);
    assert(!std::strcmp(rows[resolution].choices[int(rows[resolution].fallback)], "1080p (native)"));
    vwgpu_setting_set(display, 2.0f);
    vwgpu_setting_set(resolution, 1.0f);
    vwgpu_setting_set(aspect, 1.0f);
    vwgpu_setting_set(strength, 39.6f);  // a slider's float steps round to whole values
    vwgpu_setting_set(icons, 2.0f);
    vwgpu_setting_set(overlay, 1.0f);
    vwgpu_setting_set(display, 7.0f);  // past the last choice: the last one
    assert(v.display_mode == G::Fullscreen && v.render_resolution == 1 && v.rcas_strength == 40 && v.show_fps);
    assert(VwSettings::Text("button_icons") == "xbox" && vwgpu_setting_value(icons) == 2.0f);
    assert(v.aspect_ratio == 1 && vwgpu_setting_value(aspect) == 1.0f);
    assert(vwgpu_setting_value(resolution) == 1.0f && vwgpu_setting_value(strength) == 40.0f);
    const std::string written = Read(path);
    for (const char* line : {"display_mode=fullscreen\n", "render_resolution=720p\n", "aspect_ratio=21:9\n", "rcas_strength=40\n",
                             "button_icons=xbox\n", "show_fps=1\n"}) {
        assert(written.find(line) != std::string::npos);
    }
    assert(!std::strcmp(rows[count - 1].label, "Performance Overlay"));  // updates: the Insert menu only
    vwgpu_setting_set(icons, 0.0f);  // Auto: no key in the file
    assert(VwSettings::Text("button_icons").empty() && vwgpu_setting_value(icons) == 0.0f);
    std::remove(path);
    std::puts("PASS: graphics settings");
}

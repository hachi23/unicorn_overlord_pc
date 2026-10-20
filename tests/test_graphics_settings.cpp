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
           G::ParseTextLanguage("english") == 0);
    assert(G::ParseFramesQueued(2) == 2 && G::ParseFramesQueued(0) == 0 && G::ParseFramesQueued(5) == 1);
    assert(G::ParseRcasStrength(100) == 100 && G::ParseRcasStrength(101) == 75 && G::ParseRcasStrength(-1) == 75);
    assert(G::RcasAttenuation(100) == 0.0f && G::RcasAttenuation(0) == 2.0f && G::RcasAttenuation(75) == 0.5f);
    assert(G::FsrPassRuns(true, false, 1280, 720, 2560, 1440) && !G::FsrPassRuns(false, false, 1280, 720, 2560, 1440));
    assert(G::FsrPassRuns(false, true, 1920, 1080, 1920, 1080) && !G::FsrPassRuns(true, false, 1920, 1080, 1920, 1080));
    assert(!G::FsrPassRuns(true, true, 2560, 1440, 1920, 1080));  // larger image: regular scaling

    const char* path = "graphics-settings-test.ini";
    {
        std::ofstream out(path);
        out << "display_mode=borderless\nvsync=0\nfps_limit=240\nasync_shaders=0\n"
               "render_resolution=1800p\ndeferred_readback=0\nstale_readback=1\n"
               "fsr1=1\nrcas=0\nrcas_strength=40\nframes_queued=2\ntext_language=simplified_chinese\n";
    }
    _putenv_s("VW_CONFIG", path);
    _putenv_s("VW_ASYNC_SHADERS", "");
    auto& v = VwSettings::Get();
    VwSettings::Load();
    assert(v.display_mode == G::Borderless && !v.vsync && !v.async_shaders);
    assert(v.render_resolution == 3 && !v.deferred_readback && v.stale_readback);
    assert(v.fsr1 && !v.rcas && v.rcas_strength == 40 && v.frames_queued == 2);
    assert(v.text_language == 3 && !VwSettings::GraphicsNeedRestart());
    v.text_language = 1;  // applied by the probe at start
    assert(VwSettings::GraphicsNeedRestart());
    v.text_language = 3;
    v.render_resolution = 1;  // 720p
    assert(VwSettings::GraphicsNeedRestart());
    v.vsync = false;  // live: no restart because of it
    v.render_resolution = 3;
    assert(!VwSettings::GraphicsNeedRestart());

    VwSettings::Save();
    const std::string saved = Read(path);
    for (const char* line : {"display_mode=borderless\n", "vsync=0\n", "async_shaders=0\n",
                             "render_resolution=1800p\n", "deferred_readback=0\n",
                             "stale_readback=1\n", "fsr1=1\n", "rcas=0\n", "rcas_strength=40\n",
                             "frames_queued=2\n", "text_language=simplified_chinese\n"}) {
        assert(saved.find(line) != std::string::npos);
    }
    assert(saved.find("fps_limit") == std::string::npos);  // removed: the game runs at its own 60

    {
        std::ofstream out(path);
        out << "display_mode=huge\nfps_limit=7\nrender_resolution=8K\n"
               "rcas_strength=250\nframes_queued=9\n";
    }
    _putenv_s("VW_ASYNC_SHADERS", "1");  // the environment still wins at start
    v.async_shaders = false;
    VwSettings::Load();
    assert(v.display_mode == G::Windowed && v.render_resolution == 0);
    assert(v.rcas_strength == 75 && v.frames_queued == 1);
    assert(v.async_shaders);

    // The rows of the game's Options screen (vwgpu.h).
    const VwgpuSettingRow* rows = nullptr;
    const int count = vwgpu_setting_rows(&rows);
    assert(count == 11 && rows[0].kind == VWGPU_ROW_SECTION && !std::strcmp(rows[0].label, "PC"));
    int display = -1, resolution = -1, strength = -1, icons = -1, language = -1, overlay = -1;
    for (int i = 0; i < count; ++i) {
        const std::string label = rows[i].label;
        display = label == "Display Mode" ? i : display;
        resolution = label == "Render Resolution" ? i : resolution;
        assert(label != "Frame Rate Limit");
        strength = label == "Sharpening Strength" ? i : strength;
        icons = label == "Button Icons" ? i : icons;
        language = label == "Text Language" ? i : language;
        overlay = label == "Performance Overlay" ? i : overlay;
        if (rows[i].kind == VWGPU_ROW_CHOICE) assert(rows[i].count > 1 && rows[i].choices[rows[i].count - 1]);
    }
    assert(display > 0 && resolution > 0 && strength > 0 && icons > 0 && language > 0 && overlay > 0);
    assert(!std::strcmp(rows[resolution].choices[int(rows[resolution].fallback)], "1080p (native)"));
    vwgpu_setting_set(display, 2.0f);
    vwgpu_setting_set(resolution, 1.0f);
    vwgpu_setting_set(strength, 39.6f);  // a slider's float steps round to whole values
    vwgpu_setting_set(icons, 2.0f);
    vwgpu_setting_set(overlay, 1.0f);
    vwgpu_setting_set(language, 1.0f);
    vwgpu_setting_set(display, 7.0f);  // past the last choice: the last one
    assert(v.display_mode == G::Fullscreen && v.render_resolution == 1 && v.rcas_strength == 40 && v.show_fps);
    assert(VwSettings::Text("button_icons") == "xbox" && vwgpu_setting_value(icons) == 2.0f);
    assert(vwgpu_setting_value(resolution) == 1.0f && vwgpu_setting_value(strength) == 40.0f);
    const std::string written = Read(path);
    for (const char* line : {"display_mode=fullscreen\n", "render_resolution=720p\n", "rcas_strength=40\n",
                             "button_icons=xbox\n", "show_fps=1\n", "text_language=japanese\n"}) {
        assert(written.find(line) != std::string::npos);
    }
    const int updates = count - 1;
    assert(!std::strcmp(rows[updates].label, "Check for Updates") && vwgpu_setting_value(updates) == 1.0f);
    vwgpu_setting_set(updates, 0.0f);
    assert(!v.check_updates && Read(path).find("check_updates=0\n") != std::string::npos);
    vwgpu_setting_set(updates, 1.0f);
    vwgpu_setting_set(icons, 0.0f);  // Auto: no key in the file
    assert(VwSettings::Text("button_icons").empty() && vwgpu_setting_value(icons) == 0.0f);
    std::remove(path);
    std::puts("PASS: graphics settings");
}

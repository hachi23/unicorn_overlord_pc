# vw GPU library: the port's graphics platform (platform/): window, Vulkan device, presenter
# and overlay, and the PS4 GnmDriver, VideoOut and event queue libraries. The game's drawing is
# done by the native renderer in ../src/native_*.c.
cmake_minimum_required(VERSION 3.24)
project(vwgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)
if(NOT WIN32)
    # libvwgpu is linked by vwport, never dlopen'ed: its thread_locals can live in the static TLS
    # block instead of going through __tls_get_addr.
    add_compile_options(-ftls-model=initial-exec)
endif()

# vw: link-time optimization (VW_LTO, default on) and profile-guided optimization
# (VW_PGO: off, generate = instrumented build writing into VW_PGO_DIR, use = build with that
# profile). No -march: the same build runs on the Steam Deck.
option(VW_LTO "Link-time optimization of vwgpu" ON)
set(VW_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(VW_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (VW_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT vw_ipo_ok OUTPUT vw_ipo_error)
    if (vw_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${vw_ipo_error}")
    endif()
endif()
if (VW_PGO STREQUAL "generate")
    # Atomic counters: the game, renderer, command and flip threads run the same code.
    add_compile_options(-fprofile-generate=${VW_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${VW_PGO_DIR})
    add_compile_definitions(VW_PGO_GENERATE)
elseif (VW_PGO STREQUAL "use")
    add_compile_options(-fprofile-use=${VW_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

# Release builds: static libraries from packaging/deps.sh (FFmpeg, SDL3) are found first, and
# their own dependencies are linked from pkg-config --static.
set(VW_DEPS "" CACHE PATH "Prefix of the static dependencies (packaging/deps.sh)")
if (VW_DEPS)
    list(PREPEND CMAKE_PREFIX_PATH ${VW_DEPS})
    link_directories(${VW_DEPS}/lib)
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

# Platform shaders -> headers with a uint32_t array named after the shader (upper case).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/platform/shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(fullscreen.vert fullscreen_vert)
add_host_shader(present.frag present_frag)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
include(${SHADER_SRC}/anime4k/shaders.cmake) # tools/anime4k.py
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

# Windows: a DLL cannot leave the runtime_* symbols for vwport.exe to resolve, so the
# library is static there and linked into the executable. VW_STATIC does the same on Linux
# (release packages: one executable); development builds keep libvwgpu.so (faster links).
option(VW_STATIC "Link the GPU library into vwport" OFF)
if (WIN32 OR VW_STATIC)
    set(VWGPU_KIND STATIC)
else()
    set(VWGPU_KIND SHARED)
endif()
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS platform/*.cpp)
add_library(vwgpu ${VWGPU_KIND} ${PLATFORM_SOURCES})
add_dependencies(vwgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(vwgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(vwgpu PRIVATE platform third_party/imgui third_party/imgui/backends ${SHADER_INCLUDE})
target_compile_definitions(vwgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    VW_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(platform/vw_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_compile_definitions(vwgpu PRIVATE NDEBUG)
target_compile_options(vwgpu PRIVATE -Wno-missing-field-initializers)
# Runtime symbols (runtime_process_time_us, runtime_restart, the pad and the native renderer's
# native_present_image) resolve against vwport.
if (NOT WIN32)
    target_link_options(vwgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(vwgpu PUBLIC Vulkan::Headers PkgConfig::SDL3 PkgConfig::FFMPEG)
if (VW_DEPS)
    target_link_libraries(vwgpu PUBLIC ${FFMPEG_STATIC_LDFLAGS} ${SDL3_STATIC_LDFLAGS})
    if (NOT WIN32 AND NOT VW_STATIC)
        # FFmpeg's PIC assembly uses PC-relative references to its own tables.
        # Keep its archive symbols local when embedding it in a shared library.
        # SDL symbols stay public so the loader and GPU share the same SDL state.
        target_link_options(vwgpu PRIVATE
            "-Wl,--exclude-libs,libavcodec.a:libavformat.a:libavutil.a:libswscale.a:libswresample.a")
    endif()
endif()
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(vwgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(vwgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/platform/vw_win_compat.h)
    target_link_libraries(vwgpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm pdh)
else()
    target_link_libraries(vwgpu PUBLIC Vulkan::Vulkan)
endif()

# vw: with a static vwgpu (Windows, VW_STATIC) the loader and the HLE runtime are linked here,
# where CMake knows its dependencies (build.sh builds them with ninja).
if (WIN32 OR VW_STATIC)
    file(GLOB VW_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c ../src/native*.c)
    file(GLOB VW_ATRAC9 CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
    add_library(atrac9 STATIC ${VW_ATRAC9})
    target_compile_options(atrac9 PRIVATE -w)
    set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
    add_executable(vwport ../src/probe.c ${VW_RUNTIME} ../src/vulkan_smoke.c ../src/gpu_device.c)
    set_target_properties(vwport PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_include_directories(vwport PRIVATE .. ../src)
endif()
if (VW_STATIC AND NOT WIN32)
    target_compile_options(vwport PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -pthread>)
    # Guest pointers must stay below 1 TiB: no PIE (as build.sh). The C++ runtime is linked in
    # so the package runs on other distributions' libstdc++.
    target_link_options(vwport PRIVATE -no-pie -static-libstdc++ -static-libgcc -Wl,--as-needed)
    target_link_libraries(vwport PRIVATE vwgpu atrac9 Vulkan::Vulkan m -pthread)
endif()
if (WIN32)
    enable_language(RC)
    target_sources(vwport PRIVATE ../src/compat_win.c ../packaging/windows/vwport.rc)
    # clang 23 added -Wunused-but-set-global (must follow -Wall to stay off).
    target_compile_options(vwport PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror
        -Wno-unknown-warning-option -Wno-unused-but-set-global>)
    target_link_libraries(vwport PRIVATE vwgpu atrac9 onecore bcrypt ws2_32 -pthread)
    if (VW_DEPS)
        # libc++, libunwind and winpthread linked in: vwport.exe then needs only system DLLs.
        target_link_options(vwport PRIVATE -static)
    endif()
    # The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR for heaps and stacks.
    # The guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    target_link_options(vwport PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    # vw: runtime unit tests (ninja -C out/gpu mp3-test videodec-test services-test).
    add_executable(mp3-test EXCLUDE_FROM_ALL ../tests/test_mp3.c ../src/runtime_mp3.c ../src/compat_win.c)
    add_executable(videodec-test EXCLUDE_FROM_ALL ../tests/test_videodec.c ../src/compat_win.c)
    add_executable(services-test EXCLUDE_FROM_ALL ../tests/test_services.c ../src/compat_win.c)
    foreach(test mp3-test videodec-test services-test)
        set_target_properties(${test} PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
        target_compile_options(${test} PRIVATE -Wall -Wextra -Werror)
        target_include_directories(${test} PRIVATE .. ../src)
        target_link_libraries(${test} PRIVATE atrac9 PkgConfig::FFMPEG bcrypt -pthread)
    endforeach()
    add_executable(pad-map-test EXCLUDE_FROM_ALL ../tests/test_pad_map.c ../src/compat_win.c)
    set_target_properties(pad-map-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(pad-map-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_include_directories(pad-map-test PRIVATE .. ../src)
    target_link_libraries(pad-map-test PRIVATE PkgConfig::SDL3 bcrypt -pthread)
endif()

# Explicit test targets; none launches the game or needs a Vulkan device.
add_executable(png-decode-test EXCLUDE_FROM_ALL ../tests/test_png_decode.cpp platform/vw_png.cpp)
target_compile_options(png-decode-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(png-decode-test PRIVATE PkgConfig::FFMPEG)
add_executable(perf-overlay-test EXCLUDE_FROM_ALL ../tests/test_perf_overlay.cpp platform/vw_settings.cpp)
target_include_directories(perf-overlay-test PRIVATE platform)
target_compile_options(perf-overlay-test PRIVATE -UNDEBUG)
# The Gnm command thread and the event queues, through their NIDs (no window or GPU needed).
add_executable(gpu-platform-test EXCLUDE_FROM_ALL ../tests/test_gpu_platform.cpp platform/gnm.cpp platform/equeue.cpp)
target_include_directories(gpu-platform-test PRIVATE platform)
target_link_libraries(gpu-platform-test PRIVATE Vulkan::Headers Threads::Threads)
target_compile_options(gpu-platform-test PRIVATE -UNDEBUG)
add_executable(graphics-settings-test EXCLUDE_FROM_ALL ../tests/test_graphics_settings.cpp
    platform/vw_settings.cpp)
target_include_directories(graphics-settings-test PRIVATE platform)
target_compile_options(graphics-settings-test PRIVATE -UNDEBUG)

# vw GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
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
    # block instead of going through __tls_get_addr (2-3% of the GPU command and recording threads).
    add_compile_options(-ftls-model=initial-exec)
endif()
# MinGW + newer libc++ (clang 21+): vendored code (sirit, ...) relied on
# transitive includes for std::abort, _countof, swprintf_s, log2 and placement new.
if (MINGW)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdlib>"
                        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cmath>"
                        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include new>"
                        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include wchar.h>")
endif()

# vw: link-time optimization (VW_LTO, default on) and profile-guided optimization
# (VW_PGO: off, generate = instrumented build writing into VW_PGO_DIR, use = build with that
# profile). Covers vwgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
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
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${VW_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${VW_PGO_DIR})
    add_compile_definitions(VW_PGO_GENERATE)
elseif (VW_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, vwgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${VW_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

# Release builds: static libraries from packaging/deps.sh (FFmpeg, SDL3, fmt, Zydis, xxHash, zlib)
# are found first, and their own dependencies are linked from pkg-config --static.
set(VW_DEPS "" CACHE PATH "Prefix of the static dependencies (packaging/deps.sh)")
if (VW_DEPS)
    list(PREPEND CMAKE_PREFIX_PATH ${VW_DEPS})
    # Zydis' package names its Zycore dependency without a path.
    link_directories(${VW_DEPS}/lib)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# vw (Windows): MSYS2 has no magic_enum/miniz packages; the submodules are used instead.
find_package(magic_enum QUIET)
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE third_party/magic_enum/include
                                                    third_party/magic_enum/include/magic_enum)
endif()
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # MSYS2 ships the header (include/vma/vk_mem_alloc.h) without a CMake package.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
# SMAA (third_party/smaa, MIT) through host_shaders/smaa/smaa.glsl.
foreach(PASS IN ITEMS edge weight blend)
    add_host_shader(smaa_${PASS}.frag smaa_${PASS}_frag)
endforeach()
include(${SHADER_SRC}/anime4k/shaders.cmake) # tools/anime4k.py
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# Windows: a DLL cannot leave the runtime_* symbols for vwport.exe to resolve, so the
# library is static there and linked into the executable. VW_STATIC does the same on Linux
# (release packages: one executable); development builds keep libvwgpu.so (faster links).
option(VW_STATIC "Link the GPU library into vwport" OFF)
if (WIN32 OR VW_STATIC)
    set(VWGPU_KIND STATIC)
else()
    set(VWGPU_KIND SHARED)
endif()
add_library(vwgpu ${VWGPU_KIND} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(vwgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(vwgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(vwgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(vwgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    VW_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/vw_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(vwgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(vwgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(vwgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against vwport.
if (NOT WIN32)
    target_link_options(vwgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(vwgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
if (VW_DEPS)
    target_link_libraries(vwgpu PUBLIC ${FFMPEG_STATIC_LDFLAGS} ${SDL3_STATIC_LDFLAGS} ${XXHASH_STATIC_LDFLAGS})
    if (NOT WIN32 AND NOT VW_STATIC)
        # FFmpeg's PIC assembly uses PC-relative references to its own tables.
        # Keep its archive symbols local when embedding it in a shared library.
        # SDL symbols stay public so the loader and GPU share the same SDL state.
        target_link_options(vwgpu PRIVATE
            "-Wl,--exclude-libs,libavcodec.a:libavformat.a:libavutil.a:libswscale.a:libswresample.a")
    endif()
endif()
target_include_directories(vwgpu PRIVATE third_party/xbyak)
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(vwgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(vwgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/vw_win_compat.h)
    target_link_libraries(vwgpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm pdh)
else()
    target_link_libraries(vwgpu PUBLIC PkgConfig::X11)
endif()

# vw: with a static vwgpu (Windows, VW_STATIC) the loader and the HLE runtime are linked here,
# where CMake knows its dependencies (build.sh builds them with ninja).
if (WIN32 OR VW_STATIC)
    file(GLOB VW_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c)
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

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(pipeline-binary-cache-test EXCLUDE_FROM_ALL ../tests/test_pipeline_binary_cache.cpp
    ../tests/test_gpu_runtime_stubs.cpp)
target_compile_definitions(pipeline-binary-cache-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pipeline-binary-cache-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(pipeline-binary-cache-test PRIVATE vwgpu)
target_compile_options(pipeline-binary-cache-test PRIVATE -fPIC)
if (WIN32)
    target_sources(pipeline-binary-cache-test PRIVATE ../src/compat_win.c)
    target_compile_definitions(pipeline-binary-cache-test PRIVATE sigjmp_buf=vw_jmp_buf)
    target_compile_options(pipeline-binary-cache-test PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/../src/compat_win.h)
    target_link_libraries(pipeline-binary-cache-test PRIVATE onecore)
endif()
set_source_files_properties(../tests/test_pipeline_binary_cache.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
add_executable(png-decode-test EXCLUDE_FROM_ALL ../tests/test_png_decode.cpp shim/vw_png.cpp)
target_compile_options(png-decode-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(png-decode-test PRIVATE PkgConfig::FFMPEG)
add_executable(aliasing-test EXCLUDE_FROM_ALL ../tests/test_aliasing.cpp
    shadps4/video_core/texture_cache/aliasing.cpp shadps4/video_core/texture_cache/host_compatibility.cpp)
target_compile_options(aliasing-test PRIVATE -UNDEBUG)
target_include_directories(aliasing-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(aliasing-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(blend-rewrite-test EXCLUDE_FROM_ALL ../tests/test_blend_rewrite.cpp)
target_compile_options(blend-rewrite-test PRIVATE -UNDEBUG)
target_include_directories(blend-rewrite-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(blend-rewrite-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
add_executable(depth-stencil-state-test EXCLUDE_FROM_ALL ../tests/test_depth_stencil_state.cpp)
add_executable(async-compiler-test EXCLUDE_FROM_ALL ../tests/test_async_compiler.cpp)
target_compile_options(async-compiler-test PRIVATE -UNDEBUG)
target_include_directories(async-compiler-test PRIVATE shadps4)
add_executable(warmup-inbox-test EXCLUDE_FROM_ALL ../tests/test_warmup_inbox.cpp)
target_compile_options(warmup-inbox-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(warmup-inbox-test PRIVATE shadps4)
add_executable(async-pipeline-cache-test EXCLUDE_FROM_ALL ../tests/test_async_pipeline_cache.cpp)
target_compile_options(async-pipeline-cache-test PRIVATE -UNDEBUG)
target_include_directories(async-pipeline-cache-test PRIVATE shadps4)
target_link_libraries(async-pipeline-cache-test PRIVATE Vulkan::Vulkan)
add_executable(async-fetch-snapshot-test EXCLUDE_FROM_ALL ../tests/test_async_fetch_snapshot.cpp
    shadps4/shader_recompiler/frontend/fetch_shader.cpp
    shadps4/shader_recompiler/frontend/decode.cpp
    shadps4/shader_recompiler/frontend/format.cpp)
target_compile_options(async-fetch-snapshot-test PRIVATE -UNDEBUG)
target_include_directories(async-fetch-snapshot-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(async-fetch-snapshot-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)
target_compile_options(depth-stencil-state-test PRIVATE -UNDEBUG)
target_include_directories(depth-stencil-state-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(depth-stencil-state-test PRIVATE Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum)

add_executable(perf-overlay-test EXCLUDE_FROM_ALL ../tests/test_perf_overlay.cpp
    shim/vw_settings.cpp)
target_include_directories(perf-overlay-test PRIVATE shim)
target_compile_options(perf-overlay-test PRIVATE -UNDEBUG)
add_executable(readback-queue-test EXCLUDE_FROM_ALL ../tests/test_readback_queue.cpp)
target_include_directories(readback-queue-test PRIVATE shadps4)
target_compile_options(readback-queue-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(wait-diagnostics-test EXCLUDE_FROM_ALL ../tests/test_wait_diagnostics.cpp)
target_include_directories(wait-diagnostics-test PRIVATE shadps4)
target_compile_options(wait-diagnostics-test PRIVATE -UNDEBUG)
add_executable(graphics-settings-test EXCLUDE_FROM_ALL ../tests/test_graphics_settings.cpp
    shim/vw_settings.cpp)
target_include_directories(graphics-settings-test PRIVATE shim)
target_compile_options(graphics-settings-test PRIVATE -UNDEBUG)
add_executable(readback-sources-test EXCLUDE_FROM_ALL ../tests/test_readback_sources.cpp)
target_include_directories(readback-sources-test PRIVATE shadps4)
target_compile_options(readback-sources-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_executable(page-state-test EXCLUDE_FROM_ALL ../tests/test_page_state.cpp)
target_include_directories(page-state-test PRIVATE ../tests/stubs shim shadps4)
target_link_libraries(page-state-test PRIVATE Boost::headers)
target_compile_options(page-state-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)



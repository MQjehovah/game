cmake_minimum_required(VERSION 3.15)
project(NeonEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optional backends. QuickJS is a vendored C99 codebase that does not compile
# with the MSVC compiler, so the JS backend follows the same opt-in pattern as
# Jolt: on MSVC it defaults to OFF (scene scripts fall back to Lua;
# CreateJsHost() returns nullptr). GCC/Clang toolchains keep it ON.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  option(NEON_ENABLE_JS
         "Build the QuickJS (ES2020) JavaScript script backend (vendored QuickJS does not compile with MSVC)" OFF)
else()
  option(NEON_ENABLE_JS "Build the QuickJS (ES2020) JavaScript script backend" ON)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Engine core (platform-independent)
# ---------------------------------------------------------------------------
set(ENGINE_CORE_SOURCES
  engine/src/assets/asset_db.cpp
  engine/src/assets/asset_importer.cpp
  engine/src/assets/asset_manager.cpp
  engine/src/assets/async_loader.cpp
  engine/src/assets/bc1.cpp
  engine/src/anim/anim.cpp
  engine/src/audio/audio.cpp
  engine/src/audio/miniaudio/ma_audio.cpp
  engine/src/core/app.cpp
  engine/src/core/config.cpp
  engine/src/core/crash.cpp
  engine/src/core/log.cpp
  engine/src/core/mem_stats.cpp
  engine/src/core/profiler.cpp
  engine/src/ecs/world.cpp
  engine/src/ecs/parallel.cpp
  engine/src/ecs/task_graph.cpp
  engine/src/ecs/system_scheduler.cpp
  engine/src/io/vfs.cpp
  engine/src/core/json.cpp
  engine/src/core/localization.cpp
  engine/src/core/serialize.cpp
  engine/src/core/pack.cpp
  engine/src/net/protocol.cpp
  engine/src/net/socket.cpp
  engine/src/net/reliable.cpp
  engine/src/nav/nav_grid.cpp
  engine/src/plugin/plugin.cpp
  engine/src/plugin/runtime_plugin.cpp
  engine/src/plugin/native.cpp
  engine/src/plugin/backend.cpp
engine/src/gfx/color.cpp
  engine/src/gfx/csm.cpp
  engine/src/gfx/font.cpp
  engine/src/gfx/ibl.cpp
  engine/src/gfx/mesh.cpp
  engine/src/gfx/scene_props.cpp
  engine/src/gfx/terrain.cpp
  engine/src/gfx/light_probe.cpp
  engine/src/gfx/particles.cpp
  engine/src/gfx/renderer.cpp
  engine/src/gfx/gl/gl_loader.cpp
  engine/src/gfx/gl/gl_backend.cpp
  engine/src/physics/physics.cpp
  engine/src/platform/input.cpp
  engine/src/scene/scene_file.cpp
  engine/src/scene/component_schema.cpp
  engine/src/scene/scene_manager.cpp
  engine/src/scene/game_manifest.cpp
  engine/src/scene/game_runtime.cpp
  engine/src/scene/game_runtime_combat.cpp
  engine/src/scene/game_runtime_content.cpp
  engine/src/scene/skinned_model.cpp
  engine/src/scene/skills.cpp
  engine/src/scene/status.cpp
  engine/src/scene/world_chunk.cpp
  engine/src/script/lua_host.cpp
  engine/src/script/script_manager.cpp
  engine/src/script/gamevars.cpp
  engine/src/script/bindings.cpp
  engine/src/script/input_map.cpp
  engine/src/bt/behavior_tree.cpp
engine/src/ui/ui.cpp
engine/src/ui/system.cpp
engine/src/ui/document.cpp
engine/src/ui/layout_solver.cpp
engine/src/ui/ui_system.cpp
)
if(NEON_ENABLE_JS)
  list(APPEND ENGINE_CORE_SOURCES engine/src/script/js_host.cpp)
endif()

add_library(neon_engine STATIC ${ENGINE_CORE_SOURCES})
# C12: precompiled headers cut rebuild time on the hot engine + editor +
# test translation units (MSVC /FI, GCC -include). Standard-library only so no
# TU is forced to compile an umbrella header.
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_precompile_headers(neon_engine PRIVATE
    <algorithm> <atomic> <cassert> <cmath> <cstdint> <cstdio> <cstring>
    <map> <memory> <string> <unordered_map> <vector>
  )
endif()
target_include_directories(neon_engine
  PUBLIC
    engine/include
  PRIVATE
    third_party/stb
    assets/generated
)

if(MSVC)
  target_compile_options(neon_engine PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_engine PRIVATE -Wall -Wextra
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------
# Vulkan backend (T7.1): opt-in, SDK-free via volk.
#
# When NEON_ENABLE_VULKAN=ON the backend is compiled and the app's
# `--backend vulkan` flag works. The built-in shaders are precompiled to SPIR-V
# at build time by a vendored glslang (third_party/glslang_tool) into
# assets/generated/vk_shaders.hpp (a committed generated header, so tool-less
# builds still work). When OFF the Vulkan backend is not compiled; requesting
# `--backend vulkan` falls back to OpenGL with a log.
# ---------------------------------------------------------------------------
option(NEON_ENABLE_VULKAN "Build the Vulkan render backend (volk, SDK-free)" OFF)
if(NEON_ENABLE_VULKAN)
  add_library(neon_volk STATIC third_party/volk/volk.c)
  target_include_directories(neon_volk PUBLIC
    third_party/volk
    third_party/vulkan_headers/include
  )
  target_compile_definitions(neon_volk PRIVATE VK_USE_PLATFORM_WIN32_KHR)
  # Vendored C loader - silence warnings from foreign code.
  if(MSVC)
    target_compile_options(neon_volk PRIVATE /W3)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(neon_volk PRIVATE -w)
  endif()
  target_link_libraries(neon_engine PRIVATE neon_volk)
  target_include_directories(neon_engine PUBLIC
    third_party/volk
    third_party/vulkan_headers/include
  )
  target_compile_definitions(neon_engine PUBLIC NEON_ENABLE_VULKAN=1)
  target_sources(neon_engine PRIVATE engine/src/gfx/vulkan/vk_backend.cpp)
  target_include_directories(neon_engine PRIVATE engine/src/gfx/vulkan)

  # Build-time SPIR-V generation (glslang vendored in third_party/glslang_tool).
  # Only re-runs when a shader source is newer than the committed header; the
  # header itself is committed so builds without the tool keep working.
  set(VK_GLSLANG ${CMAKE_SOURCE_DIR}/third_party/glslang_tool/bin/glslang.exe)
  set(VK_SHADER_DIR ${CMAKE_SOURCE_DIR}/engine/src/gfx/vulkan/shaders)
  set(VK_SHADER_OUTPUT ${CMAKE_SOURCE_DIR}/assets/generated/vk_shaders.hpp)
  add_custom_command(
    OUTPUT ${VK_SHADER_OUTPUT}
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
      ${CMAKE_SOURCE_DIR}/tools/gen_vk_shaders.ps1
      -Glslang ${VK_GLSLANG}
      -ShaderDir ${VK_SHADER_DIR}
      -SpvDir ${CMAKE_CURRENT_BINARY_DIR}/vk_spv
      -OutputHeader ${VK_SHADER_OUTPUT}
    DEPENDS
      ${CMAKE_SOURCE_DIR}/tools/gen_vk_shaders.ps1
      ${VK_GLSLANG}
      ${VK_SHADER_DIR}/engine_ubo.glsl
      ${VK_SHADER_DIR}/lit.vert
      ${VK_SHADER_DIR}/lit.frag
      ${VK_SHADER_DIR}/lit_skinned.vert
      ${VK_SHADER_DIR}/unlit.vert
      ${VK_SHADER_DIR}/unlit.frag
      ${VK_SHADER_DIR}/ui.vert
      ${VK_SHADER_DIR}/ui.frag
      ${VK_SHADER_DIR}/lines.vert
      ${VK_SHADER_DIR}/lines.frag
      ${VK_SHADER_DIR}/lit_instanced.vert
      ${VK_SHADER_DIR}/unlit_instanced.vert
      ${VK_SHADER_DIR}/shadow.vert
      ${VK_SHADER_DIR}/shadow.frag
      ${VK_SHADER_DIR}/shadow_instanced.vert
      ${VK_SHADER_DIR}/shadow_skinned.vert
      ${VK_SHADER_DIR}/point_shadow.vert
      ${VK_SHADER_DIR}/point_shadow.frag
      ${VK_SHADER_DIR}/point_shadow_instanced.vert
      ${VK_SHADER_DIR}/point_shadow_skinned.vert
      ${VK_SHADER_DIR}/post.vert
      ${VK_SHADER_DIR}/bright.frag
      ${VK_SHADER_DIR}/blur.frag
      ${VK_SHADER_DIR}/downsample.frag
      ${VK_SHADER_DIR}/upsample_add.frag
      ${VK_SHADER_DIR}/composite.frag
    COMMENT "Generating embedded Vulkan SPIR-V (vk_shaders.hpp)..."
    VERBATIM
  )
  add_custom_target(neon_vk_shaders ALL DEPENDS ${VK_SHADER_OUTPUT})
  add_dependencies(neon_engine neon_vk_shaders)
endif()

# ---------------------------------------------------------------------------
# Platform backends: one window/input implementation per OS
# ---------------------------------------------------------------------------
if(WIN32)
  # winmm stays compiled as the Windows fallback when miniaudio cannot open a
  # device (see engine/src/audio/audio.cpp backend selection).
  target_sources(neon_engine PRIVATE
    engine/src/platform/win32/win32_window.cpp
    engine/src/audio/winmm/winmm_audio.cpp
  )
  target_compile_definitions(neon_engine PUBLIC NEONRUSH_PLATFORM_WINDOWS)
  target_link_libraries(neon_engine PUBLIC opengl32 winmm user32 gdi32 shell32 ole32 ws2_32 imm32)
elseif(APPLE)
  target_sources(neon_engine PRIVATE engine/src/platform/cocoa/cocoa_window.mm)
  set_source_files_properties(engine/src/platform/cocoa/cocoa_window.mm PROPERTIES LANGUAGE OBJCXX)
  target_compile_definitions(neon_engine PUBLIC NEONRUSH_PLATFORM_MACOS)
  target_link_libraries(neon_engine PUBLIC "-framework Cocoa" "-framework OpenGL")
else()
  find_package(X11 REQUIRED)
  target_sources(neon_engine PRIVATE engine/src/platform/x11/x11_window.cpp)
  target_compile_definitions(neon_engine PUBLIC NEONRUSH_PLATFORM_LINUX)
  target_link_libraries(neon_engine PUBLIC ${X11_LIBRARIES} GL)
endif()

# ---------------------------------------------------------------------------
# miniaudio (T5.1): vendored single header compiled once (implementation lives
# in ma_impl.cpp), mirroring how neon_lua is built. The engine's cross-platform
# IAudioBackend (engine/src/audio/miniaudio/ma_audio.cpp) links it. Windows
# needs no extra libs (WASAPI/DirectSound/WinMM are runtime-linked); POSIX
# builds need libm/libdl for miniaudio's internal use.
# ---------------------------------------------------------------------------
add_library(neon_miniaudio STATIC
  engine/src/audio/miniaudio/ma_impl.cpp
)
target_include_directories(neon_miniaudio PUBLIC third_party/miniaudio)
# Vendored C — do not police its warnings as if it were our code.
if(MSVC)
  target_compile_options(neon_miniaudio PRIVATE /W3)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_miniaudio PRIVATE -w)
endif()
if(UNIX)
  target_link_libraries(neon_miniaudio PRIVATE m dl)
endif()
target_link_libraries(neon_engine PRIVATE neon_miniaudio)

# ---------------------------------------------------------------------------
# Jolt physics (P0-1): optional rigid-body backend, vendored at third_party/jolt.
# The deterministic custom physics::World stays compiled as the server fallback;
# NEON_ENABLE_JOLT adds JoltWorld behind the same interface (game/playtest use
# it by default when available).
# ---------------------------------------------------------------------------
option(NEON_ENABLE_JOLT "Build the Jolt rigid-body physics backend (vendored)" ON)
if(NEON_ENABLE_JOLT)
  # Jolt build knobs (see third_party/jolt/Jolt/Jolt.cmake). Keep the baseline
  # x86-64 instruction set so packaged builds run on any modern CPU.
  set(DOUBLE_PRECISION OFF)
  set(CROSS_PLATFORM_DETERMINISTIC OFF)
  set(OBJECT_LAYER_BITS 16)
  set(USE_SSE4_1 ON)
  set(USE_SSE4_2 ON)
  set(USE_AVX OFF)
  set(USE_AVX2 OFF)
  set(USE_AVX512 OFF)
  set(USE_LZCNT ON)
  set(USE_TZCNT ON)
  set(USE_F16C ON)
  set(USE_FMADD OFF)
  # Use the CRT allocator everywhere: the engine/editor mix Jolt objects with
  # their own new/delete, and Jolt's custom operator new hooks conflict with
  # that (a null allocator call crashes at JoltWorld construction).
  set(DISABLE_CUSTOM_ALLOCATOR ON)
  set(ENABLE_ALL_WARNINGS OFF)
  set(INTERPROCEDURAL_OPTIMIZATION OFF)
  set(PHYSICS_REPO_ROOT ${CMAKE_SOURCE_DIR}/third_party/jolt)
  include(${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake)
  # Foreign library: do not police its warnings as if it were our code.
  target_compile_options(Jolt PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-w>
    $<$<CXX_COMPILER_ID:MSVC>:/W3>)
  target_compile_definitions(neon_engine PUBLIC NEON_ENABLE_JOLT=1)
  target_include_directories(neon_engine PUBLIC third_party/jolt)
  target_link_libraries(neon_engine PRIVATE Jolt)
  target_sources(neon_engine PRIVATE engine/src/physics/jolt_world.cpp)
endif()

# ---------------------------------------------------------------------------
# Demo executable
# ---------------------------------------------------------------------------
add_executable(neon_rush
  game/src/main.cpp
  game/src/demo.cpp
  game/src/art.cpp
  game/src/sfx.cpp
)
target_include_directories(neon_rush PRIVATE assets/generated third_party/stb)
target_link_libraries(neon_rush PRIVATE neon_engine)
if(MSVC)
  target_compile_options(neon_rush PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_rush PRIVATE -Wall -Wextra
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------
# Generic data-driven player (T4.7): loads a game.pack and runs it with no
# hardcoded gameplay. Reuses the engine renderer + GameRuntime.
# ---------------------------------------------------------------------------
add_executable(neon_game
  game/src/player_main.cpp
  game/src/player.cpp
  game/src/client_sync.cpp
)
target_include_directories(neon_game PRIVATE assets/generated third_party/stb)
target_link_libraries(neon_game PRIVATE neon_engine)
if(WIN32)
  # P2-5: Windows app icon + version resource (windres / rc).
  target_sources(neon_game PRIVATE platform/win32/neon.rc)
endif()
if(MSVC)
  target_compile_options(neon_game PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_game PRIVATE -Wall -Wextra
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui tool UI (optional module; only the editor links it)
# ---------------------------------------------------------------------------
add_library(neon_imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/imgui_demo.cpp
  third_party/ImGuizmo/ImGuizmo.cpp
  engine/src/gfx/imgui_neon.cpp
)
target_include_directories(neon_imgui
  PUBLIC third_party/imgui
  PUBLIC third_party/ImGuizmo
  PRIVATE engine/include
)
# Dear ImGui 1.92+ defaults to the FreeType rasterizer; use the bundled
# stb_truetype backend instead (zero external dependencies).
target_compile_definitions(neon_imgui PRIVATE IMGUI_ENABLE_STB_TRUETYPE)
if(MSVC)
  target_compile_options(neon_imgui PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_imgui PRIVATE -Wall -Wextra
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------
# Lua 5.4 scripting runtime (optional module; only scripting targets link it)
# ---------------------------------------------------------------------------
set(LUA_SOURCES
  third_party/lua/lapi.c
  third_party/lua/lauxlib.c
  third_party/lua/lbaselib.c
  third_party/lua/lcode.c
  third_party/lua/lcorolib.c
  third_party/lua/lctype.c
  third_party/lua/ldblib.c
  third_party/lua/ldebug.c
  third_party/lua/ldo.c
  third_party/lua/ldump.c
  third_party/lua/lfunc.c
  third_party/lua/lgc.c
  third_party/lua/linit.c
  third_party/lua/liolib.c
  third_party/lua/llex.c
  third_party/lua/lmathlib.c
  third_party/lua/lmem.c
  third_party/lua/loadlib.c
  third_party/lua/lobject.c
  third_party/lua/lopcodes.c
  third_party/lua/loslib.c
  third_party/lua/lparser.c
  third_party/lua/lstate.c
  third_party/lua/lstring.c
  third_party/lua/lstrlib.c
  third_party/lua/ltable.c
  third_party/lua/ltablib.c
  third_party/lua/ltm.c
  third_party/lua/lundump.c
  third_party/lua/lutf8lib.c
  third_party/lua/lvm.c
  third_party/lua/lzio.c
)

add_library(neon_lua STATIC ${LUA_SOURCES})
target_include_directories(neon_lua PUBLIC third_party/lua)
# Lua ships strict-C sources; pin gnu99 so glibc exposes gmtime_r/localtime_r
# (used by loslib.c) and silence the -Wpedantic noise around Lua's macros.
# -Wno-cast-function-type covers MinGW's FARPROC-style cast warnings.
set_target_properties(neon_lua PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS ON
)
if(MSVC)
  target_compile_options(neon_lua PRIVATE /W3)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_lua PRIVATE -Wall -Wextra -Wno-cast-function-type)
endif()
if(APPLE)
  target_compile_definitions(neon_lua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
  target_compile_definitions(neon_lua PRIVATE LUA_USE_LINUX)
  # loadlib.c needs dlopen/dlsym; lmathlib.c needs libm.
  target_link_libraries(neon_lua PRIVATE m dl)
endif()
# The script backend is part of the engine core; consumers inherit neon_lua's
# include dirs and link flow (PUBLIC because neon_lua's include dir is PUBLIC).
target_link_libraries(neon_engine PUBLIC neon_lua)

# ---------------------------------------------------------------------------
# QuickJS (ES2020 JavaScript runtime): the second script backend (JS host).
# Same vendored-source pattern as Lua; quickjs.c + cutils/libregexp/libunicode/
# libbf are compiled as C99 and the JS host (engine/src/script/js_host.cpp)
# implements the same IScriptHost interface as LuaHost.
# ---------------------------------------------------------------------------
if(NEON_ENABLE_JS)
  set(QUICKJS_SOURCES
    third_party/quickjs/quickjs.c
    third_party/quickjs/cutils.c
    third_party/quickjs/libregexp.c
    third_party/quickjs/libunicode.c
    third_party/quickjs/libbf.c
  )

  add_library(neon_quickjs STATIC ${QUICKJS_SOURCES})
  target_include_directories(neon_quickjs PUBLIC third_party/quickjs)
  set_target_properties(neon_quickjs PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
  )
  target_compile_definitions(neon_quickjs PRIVATE
    CONFIG_VERSION="2024-01-13"
    CONFIG_BIGNUM
  )
  if(MSVC)
    target_compile_definitions(neon_quickjs PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(neon_quickjs PRIVATE /W3)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(neon_quickjs PRIVATE -w)
  endif()
  target_link_libraries(neon_engine PUBLIC neon_quickjs)
  target_compile_definitions(neon_engine PUBLIC NEON_ENABLE_JS=1)
endif()

# ---------------------------------------------------------------------------
# Headless authoritative server (T6.3): binds a UDP socket, runs the data-driven
# scene at a fixed 60Hz step and replicates MsgSnapshot streams. No window, no
# GL, no audio — the engine's platform/gfx code is linked (static lib) but never
# called. game_server.cpp is also compiled into neon_tests for loopback tests.
# ---------------------------------------------------------------------------
add_executable(neon_server
  server/src/main.cpp
  server/src/game_server.cpp
  server/src/aoi.cpp
)
target_include_directories(neon_server PRIVATE server/src)
target_link_libraries(neon_server PRIVATE neon_engine)
if(MSVC)
  target_compile_options(neon_server PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_server PRIVATE -Wall -Wextra
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------
# Editor
# ---------------------------------------------------------------------------
add_executable(neon_editor
  editor/src/main.cpp
  editor/src/editor.cpp
  editor/src/editor_viewport.cpp
  editor/src/editor_scene.cpp
  editor/src/editor_ui.cpp
  editor/src/editor_assets.cpp
  editor/src/editor_util.cpp
  editor/src/editor_play.cpp
  editor/src/editor_smoke.cpp
  editor/src/editor_plugin.cpp
  editor/src/editor_plugins.cpp
  editor/src/panels.cpp
  editor/src/history.cpp
  editor/src/bt_editor.cpp
  editor/src/packager.cpp
)
target_include_directories(neon_editor PRIVATE assets/generated third_party/stb)
target_link_libraries(neon_editor PRIVATE neon_engine neon_imgui)
if(WIN32)
  # Native open-file dialog for the asset panel's 导入 action.
  target_link_libraries(neon_editor PRIVATE comdlg32 ole32)
  # P2-5: Windows app icon + version resource.
  target_sources(neon_editor PRIVATE platform/win32/neon.rc)
endif()
if(MSVC)
  target_compile_options(neon_editor PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_editor PRIVATE -Wall -Wextra
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
if(NEON_ENABLE_JS)
  set(NEON_JS_TEST tests/test_script_js.cpp)
endif()
# G4-1 native binary plugin demo: a self-contained physics module built as a
# DLL/SO. Loaded at runtime by the engine's plugin::NativePlugin loader. G5-1:
# the plugin also compiles the engine's real deterministic solver into the DLL,
# so the runtime can use this DLL as a drop-in physics *middleware* backend.
add_library(neon_plugin_physics SHARED plugins/physics_plugin/plugin.cpp
  engine/src/physics/physics.cpp)
target_include_directories(neon_plugin_physics PRIVATE engine/include plugins)
if(MSVC)
  target_compile_options(neon_plugin_physics PRIVATE /utf-8)
endif()
set_target_properties(neon_plugin_physics PROPERTIES
  OUTPUT_NAME "neon_plugin_physics"
  PREFIX "")
if(NOT WIN32)
  set_target_properties(neon_plugin_physics PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

# G5-1 audio middleware plugin (Windows: the engine's WinMM waveOut mixer is
# compiled into the DLL, so the runtime can take audio from a native plugin).
# winmm is Windows-only, so the target is skipped elsewhere.
set(NEON_AUDIO_TEST "")
if(WIN32)
  set(NEON_AUDIO_TEST tests/test_plugin_audio.cpp)
  add_library(neon_plugin_audio SHARED plugins/audio_plugin/plugin.cpp
    engine/src/audio/winmm/winmm_audio.cpp engine/src/core/log.cpp)
  target_include_directories(neon_plugin_audio PRIVATE engine/include plugins)
  if(MSVC)
    target_compile_options(neon_plugin_audio PRIVATE /utf-8)
  endif()
  set_target_properties(neon_plugin_audio PROPERTIES
    OUTPUT_NAME "neon_plugin_audio"
    PREFIX "")
  target_link_libraries(neon_plugin_audio PRIVATE winmm)
endif()

add_executable(neon_tests tests/test_main.cpp tests/test_serialize.cpp tests/test_pack.cpp
  tests/test_physics.cpp tests/test_protocol.cpp tests/test_obj.cpp tests/test_gltf.cpp tests/test_json.cpp
  tests/test_ui_document.cpp
  tests/test_vfs.cpp
  tests/test_scene_tree.cpp
  tests/test_asset_deps.cpp
  tests/test_asset_db.cpp
  tests/test_asset_importer.cpp
  tests/test_bvh.cpp
  tests/test_terrain.cpp
  tests/test_light_probe.cpp
  tests/test_ssao.cpp
  tests/test_volumetric.cpp
  tests/test_ssr.cpp
  tests/test_fog.cpp
  tests/test_pvz_project.cpp
  tests/test_profiler.cpp
  tests/test_task_graph.cpp
  tests/test_script.cpp tests/test_script_bindings.cpp tests/test_script_determinism.cpp
  ${NEON_JS_TEST}
  tests/test_plugin.cpp
  tests/test_plugin_native.cpp
  tests/test_input_map.cpp
  tests/test_mem_stats.cpp
  tests/test_gpu_memory.cpp
  ${NEON_AUDIO_TEST}
  tests/test_bt.cpp tests/test_scene_file.cpp tests/test_scene_export.cpp tests/test_manifest.cpp
  tests/test_nav.cpp
  tests/test_localization.cpp
  tests/test_game_runtime.cpp tests/test_game_runtime_m1.cpp tests/test_gltf_skin.cpp tests/test_anim.cpp
  tests/test_combat.cpp
  tests/test_lagcomp.cpp
  tests/test_skinned.cpp tests/test_shadow.cpp tests/test_log.cpp
  tests/test_spatial.cpp tests/test_post.cpp tests/test_ibl.cpp tests/test_history.cpp
  tests/test_bt_editor.cpp tests/test_script_panel.cpp tests/test_packager.cpp
  tests/test_player.cpp
  tests/test_audio.cpp
  tests/test_async.cpp
  tests/test_lod.cpp
  tests/test_world_chunk.cpp
  tests/test_ecs_parallel.cpp
  tests/test_system_scheduler.cpp
  tests/test_reliable.cpp
  tests/test_server.cpp
  tests/test_client_sync.cpp
  tests/test_determinism.cpp
  tests/test_aoi.cpp
  tests/test_asset_lifecycle.cpp
  tests/test_jolt.cpp
  editor/src/history.cpp editor/src/packager.cpp server/src/game_server.cpp server/src/aoi.cpp
  game/src/client_sync.cpp)
target_link_libraries(neon_tests PRIVATE neon_engine)
if(MINGW)
  # test_main.cpp's crash handler calls SymInitialize (dbghelp.h), which
  # MSVC's #pragma comment(lib) does not carry over to MinGW links.
  target_link_libraries(neon_tests PRIVATE dbghelp)
endif()
# test_history.cpp includes the editor's generic undo/redo stack header.
target_include_directories(neon_tests PRIVATE editor/src third_party/stb server/src game/src plugins)
if(WIN32)
  target_compile_definitions(neon_tests PRIVATE
    NEON_PLUGIN_PHYSICS_PATH="$<TARGET_FILE:neon_plugin_physics>"
    NEON_PLUGIN_AUDIO_PATH="$<TARGET_FILE:neon_plugin_audio>")
else()
  target_compile_definitions(neon_tests PRIVATE
    NEON_PLUGIN_PHYSICS_PATH="$<TARGET_FILE:neon_plugin_physics>")
endif()
if(MSVC)
  target_compile_options(neon_tests PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neon_tests PRIVATE -Wall -Wextra
    -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()
add_test(NAME unit COMMAND neon_tests)
# ctest runs the binary from the build dir by default; run it from the source
# root so tests that load repo-relative assets (assets/...) resolve identically
# to ./build/neon_tests.exe invoked from the repo root.
set_tests_properties(unit PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(neon_engine PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# MinGW runtime DLLs next to each executable (launch robustness)
#
# The exe is built against C:/msys64/mingw64 (GCC 16.2). Machines that also
# have an OLD MinGW earlier on PATH (e.g. D:\mingw64 with GCC 8.1) would load
# its stale libstdc++-6.dll and fail to start with STATUS_ENTRYPOINT_NOT_FOUND
# (0xC0000139) from a terminal. Windows loads DLLs from the exe's directory
# FIRST, so copying the three runtime DLLs next to every target makes launch
# independent of PATH.
# ---------------------------------------------------------------------------
if(MINGW)
  get_filename_component(NEON_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  function(neon_copy_runtime_dlls target)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NEON_MINGW_BIN}/libstdc++-6.dll"
        "${NEON_MINGW_BIN}/libgcc_s_seh-1.dll"
        "${NEON_MINGW_BIN}/libwinpthread-1.dll"
        $<TARGET_FILE_DIR:${target}>
      VERBATIM)
  endfunction()
  neon_copy_runtime_dlls(neon_editor)
  neon_copy_runtime_dlls(neon_tests)
  neon_copy_runtime_dlls(neon_server)
endif()

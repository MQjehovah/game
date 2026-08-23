cmake_minimum_required(VERSION 3.15)
project(NeonEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Engine core (platform-independent)
# ---------------------------------------------------------------------------
set(ENGINE_CORE_SOURCES
  engine/src/assets/asset_manager.cpp
  engine/src/assets/async_loader.cpp
  engine/src/assets/bc1.cpp
  engine/src/anim/anim.cpp
  engine/src/audio/audio.cpp
  engine/src/audio/miniaudio/ma_audio.cpp
  engine/src/core/app.cpp
  engine/src/core/config.cpp
  engine/src/core/log.cpp
  engine/src/ecs/world.cpp
  engine/src/ecs/parallel.cpp
  engine/src/core/json.cpp
  engine/src/core/serialize.cpp
  engine/src/core/pack.cpp
  engine/src/net/protocol.cpp
  engine/src/net/socket.cpp
  engine/src/net/reliable.cpp
  engine/src/gfx/color.cpp
  engine/src/gfx/csm.cpp
  engine/src/gfx/font.cpp
  engine/src/gfx/ibl.cpp
  engine/src/gfx/mesh.cpp
  engine/src/gfx/scene_props.cpp
  engine/src/gfx/particles.cpp
  engine/src/gfx/renderer.cpp
  engine/src/gfx/gl/gl_loader.cpp
  engine/src/gfx/gl/gl_backend.cpp
  engine/src/physics/physics.cpp
  engine/src/platform/input.cpp
  engine/src/scene/scene_file.cpp
  engine/src/scene/scene_manager.cpp
  engine/src/scene/game_manifest.cpp
  engine/src/scene/game_runtime.cpp
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
)

add_library(neon_engine STATIC ${ENGINE_CORE_SOURCES})
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
  editor/src/panels.cpp
  editor/src/history.cpp
  editor/src/bt_editor.cpp
  editor/src/packager.cpp
)
target_include_directories(neon_editor PRIVATE assets/generated third_party/stb)
target_link_libraries(neon_editor PRIVATE neon_engine neon_imgui)
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
add_executable(neon_tests tests/test_main.cpp tests/test_serialize.cpp tests/test_pack.cpp
  tests/test_physics.cpp tests/test_protocol.cpp tests/test_obj.cpp tests/test_gltf.cpp tests/test_json.cpp
  tests/test_script.cpp tests/test_script_bindings.cpp tests/test_script_determinism.cpp
  tests/test_bt.cpp tests/test_scene_file.cpp tests/test_scene_export.cpp tests/test_manifest.cpp
  tests/test_game_runtime.cpp tests/test_gltf_skin.cpp tests/test_anim.cpp
  tests/test_combat.cpp
  tests/test_skinned.cpp tests/test_shadow.cpp tests/test_log.cpp
  tests/test_spatial.cpp tests/test_post.cpp tests/test_ibl.cpp tests/test_history.cpp
  tests/test_bt_editor.cpp tests/test_script_panel.cpp tests/test_packager.cpp
  tests/test_player.cpp
  tests/test_audio.cpp
  tests/test_async.cpp
  tests/test_lod.cpp
  tests/test_world_chunk.cpp
  tests/test_ecs_parallel.cpp
  tests/test_reliable.cpp
  tests/test_server.cpp
  tests/test_client_sync.cpp
  tests/test_determinism.cpp
  tests/test_aoi.cpp
  editor/src/history.cpp editor/src/packager.cpp server/src/game_server.cpp server/src/aoi.cpp
  game/src/client_sync.cpp)
target_link_libraries(neon_tests PRIVATE neon_engine)
# test_history.cpp includes the editor's generic undo/redo stack header.
target_include_directories(neon_tests PRIVATE editor/src third_party/stb server/src game/src)
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

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
  engine/src/anim/anim.cpp
  engine/src/audio/audio.cpp
  engine/src/core/app.cpp
  engine/src/core/config.cpp
  engine/src/core/log.cpp
  engine/src/ecs/world.cpp
  engine/src/core/json.cpp
  engine/src/core/serialize.cpp
  engine/src/core/pack.cpp
  engine/src/gfx/color.cpp
  engine/src/gfx/csm.cpp
  engine/src/gfx/font.cpp
  engine/src/gfx/ibl.cpp
  engine/src/gfx/mesh.cpp
  engine/src/gfx/particles.cpp
  engine/src/gfx/renderer.cpp
  engine/src/gfx/gl/gl_loader.cpp
  engine/src/gfx/gl/gl_backend.cpp
  engine/src/gfx/vulkan/vk_backend.cpp
  engine/src/physics/physics.cpp
  engine/src/platform/input.cpp
  engine/src/scene/scene_file.cpp
  engine/src/scene/scene_manager.cpp
  engine/src/scene/game_manifest.cpp
  engine/src/scene/game_runtime.cpp
  engine/src/script/lua_host.cpp
  engine/src/script/script_manager.cpp
  engine/src/script/gamevars.cpp
  engine/src/script/bindings.cpp
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
# Platform backends: one window/input implementation per OS
# ---------------------------------------------------------------------------
if(WIN32)
  target_sources(neon_engine PRIVATE
    engine/src/platform/win32/win32_window.cpp
    engine/src/audio/winmm/winmm_audio.cpp
  )
  target_compile_definitions(neon_engine PUBLIC NEONRUSH_PLATFORM_WINDOWS)
  target_link_libraries(neon_engine PUBLIC opengl32 winmm user32 gdi32 shell32 ole32)
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
# Editor
# ---------------------------------------------------------------------------
add_executable(neon_editor
  editor/src/main.cpp
  editor/src/editor.cpp
  editor/src/panels.cpp
  editor/src/history.cpp
  editor/src/bt_editor.cpp
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
  tests/test_physics.cpp tests/test_obj.cpp tests/test_gltf.cpp tests/test_json.cpp
  tests/test_script.cpp tests/test_script_bindings.cpp tests/test_script_determinism.cpp
  tests/test_bt.cpp tests/test_scene_file.cpp tests/test_scene_export.cpp tests/test_manifest.cpp
  tests/test_game_runtime.cpp tests/test_gltf_skin.cpp tests/test_anim.cpp
  tests/test_skinned.cpp tests/test_shadow.cpp tests/test_log.cpp
  tests/test_spatial.cpp tests/test_post.cpp tests/test_ibl.cpp tests/test_history.cpp
  tests/test_bt_editor.cpp
  editor/src/history.cpp)
target_link_libraries(neon_tests PRIVATE neon_engine)
# test_history.cpp includes the editor's generic undo/redo stack header.
target_include_directories(neon_tests PRIVATE editor/src)
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

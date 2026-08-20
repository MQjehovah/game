cmake_minimum_required(VERSION 3.15)
project(NeonEngine VERSION 0.1.0 LANGUAGES CXX)

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
  engine/src/audio/audio.cpp
  engine/src/core/app.cpp
  engine/src/core/config.cpp
  engine/src/core/log.cpp
  engine/src/ecs/world.cpp
  engine/src/core/json.cpp
  engine/src/core/serialize.cpp
  engine/src/gfx/color.cpp
  engine/src/gfx/font.cpp
  engine/src/gfx/mesh.cpp
  engine/src/gfx/particles.cpp
  engine/src/gfx/renderer.cpp
  engine/src/gfx/gl/gl_loader.cpp
  engine/src/gfx/gl/gl_backend.cpp
  engine/src/gfx/vulkan/vk_backend.cpp
  engine/src/physics/physics.cpp
  engine/src/platform/input.cpp
  engine/src/scene/scene_manager.cpp
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
  engine/src/gfx/imgui_neon.cpp
)
target_include_directories(neon_imgui
  PUBLIC third_party/imgui
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
# Editor
# ---------------------------------------------------------------------------
add_executable(neon_editor
  editor/src/main.cpp
  editor/src/editor.cpp
  editor/src/panels.cpp
)
target_include_directories(neon_editor PRIVATE assets/generated third_party/stb)
target_link_libraries(neon_editor PRIVATE neon_engine neon_imgui)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(neon_tests tests/test_main.cpp tests/test_serialize.cpp)
target_link_libraries(neon_tests PRIVATE neon_engine)
add_test(NAME unit COMMAND neon_tests)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(neon_engine PUBLIC Threads::Threads)
endif()

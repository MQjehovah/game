#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "neon/audio/audio.hpp"
#include "neon/gfx/camera.hpp"
#include "neon/gfx/color.hpp"
#include "neon/math/math.hpp"
#include "neon/math/quat.hpp"

namespace neon::editor {

// ---------------------------------------------------------------------------
// Shared editor utilities: file/string/color/transform helpers that used to be
// copy-pasted across editor.cpp / panels.cpp (with drift). Keeping them in one
// translation unit makes every editor TU share a single implementation.
// ---------------------------------------------------------------------------

// Project-relative asset path for a file (absolute or relative). Defined once
// (used by the asset panel and the editor core).
std::string ToProjectRelPath(const std::string& path, const std::string& projectDir);

// Colors <-> "#RRGGBB" strings.
gfx::Color ColorFromHex(const std::string& hex);
std::string ColorToHex(const gfx::Color& c);

// Path pieces.
std::string DirName(const std::string& path);
std::string BaseName(const std::string& path);
std::string ExtLower(const std::string& path);
std::string GetWorkingDir();

// Filesystem helpers.
bool MakeDir(const std::string& path);
bool EnsureDirs(const std::string& path);
std::string GetTempDir();
bool WriteFileUtf8(const std::string& path, const std::string& content);
bool TouchFileMTime(const std::string& path, int64_t offsetSeconds);
// File modification time in seconds (0 when the file does not exist).
uint64_t FileMTime(const std::string& path);

// Mesh-key / scene display helpers.
std::string ExportMeshKey(const std::string& key);
std::string ResolveMeshAssetPath(const std::string& rel, const std::string& projectDir);
std::string MeshKeyAssetPath(const std::string& key, const std::string& projectDir);
std::string SceneDisplayName(const std::string& path);
bool IsBakedColorKey(const std::string& key);

// Camera unprojection + projection for the viewport overlays.
math::Ray RayFromNDC(const gfx::Camera& cam, float aspect, float ndcX, float ndcY);
math::Ray ScreenRay(const gfx::Camera& cam, float aspect, const math::Vec2& designPos);
bool WorldToScreenImGui(const gfx::Camera& cam, float aspect, const math::Rect2& vp,
                        const math::Vec3& w, math::Vec2& out);

// ImGuizmo matrix boundary (engine row-major Mat4 <-> ImGuizmo float[16]).
void Mat4ToGizmo(const math::Mat4& m, float out[16]);
void GizmoToMat4(const float in[16], math::Mat4& m);
void DecomposeModel(const math::Mat4& m, math::Vec3& pos, math::Vec3& scale,
                    math::Quat& rot);

// Play SFX for a 2D game project. Tries the project's real WAV clip
// (<projectDir>/assets/audio/<name>.wav) first, falling back to the
// procedural synth so games have sound without shipping audio files.
// Loaded WAVs are cached per project dir + name.
neon::audio::SoundFx MakePvzSfx(const std::string& name, const std::string& projectDir = "");

} // namespace neon::editor

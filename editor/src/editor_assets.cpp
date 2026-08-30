#include "editor.hpp"
#include "editor_util.hpp"
#include "neon/assets/mesh_format.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

#include "neon/gfx/imgui_neon.hpp"

namespace neon::editor {

void EditorApp::RequestMeshThumbnail(const std::string& path) {
    if (path.empty()) return;
    const uint64_t m = FileMTime(path);
    auto it = meshThumbs_.find(path);
    if (it != meshThumbs_.end() && it->second.mtime == m) return; // fresh
    if (std::find(meshThumbQueue_.begin(), meshThumbQueue_.end(), path) ==
        meshThumbQueue_.end()) {
        meshThumbQueue_.push_back(path);
    }
}

void EditorApp::GenerateMeshThumbnails() {
    if (meshThumbQueue_.empty()) return;
    gfx::IRenderBackend* backend = renderer_.Backend();
    if (!backend) {
        meshThumbQueue_.clear();
        return;
    }
    constexpr int kThumb = 96;
    const bool savedShadowRec = renderer_.ShadowRecording();
    // A thumbnail is tooling, not scene geometry: never record its mesh as a
    // shadow caster for the main scene's next shadow pass.
    renderer_.SetShadowRecording(false);
    for (const std::string& path : meshThumbQueue_) {
        const uint64_t m = FileMTime(path);
        auto it = meshThumbs_.find(path);
        if (it != meshThumbs_.end() && it->second.mtime == m) continue; // already fresh

        // Resolve the asset's first mesh; a failed load caches a "miss" (same
        // mtime) so the panel only retries when the file actually changes.
        const std::string ext = ExtLower(path);
        // Mesh models dispatch through the mesh-format registry (unified). The
        // thumbnail wraps the first mesh into a skeleton-less SkinnedModel so
        // the shared preview path draws it (static path, bones.empty()).
        scene::SkinnedModel sm;
        bool loaded = false;
        const std::string prefix =
            assets::MeshFormatRegistry::Instance().FormatFromExt(path);
        if (!prefix.empty()) {
            assets::MeshLoadResult res =
                assets::MeshFormatRegistry::Instance().Load(assetMgr_, prefix + ":" + path);
            if (res.mesh.Valid()) {
                scene::SkinnedModel::Part p;
                p.mesh = res.mesh;
                p.material = gfx::Material::Lit({}, gfx::Color::White, 16.0f);
                sm.parts.push_back(std::move(p));
                loaded = true;
            }
        } else if (ext == ".gltf" || ext == ".glb") {
            // LoadSkinnedModel handles .gltf AND .glb, static (no skin) AND skinned
            // models, and renders the SKINNED bind pose (skinning can flip a model
            // upright from its raw vertex orientation — a plain DrawMesh of the raw
            // vertices would show e.g. CesiumMan upside down).
            core::Result<scene::SkinnedModel> r = scene::LoadSkinnedModel(assetMgr_, path);
            if (r.Ok()) {
                sm = std::move(r.Value());
                loaded = true;
            }
        }
        if (!loaded || sm.parts.empty()) {
            if (it != meshThumbs_.end()) {
                if (it->second.texId != ImTextureID_Invalid)
                    gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
                if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
                meshThumbs_.erase(it);
            }
            meshThumbs_[path] = {{}, {}, ImTextureID_Invalid, m};
            continue;
        }

        // World-space bounds over all parts (mesh local bounds * node transform).
        math::AABB b;
        bool first = true;
        for (const scene::SkinnedModel::Part& part : sm.parts) {
            const math::AABB pb = math::TransformAABB(part.mesh.Bounds(), part.localTransform);
            if (first) {
                b = pb;
                first = false;
            } else {
                b.min = {std::min(b.min.x, pb.min.x), std::min(b.min.y, pb.min.y),
                         std::min(b.min.z, pb.min.z)};
                b.max = {std::max(b.max.x, pb.max.x), std::max(b.max.y, pb.max.y),
                         std::max(b.max.z, pb.max.z)};
            }
        }
        if (first) b = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        const math::Vec3 center = (b.min + b.max) * 0.5f;
        const math::Vec3 extents = b.max - b.min;
        const float size = std::max({extents.x, extents.y, extents.z, 0.001f}) * 1.2f;
        gfx::Camera cam;
        cam.position = center + math::Vec3{0.45f, 0.35f, 1.0f} * size;
        cam.target = center;
        cam.up = {0, 1, 0};
        cam.ortho = true;
        cam.orthoSize = size * 0.62f;
        cam.nearPlane = 0.05f;
        cam.farPlane = size * 6.0f + 1.0f;

        // RGBA16F so the target carries a depth attachment (a plain RGBA8
        // target has none); the lit mesh then occludes correctly.
        gfx::RenderTargetHandle rt = backend->CreateRenderTarget(kThumb, kThumb, true, 0);
        if (!rt.Valid()) continue;
        backend->BindRenderTarget(rt); // sets the 96x96 viewport
        backend->Clear({0.10f, 0.11f, 0.14f, 1.0f}, 1.0f);
        renderer_.SetCamera(cam, 1.0f);
        // SetCamera's CSM shadow pass may rebind the main HDR target (when the
        // viewport did not run first this frame), silently stealing our thumb
        // FBO. Re-assert it + full viewport like the model-viewer preview.
        backend->BindRenderTarget(rt);
        backend->SetViewport(0, 0, kThumb, kThumb);
        backend->SetScissor(0, 0, kThumb, kThumb, true);
        // Bind pose (static models have an empty skeleton -> DrawMesh path).
        const anim::Pose pose = sm.skeleton.BindPose();
        const std::vector<math::Mat4> bones = sm.skeleton.ComputeBoneMatrices(pose);
        if (bones.empty()) {
            for (const scene::SkinnedModel::Part& part : sm.parts)
                renderer_.DrawMesh(part.mesh, part.material, part.localTransform);
        } else {
            for (const scene::SkinnedModel::Part& part : sm.parts)
                renderer_.DrawSkinnedMesh(part.mesh, part.material, part.localTransform, bones,
                                          static_cast<int>(bones.size()));
        }
        const gfx::TextureHandle tex = backend->RenderTargetColorTexture(rt);
        // Leave the GL scissor disabled for the ImGui pass / next frame.
        backend->SetScissor(0, 0, 0, 0, false);

        if (it != meshThumbs_.end()) {
            if (it->second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
            if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
            meshThumbs_.erase(it);
        }
        MeshThumb nt;
        nt.rt = rt;
        nt.texHandle = tex;
        nt.texId = gfx::ImGuiNeon_RegisterTexture(tex);
        nt.mtime = m;
        meshThumbs_[path] = nt;
    }
    meshThumbQueue_.clear();
    renderer_.SetShadowRecording(savedShadowRec);
    // Leave the backbuffer bound + the viewport at window size for the ImGui
    // pass (EndScene already composited to it).
    backend->BindDefaultTarget();
}

void EditorApp::RequestMaterialThumbnail(const std::string& path) {
    if (path.empty()) return;
    const uint64_t m = FileMTime(path);
    auto it = materialThumbs_.find(path);
    if (it != materialThumbs_.end() && it->second.mtime == m) return; // fresh
    if (std::find(materialThumbQueue_.begin(), materialThumbQueue_.end(), path) ==
        materialThumbQueue_.end()) {
        materialThumbQueue_.push_back(path);
    }
}

void EditorApp::GenerateMaterialThumbnails() {
    if (materialThumbQueue_.empty()) return;
    gfx::IRenderBackend* backend = renderer_.Backend();
    if (!backend) {
        materialThumbQueue_.clear();
        return;
    }
    constexpr int kThumb = 96;
    const bool savedShadowRec = renderer_.ShadowRecording();
    renderer_.SetShadowRecording(false);
    for (const std::string& path : materialThumbQueue_) {
        const uint64_t m = FileMTime(path);
        auto it = materialThumbs_.find(path);
        if (it != materialThumbs_.end() && it->second.mtime == m) continue;

        // Expand the material asset into entity-style params, then build a
        // PBR material from them (texture slots resolve through the cache).
        SceneEntity params;
        if (!LoadMaterialParamsInto(params, path)) {
            // Cache a miss so the panel only retries when the file changes.
            if (it != materialThumbs_.end()) {
                if (it->second.texId != ImTextureID_Invalid)
                    gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
                if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
                materialThumbs_.erase(it);
            }
            materialThumbs_[path] = {{}, {}, ImTextureID_Invalid, m};
            continue;
        }
        gfx::Material mat = gfx::Material::Lit({}, params.tint, 8.0f);
        mat.metallic = params.metallic;
        mat.roughness = params.roughness;
        mat.aoStrength = params.ao;
        mat.emissiveIntensity = params.emissiveIntensity;
        if (!params.albedoTex.empty())
            mat.albedo = assetMgr_.LoadTexture(params.albedoTex).Handle();
        if (!params.mrTex.empty())
            mat.metallicRoughness = assetMgr_.LoadTexture(params.mrTex).Handle();
        if (!params.aoTex.empty())
            mat.occlusion = assetMgr_.LoadTexture(params.aoTex).Handle();
        if (!params.emissiveTex.empty())
            mat.emissive = assetMgr_.LoadTexture(params.emissiveTex).Handle();

        gfx::Mesh sphere = gfx::Mesh::CreateSphere(renderer_, 0.8f, 16, 12, "matball");
        const math::AABB& b = sphere.Bounds();
        const math::Vec3 center = (b.min + b.max) * 0.5f;
        const float size = std::max({b.max.x - b.min.x, b.max.y - b.min.y,
                                     b.max.z - b.min.z, 0.001f}) *
                           1.2f;
        gfx::Camera cam;
        cam.position = center + math::Vec3{0.45f, 0.35f, 1.0f} * size;
        cam.target = center;
        cam.up = {0, 1, 0};
        cam.ortho = true;
        cam.orthoSize = size * 0.62f;
        cam.nearPlane = 0.05f;
        cam.farPlane = size * 6.0f + 1.0f;

        gfx::RenderTargetHandle rt = backend->CreateRenderTarget(kThumb, kThumb, true, 0);
        if (!rt.Valid()) continue;
        backend->BindRenderTarget(rt);
        backend->Clear({0.10f, 0.11f, 0.14f, 1.0f}, 1.0f);
        renderer_.SetCamera(cam, 1.0f);
        renderer_.DrawMesh(sphere, mat, math::Mat4::Identity());
        const gfx::TextureHandle tex = backend->RenderTargetColorTexture(rt);

        if (it != materialThumbs_.end()) {
            if (it->second.texId != ImTextureID_Invalid)
                gfx::ImGuiNeon_UnregisterTexture(it->second.texHandle);
            if (it->second.rt.Valid()) backend->DestroyRenderTarget(it->second.rt);
            materialThumbs_.erase(it);
        }
        MeshThumb nt;
        nt.rt = rt;
        nt.texHandle = tex;
        nt.texId = gfx::ImGuiNeon_RegisterTexture(tex);
        nt.mtime = m;
        materialThumbs_[path] = nt;
    }
    materialThumbQueue_.clear();
    renderer_.SetShadowRecording(savedShadowRec);
    backend->BindDefaultTarget();
}

void EditorApp::PollHotReload() {
    const std::string base = projectDir_.empty() ? "." : projectDir_;

    // Scripts: only while a play runs (that is what executes scripts). A
    // changed *.lua under <projectDir>/assets/scripts/ is applied as a play
    // restart (Stop + Start), which resets all script/entity/BT state - a safe,
    // deterministic reload for the editor. Shaders are compiled from strings
    // at init and are deliberately NOT hot-reloaded (YAGNI; see T4.8 notes).
    if (playActive_ && play_) {
        std::vector<std::string> files;
        ListScriptFiles(ScriptsDir(projectDir_), "assets/scripts", files);
        bool scriptChanged = false;
        for (const std::string& rel : files) {
            const std::string full = base + "/" + rel;
            const uint64_t m = FileMTime(full);
            auto it = scriptMtimes_.find(full);
            if (it != scriptMtimes_.end() && it->second != m) {
                scriptChanged = true;
                break;
            }
            scriptMtimes_[full] = m;
        }
        if (scriptChanged) {
            ++hotReloadCount_;
            NEON_LOG_INFO(
                "Editor: hot reload: a script changed on disk -> restarting play "
                "(play state resets)");
            StopPlay();
            StartPlay();
        }
    }

    // Assets referenced by the editor scene: textures + file-backed meshes,
    // including the file-backed built-in mesh keys (helmet/tree resolve to
    // files via ResolveMesh). glTF is re-parsed by ResolveMesh on every call,
    // so a change only needs the mtime gate here; OBJ/textures drop through
    // the AssetManager cache.
    std::set<std::string> changedPaths;
    auto checkFile = [&](const std::string& path) {
        if (path.empty()) return;
        const uint64_t m = FileMTime(path);
        auto it = assetMtimes_.find(path);
        if (it != assetMtimes_.end() && it->second != m && m != 0) changedPaths.insert(path);
        assetMtimes_[path] = m;
    };
    for (const SceneEntity& e : entities_) {
        const std::string meshPath = MeshKeyAssetPath(e.meshKey, projectDir_);
        if (!meshPath.empty()) checkFile(meshPath);
        if (!e.shaderPath.empty()) checkFile(e.shaderPath);
        checkFile(e.albedoTex);
        checkFile(e.mrTex);
        checkFile(e.aoTex);
        checkFile(e.emissiveTex);
    }
    if (!changedPaths.empty()) {
        for (const std::string& p : changedPaths) {
            const std::string ext = ExtLower(p);
            const std::string prefix =
                assets::MeshFormatRegistry::Instance().FormatFromExt(p);
            if (prefix == "obj") {
                assetMgr_.ReloadMeshOBJ(p);
            } else if (prefix == "gltf" || ext == ".glb") {
                // Drop the resolved-model cache so the next ResolveMesh
                // re-parses the updated file. (The old GPU meshes are not
                // explicitly destroyed; same as the pre-existing mesh path.)
                skinnedModelCache_.erase(p);
                gltfStaticMeshCache_.erase(p);
                gltfStaticMaterialCache_.erase(p);
            } else if (!prefix.empty()) {
                // FBX / future formats have no per-format reload API yet: fall
                // back to a texture reload no-op is wrong, so log it. (Hot reload
                // for these formats can be added with the format's loader.)
                NEON_LOG_INFO("Editor: hot reload '%s' (format '%s') — full reload",
                              p.c_str(), prefix.c_str());
            } else {
                assetMgr_.ReloadTexture(p);
            }
            NEON_LOG_INFO("Editor: hot reload: asset '%s' reloaded", p.c_str());
        }
        ++hotReloadCount_;
        // Re-resolve only the entities that reference a changed asset.
        for (SceneEntity& e : entities_) {
            const bool touches =
                changedPaths.count(MeshKeyAssetPath(e.meshKey, projectDir_)) ||
                changedPaths.count(e.shaderPath) ||
                changedPaths.count(e.albedoTex) || changedPaths.count(e.mrTex) ||
                changedPaths.count(e.aoTex) || changedPaths.count(e.emissiveTex);
            if (touches) {
                if (changedPaths.count(e.shaderPath)) ReloadEntityShader(e);
                ResolveMesh(e);
                ApplyMaterialParams(e);
            }
        }
    }

    // Asset directory watch (P1-1 import pipeline): new/changed files under
    // the project's assets dir show up in the asset panel automatically (the
    // panel still owns the actual import/copy actions).
    if (!assetDir_.empty()) {
        std::vector<AssetEntry> now;
        if (ListDirectory(assetDir_, now)) {
            std::string sig;
            for (const AssetEntry& a : now) {
                sig += a.name;
                sig += a.isDir ? "/" : "|";
            }
            if (sig != assetDirSignature_) {
                assetDirSignature_ = sig;
                RefreshAssetDir();
                NEON_LOG_INFO("Editor: asset dir watch: %zu entries", now.size());
            }
        }
    }
}

void EditorApp::LoadEditorConfig() {
    projectDir_ = kDefaultProjectDir;
    std::ifstream in("neon_editor_config.json");
    if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        core::Json root = core::Json::Parse(ss.str(), &err);
        if (root.IsObject()) {
            if (const core::Json* p = root.Get("projectDir")) projectDir_ = p->GetString();
        }
    }
    // "." is the legacy repo-root sandbox (pre projects/default); normalize it
    // so a stale config does not anchor the editor to the bare repo root.
    if (projectDir_.empty() || projectDir_ == ".") projectDir_ = kDefaultProjectDir;
    std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
    projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
}

void EditorApp::SaveEditorConfig() {
    if (projectDir_.empty() || projectDir_ == ".") projectDir_ = kDefaultProjectDir;
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json p;
    p.type_ = core::Json::Type::String;
    p.string_ = projectDir_;
    root.object_["projectDir"] = p;
    std::string json = core::JsonWriter::WritePretty(root);
    if (std::ofstream out("neon_editor_config.json"); out.is_open()) {
        out << json;
        NEON_LOG_INFO("Editor: config saved (project dir '%s')", projectDir_.c_str());
    } else {
        NEON_LOG_WARN("Editor: cannot write editor config");
    }
}

} // namespace neon::editor


void EditorApp::RefreshAssetDir() {
    assetEntries_.clear();
    selectedAsset_ = -1;
    ListDirectory(assetDir_, assetEntries_);
    assetDirSignature_.clear();
    for (const AssetEntry& a : assetEntries_) {
        assetDirSignature_ += a.name;
        assetDirSignature_ += a.isDir ? "/" : "|";
    }
    std::sort(assetEntries_.begin(), assetEntries_.end(),
              [](const AssetEntry& a, const AssetEntry& b) {
                  if (a.isDir != b.isDir) return a.isDir;
                  return ToLower(a.name) < ToLower(b.name);
              });
    NEON_LOG_DEBUG("Editor: asset dir '%s' (%zu entries)", assetDir_.c_str(),
                   assetEntries_.size());
}

bool EditorApp::InPrefabsDir() const {
    // Normalize separators then compare the final path component.
    std::string d = assetDir_;
    std::replace(d.begin(), d.end(), '\\', '/');
    const size_t last = d.find_last_of('/');
    const std::string base = last == std::string::npos ? d : d.substr(last + 1);
    return ToLower(base) == "prefabs";
}

// Copies a file into the current asset dir (skipping a duplicate name by
// appending _1/_2/...), then refreshes the listing.
void EditorApp::ImportAssetFile(const std::string& srcPath) {
    if (srcPath.empty()) return;
    if (IsDirPath(srcPath)) {
        // A whole resource directory (model + textures + subfolders) is
        // copied recursively into the project assets/.
        const std::string name = FileName(srcPath);
        if (CopyDirRecursive(srcPath, assetDir_ + "/" + name)) {
            NEON_LOG_INFO("Asset: imported directory '%s' -> '%s/%s'", srcPath.c_str(),
                          assetDir_.c_str(), name.c_str());
            RefreshAssetDir();
        } else {
            NEON_LOG_ERROR("Asset: failed to import directory '%s'", srcPath.c_str());
        }
        return;
    }
    const std::string base = FileName(srcPath);
    if (base.empty() || base == "." || base == "..") {
        NEON_LOG_ERROR("Asset: cannot import '%s'", srcPath.c_str());
        return;
    }
    const std::string name = UniqueNameIn(assetDir_, base);
    if (CopyFileBinary(srcPath, assetDir_ + "/" + name)) {
        NEON_LOG_INFO("Asset: imported '%s' -> '%s/%s'", srcPath.c_str(), assetDir_.c_str(),
                      name.c_str());
        RefreshAssetDir();
    } else {
        NEON_LOG_ERROR("Asset: failed to import '%s' (target '%s/%s')", srcPath.c_str(),
                       assetDir_.c_str(), name.c_str());
    }
}

// Creates a new asset in the current asset dir: kind 0 = directory, 1 = Lua
// script, 2 = JSON, 3 = empty text file, 4 = material ball, 5 = JS script.
void EditorApp::CreateAssetFile(const std::string& name, int kind) {
    if (name.empty() || name == "." || name == "..") {
        NEON_LOG_ERROR("Asset: invalid asset name '%s'", name.c_str());
        return;
    }
    const std::string path = assetDir_ + "/" + name;
    if (kind == 0) {
        if (MakeDirSingle(path)) {
            NEON_LOG_INFO("Asset: created directory '%s'", path.c_str());
            RefreshAssetDir();
        } else {
            NEON_LOG_ERROR("Asset: cannot create directory '%s'", path.c_str());
        }
        return;
    }
    if (std::ifstream(path, std::ios::binary).is_open()) {
        NEON_LOG_ERROR("Asset: '%s' already exists", path.c_str());
        return;
    }
    std::string content;
    if (kind == 1) {
        content = "-- New script (data-driven)\nfunction on_start(ent)\nend\n"
                  "function on_update(ent, dt)\nend\n";
    } else if (kind == 5) {
        content = "// New script (data-driven)\n"
                  "function on_start(ent) {\n}\n"
                  "function on_update(ent, dt) {\n}\n";
    } else if (kind == 2) {
        content = "{\n}\n";
    } else if (kind == 4) {
        // Material ball asset (Unity .mat / Godot Material style).
        content = "{\n  \"colorHex\": \"#FFFFFF\",\n  \"metallic\": 0.0,\n"
                  "  \"roughness\": 0.8,\n  \"ao\": 1.0,\n"
                  "  \"emissiveIntensity\": 1.0,\n  \"albedoTex\": \"\",\n"
                  "  \"mrTex\": \"\",\n  \"aoTex\": \"\",\n  \"emissiveTex\": \"\",\n"
                  "  \"normalTex\": \"\",\n  \"normalScale\": 1.0\n}\n";
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Asset: cannot create '%s'", path.c_str());
        return;
    }
    out << content;
    NEON_LOG_INFO("Asset: created '%s'", path.c_str());
    RefreshAssetDir();
}

void EditorApp::ImportAssetPath(const std::string& path) {
    // Mesh models are dispatched through the mesh-format registry: any
    // registered extension (.obj/.gltf/.glb/.fbx/...) becomes a draft entity
    // with the format's meshKey prefix. Adding a new format here is automatic.
    const std::string prefix =
        assets::MeshFormatRegistry::Instance().FormatFromExt(path);
    if (!prefix.empty()) {
        AddEntity(prefix + ":" + path);
        const std::string label = assets::MeshFormatRegistry::Instance().DisplayName(prefix);
        NEON_LOG_INFO("Editor: imported %s '%s'", label.c_str(), path.c_str());
    } else if (IsImageExt(path)) {
        gfx::Texture tex = assetMgr_.LoadTexture(path);
        if (tex.Valid()) {
            previewTexture_ = tex;
            if (previewTexId_ != ImTextureID_Invalid) {
                gfx::ImGuiNeon_UnregisterTexture(previewTexture_.Handle());
            }
            previewTexId_ = gfx::ImGuiNeon_RegisterTexture(previewTexture_.Handle());
            NEON_LOG_INFO("Editor: preview texture '%s' (%dx%d)", path.c_str(), tex.Width(),
                          tex.Height());
        }
    } else if (IsScriptExt(path)) {
        // Double-clicking a .lua opens the built-in script editor.
        OpenScriptEditor(path);
    } else {
        NEON_LOG_INFO("Editor: '%s' (%llu bytes) - no import action",
                      path.c_str(), static_cast<unsigned long long>(0));
    }
}

// Deletes the selected asset immediately (recycle bin on Windows, so it is
// recoverable). A modal confirm popup proved unreliable in the docked panel,
// so deletion is direct and logged.
void EditorApp::DeleteSelectedAsset() {
    if (selectedAsset_ < 0 ||
        selectedAsset_ >= static_cast<int>(assetEntries_.size())) {
        NEON_LOG_WARN("Asset: delete ignored (selected=%d entries=%zu)",
                      selectedAsset_, assetEntries_.size());
        return;
    }
    const AssetEntry& e = assetEntries_[static_cast<size_t>(selectedAsset_)];
    NEON_LOG_INFO("Asset: deleting '%s'", e.path.c_str());
    if (DeletePathRecursive(e.path)) {
        NEON_LOG_INFO("Asset: deleted '%s'", e.path.c_str());
    } else {
        NEON_LOG_ERROR("Asset: failed to delete '%s'", e.path.c_str());
    }
    RefreshAssetDir();
}

void EditorApp::ImportSelectedAsset() {
    if (selectedAsset_ >= 0 && selectedAsset_ < static_cast<int>(assetEntries_.size()) &&
        !assetEntries_[static_cast<size_t>(selectedAsset_)].isDir) {
        ImportAssetPath(assetEntries_[static_cast<size_t>(selectedAsset_)].path);
    }
}


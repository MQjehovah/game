#include "editor.hpp"
#include "editor_history.hpp"
#include "editor_util.hpp"
#include "neon/assets/asset_path.hpp"
#include "neon/assets/mesh_format.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "neon/assets/asset_db.hpp"
#include "neon/gfx/scene_props.hpp"

namespace neon::editor {

namespace {
// 2D level layout (must match projects/pvz/assets/scripts/game.lua: 9x5 cells
// of 100 at (190,160)). LoadScene parses plant/zombie entities into these
// vectors so
// 2D level data stays scene-driven (the editor does not draw a canvas itself).
constexpr int kPvzRows = 5;
constexpr int kPvzCols = 9;
const char* kPvzPlantNames[5] = {"sunflower", "peashooter", "wallnut", "snowpea", "cherry"};
const char* kPvzZombieNames[3] = {"basic", "cone", "bucket"};

// P1-1 scene inheritance: parent entities first; a child entity with the same
// name replaces the parent's entry (keeping its position), new names append.
// gameVars / level: the child wins when present.
static core::Json MergeSceneJson(const core::Json& parent, const core::Json& child) {
    core::Json out = parent;
    if (const core::Json* gv = child.Get("gameVars")) out.object_["gameVars"] = *gv;
    if (const core::Json* lv = child.Get("level")) out.object_["level"] = *lv;
    std::vector<core::Json> merged;
    if (const core::Json* pents = parent.Get("entities")) {
        if (pents->IsArray())
            for (const core::Json& e : pents->Items()) merged.push_back(e);
    }
    if (const core::Json* cents = child.Get("entities")) {
        if (cents->IsArray()) {
            for (const core::Json& c : cents->Items()) {
                const std::string cname =
                    c.Get("name") ? c.Get("name")->GetString("") : std::string();
                bool replaced = false;
                for (core::Json& e : merged) {
                    const std::string ename =
                        e.Get("name") ? e.Get("name")->GetString("") : std::string();
                    // Same-name entities only override when the child differs
                    // (a full-copy child inherits identical entities from the
                    // parent, so parent edits propagate to the child).
                    if (!cname.empty() && ename == cname &&
                        core::JsonWriter::Write(c) != core::JsonWriter::Write(e)) {
                        e = c;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) merged.push_back(c);
            }
        }
    }
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    arr.array_ = std::move(merged);
    out.object_["entities"] = std::move(arr);
    return out;
}

} // namespace

void EditorApp::AddEntity(const std::string& meshKey) {
    static int counter = 1;
    math::Vec3 pos = camTarget_ + math::Vec3{0, 1.0f, -3.0f};
    std::string name;
    if (meshKey == "camera") {
        SceneEntity e;
        e.name = "相机" + std::to_string(counter++);
        e.nodeType = "Camera3D";
        e.pos = pos;
        e.cameraFov = 60.0f;
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        return;
    }
    if (meshKey == "light:directional") {
        SceneEntity e;
        e.name = "方向光" + std::to_string(counter++);
        e.nodeType = "Light3D";
        e.hasLight = true;
        e.light.type = "directional";
        e.pos = pos;
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        return;
    }
    if (meshKey == "light:point") {
        SceneEntity e;
        e.name = "点光源" + std::to_string(counter++);
        e.nodeType = "Light3D";
        e.hasLight = true;
        e.light.type = "point";
        e.light.color = {1.0f, 0.8f, 0.5f, 1.0f};
        e.light.radius = 10.0f;
        e.pos = pos;
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        return;
    }
    if (meshKey.rfind("prefab:", 0) == 0) {
        // Instantiate a project prefab (assets/prefabs/<name>.json): materialize its
        // component template into a new editable entity.
        const std::string pfName = meshKey.substr(7);
        auto tpl = prefabLib_.Get(pfName);
        if (!tpl.Ok()) {
            NEON_LOG_ERROR("Editor: prefab '%s' not found in '%s/prefabs'", pfName.c_str(),
                           projectDir_.c_str());
            return;
        }
        SceneEntity e = MaterializePrefabEntity(pfName, pos);
        e.name = pfName + std::to_string(counter++);
        if (ResolveMesh(e)) {
            ApplyMaterialParams(e);
            const size_t insertAt = entities_.size();
            history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
            SetSelection(static_cast<int>(entities_.size()) - 1);
        }
        return;
    }
    if (assets::MeshFormatRegistry::Instance().HasPrefix(meshKey)) {
        std::string path = meshKey.substr(meshKey.find(':') + 1);
        size_t slash = path.find_last_of("/\\");
        size_t dot = path.find_last_of('.');
        size_t begin = slash == std::string::npos ? 0 : slash + 1;
        size_t len = (dot == std::string::npos || dot < begin) ? std::string::npos : dot - begin;
        name = path.substr(begin, len) + std::to_string(counter++);
    } else {
        name = meshKey + std::to_string(counter++);
    }
    SceneEntity e;
    e.name = name;
    e.meshKey = meshKey;
    e.pos = pos;
    if (meshKey == "tree") {
        e.scale = {1.6f, 1.6f, 1.6f};
    }
    if (ResolveMesh(e)) {
        ApplyMaterialParams(e);
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
    }
}

SceneEntity EditorApp::MaterializePrefabEntity(const std::string& pfName, const math::Vec3& pos) {
    SceneEntity e;
    e.prefab = pfName;
    e.pos = pos;
    const core::Json* comps = nullptr;
    auto tpl = prefabLib_.Get(pfName);
    if (tpl.Ok() && tpl.Value()->IsObject()) comps = tpl.Value();
    if (!comps) return e;
    if (const core::Json* m = comps->Get("mesh")) {
        if (m->IsObject()) {
            e.meshKey = m->Get("meshKey") ? m->Get("meshKey")->GetString("cube") : "cube";
            if (const core::Json* c = m->Get("colorHex")) e.tint = ColorFromHex(c->GetString());
            if (const core::Json* v = m->Get("metallic")) e.metallic = static_cast<float>(v->GetNumber());
            if (const core::Json* v = m->Get("roughness")) e.roughness = static_cast<float>(v->GetNumber());
        }
    }
    if (const core::Json* h = comps->Get("health")) {
        if (h->IsObject()) {
            if (const core::Json* v = h->Get("hp")) e.hp = static_cast<float>(v->GetNumber());
            if (const core::Json* v = h->Get("maxHp")) e.maxHp = static_cast<float>(v->GetNumber());
        }
    }
    auto addScript = [&](const core::Json& s) {
        if (!s.IsObject()) return;
        SceneScriptFields f;
        f.path = s.Get("path") ? s.Get("path")->GetString() : "";
        f.backend = s.Get("backend") ? s.Get("backend")->GetString("lua") : "lua";
        if (const core::Json* v = s.Get("vars")) f.vars = *v;
        if (!f.path.empty()) e.scripts.push_back(std::move(f));
    };
    if (const core::Json* s = comps->Get("script")) addScript(*s);
    if (const core::Json* list = comps->Get("scripts")) {
        if (const core::Json* items = list->Get("items")) {
            if (items->IsArray()) {
                for (const core::Json& it : items->Items()) addScript(it);
            }
        }
    }
    for (const auto& [cname, cdata] : comps->Members()) {
        if (cname == "transform" || cname == "mesh" || cname == "health" || cname == "script")
            continue;
        e.extraComponents[cname] = cdata;
    }
    return e;
}

void EditorApp::AddSpriteEntity(const std::string& texPath) {
    static int counter = 1;
    const std::string rel = ToProjectRelPath(texPath, projectDir_);
    SceneEntity e;
    e.name = BaseName(rel) + std::to_string(counter++);
    e.spriteTex = rel;
    // Spawn at the camera target on the front-ortho plane (z = 0), a visible
    // default size; the gizmo/inspector can move and scale it from there.
    e.pos = {camTarget_.x, camTarget_.y, 0.0f};
    e.scale = {2.0f, 2.0f, 1.0f};
    if (ResolveMesh(e)) {
        ApplyMaterialParams(e);
        const size_t insertAt = entities_.size();
        history_.Push(std::make_unique<AddEntityCommand>(&entities_, e, insertAt));
        SetSelection(static_cast<int>(entities_.size()) - 1);
        NEON_LOG_INFO("Editor: sprite added '%s' (%s)", e.name.c_str(), e.spriteTex.c_str());
    }
}

bool EditorApp::ResolveMesh(SceneEntity& e) {
    const std::string& key = e.meshKey;
    if (key == "tilemap") {
        // 2D tilemap: cells draw as sprite quads in the render loop.
        e.mesh = {};
        return true;
    } else if (key == "terrain") {
        RebuildTerrainMesh(e);
        e.material = gfx::Material::Lit({}, e.tint, 4.0f);
    } else if (key == "helmet") {
        // Scene files store project-relative mesh paths; resolve against the
        // active project (the bundled helmet lives in the default project).
        assets::GltfAsset gltf = assetMgr_.LoadGLTF(
            projectDir_ + "/assets/models/DamagedHelmet/DamagedHelmet.gltf");
        if (!gltf.nodes.empty()) {
            e.mesh = gltf.nodes[0].mesh;
            e.material = gltf.nodes[0].material;
        }
    } else if (key == "cube") {
        e.mesh = gfx::Mesh::CreateCube(renderer_, 1, 1, 1, "cube");
        e.material = gfx::Material::Lit({}, e.tint, 12.0f);
    } else if (key == "tree") {
        e.mesh = gfx::MakeTreeMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    } else if (key == "plane") {
        // Ground / floor surface: a 20x20 quad, scaled by the entity transform
        // (wc3's Ground uses scale 130x100 to span the map).
        e.mesh = gfx::Mesh::CreatePlane(renderer_, 20.0f, 20.0f, 4, 4, "plane");
        e.material = gfx::Material::Lit({}, e.tint, 8.0f);
    } else if (key == "house") {
        e.mesh = gfx::MakeHouseMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    } else if (key == "npc" || key.compare(0, 4, "npc:") == 0) {
        // The entity tint selects the villager's tunic; head stays skin-tone.
        if (key.compare(0, 4, "npc:") == 0) {
            // "npc:r,g,b" encodes the tunic tint; decode it onto e.tint.
            int r = 128, g = 128, b = 128;
            std::sscanf(key.c_str() + 4, "%d,%d,%d", &r, &g, &b);
            e.tint = {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
        }
        e.mesh = gfx::MakeNPCMesh(renderer_, {e.tint.r, e.tint.g, e.tint.b, 1.0f});
        e.material = gfx::Material::Lit({}, gfx::Color::White, 12.0f);
    } else if (key == "hero") {
        e.mesh = gfx::MakeHeroMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 12.0f);
    } else if (key == "wolf") {
        e.mesh = gfx::MakeWolfMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    } else if (key == "bush") {
        e.mesh = gfx::MakeBushMesh(renderer_);
        e.material = gfx::Material::Lit({}, gfx::Color::White, 8.0f);
    } else if (key == "rock") {
        e.mesh = gfx::Mesh::CreateSphere(renderer_, 0.8f, 10, 7, "rock");
        e.material = gfx::Material::Lit({}, e.tint, 4.0f);
    } else if (key == "water") {
        e.mesh = gfx::Mesh::CreatePlane(renderer_, 20.0f, 20.0f, 8, 8, "water");
        e.material = gfx::Material::Lit({}, e.tint, 64.0f);
    } else if (key == "road") {
        e.mesh = gfx::Mesh::CreatePlane(renderer_, 1.0f, 1.0f, 1, 1, "road");
        e.material = gfx::Material::Lit({}, e.tint, 4.0f);
    } else if (key.rfind("obj:", 0) == 0 || key.rfind("fbx:", 0) == 0) {
        // OBJ / FBX route through the mesh-format registry (unified result).
        assets::MeshLoadResult res =
            assets::MeshFormatRegistry::Instance().Load(assetMgr_, key);
        e.mesh = res.mesh;
        e.material = res.material;
    } else if (key.rfind("gltf:", 0) == 0) {
        const std::string gltfPath = assets::NormalizeAssetPath(key.substr(5));
        // Cache the resolved model per path: the first entity pays the full
        // parse + upload, the rest clone the result (GPU handles shared, the
        // per-entity Animator state is a fresh copy).
        if (skinnedModelCache_.count(gltfPath) == 0 &&
            gltfStaticMeshCache_.count(gltfPath) == 0) {
            assets::GltfAsset gltf = assetMgr_.LoadGLTF(gltfPath);
            if (!gltf.nodes.empty()) {
                gltfStaticMeshCache_[gltfPath] = gltf.nodes[0].mesh;
                gltfStaticMaterialCache_[gltfPath] = gltf.nodes[0].material;
                if (gltf.nodes[0].mesh.Skinned()) {
                    core::Result<scene::SkinnedModel> sm =
                        scene::LoadSkinnedModel(assetMgr_, gltfPath);
                    if (sm.Ok()) {
                        skinnedModelCache_[gltfPath] =
                            std::make_shared<scene::SkinnedModel>(std::move(sm.Value()));
                    } else {
                        NEON_LOG_WARN("Editor: skinned model '%s' failed to resolve: %s",
                                      key.c_str(), sm.Error().c_str());
                    }
                }
            }
        }
        const auto meshIt = gltfStaticMeshCache_.find(gltfPath);
        if (meshIt != gltfStaticMeshCache_.end()) {
            e.mesh = meshIt->second;
            e.material = gltfStaticMaterialCache_[gltfPath];
            // glTF materials carry their own PBR params (factors + texture
            // slots); sync them into the flattened fields so ApplyMaterialParams
            // applies the asset's values instead of the editor defaults.
            e.metallic = e.material.metallic;
            e.roughness = e.material.roughness;
            e.ao = e.material.aoStrength;
            e.emissiveIntensity = e.material.emissiveIntensity;
            e.tint = e.material.tint;
            // Animated skinned glTF: clone the cached model (per-entity
            // Animator state, shared skeleton/clips/meshes).
            const auto smIt = skinnedModelCache_.find(gltfPath);
            if (e.mesh.Skinned() && smIt != skinnedModelCache_.end())
                e.skinned = std::make_shared<scene::SkinnedModel>(*smIt->second);
            else
                e.skinned.reset();
        }
    } else if (key.empty()) {
        if (!e.spriteTex.empty()) {
            // 2D sprite: image texture on an XY quad (facing the front-ortho
            // camera) rendered with an unlit material so colors are exactly
            // the texture's.
            // Sprite paths are stored project-relative ("assets/sprites/x.png"),
            // so resolve them against the project dir first (fall back to the
            // raw path for absolute paths and the repo-wide assets/ folder.
            // The default sandbox (projectDir_ == ".") can hold sprites dragged
            // in from any bundled project, so also probe every projects/*/.
            std::string texPath = e.spriteTex;
            const bool absolute = texPath.size() >= 2 && texPath[1] == ':' ||
                                  (!texPath.empty() &&
                                   (texPath[0] == '/' || texPath[0] == '\\'));
            if (!absolute) {
                auto exists = [](const std::string& f) {
                    std::ifstream probe(f, std::ios::binary);
                    return probe.is_open();
                };
                if (projectDir_ != "." && exists(projectDir_ + "/" + texPath)) {
                    texPath = projectDir_ + "/" + texPath;
                } else {
                    std::vector<AssetEntry> projDirs;
                    if (ListDirectory("projects", projDirs)) {
                        for (const AssetEntry& d : projDirs) {
                            if (!d.isDir) continue;
                            const std::string cand = d.path + "/" + texPath;
                            if (exists(cand)) {
                                texPath = cand;
                                break;
                            }
                        }
                    }
                }
            }
            gfx::Texture tex = assetMgr_.LoadTexture(texPath);
            if (!tex.Valid()) {
                NEON_LOG_ERROR("Editor: sprite texture '%s' failed to load", texPath.c_str());
                return false;
            }
            e.spriteMesh = gfx::Mesh::CreateQuad(renderer_, 1.0f, 1.0f, "sprite");
            // 2D sprites are lit so the scene's ambient/sun/lights affect them.
            e.spriteMaterial = gfx::Material::Lit(tex.Handle(), e.tint, 8.0f);
            e.spriteMaterial.transparent = true; // PNG sprites keep their alpha
        }
        // Script-only / logical entities (e.g. a 2D game's entry entity that
        // carries no mesh) are valid without geometry.
        return true;
    }
    return e.mesh.Valid();
}

void EditorApp::ApplyMaterialParams(SceneEntity& e) {
    if (!e.spriteTex.empty()) {
        // Sprite tint follows the entity color (unlit material, so the color
        // tints the texture exactly like a 2D sprite's modulate color).
        e.spriteMaterial.tint = e.tint;
        return;
    }
    // Custom fragment shader (P2-6) wins over the built-in lit/unlit shader.
    if (e.customShader.Valid()) {
        e.material.shader = e.customShader.Handle();
        e.material.lit = false;
    } else {
        e.material.shader = {};
    }
    // Props that bake colors into vertex data keep a white material tint (for
    // "npc" the entity tint already selected the tunic at mesh-build time).
    e.material.tint = IsBakedColorKey(e.meshKey) ? gfx::Color::White : e.tint;
    e.material.metallic = e.metallic;
    e.material.roughness = e.roughness;
    e.material.uvRepeat = e.uvRepeat;
    e.material.aoStrength = e.ao;
    e.material.emissiveIntensity = e.emissiveIntensity;
    // Texture slots: load any non-empty path through the cached AssetManager.
    // Empty paths leave the existing handle untouched (e.g. a glTF material's
    // baked PBR textures survive until the user explicitly overrides/clears).
    // Paths are stored project-relative ("assets/x.png"); resolve against the
    // project dir so the editor viewport finds them (CWD is the repo root).
    // Asset paths are normalized ("@assets/x" -> "assets/x") then resolved by
    // the AssetManager against the project root VFS (IoRead). No per-call
    // project-dir prefixing — the unified asset path model handles all forms.
    // Load a texture, using REPEAT wrap when the material tiles UVs (uvRepeat>1)
    // so a small texture repeats across the surface instead of stretching
    // (Clamp would smear the edge pixels over the whole UV>1 range).
    auto loadTex = [this](const std::string& p, float repeat) {
        assets::TextureLoadOptions o;
        o.wrap = repeat > 1.01f ? gfx::Wrap::Repeat : gfx::Wrap::Clamp;
        return assetMgr_.LoadTexture(assets::NormalizeAssetPath(p), o);
    };
    if (!e.albedoTex.empty())
        e.material.albedo = loadTex(e.albedoTex, e.uvRepeat).Handle();
    if (!e.mrTex.empty())
        e.material.metallicRoughness = loadTex(e.mrTex, e.uvRepeat).Handle();
    if (!e.aoTex.empty())
        e.material.occlusion = assetMgr_.LoadTexture(assets::NormalizeAssetPath(e.aoTex)).Handle();
    if (!e.emissiveTex.empty())
        e.material.emissive = assetMgr_.LoadTexture(assets::NormalizeAssetPath(e.emissiveTex)).Handle();
}

void EditorApp::RebuildTerrainMesh(SceneEntity& e) {
    const size_t need = static_cast<size_t>(e.terrainSegments_ + 1) *
                        (e.terrainSegments_ + 1);
    if (e.terrainHeights_.size() != need) {
        e.terrainHeights_.assign(need, 0.0f);
        // Match the runtime's default rolling terrain so a fresh 地面 matches
        // what the packed game shows before the user sculpts.
        const float half = e.terrainSize_ * 0.5f;
        const float cell = e.terrainSize_ / static_cast<float>(e.terrainSegments_);
        for (int row = 0; row <= e.terrainSegments_; ++row) {
            for (int col = 0; col <= e.terrainSegments_; ++col) {
                const float x = -half + col * cell;
                const float z = -half + row * cell;
                float h = std::sin(x * 0.11f) * std::cos(z * 0.13f) * 0.8f +
                          std::sin(x * 0.31f + z * 0.27f) * 0.35f;
                const float d = std::sqrt(x * x + z * z);
                h *= math::Saturate((d - 6.0f) / 10.0f);
                e.terrainHeights_[static_cast<size_t>(row) * (e.terrainSegments_ + 1) + col] = h;
            }
        }
    }
    e.mesh = gfx::Mesh::CreateTerrain(renderer_, e.terrainSegments_, e.terrainSize_,
                                      e.terrainHeights_, e.terrainHeightScale_, "terrain");
}

void EditorApp::PaintTerrain(const math::Ray& ray) {
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) return;
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    if (e.meshKey != "terrain") return;
    if (e.terrainHeights_.size() !=
        static_cast<size_t>(e.terrainSegments_ + 1) * (e.terrainSegments_ + 1))
        RebuildTerrainMesh(e);
    // Intersect the ray with the terrain's ground plane (y = e.pos.y).
    if (std::fabs(ray.dir.y) < 1e-6f) return;
    const float t = (e.pos.y - ray.origin.y) / ray.dir.y;
    if (t < 0.0f) return;
    const math::Vec3 hit = ray.origin + ray.dir * t;
    const float half = e.terrainSize_ * 0.5f;
    const float cell = e.terrainSize_ / static_cast<float>(e.terrainSegments_);
    const float localX = hit.x - e.pos.x;
    const float localZ = hit.z - e.pos.z;
    if (localX < -half || localX > half || localZ < -half || localZ > half) return;
    const int seg = e.terrainSegments_;
    const float radius = terrain_.brushRadius;
    const float r2 = radius * radius;
    const float delta = terrain_.brushStrength * (terrain_.raise ? 1.0f : -1.0f) / e.terrainHeightScale_;
    for (int row = 0; row <= seg; ++row) {
        for (int col = 0; col <= seg; ++col) {
            const float x = -half + col * cell - localX;
            const float z = -half + row * cell - localZ;
            const float d2 = x * x + z * z;
            if (d2 > r2) continue;
            const float falloff = 1.0f - d2 / r2;
            size_t idx = static_cast<size_t>(row) * (seg + 1) + col;
            e.terrainHeights_[idx] = math::Clamp(e.terrainHeights_[idx] + delta * falloff,
                                                 -10.0f, 10.0f);
        }
    }
    RebuildTerrainMesh(e);
    sceneDirty_ = true;
}

void EditorApp::ReloadEntityShader(SceneEntity& e) {
    if (e.shaderPath.empty()) {
        if (e.customShader.Valid() && renderer_.Backend()) {
            renderer_.Backend()->DestroyShader(e.customShader.Handle());
            e.customShader = {};
        }
        ApplyMaterialParams(e);
        return;
    }
    std::ifstream in(e.shaderPath, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_ERROR("Editor: cannot open shader '%s'", e.shaderPath.c_str());
        return;
    }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    gfx::Shader sh = renderer_.CreateUnlitFragmentShader(src, e.shaderPath);
    if (!sh.Valid()) {
        NEON_LOG_ERROR("Editor: shader compile failed for '%s'", e.shaderPath.c_str());
        return;
    }
    if (e.customShader.Valid() && renderer_.Backend())
        renderer_.Backend()->DestroyShader(e.customShader.Handle());
    e.customShader = sh;
    ApplyMaterialParams(e);
    NEON_LOG_INFO("Editor: shader '%s' compiled", e.shaderPath.c_str());
}

// G2-2 编辑器 ECS 化：用运行时 Instantiate 把当前场景装载进 sceneWorld_（与
// 播放器相同的组件表示）。entities_ 仍为 UI 读写模型；本函数使编辑器持有
// 规范 ecs::World，供后续阶段 UI 直读/直写组件。
void EditorApp::RefreshSceneWorld() {
    // G5-4: the World is the canonical runtime store, rebuilt from the editor's
    // working model (entities_) via the canonical builders + Instantiate. This
    // works for both the legacy flat scene format and the componentized one —
    // the editor flattens the file into entities_, then RefreshSceneWorld
    // canonicalizes them into the runtime World.
    SyncWorldFromEntities();
}

// G5-4 阶段4: rebuild entities_ FROM the runtime World's components (the
// reverse of SyncWorldFromEntities), proving the World can drive the editor's
// working model. DATA fields come from the World; mesh GPU handles resolve via
// ResolveMesh (sprites keep their texture path — the render path resolves the
// quad). Equivalent in data to LoadScene's JSON flattening.
void EditorApp::UnflattenWorldToEntities() {
    std::vector<SceneEntity> loaded;
    auto view = sceneWorld_.ViewAll<scene::SceneTransform>();
    for (size_t i = 0; i < view.Size(); ++i) {
        ecs::Entity e = sceneWorld_.EntityAt<scene::SceneTransform>(i);
        SceneEntity out;
        if (const scene::SceneId* id = sceneWorld_.Get<scene::SceneId>(e)) out.id = id->id;
        if (const scene::SceneName* n = sceneWorld_.Get<scene::SceneName>(e)) out.name = n->name;
        if (const scene::SceneTransform* t = sceneWorld_.Get<scene::SceneTransform>(e)) {
            out.pos = t->pos;
            out.rot = t->rot;
            out.scale = t->scale;
        }
        // G5-4: hierarchy is entity-level — derive the parent id from the
        // resolved SceneParentLink (the parent entity's stable SceneId).
        if (const scene::SceneParentLink* link = sceneWorld_.Get<scene::SceneParentLink>(e)) {
            if (const scene::SceneId* pid = sceneWorld_.Get<scene::SceneId>(link->parent))
                out.parentId = pid->id;
        }
        if (const scene::SceneMesh* m = sceneWorld_.Get<scene::SceneMesh>(e)) {
            out.meshKey = m->meshKey;
            out.metallic = m->metallic;
            out.roughness = m->roughness;
            out.albedoTex = m->albedoTex;
            out.mrTex = m->mrTex;
            out.aoTex = m->aoTex;
            out.emissiveTex = m->emissiveTex;
            out.ao = m->ao;
            out.emissiveIntensity = m->emissiveIntensity;
            if (!m->colorHex.empty()) out.tint = ColorFromHex(m->colorHex);
        }
        if (const scene::SceneSprite* s = sceneWorld_.Get<scene::SceneSprite>(e)) {
            out.spriteTex = s->texture;
            out.spriteFlipX = s->flipX;
            out.spriteFlipY = s->flipY;
            out.spriteFrames = s->frames;
            out.spriteFps = s->fps;
            out.spriteLoop = s->loop;
            out.spriteSheet = s->sheet;
            out.spriteSheetFrames = s->sheetFrames;
            if (!s->colorHex.empty()) out.tint = ColorFromHex(s->colorHex);
        }
        if (const scene::SceneHealth* h = sceneWorld_.Get<scene::SceneHealth>(e)) {
            out.hp = h->hp;
            out.maxHp = h->maxHp;
        }
        if (const scene::SceneNodeType* t = sceneWorld_.Get<scene::SceneNodeType>(e))
            out.nodeType = t->value;
        if (const scene::SceneCamera* c = sceneWorld_.Get<scene::SceneCamera>(e)) {
            out.cameraFov = c->fov;
            out.cameraOrtho = c->ortho;
            out.cameraOrthoSize = c->orthoSize;
        }
        if (const scene::SceneLight* l = sceneWorld_.Get<scene::SceneLight>(e)) {
            out.hasLight = true;
            out.light = *l;
        }
        if (const scene::SceneSortOrder* so = sceneWorld_.Get<scene::SceneSortOrder>(e))
            out.zOrder = so->z;
        if (const scene::SceneScripts* ss = sceneWorld_.Get<scene::SceneScripts>(e)) {
            for (const scene::SceneScript& sc : ss->items) {
                SceneScriptFields f;
                f.backend = sc.backend;
                f.path = sc.path;
                f.vars = sc.vars;
                out.scripts.push_back(std::move(f));
            }
        }
        if (const scene::SceneTerrain* t = sceneWorld_.Get<scene::SceneTerrain>(e)) {
            out.meshKey = "terrain";
            out.terrainSegments_ = t->segments;
            out.terrainSize_ = t->size;
            out.terrainHeightScale_ = t->heightScale;
            out.terrainHeights_ = t->heights;
            out.chunkGridDiv_ = t->chunkGridDiv;
            out.chunkLodLevels_ = t->chunkLodLevels;
            out.chunkBaseSubdiv_ = t->chunkBaseSubdiv;
            out.vegMeshKey_ = t->vegMeshKey;
            out.vegCount_ = t->vegCount;
            out.vegSeed_ = t->vegSeed;
            out.vegSize_ = t->vegSize;
            out.vegImpostorDistance_ = t->vegImpostorDistance;
            out.vegMinHeight_ = t->vegMinHeight;
            out.vegMaxHeight_ = t->vegMaxHeight;
            out.vegMaxSlope_ = t->vegMaxSlope;
        }
        if (const scene::SceneTilemap* t = sceneWorld_.Get<scene::SceneTilemap>(e)) {
            out.meshKey = "tilemap";
            out.tilemapCols_ = t->cols;
            out.tilemapRows_ = t->rows;
            out.tilemapCellSize_ = t->cellSize;
            out.tilemapTiles_ = t->tiles;
        }
        if (const scene::SceneDecal* d = sceneWorld_.Get<scene::SceneDecal>(e)) {
            out.decalTex = d->texture;
            out.decalSize = d->size;
            out.decalAlpha = d->alpha;
        }
        if (const scene::SceneData* sd = sceneWorld_.Get<scene::SceneData>(e)) {
            for (const auto& [cname, cdata] : sd->components) {
                if (cname == "prefab") {
                    if (cdata.IsString()) out.prefab = cdata.GetString();
                } else if (cname == "materialRef") {
                    if (cdata.IsString()) out.materialRef = cdata.GetString();
                } else {
                    out.extraComponents[cname] = cdata;
                }
            }
        }
        // Mesh GPU handles: re-resolve (sprites keep their texture path; the
        // render pass resolves the quad lazily).
        if (out.meshKey == "terrain" || out.meshKey == "tilemap") {
            ResolveMesh(out);
        } else if (!out.meshKey.empty()) {
            ResolveMesh(out);
        }
        if (!out.meshKey.empty()) ApplyMaterialParams(out);
        loaded.push_back(std::move(out));
    }
    entities_ = std::move(loaded);
    NormalizeEntityIds();
}

void EditorApp::SaveScene() {
    NormalizeEntityIds(); // stable ids before serialization
    // Serialize in the runtime componentized format (same as play/export)
    // so project scenes stay loadable by neon_game and no field is dropped
    // (health, materialRef, prefab, extraComponents, parentId, rotation...).
    auto rootRes = BuildPlaySceneJson();
    if (!rootRes.Ok()) {
        NEON_LOG_ERROR("Scene save aborted: %s", rootRes.Error().c_str());
        return;
    }
    core::Json root = rootRes.Value();
    // Preserve scene-level metadata from the loaded file (inheritance chain,
    // gameVars / level / title ...); entities were just rebuilt.
    if (!sceneExtends_.empty()) {
        core::Json ex;
        ex.type_ = core::Json::Type::String;
        ex.string_ = sceneExtends_;
        root.object_["extends"] = std::move(ex);
    }
    if (currentSceneRoot_.IsObject()) {
        for (const auto& [k, v] : currentSceneRoot_.Members()) {
            if (k == "entities" || k == "extends") continue;
            root.object_[k] = v;
        }
    }
    // Save to the scene file that is actually loaded (project scenes live in
    // <project>/assets/scenes/*.json). Previously this hardcoded
    // editor_scene.json,
    // so saving a project scene silently wrote the sandbox file and the
    // hierarchy (plus every other edit) was lost on restart.
    const std::string savePath = currentScenePath_.empty()
                                     ? std::string(kDefaultProjectDir) + "/" + kSandboxSceneRel
                                     : currentScenePath_;
    if (std::ofstream out(savePath); out.is_open()) {
        out << core::JsonWriter::WritePretty(root);
        sceneDirty_ = false;
        NEON_LOG_INFO("Scene saved (%zu entities) -> %s", entities_.size(),
                      savePath.c_str());
    } else {
        NEON_LOG_ERROR("Scene save failed: cannot write '%s'", savePath.c_str());
    }
}

void EditorApp::SaveSceneAsChild() {
    if (currentScenePath_.empty()) {
        NEON_LOG_WARN("Editor: 另存为子场景 needs a loaded scene file");
        return;
    }
    // D6: base the child on the CURRENT edited state (currentSceneRoot_), not
    // a stale re-read of the file -- the old code silently discarded every
    // unsaved edit when "另存为子场景" was used.
    core::Json root = currentSceneRoot_;
    if (root.IsNull() || !root.IsObject()) {
        NEON_LOG_ERROR("Editor: cannot save child scene (no current scene state)");
        return;
    }
    // A child scene overrides the parent; drop any inherited parent link so it
    // points only at the CURRENT file.
    root.object_.erase("extends");
    const size_t slash = currentScenePath_.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "" : currentScenePath_.substr(0, slash + 1);
    const std::string base =
        slash == std::string::npos ? currentScenePath_ : currentScenePath_.substr(slash + 1);
    const size_t dot = base.rfind('.');
    const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    core::Json ex;
    ex.type_ = core::Json::Type::String;
    ex.string_ = currentScenePath_;
    root.object_["extends"] = ex;
    const std::string childPath = dir + stem + "_child.json";
    if (std::ofstream out(childPath, std::ios::binary); out.is_open()) {
        out << core::JsonWriter::WritePretty(root);
        NEON_LOG_INFO("Editor: child scene saved -> %s (extends %s)", childPath.c_str(),
                      currentScenePath_.c_str());
        LoadScene(childPath);
    }
}

void EditorApp::NormalizeEntityIds() {
    int maxId = 0;
    for (const SceneEntity& e : entities_)
        if (e.id > maxId) maxId = e.id;
    std::set<int> used;
    for (SceneEntity& e : entities_) {
        // id 0 (unassigned, e.g. entities added mid-session) and DUPLICATE ids
        // (e.g. the duplicate command copies the source id) both get a fresh
        // unique id: without this, the id-based tree + drag cycle guards
        // misbehave (a duplicate id triggers the self-parent rejection).
        if (e.id == 0 || used.count(e.id) != 0) e.id = ++maxId;
        used.insert(e.id);
    }
    // Orphaned parent references: a parentId that points at an entity which no
    // longer exists (its parent was deleted) would hide the whole subtree from
    // the hierarchy tree and make every drag fail. Re-parent orphans to root.
    std::set<int> live;
    for (const SceneEntity& e : entities_) live.insert(e.id);
    for (SceneEntity& e : entities_)
        if (e.parentId != 0 && live.count(e.parentId) == 0) e.parentId = 0;
}

void EditorApp::SortSceneTreeByName() {
    if (entities_.size() < 2) return;
    NormalizeEntityIds(); // parentId references must resolve for the DFS
    auto lower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    std::vector<size_t> order;
    order.reserve(entities_.size());
    std::set<int> placed;
    std::function<void(int)> visit = [&](int parentId) {
        std::vector<size_t> kids;
        for (size_t i = 0; i < entities_.size(); ++i)
            if (entities_[i].parentId == parentId) kids.push_back(i);
        std::stable_sort(kids.begin(), kids.end(), [&](size_t a, size_t b) {
            return lower(entities_[a].name) < lower(entities_[b].name);
        });
        for (size_t k : kids) {
            if (placed.count(static_cast<int>(k)) != 0) continue;
            placed.insert(static_cast<int>(k));
            order.push_back(k);
            visit(entities_[k].id);
        }
    };
    visit(0);
    for (size_t i = 0; i < entities_.size(); ++i)
        if (placed.count(static_cast<int>(i)) == 0) {
            placed.insert(static_cast<int>(i));
            order.push_back(i);
        }
    bool changed = false;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] != i) {
            changed = true;
            break;
        }
    if (!changed) return;
    history_.Push(std::make_unique<SortSceneTreeCommand>(&entities_, std::move(order)));
}

void EditorApp::LoadScene(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        // D6: a missing/unreadable scene is worth a visible log line -- the
        // old silent return left the editor looking empty with no clue why.
        NEON_LOG_ERROR("Editor: cannot open scene file '%s'", path.c_str());
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    core::Json root = core::Json::Parse(ss.str(), &err);
    if (root.IsNull() || !err.empty()) {
        // D6: a corrupt scene must not silently "load nothing".
        NEON_LOG_ERROR("Editor: scene '%s' failed to parse: %s", path.c_str(),
                       err.empty() ? "empty document" : err.c_str());
        return;
    }
    // P1-1 scene inheritance: resolve "extends" chains by loading the parent
    // file(s) and overlaying same-named entities (child wins, new names
    // append). The parent path is kept so SaveScene writes it back.
    sceneExtends_.clear();
    if (const core::Json* ex = root.Get("extends")) {
        if (ex->IsString() && !ex->GetString().empty()) {
            const std::string parentPath = ex->GetString();
            sceneExtends_ = parentPath;
            std::ifstream pin(parentPath);
            if (pin.is_open()) {
                std::stringstream pss;
                pss << pin.rdbuf();
                core::Json parent = core::Json::Parse(pss.str(), &err);
                if (parent.IsObject() && parent.Get("entities")) {
                    // Recursively resolve the parent's own inheritance first.
                    std::ifstream prein(parentPath);
                    (void)prein;
                    if (const core::Json* pex = parent.Get("extends")) {
                        if (pex->IsString() && !pex->GetString().empty()) {
                            std::ifstream pin2(pex->GetString());
                            if (pin2.is_open()) {
                                std::stringstream pss2;
                                pss2 << pin2.rdbuf();
                                core::Json grand = core::Json::Parse(pss2.str(), &err);
                                if (grand.IsObject() && grand.Get("entities")) {
                                    parent = MergeSceneJson(grand, parent);
                                    parent.object_.erase("extends");
                                }
                            }
                        }
                    }
                    parent.object_.erase("extends");
                    root = MergeSceneJson(parent, root);
                }
            }
            root.object_.erase("extends");
        }
    }
    const core::Json* arr = root.Get("entities");
    if (!arr) return;
    // Keep the parsed scene root + path: 2D levels live inside the scene as
    // plant/zombie ENTITIES in the scene file, so scenes are the single
    // source of truth for both 3D and 2D.
    currentSceneRoot_ = root;
    currentScenePath_ = path;
    pvzPlants_.clear();
    pvzZombies_.clear();
    // Replace entity list, re-resolve meshes.
    std::vector<SceneEntity> loaded;
    std::vector<std::string> legacyParents; // old name-based parents, resolved below
    // Destroy custom shader handles from the previous scene (P2-6).
    if (renderer_.Backend()) {
        for (SceneEntity& old : entities_) {
            if (old.customShader.Valid())
                renderer_.Backend()->DestroyShader(old.customShader.Handle());
        }
    }
    bool has2DData = false; // any plant/zombie entity -> a 2D level scene
    // Support both the editor's flat format and the runtime's componentized
    // SceneFile format ("components": {transform/mesh/health/script}) so a
    // data-driven project scene (e.g. projects/neon_realm) opens directly.
    const bool componentized =
        arr->Size() > 0 && arr->At(0) != nullptr && arr->At(0)->Get("components") != nullptr;
    for (size_t i = 0; i < arr->Size(); ++i) {
        const core::Json* j = arr->At(i);
        if (!j) continue;
        SceneEntity e;
        if (const core::Json* id = j->Get("id")) e.id = id->GetInt(0);
        e.name = j->Get("name")->GetString("entity");
        std::string legacyParent;
        if (componentized) {
            if (const core::Json* pf = j->Get("prefab")) e.prefab = pf->GetString();
            // Effective components = prefab template merged with instance
            // overrides (instance fields win), mirroring the runtime.
            core::Json effective;
            effective.type_ = core::Json::Type::Object;
            if (!e.prefab.empty() && prefabLib_.Has(e.prefab)) {
                auto tpl = prefabLib_.Get(e.prefab);
                if (tpl.Ok()) {
                    // PrefabLibrary stores the component map directly.
                    const core::Json* tc = tpl.Value();
                    if (tc && tc->IsObject()) effective = *tc;
                }
            }
            if (const core::Json* inst = j->Get("components")) {
                if (inst->IsObject()) {
                    // G5-4-4(项1): deep merge instance overrides over the
                    // template (field-level), mirroring the runtime's
                    // Instantiate — so a prefab instance stores only its diff.
                    effective = scene::MergePrefabOverrides(effective, *inst);
                }
            }
            const core::Json* comps = &effective;
            if (const core::Json* t = comps->Get("transform")) {
                if (const core::Json* p = t->Get("pos"))
                    e.pos = {static_cast<float>(p->At(0)->GetNumber()),
                             static_cast<float>(p->At(1)->GetNumber()),
                             static_cast<float>(p->At(2)->GetNumber())};
                if (const core::Json* r = t->Get("rot"))
                    e.rot = {static_cast<float>(r->At(0)->GetNumber()),
                             static_cast<float>(r->At(1)->GetNumber()),
                             static_cast<float>(r->At(2)->GetNumber()),
                             static_cast<float>(r->At(3)->GetNumber())};
                if (const core::Json* s = t->Get("scale"))
                    e.scale = {static_cast<float>(s->At(0)->GetNumber()),
                               static_cast<float>(s->At(1)->GetNumber()),
                               static_cast<float>(s->At(2)->GetNumber())};
                // G5-4: hierarchy is entity-level — top-level parentId/parent
                // win; the legacy transform placement is the fallback.
                if (const core::Json* p = j->Get("parentId")) e.parentId = p->GetInt(0);
                if (const core::Json* p = j->Get("parent")) legacyParent = p->GetString();
                if (e.parentId == 0 && legacyParent.empty()) {
                    if (const core::Json* p = t->Get("parentId")) e.parentId = p->GetInt(0);
                    if (const core::Json* p = t->Get("parent")) legacyParent = p->GetString();
                }
            }
            if (const core::Json* m = comps->Get("mesh")) {
                e.meshKey = m->Get("meshKey") ? m->Get("meshKey")->GetString("cube") : "cube";
                if (const core::Json* mr = m->Get("materialRef"))
                    e.materialRef = mr->GetString();
                // Material params live in the nested "material" object
                // (mesh.material.{colorHex,metallic,roughness,ao,albedoTex,...}).
                const core::Json* mat = m->Get("material");
                auto matVal = [&](const char* k) -> const core::Json* {
                    if (mat) if (const core::Json* v = mat->Get(k)) return v;
                    return m->Get(k); // fall back to a mesh-level field if present
                };
                if (const core::Json* c = matVal("colorHex")) e.tint = ColorFromHex(c->GetString());
                if (const core::Json* v = matVal("metallic")) e.metallic = static_cast<float>(v->GetNumber());
                if (const core::Json* v = matVal("roughness")) e.roughness = static_cast<float>(v->GetNumber());
                if (const core::Json* v = matVal("uvRepeat")) e.uvRepeat = static_cast<float>(v->GetNumber(1.0f));
                if (const core::Json* v = matVal("ao")) e.ao = static_cast<float>(v->GetNumber());
                if (const core::Json* v = matVal("emissiveIntensity")) e.emissiveIntensity = static_cast<float>(v->GetNumber());
                if (const core::Json* v = matVal("albedoTex")) e.albedoTex = assets::NormalizeAssetPath(v->GetString());
                if (const core::Json* v = matVal("mrTex")) e.mrTex = assets::NormalizeAssetPath(v->GetString());
                if (const core::Json* v = matVal("aoTex")) e.aoTex = assets::NormalizeAssetPath(v->GetString());
                if (const core::Json* v = matVal("emissiveTex")) e.emissiveTex = assets::NormalizeAssetPath(v->GetString());
            }
            if (const core::Json* sp = comps->Get("sprite")) {
                e.spriteTex = sp->Get("texture") ? sp->Get("texture")->GetString() : "";
                if (const core::Json* fx = sp->Get("flipX")) e.spriteFlipX = fx->GetBool();
                if (const core::Json* fy = sp->Get("flipY")) e.spriteFlipY = fy->GetBool();
                if (const core::Json* c = sp->Get("colorHex")) e.tint = ColorFromHex(c->GetString());
                if (const core::Json* fr = sp->Get("frames")) {
                    if (fr->IsArray()) {
                        e.spriteFrames.clear();
                        for (size_t i = 0; i < fr->Size(); ++i) {
                            const core::Json* item = fr->At(i);
                            if (item && item->IsString()) e.spriteFrames.push_back(item->GetString());
                        }
                    }
                    if (const core::Json* fps = sp->Get("fps")) e.spriteFps = static_cast<float>(fps->GetNumber());
                    if (const core::Json* lp = sp->Get("loop")) e.spriteLoop = lp->GetBool();
                }
                if (const core::Json* sh = sp->Get("sheet")) {
                    e.spriteSheet = sh->GetString();
                    if (const core::Json* n = sp->Get("sheetFrames")) e.spriteSheetFrames = n->GetInt();
                    if (const core::Json* fps = sp->Get("fps")) e.spriteFps = static_cast<float>(fps->GetNumber());
                    if (const core::Json* lp = sp->Get("loop")) e.spriteLoop = lp->GetBool();
                }
            }
            if (const core::Json* h = comps->Get("health")) {
                if (const core::Json* v = h->Get("hp")) e.hp = static_cast<float>(v->GetNumber());
                if (const core::Json* v = h->Get("maxHp")) e.maxHp = static_cast<float>(v->GetNumber());
            }
            if (const core::Json* nt = comps->Get("type")) {
                e.nodeType =
                    nt->Get("value") ? nt->Get("value")->GetString() : nt->GetString();
            }
            if (const core::Json* cam = comps->Get("camera")) {
                if (const core::Json* v = cam->Get("fov"))
                    e.cameraFov = static_cast<float>(v->GetNumber());
                if (const core::Json* v = cam->Get("ortho")) e.cameraOrtho = v->GetBool();
                if (const core::Json* v = cam->Get("orthoSize"))
                    e.cameraOrthoSize = static_cast<float>(v->GetNumber());
                if (const core::Json* v = cam->Get("aspect"))
                    e.cameraAspect = static_cast<float>(v->GetNumber());
                if (e.nodeType.empty()) e.nodeType = "Camera3D";
            }
            if (const core::Json* li = comps->Get("light")) {
                e.hasLight = true;
                if (const core::Json* v = li->Get("type")) e.light.type = v->GetString();
                if (const core::Json* v = li->Get("sunDir")) {
                    float vv[3] = {0.0f, 0.0f, 0.0f};
                    size_t n = 0;
                    for (const core::Json& x : v->Items())
                        if (n < 3) vv[n++] = static_cast<float>(x.GetNumber());
                    e.light.sunDir = {vv[0], vv[1], vv[2]};
                }
                if (const core::Json* v = li->Get("color")) {
                    float vv[4] = {1, 1, 1, 1};
                    size_t n = 0;
                    for (const core::Json& x : v->Items())
                        if (n < 4) vv[n++] = static_cast<float>(x.GetNumber());
                    e.light.color = {vv[0], vv[1], vv[2], vv[3]};
                }
                if (const core::Json* v = li->Get("radius"))
                    e.light.radius = static_cast<float>(v->GetNumber());
                if (const core::Json* v = li->Get("intensity"))
                    e.light.intensity = static_cast<float>(v->GetNumber());
                if (const core::Json* v = li->Get("ambientStrength"))
                    e.light.ambientStrength = static_cast<float>(v->GetNumber());
                if (const core::Json* v = li->Get("skyTexture"))
                    e.light.skyTexture = v->GetString();
                if (e.nodeType.empty()) e.nodeType = "Light3D";
            }
            if (const core::Json* so = comps->Get("sortOrder")) {
                if (const core::Json* z = so->Get("z"))
                    e.zOrder = static_cast<float>(z->GetNumber());
            }
            if (const core::Json* te = comps->Get("terrain")) {
                if (const core::Json* seg = te->Get("segments"))
                    e.terrainSegments_ = seg->GetInt(48);
                if (const core::Json* sz = te->Get("size"))
                    e.terrainSize_ = static_cast<float>(sz->GetNumber());
                if (const core::Json* hscale = te->Get("heightScale"))
                    e.terrainHeightScale_ = static_cast<float>(hscale->GetNumber());
                if (const core::Json* h = te->Get("heights")) {
                    if (h->IsArray())
                        for (const core::Json& v : h->Items())
                            e.terrainHeights_.push_back(static_cast<float>(v.GetNumber()));
                }
                // G2-3 chunked LOD + vegetation knobs (round-trip).
                if (const core::Json* v = te->Get("chunkGridDiv")) e.chunkGridDiv_ = v->GetInt(0);
                if (const core::Json* v = te->Get("chunkLodLevels")) e.chunkLodLevels_ = v->GetInt(3);
                if (const core::Json* v = te->Get("chunkBaseSubdiv")) e.chunkBaseSubdiv_ = v->GetInt(16);
                if (const core::Json* v = te->Get("vegMeshKey")) e.vegMeshKey_ = v->GetString();
                if (const core::Json* v = te->Get("vegCount")) e.vegCount_ = static_cast<uint32_t>(v->GetNumber());
                if (const core::Json* v = te->Get("vegSeed")) e.vegSeed_ = static_cast<uint32_t>(v->GetNumber());
                if (const core::Json* v = te->Get("vegSize")) e.vegSize_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = te->Get("vegImpostorDistance")) e.vegImpostorDistance_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = te->Get("vegMinHeight")) e.vegMinHeight_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = te->Get("vegMaxHeight")) e.vegMaxHeight_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = te->Get("vegMaxSlope")) e.vegMaxSlope_ = static_cast<float>(v->GetNumber());
            }
            if (const core::Json* tlm = comps->Get("tilemap")) {
                if (const core::Json* cols = tlm->Get("cols"))
                    e.tilemapCols_ = cols->GetInt(8);
                if (const core::Json* rows = tlm->Get("rows"))
                    e.tilemapRows_ = rows->GetInt(5);
                if (const core::Json* cs = tlm->Get("cellSize"))
                    e.tilemapCellSize_ = static_cast<float>(cs->GetNumber());
                if (const core::Json* tls = tlm->Get("tiles")) {
                    if (tls->IsArray())
                        for (const core::Json& v : tls->Items())
                            e.tilemapTiles_.push_back(v.GetString());
                }
            }
            if (const core::Json* dc = comps->Get("decal")) {
                if (const core::Json* tex = dc->Get("texture"))
                    e.decalTex = tex->GetString();
                if (const core::Json* sz = dc->Get("size"))
                    e.decalSize = static_cast<float>(sz->GetNumber());
                if (const core::Json* al = dc->Get("alpha"))
                    e.decalAlpha = static_cast<float>(al->GetNumber());
            }
            if (const core::Json* sh = comps->Get("shader")) {
                if (const core::Json* p = sh->Get("path")) e.shaderPath = p->GetString();
            }
            if (const core::Json* s = comps->Get("script")) {
                // Legacy single "script" component: one mounted script.
                if (s->IsObject()) {
                    SceneScriptFields f;
                    f.path = s->Get("path") ? s->Get("path")->GetString() : "";
                    f.backend = s->Get("backend") ? s->Get("backend")->GetString("lua") : "lua";
                    if (const core::Json* v = s->Get("vars")) f.vars = *v;
                    if (!f.path.empty()) e.scripts.push_back(std::move(f));
                }
            }
            if (const core::Json* list = comps->Get("scripts")) {
                if (const core::Json* items = list->Get("items")) {
                    if (items->IsArray()) {
                        for (const core::Json& it : items->Items()) {
                            SceneScriptFields f;
                            f.backend = it.Get("backend") ? it.Get("backend")->GetString("lua")
                                                          : "lua";
                            f.path = it.Get("path") ? it.Get("path")->GetString() : "";
                            if (const core::Json* v = it.Get("vars")) f.vars = *v;
                            if (!f.path.empty()) e.scripts.push_back(std::move(f));
                        }
                    }
                }
            }
            // Keep every non-flattened component as editable extra data
            // (schema-driven inspector; plant/zombie mirror the 2D canvas).
            for (const auto& [cname, cdata] : comps->Members()) {
                if (cname == "transform" || cname == "mesh" || cname == "health" ||
                    cname == "script" || cname == "sprite" || cname == "shader")
                    continue;
                e.extraComponents[cname] = cdata;
            }
            if (const core::Json* pl = comps->Get("plant")) {
                if (pl->IsObject()) {
                    const int row = pl->Get("row") ? pl->Get("row")->GetInt(-1) : -1;
                    const int col = pl->Get("col") ? pl->Get("col")->GetInt(-1) : -1;
                    const std::string name =
                        pl->Get("type") ? pl->Get("type")->GetString("sunflower") : "sunflower";
                    int type = -1;
                    for (int t = 0; t < 5; ++t)
                        if (name == kPvzPlantNames[t]) type = t;
                    if (row >= 0 && row < kPvzRows && col >= 0 && col < kPvzCols && type >= 0) {
                        pvzPlants_.push_back({row, col, type});
                        has2DData = true;
                    }
                }
            }
            if (const core::Json* zb = comps->Get("zombie")) {
                if (zb->IsObject()) {
                    const int row = zb->Get("row") ? zb->Get("row")->GetInt(-1) : -1;
                    const float delay =
                        zb->Get("delay")
                            ? static_cast<float>(zb->Get("delay")->GetNumber())
                            : 8.0f;
                    const std::string name =
                        zb->Get("type") ? zb->Get("type")->GetString("basic") : "basic";
                    int type = 0;
                    for (int t = 0; t < 3; ++t)
                        if (name == kPvzZombieNames[t]) type = t;
                    if (row >= 0 && row < kPvzRows) {
                        pvzZombies_.push_back({row, delay, type});
                        has2DData = true;
                    }
                }
            }
        } else {
            if (const core::Json* p = j->Get("parentId")) e.parentId = p->GetInt(0);
            if (const core::Json* p = j->Get("parent")) legacyParent = p->GetString();
            if (const core::Json* nt = j->Get("nodeType")) e.nodeType = nt->GetString();
            if (const core::Json* cf = j->Get("cameraFov"))
                e.cameraFov = static_cast<float>(cf->GetNumber());
            if (const core::Json* co = j->Get("cameraOrtho"))
                e.cameraOrtho = co->GetBool() || co->GetNumber() != 0;
            if (const core::Json* sp = j->Get("shaderPath")) e.shaderPath = sp->GetString();
            if (const core::Json* mj = j->Get("mesh"))
                e.meshKey = mj->GetString("cube");
            else if (e.nodeType.empty())
                e.meshKey = "cube";
            if (const core::Json* zo = j->Get("zOrder"))
                e.zOrder = static_cast<float>(zo->GetNumber());
            if (const core::Json* td = j->Get("terrainData")) {
                if (const core::Json* seg = td->Get("segments"))
                    e.terrainSegments_ = seg->GetInt(48);
                if (const core::Json* sz = td->Get("size"))
                    e.terrainSize_ = static_cast<float>(sz->GetNumber());
                if (const core::Json* hscale = td->Get("heightScale"))
                    e.terrainHeightScale_ = static_cast<float>(hscale->GetNumber());
                if (const core::Json* h = td->Get("heights")) {
                    if (h->IsArray())
                        for (const core::Json& v : h->Items())
                            e.terrainHeights_.push_back(static_cast<float>(v.GetNumber()));
                }
                // G2-3 chunked LOD + vegetation knobs (round-trip).
                if (const core::Json* v = td->Get("chunkGridDiv")) e.chunkGridDiv_ = v->GetInt(0);
                if (const core::Json* v = td->Get("chunkLodLevels")) e.chunkLodLevels_ = v->GetInt(3);
                if (const core::Json* v = td->Get("chunkBaseSubdiv")) e.chunkBaseSubdiv_ = v->GetInt(16);
                if (const core::Json* v = td->Get("vegMeshKey")) e.vegMeshKey_ = v->GetString();
                if (const core::Json* v = td->Get("vegCount")) e.vegCount_ = static_cast<uint32_t>(v->GetNumber());
                if (const core::Json* v = td->Get("vegSeed")) e.vegSeed_ = static_cast<uint32_t>(v->GetNumber());
                if (const core::Json* v = td->Get("vegSize")) e.vegSize_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = td->Get("vegImpostorDistance")) e.vegImpostorDistance_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = td->Get("vegMinHeight")) e.vegMinHeight_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = td->Get("vegMaxHeight")) e.vegMaxHeight_ = static_cast<float>(v->GetNumber());
                if (const core::Json* v = td->Get("vegMaxSlope")) e.vegMaxSlope_ = static_cast<float>(v->GetNumber());
            }
            if (const core::Json* tlm = j->Get("tilemapData")) {
                if (const core::Json* cols = tlm->Get("cols"))
                    e.tilemapCols_ = cols->GetInt(8);
                if (const core::Json* rows = tlm->Get("rows"))
                    e.tilemapRows_ = rows->GetInt(5);
                if (const core::Json* cs = tlm->Get("cellSize"))
                    e.tilemapCellSize_ = static_cast<float>(cs->GetNumber());
                if (const core::Json* tls = tlm->Get("tiles")) {
                    if (tls->IsArray())
                        for (const core::Json& v : tls->Items())
                            e.tilemapTiles_.push_back(v.GetString());
                }
            }
            if (const core::Json* dc = j->Get("decalData")) {
                if (const core::Json* tex = dc->Get("texture"))
                    e.decalTex = tex->GetString();
                if (const core::Json* sz = dc->Get("size"))
                    e.decalSize = static_cast<float>(sz->GetNumber());
                if (const core::Json* al = dc->Get("alpha"))
                    e.decalAlpha = static_cast<float>(al->GetNumber());
            }
            if (const core::Json* st = j->Get("spriteTex")) e.spriteTex = st->GetString();
            if (const core::Json* fx = j->Get("spriteFlipX")) e.spriteFlipX = fx->GetInt(0) != 0;
            if (const core::Json* fy = j->Get("spriteFlipY")) e.spriteFlipY = fy->GetInt(0) != 0;
            if (const core::Json* p = j->Get("pos")) {
                e.pos = {static_cast<float>(p->At(0)->GetNumber()),
                         static_cast<float>(p->At(1)->GetNumber()),
                         static_cast<float>(p->At(2)->GetNumber())};
            }
            if (const core::Json* s = j->Get("scale")) {
                e.scale = {static_cast<float>(s->At(0)->GetNumber()),
                           static_cast<float>(s->At(1)->GetNumber()),
                           static_cast<float>(s->At(2)->GetNumber())};
            }
            if (const core::Json* t = j->Get("tint")) {
                e.tint = {static_cast<float>(t->At(0)->GetNumber()),
                          static_cast<float>(t->At(1)->GetNumber()),
                          static_cast<float>(t->At(2)->GetNumber()), 1.0f};
            }
            if (const core::Json* m = j->Get("metallic")) e.metallic = static_cast<float>(m->GetNumber());
            if (const core::Json* r = j->Get("roughness")) e.roughness = static_cast<float>(r->GetNumber());
            if (const core::Json* a = j->Get("ao")) e.ao = static_cast<float>(a->GetNumber());
            if (const core::Json* ei = j->Get("emissiveIntensity")) e.emissiveIntensity = static_cast<float>(ei->GetNumber());
            if (const core::Json* at = j->Get("albedoTex")) e.albedoTex = assets::NormalizeAssetPath(at->GetString());
            if (const core::Json* mt = j->Get("mrTex")) e.mrTex = assets::NormalizeAssetPath(mt->GetString());
            if (const core::Json* aot = j->Get("aoTex")) e.aoTex = assets::NormalizeAssetPath(aot->GetString());
            if (const core::Json* et = j->Get("emissiveTex")) e.emissiveTex = assets::NormalizeAssetPath(et->GetString());
            // Flat editor-scene format: a "scripts" array (new) or the legacy
            // scriptPath/scriptBackend/scriptVars keys (old saves).
            if (const core::Json* list = j->Get("scripts")) {
                if (list->IsArray()) {
                    for (const core::Json& it : list->Items()) {
                        SceneScriptFields f;
                        f.backend =
                            it.Get("backend") ? it.Get("backend")->GetString("lua") : "lua";
                        f.path = it.Get("path") ? it.Get("path")->GetString() : "";
                        if (const core::Json* v = it.Get("vars")) f.vars = *v;
                        if (!f.path.empty()) e.scripts.push_back(std::move(f));
                    }
                }
            } else if (const core::Json* sp = j->Get("scriptPath")) {
                SceneScriptFields f;
                f.path = sp->GetString();
                if (const core::Json* sb = j->Get("scriptBackend"))
                    f.backend = sb->GetString();
                if (const core::Json* sv = j->Get("scriptVars")) f.vars = *sv;
                if (!f.path.empty()) e.scripts.push_back(std::move(f));
            }
        }
        if (!e.materialRef.empty()) {
            // Material-ball reference ("assets/materials/x.mat.json"): expand it into
            // the flattened fields before resolving the mesh.
            LoadMaterialParamsInto(e, projectDir_ + "/" + e.materialRef);
        }
        if (ResolveMesh(e)) {
            if (!e.shaderPath.empty()) ReloadEntityShader(e);
            ApplyMaterialParams(e);
            loaded.push_back(std::move(e));
            legacyParents.push_back(std::move(legacyParent));
        }
    }
    if (has2DData) {
        editMode_ = EditMode::Scene2D;
        viewCam_ = ViewCam::Front; // 2D canvas view is the front-ortho camera
        // 2D scenes live in the 1280x720 design space: frame that space so the
        // editor shows exactly what the game sees (same content as play).
        camTarget_ = {640.0f, 360.0f, 0.0f};
        orthoSize_ = 360.0f;
        cameraUserAdjusted_ = false;
        NEON_LOG_INFO("Scene 2D level loaded (%zu plants, %zu zombie spawns)",
                      pvzPlants_.size(), pvzZombies_.size());
    }
    if (!loaded.empty() || has2DData) {
        entities_ = std::move(loaded);
        // G1-3: stable ids (0 -> sequential) so the scene tree can reference
        // parents by id; then migrate legacy name-based parents to ids.
        NormalizeEntityIds();
        {
            std::map<std::string, int> firstIdByName;
            for (const SceneEntity& e : entities_)
                if (firstIdByName.count(e.name) == 0) firstIdByName[e.name] = e.id;
            for (size_t i = 0; i < entities_.size() && i < legacyParents.size(); ++i) {
                if (entities_[i].parentId != 0 || legacyParents[i].empty()) continue;
                const auto it = firstIdByName.find(legacyParents[i]);
                if (it != firstIdByName.end()) entities_[i].parentId = it->second;
            }
        }
        SetSelection(-1);
        history_.Clear(); // undo history from the previous scene is invalid
        currentSceneName_ = BaseName(path);
        NEON_LOG_INFO("Scene loaded (%zu entities)", entities_.size());
        EnsureSceneDefaultObjects();
        // G2-2: hold the scene's canonical runtime representation (ecs::World)
        // as a live mirror of the loaded file — the same Instantiate the player
        // runs. The UI still reads entities_; the World is the ECS-ization base.
        RefreshSceneWorld();
    }
}

void EditorApp::EnsureSceneDefaultObjects() {
    // Unity default scene: every scene keeps a Main Camera + a Directional
    // Light object so nothing is observer-less. Old scenes that lack them get
    // them added on load (and the scene is marked dirty so a save persists it).
    bool hasCam = false, hasLight = false;
    for (const SceneEntity& e : entities_) {
        if (e.nodeType == "Camera3D") hasCam = true;
        if (e.hasLight) hasLight = true;
    }
    bool added = false;
    if (!hasCam) {
    SceneEntity e;
    e.name = "Main Camera";
    e.nodeType = "Camera3D";
    e.pos = {0.0f, 3.0f, 10.0f};
    e.cameraFov = 60.0f;
    // Detect 2D by the scene content (sprites) as well as the project mode:
    // projectMode_ may not be set yet when the defaults are injected.
    bool is2D = projectMode_ == "2d" || editMode_ == EditMode::Scene2D;
    if (!is2D) {
        for (const SceneEntity& se : entities_) {
            if (!se.spriteTex.empty()) { is2D = true; break; }
        }
    }
    if (is2D) {
        // 2D: a locked orthographic camera framing the 1280x720 design space.
        e.cameraOrtho = true;
        e.cameraOrthoSize = 360.0f;
        e.pos = {640.0f, 360.0f, 100.0f}; // behind the content plane, at the design centre
        }
        entities_.push_back(std::move(e));
        added = true;
    }
    if (!hasLight) {
        SceneEntity e;
        e.name = "Directional Light";
        e.nodeType = "Light3D";
        e.hasLight = true;
        e.light.type = "directional";
        entities_.push_back(std::move(e));
        added = true;
    }
    if (added) {
        NormalizeEntityIds();
        sceneDirty_ = true;
    }
}

bool EditorApp::ReadProjectMeta(EditorProject& p) {
    std::ifstream in(p.dir + "/game.json", std::ios::binary);
    if (!in.is_open()) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    core::Json root = core::Json::Parse(text, &err);
    const size_t slash = p.dir.find_last_of("/\\");
    p.name = slash == std::string::npos ? p.dir : p.dir.substr(slash + 1);
    if (root.IsObject()) {
        if (const core::Json* t = root.Get("title"))
            if (t->IsString() && !t->GetString().empty()) p.name = t->GetString();
        if (const core::Json* s = root.Get("startScene"))
            if (s->IsString()) p.startScene = s->GetString();
        if (const core::Json* ed = root.Get("editor"))
            if (const core::Json* m = ed->Get("mode"))
                if (m->IsString() && m->GetString() == "2d") p.mode = "2d";
    }
    std::vector<AssetEntry> files;
    if (ListDirectory(p.dir + "/assets/scenes", files)) {
        for (const AssetEntry& f : files) {
            if (f.isDir) continue;
            const std::string& n = f.name;
            const bool isJson = n.size() > 5 &&
                                (n.compare(n.size() - 5, 5, ".json") == 0 ||
                                 n.compare(n.size() - 5, 5, ".JSON") == 0);
            if (isJson) p.scenes.push_back("assets/scenes/" + n);
        }
    }
    std::sort(p.scenes.begin(), p.scenes.end());
    return true;
}

void EditorApp::ScanProjects() {
    projects_.clear();
    std::vector<AssetEntry> dirs;
    if (ListDirectory("projects", dirs)) {
        for (const AssetEntry& d : dirs) {
            if (!d.isDir) continue;
            EditorProject p;
            p.dir = d.path;
            if (ReadProjectMeta(p)) projects_.push_back(std::move(p));
        }
    }
    std::sort(projects_.begin(), projects_.end(),
              [](const EditorProject& a, const EditorProject& b) {
                  std::string al = a.name, bl = b.name;
                  std::transform(al.begin(), al.end(), al.begin(),
                                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                  std::transform(bl.begin(), bl.end(), bl.begin(),
                                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                  return al < bl;
              });
    // Re-sync the active project fields (projectSel_/name/mode/scenes).
    projectSel_ = -1;
    for (size_t i = 0; i < projects_.size(); ++i) {
        if (projects_[i].dir != projectDir_) continue;
        projectSel_ = static_cast<int>(i);
        projectName_ = projects_[i].name;
        projectMode_ = projects_[i].mode;
        projectStartScene_ = projects_[i].startScene;
        projectScenes_ = projects_[i].scenes;
        return;
    }
    // projectDir_ is not under projects/: a custom path (or the default
    // sandbox). Read its game.json directly when present.
    projectName_.clear();
    projectMode_ = "3d";
    projectStartScene_.clear();
    projectScenes_.clear();
    EditorProject custom;
    custom.dir = projectDir_;
    if (ReadProjectMeta(custom)) {
        projectName_ = custom.name;
        projectMode_ = custom.mode;
        projectStartScene_ = custom.startScene;
        projectScenes_ = custom.scenes;
    }
}

void EditorApp::LoadPrefabLibrary() {
    prefabLib_ = scene::PrefabLibrary();
    projectPrefabs_.clear();
    std::vector<AssetEntry> files;
    if (!ListDirectory(projectDir_ + "/assets/prefabs", files)) return;
    for (const AssetEntry& f : files) {
        if (f.isDir) continue;
        const std::string& n = f.name;
        const bool isJson =
            n.size() > 5 && (n.compare(n.size() - 5, 5, ".json") == 0 ||
                             n.compare(n.size() - 5, 5, ".JSON") == 0);
        if (!isJson) continue;
        std::ifstream in(f.path, std::ios::binary);
        if (!in.is_open()) continue;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string name = BaseName(f.path);
        const size_t dot = name.find_last_of('.');
        const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
        projectPrefabs_.push_back(stem);
        core::Status st = prefabLib_.Add(stem, text);
        if (!st.Ok())
            NEON_LOG_WARN("Editor: prefab '%s' failed to parse: %s", f.path.c_str(),
                          st.Error().c_str());
    }
    std::sort(projectPrefabs_.begin(), projectPrefabs_.end());
    NEON_LOG_INFO("Editor: prefab library loaded (%zu prefabs)", prefabLib_.Size());
}

void EditorApp::NormalizeEntityAssetPaths() {
    // Convert any path that lives under the project root into the portable
    // project-relative "assets/..." form. A path that is NOT under the project
    // (e.g. a bare "assets/..." reference, an external path, or an already
    // relative one) is left unchanged — do NOT prepend CWD, which would turn a
    // correct "assets/x.png" into a bogus absolute "E:\game\assets\x.png".
    const std::string projPrefix =
        projectDir_.empty() ? std::string() : projectDir_ + "/";
    auto toRel = [this, &projPrefix](const std::string& p) {
        if (p.empty()) return p;
        std::string abs = p;
        const bool isAbs = p.size() >= 2 && p[1] == ':' ||
                           (!p.empty() && (p[0] == '/' || p[0] == '\\'));
        if (!isAbs) abs = GetWorkingDir() + "/" + p;
        if (!projPrefix.empty() && abs.size() >= projPrefix.size() &&
            abs.rfind(projPrefix, 0) == 0) {
            // Under the project root: emit the portable "@assets/..." virtual
            // form. "@assets/" already denotes the project's assets/ root, so
            // strip a leading "assets/" from the remainder to avoid "@assets/
            // assets/..." duplication.
            std::string rel = abs.substr(projPrefix.size());
            std::string rl = rel;
            for (char& c : rl) c = (c == '\\' ? '/' : c);
            if (rl.rfind("assets/", 0) == 0) rel = rel.substr(7);
            return "@assets/" + rel;
        }
        // Not under the project root: keep the reference verbatim (it may be a
        // bare "assets/..." or an external/absolute path).
        return p;
    };
    for (SceneEntity& e : entities_) {
        // File-backed mesh keys carry a path after "<format>:" (obj/gltf/fbx).
        if (std::string fp = assets::MeshFormatRegistry::Instance().MatchPrefix(e.meshKey, nullptr);
            !fp.empty() && e.meshKey.find(':') != std::string::npos) {
            const size_t colon = e.meshKey.find(':');
            e.meshKey = e.meshKey.substr(0, colon + 1) + toRel(e.meshKey.substr(colon + 1));
        }
        auto norm = [&toRel](std::string& p) { if (!p.empty()) p = toRel(p); };
        norm(e.albedoTex);
        norm(e.mrTex);
        norm(e.aoTex);
        norm(e.emissiveTex);
        norm(e.decalTex);
        norm(e.spriteTex);
    }
}

void EditorApp::SavePrefab(const std::string& name) {
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) return;
    const SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    if (name.empty()) {
        NEON_LOG_WARN("Editor: prefab name is empty");
        return;
    }
    if (e.meshKey.empty()) {
        NEON_LOG_WARN("Editor: entity has no mesh; cannot save as prefab");
        return;
    }
    auto res = scene::SceneFile::MakeEntity(e.name, e.pos, e.rot, e.scale,
                                            ExportMeshKey(e.meshKey), e.metallic, e.roughness,
                                            e.tint, e.albedoTex, e.mrTex, e.aoTex,
                                            e.emissiveTex, e.ao, e.emissiveIntensity,
                                            "", "", core::Json{}, {},
                                            e.hp, e.maxHp);
    if (!res.Ok()) {
        NEON_LOG_ERROR("Editor: cannot save prefab: %s", res.Error().c_str());
        return;
    }
    core::Json root;
    root.type_ = core::Json::Type::Object;
    core::Json comps;
    if (const core::Json* c = res.Value().Get("components")) {
        if (c->IsObject()) comps = *c;
    }
    comps.type_ = core::Json::Type::Object;
    for (const auto& [cname, cdata] : e.extraComponents) comps.object_[cname] = cdata;
    if (!e.scripts.empty()) {
        auto mkStr = [](const std::string& v) {
            core::Json j;
            j.type_ = core::Json::Type::String;
            j.string_ = v;
            return j;
        };
        core::Json items;
        items.type_ = core::Json::Type::Array;
        for (const SceneScriptFields& f : e.scripts) {
            if (f.path.empty()) continue; // unconfigured script block
            core::Json it;
            it.type_ = core::Json::Type::Object;
            it.object_["backend"] = mkStr(f.backend.empty() ? "lua" : f.backend);
            it.object_["path"] = mkStr(f.path);
            if (f.vars.IsObject()) it.object_["vars"] = f.vars;
            items.array_.push_back(std::move(it));
        }
        core::Json scripts;
        scripts.type_ = core::Json::Type::Object;
        scripts.object_["items"] = std::move(items);
        comps.object_["scripts"] = std::move(scripts);
    }
    root.object_["components"] = std::move(comps);

    const std::string dir = projectDir_ + "/assets/prefabs";
    EnsureDirs(dir + "/");
    const std::string path = dir + "/" + name + ".json";
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Editor: cannot write prefab '%s'", path.c_str());
        return;
    }
    {
        out << core::JsonWriter::WritePretty(root);
        out.close(); // flush before the library reload below reads the file
    }
    LoadPrefabLibrary();
    NEON_LOG_INFO("Editor: prefab saved -> %s", path.c_str());
}

bool EditorApp::LoadMaterialParamsInto(SceneEntity& e, const std::string& path) {
    std::string text;
#if defined(_WIN32)
    // Open with the wide API: std::ifstream cannot read UTF-8 CJK filenames
    // (realm's material balls are Chinese-named), which silently broke the
    // grid-view preview for those assets.
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.data(),
                                         static_cast<int>(path.size()), nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wlen > 0 ? wlen : 0), L'\0');
    if (wlen > 0)
        MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                            &wpath[0], wlen);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        text.resize(static_cast<size_t>(sz));
        if (std::fread(&text[0], 1, static_cast<size_t>(sz), f) !=
            static_cast<size_t>(sz)) {
            std::fclose(f);
            return false;
        }
    }
    std::fclose(f);
#else
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
#endif
    std::string err;
    core::Json root = core::Json::Parse(text, &err);
    if (!root.IsObject()) return false;
    if (const core::Json* c = root.Get("colorHex"))
        e.tint = ColorFromHex(c->GetString("#FFFFFF"));
    if (const core::Json* v = root.Get("metallic"))
        e.metallic = static_cast<float>(v->GetNumber());
    if (const core::Json* v = root.Get("roughness"))
        e.roughness = static_cast<float>(v->GetNumber());
    if (const core::Json* v = root.Get("ao")) e.ao = static_cast<float>(v->GetNumber());
    if (const core::Json* v = root.Get("emissiveIntensity"))
        e.emissiveIntensity = static_cast<float>(v->GetNumber());
    if (const core::Json* v = root.Get("albedoTex")) e.albedoTex = v->GetString();
    if (const core::Json* v = root.Get("mrTex")) e.mrTex = v->GetString();
    if (const core::Json* v = root.Get("aoTex")) e.aoTex = v->GetString();
    if (const core::Json* v = root.Get("emissiveTex")) e.emissiveTex = v->GetString();
    return true;
}

void EditorApp::SaveMaterialAsset(const std::string& name) {
    if (name.empty() || selected_ < 0 ||
        selected_ >= static_cast<int>(entities_.size())) {
        NEON_LOG_WARN("Editor: material asset name/selection invalid");
        return;
    }
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    if (e.meshKey.empty()) {
        NEON_LOG_WARN("Editor: entity has no mesh; cannot save a material ball");
        return;
    }
    auto str = [](const std::string& s) {
        core::Json j;
        j.type_ = core::Json::Type::String;
        j.string_ = s;
        return j;
    };
    auto num = [](double v) {
        core::Json j;
        j.type_ = core::Json::Type::Number;
        j.number_ = v;
        return j;
    };
    core::Json root;
    root.type_ = core::Json::Type::Object;
    root.object_["colorHex"] = str(ColorToHex(e.tint));
    root.object_["metallic"] = num(e.metallic);
    root.object_["roughness"] = num(e.roughness);
    root.object_["ao"] = num(e.ao);
    root.object_["emissiveIntensity"] = num(e.emissiveIntensity);
    root.object_["albedoTex"] = str(e.albedoTex);
    root.object_["mrTex"] = str(e.mrTex);
    root.object_["aoTex"] = str(e.aoTex);
    root.object_["emissiveTex"] = str(e.emissiveTex);

    const std::string dir = projectDir_ + "/assets/materials";
    EnsureDirs(dir + "/");
    const std::string rel = "assets/materials/" + name + ".mat.json";
    const std::string path = projectDir_ + "/" + rel;
    // Wide-char open so CJK material names write correctly.
    if (!WriteFileUtf8(path, core::JsonWriter::WritePretty(root))) {
        NEON_LOG_ERROR("Editor: cannot write material asset '%s'", path.c_str());
        return;
    }

    const MaterialAssetValue oldVal{e.materialRef, ColorToHex(e.tint), e.metallic, e.roughness,
                                    e.ao, e.emissiveIntensity, e.albedoTex, e.mrTex, e.aoTex,
                                    e.emissiveTex};
    const MaterialAssetValue newVal{rel, ColorToHex(e.tint), e.metallic, e.roughness, e.ao,
                                    e.emissiveIntensity, e.albedoTex, e.mrTex, e.aoTex,
                                    e.emissiveTex};
    history_.Push(std::make_unique<EditPropertyCommand<MaterialAssetValue>>(
        &entities_, selected_, ApplyMaterialAssetProp, oldVal, newVal));
    if (assetDir_ == dir) RefreshAssetDir();
    NEON_LOG_INFO("Editor: material ball saved -> %s", path.c_str());
}

void EditorApp::ApplyMaterialAsset(const std::string& path) {
    if (selected_ < 0 || selected_ >= static_cast<int>(entities_.size())) return;
    SceneEntity& e = entities_[static_cast<size_t>(selected_)];
    SceneEntity tmp = e;
    if (!LoadMaterialParamsInto(tmp, path)) {
        NEON_LOG_ERROR("Editor: cannot load material asset '%s'", path.c_str());
        return;
    }
    const MaterialAssetValue oldVal{e.materialRef, ColorToHex(e.tint), e.metallic, e.roughness,
                                    e.ao, e.emissiveIntensity, e.albedoTex, e.mrTex, e.aoTex,
                                    e.emissiveTex};
    // Store the reference project-relative ("assets/materials/x.mat.json") so
    // scenes round-trip regardless of where the asset panel is browsing.
    std::string rel = path;
    const std::string base = projectDir_ == "." ? "" : projectDir_ + "/";
    if (!base.empty() && rel.compare(0, base.size(), base) == 0)
        rel = rel.substr(base.size());
    const MaterialAssetValue newVal{rel, ColorToHex(tmp.tint), tmp.metallic, tmp.roughness,
                                    tmp.ao, tmp.emissiveIntensity, tmp.albedoTex, tmp.mrTex,
                                    tmp.aoTex, tmp.emissiveTex};
    history_.Push(std::make_unique<EditPropertyCommand<MaterialAssetValue>>(
        &entities_, selected_, ApplyMaterialAssetProp, oldVal, newVal));
    ApplyMaterialParams(entities_[static_cast<size_t>(selected_)]);
    NEON_LOG_INFO("Editor: material asset '%s' applied", path.c_str());
}

void EditorApp::MountAssetVfs() {
    // Mount a project-root virtual file system so asset references can use the
    // "@assets/..." scheme and resolve against the current project dir. The
    // AssetManager reads through it (IoRead/IoMTime), falling back to a direct
    // CWD read for absolute/legacy paths during the gradual cutover.
    const std::string root = projectDir_.empty() ? "." : projectDir_;
    assetVfs_ = std::make_unique<neon::io::DiskFileSystem>(root);
    assetMgr_.SetFileSystem(assetVfs_.get());
}

void EditorApp::SwitchProject(const std::string& dir) {
    StopPlay();
    // Store the project dir as an ABSOLUTE path so asset/path resolution
    // (ResolveMeshAssetPath, FullAssetPath, ToProjectRelPath) never depends on
    // the current working directory — a relative "projects/wc3" would silently
    // break after a restart launched from a different CWD.
    std::string abs = dir.empty() ? "." : dir;
    if (abs != ".") {
        const bool isAbs = abs.size() >= 2 && abs[1] == ':' ||
                           (!abs.empty() && (abs[0] == '/' || abs[0] == '\\'));
        if (!isAbs) abs = GetWorkingDir() + "/" + abs;
    }
    projectDir_ = abs;
    std::strncpy(projectDirBuf_, projectDir_.c_str(), sizeof(projectDirBuf_) - 1);
    projectDirBuf_[sizeof(projectDirBuf_) - 1] = '\0';
    MountAssetVfs();
    ScanProjects();
    LoadPrefabLibrary();
    // G5-4-4(项3): detect renamed/moved assets (GUID preserved) and rewrite
    // path references so scenes never break silently.
    RefreshAssetDatabase();
    history_.Clear();
    SetSelection(-1);
    if (projectMode_ == "2d") {
        // 2D projects are scenes too: LoadScene reads scenes/<start>.json and
        // its plant/zombie entities switch the editor to the 2D canvas
        // automatically. No separate assets/levels/ data path.
        editMode_ = EditMode::Scene2D;
        viewCam_ = ViewCam::Front;
        // Frame the 1280x720 design space (2D content uses design coords).
        camTarget_ = {640.0f, 360.0f, 0.0f};
        orthoSize_ = 360.0f;
        cameraUserAdjusted_ = false;
    } else {
        editMode_ = EditMode::Scene3D;
        viewCam_ = ViewCam::Perspective;
    }
    if (!projectStartScene_.empty()) {
        LoadScene(projectDir_ + "/" + projectStartScene_);
    } else if (projectMode_ != "2d") {
        // 3D project without a startScene: fall back to the sandbox scene.
        LoadScene(std::string(projectDir_) + "/" + kSandboxSceneRel);
    }
    // The asset panel always points at the active context's assets/ dir and
    // creates it on demand (so 导入/新建 always have a home). The default
    // sandbox (no project open, projectDir_ == ".") is the repo root, whose
    // assets/ dir is that context's project assets.
    const std::string assetsDir = projectDir_ + "/assets";
    MakeDir(assetsDir);
    assetDir_ = assetsDir;
    RefreshAssetDir();
    SaveEditorConfig();
    NEON_LOG_INFO("Editor: switched project '%s' (mode=%s, %zu scenes)",
                  projectName_.c_str(), projectMode_.c_str(), projectScenes_.size());
}

void EditorApp::RefreshAssetDatabase() {
    const std::string snapshotPath = projectDir_ + "/.asset_db.json";

    std::string prevText;
    if (std::ifstream in(snapshotPath, std::ios::binary); in.is_open()) {
        prevText.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const assets::AssetDatabase prev = assets::AssetDatabase::FromJson(prevText);

    // The database IS the identity store (no sidecar files): unchanged files
    // keep their entry, a new path whose content hash matches a vanished one
    // inherits its GUID (a move), and legacy <asset>.meta sidecars from older
    // revisions are adopted once, then deleted.
    std::vector<std::string> adoptedMetas;
    const assets::AssetDatabase current = assets::AssetDatabase::Build(projectDir_, prev,
                                                                       &adoptedMetas);
    for (const std::string& meta : adoptedMetas) {
        std::error_code rmEc;
        std::filesystem::remove(meta, rmEc);
    }

    const std::vector<assets::AssetMove> moves = assets::DetectAssetMoves(prev, current);
    if (!moves.empty()) {
        NEON_LOG_INFO("Editor: %zu asset(s) moved/renamed — rewriting references",
                      moves.size());
        for (const assets::AssetMove& m : moves)
            NEON_LOG_INFO("  %s -> %s", m.oldPath.c_str(), m.newPath.c_str());
        // Rewrite path references in scene / prefab / UI JSON documents. The
        // runtime resolves PATHS, so rewriting the stored text keeps everything
        // consistent (the content hash in .asset_db.json made the rename
        // detectable).
        std::error_code ec;
        for (const char* sub : {"assets/scenes", "assets/prefabs", "assets/ui"}) {
            const std::string dir = projectDir_ + "/" + sub;
            if (!std::filesystem::exists(dir, ec)) continue;
            for (std::filesystem::recursive_directory_iterator it(dir, ec), end;
                 it != end && !ec; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string p = it->path().string();
                if (p.size() < 5 || p.compare(p.size() - 5, 5, ".json") != 0) continue;
                std::ifstream rf(p, std::ios::binary);
                if (!rf.is_open()) continue;
                std::string text((std::istreambuf_iterator<char>(rf)),
                                 std::istreambuf_iterator<char>());
                const std::string out = assets::RewriteJsonReferences(text, moves);
                if (out != text) {
                    std::ofstream w(p, std::ios::binary);
                    if (w.is_open()) {
                        w << out;
                        NEON_LOG_INFO("  rewritten '%s'", p.c_str());
                    }
                }
            }
        }
    }

    if (std::ofstream out(snapshotPath, std::ios::binary); out.is_open()) {
        out << current.ToJson();
    }
}

void EditorApp::LoadProjectScene() {
    SwitchProject(projectDir_);
}

void EditorApp::LoadProjectScene(const std::string& rel) {
    StopPlay();
    SetSelection(-1);
    history_.Clear();
    LoadScene(projectDir_ + "/" + rel);
    NEON_LOG_INFO("Editor: project scene loaded from '%s/%s'", projectDir_.c_str(), rel.c_str());
}

void EditorApp::OpenScriptEditor(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_ERROR("Editor: cannot open script '%s'", path.c_str());
        return;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    scriptEditor_.edit.SetText(text);
    scriptEditor_.path = path;
    scriptEditor_.rel = path;
    const std::string base = projectDir_.empty() ? "." : projectDir_;
    if (path.compare(0, base.size(), base) == 0 && path.size() > base.size() &&
        (path[base.size()] == '/' || path[base.size()] == '\\'))
        scriptEditor_.rel = path.substr(base.size() + 1);
    scriptEditor_.check = ScriptCheckResult{};
    script::IScriptHost* checkHost = ScriptCheckHostFor(path);
    if (checkHost) {
        if (scriptEditor_.rel != path) {
            scriptEditor_.check = CheckScriptFile(*checkHost, base, scriptEditor_.rel);
        } else {
            std::ifstream src(path, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(src)),
                             std::istreambuf_iterator<char>());
            scriptEditor_.check.path = path;
            scriptEditor_.check.ok = checkHost->CheckSyntax(text);
            if (!scriptEditor_.check.ok) {
                scriptEditor_.check.message = checkHost->LastError().message;
                scriptEditor_.check.line = checkHost->LastError().line;
            }
        }
    }
    scriptEditor_.dirty = false;
    showScriptEditor_ = true;
    NEON_LOG_INFO("Editor: script editor opened '%s'", path.c_str());
}

void EditorApp::SaveScriptEditor() {
    if (scriptEditor_.path.empty()) return;
    std::ofstream out(scriptEditor_.path, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("Editor: cannot write script '%s'", scriptEditor_.path.c_str());
        return;
    }
    out << scriptEditor_.edit.GetText();
    scriptEditor_.dirty = false;
    const std::string base = projectDir_.empty() ? "." : projectDir_;
    script::IScriptHost* checkHost = ScriptCheckHostFor(scriptEditor_.path);
    if (checkHost) {
        if (scriptEditor_.rel != scriptEditor_.path) {
            scriptEditor_.check = CheckScriptFile(*checkHost, base, scriptEditor_.rel);
        } else {
            scriptEditor_.check.path = scriptEditor_.path;
            scriptEditor_.check.ok = checkHost->CheckSyntax(scriptEditor_.edit.GetText());
            if (!scriptEditor_.check.ok) {
                scriptEditor_.check.message = checkHost->LastError().message;
                scriptEditor_.check.line = checkHost->LastError().line;
            }
        }
    }
    NEON_LOG_INFO("Editor: script saved '%s'", scriptEditor_.path.c_str());
}

void EditorApp::OpenInExternalEditor(const std::string& path) {
    if (path.empty()) return;
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const std::string cmd = std::string("xdg-open \"") + path + "\"";
    std::system(cmd.c_str());
#endif
}

void EditorApp::ClampSelection() {
    // Undo/redo can move entities under an unchanged selection index (e.g. a
    // reorder), so invalidate the script panel's index-keyed sync cache
    // unconditionally here, not only when the index changes. Also keeps the
    // multi-selection set inside the entity list bounds.
    const int n = static_cast<int>(entities_.size());
    for (auto it = selection_.begin(); it != selection_.end();) {
        if (*it >= n)
            it = selection_.erase(it);
        else
            ++it;
    }
    if (selected_ >= n) selected_ = selection_.empty() ? -1 : *selection_.rbegin();
    if (!selection_.empty() && selection_.count(selected_) == 0)
        selected_ = *selection_.rbegin();
    if (entities_.empty()) {
        selected_ = -1;
        selection_.clear();
        selectionAnchor_ = -1;
    }
}

} // namespace neon::editor

#include "neon/scene/scene_file.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"

namespace neon::scene {
namespace {

// Validates that a component's JSON data is an object with no unknown fields.
// Non-object data fails (defense-in-depth: input paths also enforce objects).
bool CheckComponentShape(const core::Json& data, const std::vector<std::string>& allowed,
                         const std::string& comp, std::string* err) {
    if (!data.IsObject()) {
        if (err) *err = "component '" + comp + "' must be a JSON object";
        return false;
    }
    for (const auto& [key, val] : data.Members()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            if (err) *err = "component '" + comp + "' has unknown field '" + key + "'";
            return false;
        }
    }
    return true;
}

bool RequireNumber(const core::Json& data, const char* key, const std::string& comp,
                   float& out, std::string* err) {
    const core::Json* node = data.Get(key);
    if (!node) return true;
    if (!node->IsNumber()) {
        if (err) *err = "component '" + comp + "' field '" + key + "' must be a number";
        return false;
    }
    out = static_cast<float>(node->GetNumber());
    return true;
}

bool RequireString(const core::Json& data, const char* key, const std::string& comp,
                   std::string& out, std::string* err) {
    const core::Json* node = data.Get(key);
    if (!node) return true;
    if (!node->IsString()) {
        if (err) *err = "component '" + comp + "' field '" + key + "' must be a string";
        return false;
    }
    out = node->GetString();
    return true;
}

bool ReadVec3(const core::Json& data, const char* key, const std::string& comp,
              math::Vec3& out, std::string* err) {
    const core::Json* node = data.Get(key);
    if (!node) return true;
    if (!node->IsArray() || node->Size() != 3 || !node->At(0)->IsNumber() ||
        !node->At(1)->IsNumber() || !node->At(2)->IsNumber()) {
        if (err) *err = "component '" + comp + "' field '" + key + "' must be an array of 3 numbers";
        return false;
    }
    out = {static_cast<float>(node->At(0)->GetNumber()),
           static_cast<float>(node->At(1)->GetNumber()),
           static_cast<float>(node->At(2)->GetNumber())};
    return true;
}

bool ReadQuat(const core::Json& data, const char* key, const std::string& comp,
              math::Quat& out, std::string* err) {
    const core::Json* node = data.Get(key);
    if (!node) return true;
    if (!node->IsArray() || node->Size() != 4 || !node->At(0)->IsNumber() ||
        !node->At(1)->IsNumber() || !node->At(2)->IsNumber() || !node->At(3)->IsNumber()) {
        if (err) *err = "component '" + comp + "' field '" + key + "' must be an array of 4 numbers";
        return false;
    }
    out = {static_cast<float>(node->At(0)->GetNumber()),
           static_cast<float>(node->At(1)->GetNumber()),
           static_cast<float>(node->At(2)->GetNumber()),
           static_cast<float>(node->At(3)->GetNumber())};
    return true;
}

// Instance fields override prefab fields; recursion for nested objects; arrays
// are replaced wholesale by the instance value.
void DeepMerge(core::Json& base, const core::Json& over) {
    if (base.IsObject() && over.IsObject()) {
        for (const auto& [key, val] : over.Members()) {
            auto it = base.object_.find(key);
            if (it != base.object_.end() && it->second.IsObject() && val.IsObject()) {
                DeepMerge(it->second, val);
            } else {
                base.object_[key] = val;
            }
        }
    } else {
        base = over;
    }
}

core::Json MakeObject() {
    core::Json j;
    j.type_ = core::Json::Type::Object;
    return j;
}

core::Json MakeString(const std::string& s) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = s;
    return j;
}

core::Json MakeNumber(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}

core::Json MakeVec3(const math::Vec3& v) {
    core::Json j;
    j.type_ = core::Json::Type::Array;
    j.array_ = {MakeNumber(v.x), MakeNumber(v.y), MakeNumber(v.z)};
    return j;
}

core::Json MakeQuat(const math::Quat& q) {
    core::Json j;
    j.type_ = core::Json::Type::Array;
    j.array_ = {MakeNumber(q.x), MakeNumber(q.y), MakeNumber(q.z), MakeNumber(q.w)};
    return j;
}

// Format a float color as the "#RRGGBB" string the mesh factory reads into
// SceneMesh::colorHex. Matches ImGui's float→byte conversion (v * 255 + 0.5).
std::string MakeColorHex(const gfx::Color& c) {
    auto byte = [](float v) {
        int i = static_cast<int>(v * 255.0f + 0.5f);
        if (i < 0) i = 0;
        if (i > 255) i = 255;
        return i;
    };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b));
    return std::string(buf);
}

} // namespace

// --- SceneFile ---------------------------------------------------------------

core::Result<SceneFile> SceneFile::Parse(const std::string& jsonText) {
    std::string perr;
    core::Json root = core::Json::Parse(jsonText, &perr);
    if (root.IsNull() && !perr.empty())
        return core::Result<SceneFile>::Err("scene: JSON parse error: " + perr);
    if (!root.IsObject())
        return core::Result<SceneFile>::Err("scene: root must be a JSON object");

    SceneFile out;
    if (const core::Json* ex = root.Get("extends")) {
        if (!ex->IsString())
            return core::Result<SceneFile>::Err("scene: 'extends' must be a string");
        out.extends = ex->GetString();
    }
    const core::Json* ents = root.Get("entities");
    if (!ents || !ents->IsArray())
        return core::Result<SceneFile>::Err("scene: 'entities' must be an array");
    for (size_t i = 0; i < ents->Size(); ++i) {
        const core::Json* e = ents->At(i);
        if (!e || !e->IsObject())
            return core::Result<SceneFile>::Err(
                "scene: entity at index " + std::to_string(i) + " must be a JSON object");

        EntityDef def;
        if (const core::Json* name = e->Get("name")) {
            if (!name->IsString())
                return core::Result<SceneFile>::Err("scene: entity 'name' must be a string");
            def.name = name->GetString();
        }
        if (const core::Json* prefab = e->Get("prefab")) {
            if (!prefab->IsString())
                return core::Result<SceneFile>::Err("scene: entity 'prefab' must be a string");
            def.prefab = prefab->GetString();
        }
        const core::Json* comps = e->Get("components");
        if (!comps || !comps->IsObject())
            return core::Result<SceneFile>::Err(
                "scene: entity '" + def.name + "' requires a 'components' object");
        for (const auto& [cname, cdata] : comps->Members()) {
            if (!cdata.IsObject())
                return core::Result<SceneFile>::Err(
                    "scene: component '" + cname + "' of entity '" + def.name +
                    "' must be a JSON object");
            ComponentDef cd;
            cd.name = cname;
            cd.data = cdata;
            def.components.push_back(std::move(cd));
        }
        out.entities.push_back(std::move(def));
    }

    if (const core::Json* gv = root.Get("gameVars")) {
        if (!gv->IsObject())
            return core::Result<SceneFile>::Err("scene: 'gameVars' must be a JSON object");
        out.gameVars = *gv;
    }
    if (const core::Json* lv = root.Get("level")) {
        if (!lv->IsObject())
            return core::Result<SceneFile>::Err("scene: 'level' must be a JSON object");
        out.level = *lv;
    }
    return core::Result<SceneFile>::Ok(std::move(out));
}

SceneFile SceneFile::Merge(const SceneFile& parent, const SceneFile& child) {
    SceneFile out;
    out.extends = child.extends.empty() ? parent.extends : child.extends;
    out.gameVars = child.gameVars.IsObject() ? child.gameVars : parent.gameVars;
    out.level = child.level.IsObject() ? child.level : parent.level;
    // Parent entities first; child entities with the same name replace the
    // parent's entry (keeping the parent's position), new names append.
    out.entities = parent.entities;
    for (const EntityDef& c : child.entities) {
        bool replaced = false;
        for (EntityDef& p : out.entities) {
            if (!c.name.empty() && p.name == c.name) {
                p = c;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.entities.push_back(c);
    }
    return out;
}

core::Json SceneFile::ToJson() const {
    core::Json root = MakeObject();
    core::Json arr;
    arr.type_ = core::Json::Type::Array;
    for (const EntityDef& def : entities) {
        core::Json e = MakeObject();
        if (!def.name.empty()) e.object_["name"] = MakeString(def.name);
        if (!def.prefab.empty()) e.object_["prefab"] = MakeString(def.prefab);
        core::Json comps = MakeObject();
        for (const ComponentDef& c : def.components) comps.object_[c.name] = c.data;
        e.object_["components"] = std::move(comps);
        arr.array_.push_back(std::move(e));
    }
    root.object_["entities"] = std::move(arr);
    if (gameVars.IsObject()) root.object_["gameVars"] = gameVars;
    if (level.IsObject()) root.object_["level"] = level;
    return root;
}

std::vector<std::string> SceneFile::MeshKeys() const {
    std::vector<std::string> keys;
    const char* texFields[] = {"albedoTex", "mrTex", "aoTex", "emissiveTex"};
    auto addString = [&keys](const core::Json* node) {
        if (node && node->IsString() && !node->GetString().empty()) keys.push_back(node->GetString());
    };
    for (const EntityDef& e : entities) {
        for (const ComponentDef& c : e.components) {
            if (c.name == "sprite") {
                addString(c.data.Get("texture"));
                continue;
            }
            if (c.name != "mesh") continue;
            addString(c.data.Get("meshKey"));
            if (const core::Json* lod = c.data.Get("lod")) {
                if (lod->IsArray()) {
                    for (const core::Json& item : lod->Items()) addString(item.Get("meshKey"));
                }
            }
            for (const char* f : texFields) addString(c.data.Get(f));
            if (const core::Json* mat = c.data.Get("material")) {
                if (mat->IsObject()) {
                    for (const char* f : texFields) addString(mat->Get(f));
                }
            }
        }
    }
    return keys;
}

core::Result<core::Json> SceneFile::MakeEntity(const std::string& name,
                                               const math::Vec3& pos,
                                               const math::Quat& rot,
                                               const math::Vec3& scale,
                                               const std::string& meshKey,
                                               float metallic,
                                               float roughness,
                                               const gfx::Color& color,
                                               const std::string& albedoTex,
                                               const std::string& mrTex,
                                               const std::string& aoTex,
                                               const std::string& emissiveTex,
                                               float ao,
                                               float emissiveIntensity,
                                                const std::string& scriptPath,
                                                const std::string& scriptBackend,
                                                const core::Json& scriptVars,
                                               const std::vector<LodEntry>& lod,
                                               float hp,
                                               float maxHp,
                                               const std::string& parent) {
    if (name.empty())
        return core::Result<core::Json>::Err("scene: exported entity name must not be empty");
    if (meshKey.empty())
        return core::Result<core::Json>::Err("scene: exported entity '" + name +
                                             "' has an empty meshKey");

    core::Json e = MakeObject();
    e.object_["name"] = MakeString(name);

    core::Json tf = MakeObject();
    tf.object_["pos"] = MakeVec3(pos);
    tf.object_["rot"] = MakeQuat(rot);
    tf.object_["scale"] = MakeVec3(scale);
    if (!parent.empty()) tf.object_["parent"] = MakeString(parent);

    core::Json mat = MakeObject();
    mat.object_["metallic"] = MakeNumber(metallic);
    mat.object_["roughness"] = MakeNumber(roughness);
    mat.object_["colorHex"] = MakeString(MakeColorHex(color));
    mat.object_["ao"] = MakeNumber(ao);
    mat.object_["emissiveIntensity"] = MakeNumber(emissiveIntensity);
    if (!albedoTex.empty()) mat.object_["albedoTex"] = MakeString(albedoTex);
    if (!mrTex.empty()) mat.object_["mrTex"] = MakeString(mrTex);
    if (!aoTex.empty()) mat.object_["aoTex"] = MakeString(aoTex);
    if (!emissiveTex.empty()) mat.object_["emissiveTex"] = MakeString(emissiveTex);

    core::Json mesh = MakeObject();
    mesh.object_["meshKey"] = MakeString(meshKey);
    mesh.object_["material"] = std::move(mat);
    // LOD chain: emitted only when non-empty, as [{distance, meshKey}, ...].
    if (!lod.empty()) {
        core::Json lodArr;
        lodArr.type_ = core::Json::Type::Array;
        for (const LodEntry& entry : lod) {
            core::Json item = MakeObject();
            item.object_["distance"] = MakeNumber(entry.distance);
            item.object_["meshKey"] = MakeString(entry.meshKey);
            lodArr.array_.push_back(std::move(item));
        }
        mesh.object_["lod"] = std::move(lodArr);
    }

    core::Json comps = MakeObject();
    comps.object_["transform"] = std::move(tf);
    comps.object_["mesh"] = std::move(mesh);

    // Optional script component: emitted only when a path is attached, exactly
    // matching the built-in `script` factory schema (backend/path/vars).
    if (!scriptPath.empty()) {
        core::Json script = MakeObject();
        script.object_["backend"] = MakeString(scriptBackend.empty() ? "lua" : scriptBackend);
        script.object_["path"] = MakeString(scriptPath);
        if (scriptVars.IsObject()) script.object_["vars"] = scriptVars;
        comps.object_["script"] = std::move(script);
    }

    // Optional health component (matching the built-in `health` factory
    // schema); omitted when maxHp is <= 0 (no health tracked).
    if (maxHp > 0.0f) {
        core::Json health = MakeObject();
        health.object_["hp"] = MakeNumber(hp > 0.0f ? hp : maxHp);
        health.object_["maxHp"] = MakeNumber(maxHp);
        comps.object_["health"] = std::move(health);
    }

    e.object_["components"] = std::move(comps);
    return core::Result<core::Json>::Ok(std::move(e));
}

// --- PrefabLibrary -----------------------------------------------------------

// Rejects a prefab component map whose values are not all JSON objects.
core::Status ValidatePrefabComponents(const core::Json& map, const std::string& name) {
    if (!map.IsObject())
        return core::Status::Err("scene: prefab '" + name + "' component map must be a JSON object");
    for (const auto& [cname, cdata] : map.Members()) {
        if (!cdata.IsObject())
            return core::Status::Err("scene: prefab '" + name + "' component '" + cname +
                                     "' must be a JSON object");
    }
    return core::Status::Ok(true);
}

core::Status PrefabLibrary::Add(const std::string& name, const std::string& jsonText) {
    if (name.empty()) return core::Status::Err("scene: prefab name must not be empty");
    std::string perr;
    core::Json root = core::Json::Parse(jsonText, &perr);
    if (root.IsNull() && !perr.empty())
        return core::Status::Err("scene: prefab '" + name + "' JSON parse error: " + perr);
    if (!root.IsObject())
        return core::Status::Err("scene: prefab '" + name + "' must be a JSON object");
    if (const core::Json* comps = root.Get("components")) {
        if (!comps->IsObject())
            return core::Status::Err("scene: prefab '" + name + "' 'components' must be a JSON object");
        for (const auto& kv : root.Members()) {
            if (kv.first != "components")
                return core::Status::Err("scene: prefab '" + name +
                                         "' has unknown top-level key '" + kv.first +
                                         "' (expected only 'components')");
        }
        core::Status s = ValidatePrefabComponents(*comps, name);
        if (!s.Ok()) return s;
        prefs_[name] = *comps;
    } else {
        core::Status s = ValidatePrefabComponents(root, name);
        if (!s.Ok()) return s;
        prefs_[name] = root;
    }
    return core::Status::Ok(true);
}

bool PrefabLibrary::Has(const std::string& name) const {
    return prefs_.find(name) != prefs_.end();
}

core::Result<const core::Json*> PrefabLibrary::Get(const std::string& name) const {
    auto it = prefs_.find(name);
    if (it == prefs_.end())
        return core::Result<const core::Json*>::Err("scene: prefab '" + name + "' not found");
    return core::Result<const core::Json*>::Ok(&it->second);
}

// --- ComponentRegistry -------------------------------------------------------

void ComponentRegistry::Register(const std::string& name, ComponentFactory fn) {
    factories_[name] = std::move(fn);
}

bool ComponentRegistry::Has(const std::string& name) const {
    return factories_.find(name) != factories_.end();
}

const std::map<std::string, ComponentFactory>& ComponentRegistry::All() const {
    return factories_;
}

// --- Built-in factories ------------------------------------------------------

void RegisterBuiltinComponents(ComponentRegistry& reg, assets::AssetManager* assets) {
    reg.Register("transform",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"pos", "rot", "scale", "parent"},
                                              "transform", err))
                         return false;
                     SceneTransform t;
                     if (!ReadVec3(data, "pos", "transform", t.pos, err)) return false;
                     if (!ReadQuat(data, "rot", "transform", t.rot, err)) return false;
                     if (!ReadVec3(data, "scale", "transform", t.scale, err)) return false;
                     if (const core::Json* p = data.Get("parent")) {
                         if (!p->IsString()) {
                             if (err) *err = "component 'transform' field 'parent' must be a string";
                             return false;
                         }
                         t.parent = p->GetString();
                     }
                     world.Add<SceneTransform>(ent, t);
                     return true;
                 });

    reg.Register("mesh",
                 [assets](ecs::World& world, ecs::Entity ent, const core::Json& data,
                          const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data,
                                    {"meshKey", "lod", "material", "metallic", "roughness", "colorHex",
                                     "albedoTex", "mrTex", "aoTex", "emissiveTex",
                                     "ao", "emissiveIntensity"},
                                    "mesh", err))
                         return false;
                     const core::Json* key = data.Get("meshKey");
                     if (!key || !key->IsString() || key->GetString().empty()) {
                         if (err) *err = "component 'mesh' requires a non-empty 'meshKey' string";
                         return false;
                     }
                     SceneMesh m;
                     m.meshKey = key->GetString();
                     if (assets) {
                         const std::string& k = m.meshKey;
                         if (k.compare(0, 4, "obj:") != 0 && k.compare(0, 5, "gltf:") != 0) {
                             if (err)
                                 *err = "component 'mesh' meshKey '" + k +
                                        "' has no known loader prefix (expected 'obj:' or 'gltf:')";
                             return false;
                         }
                     }
                     // Optional LOD chain: [{distance, meshKey}, ...] with
                     // strictly increasing distances. Each level's meshKey must
                     // be non-empty and (when an AssetManager is present) use a
                     // known loader prefix, mirroring the base meshKey rules.
                     if (const core::Json* lod = data.Get("lod")) {
                         if (!lod->IsArray()) {
                             if (err) *err = "component 'mesh' field 'lod' must be an array";
                             return false;
                         }
                         float prevDistance = 0.0f;
                         bool first = true;
                         for (size_t i = 0; i < lod->Size(); ++i) {
                             const core::Json* item = lod->At(i);
                             if (!item || !item->IsObject() ||
                                 !CheckComponentShape(*item, {"distance", "meshKey"},
                                                      "mesh.lod[" + std::to_string(i) + "]", err))
                                 return false;
                             const core::Json* d = item->Get("distance");
                             if (!d || !d->IsNumber()) {
                                 if (err)
                                     *err = "component 'mesh' lod entry " + std::to_string(i) +
                                            " requires a numeric 'distance'";
                                 return false;
                             }
                             const core::Json* lk = item->Get("meshKey");
                             if (!lk || !lk->IsString() || lk->GetString().empty()) {
                                 if (err)
                                     *err = "component 'mesh' lod entry " + std::to_string(i) +
                                            " requires a non-empty 'meshKey' string";
                                 return false;
                             }
                             const float dist = static_cast<float>(d->GetNumber());
                             if (!first && dist <= prevDistance) {
                                 if (err)
                                     *err = "component 'mesh' lod distances must be strictly "
                                            "increasing (entry " + std::to_string(i) + ")";
                                 return false;
                             }
                             if (assets) {
                                 const std::string& lkStr = lk->GetString();
                                 if (lkStr.compare(0, 4, "obj:") != 0 &&
                                     lkStr.compare(0, 5, "gltf:") != 0) {
                                     if (err)
                                         *err = "component 'mesh' lod meshKey '" + lkStr +
                                                "' has no known loader prefix (expected 'obj:' or "
                                                "'gltf:')";
                                     return false;
                                 }
                             }
                             m.lod.push_back({dist, lk->GetString()});
                             prevDistance = dist;
                             first = false;
                         }
                     }
                     if (const core::Json* mat = data.Get("material")) {
                         if (!mat->IsObject()) {
                             if (err) *err = "component 'mesh' field 'material' must be an object";
                             return false;
                         }
                         if (!CheckComponentShape(*mat,
                                        {"metallic", "roughness", "colorHex", "albedoTex", "mrTex",
                                         "aoTex", "emissiveTex", "ao", "emissiveIntensity"},
                                        "mesh.material", err))
                             return false;
                         if (!RequireNumber(*mat, "metallic", "mesh.material", m.metallic, err))
                             return false;
                         if (!RequireNumber(*mat, "roughness", "mesh.material", m.roughness, err))
                             return false;
                         if (!RequireString(*mat, "albedoTex", "mesh.material", m.albedoTex, err))
                             return false;
                         if (!RequireString(*mat, "mrTex", "mesh.material", m.mrTex, err))
                             return false;
                         if (!RequireString(*mat, "aoTex", "mesh.material", m.aoTex, err))
                             return false;
                         if (!RequireString(*mat, "emissiveTex", "mesh.material", m.emissiveTex, err))
                             return false;
                         if (!RequireNumber(*mat, "ao", "mesh.material", m.ao, err)) return false;
                         if (!RequireNumber(*mat, "emissiveIntensity", "mesh.material",
                                            m.emissiveIntensity, err))
                             return false;
                         if (const core::Json* col = mat->Get("colorHex")) {
                             if (!col->IsString()) {
                                 if (err)
                                     *err = "component 'mesh' field 'material.colorHex' must be a string";
                                 return false;
                             }
                             m.colorHex = col->GetString();
                         }
                     }
                     if (!RequireNumber(data, "metallic", "mesh", m.metallic, err)) return false;
                     if (!RequireNumber(data, "roughness", "mesh", m.roughness, err)) return false;
                     if (!RequireString(data, "albedoTex", "mesh", m.albedoTex, err)) return false;
                     if (!RequireString(data, "mrTex", "mesh", m.mrTex, err)) return false;
                     if (!RequireString(data, "aoTex", "mesh", m.aoTex, err)) return false;
                     if (!RequireString(data, "emissiveTex", "mesh", m.emissiveTex, err)) return false;
                     if (!RequireNumber(data, "ao", "mesh", m.ao, err)) return false;
                     if (!RequireNumber(data, "emissiveIntensity", "mesh", m.emissiveIntensity, err))
                         return false;
                     if (const core::Json* col = data.Get("colorHex")) {
                         if (!col->IsString()) {
                             if (err) *err = "component 'mesh' field 'colorHex' must be a string";
                             return false;
                         }
                         m.colorHex = col->GetString();
                     }
                     world.Add<SceneMesh>(ent, m);
                     return true;
                 });

    reg.Register("health",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"hp", "maxHp"}, "health", err)) return false;
                     const core::Json* hp = data.Get("hp");
                     const core::Json* maxHp = data.Get("maxHp");
                     if (!hp || !hp->IsNumber() || !maxHp || !maxHp->IsNumber()) {
                         if (err) *err = "component 'health' requires numeric 'hp' and 'maxHp'";
                         return false;
                     }
                     SceneHealth h;
                     h.hp = static_cast<float>(hp->GetNumber());
                     h.maxHp = static_cast<float>(maxHp->GetNumber());
                     world.Add<SceneHealth>(ent, h);
                     return true;
                 });

    reg.Register("sprite",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data,
                                              {"texture", "flipX", "flipY", "colorHex"},
                                              "sprite", err))
                         return false;
                     const core::Json* tex = data.Get("texture");
                     if (!tex || !tex->IsString() || tex->GetString().empty()) {
                         if (err) *err = "component 'sprite' requires a non-empty 'texture' string";
                         return false;
                     }
                     SceneSprite s;
                     s.texture = tex->GetString();
                     if (const core::Json* fx = data.Get("flipX")) {
                         if (!fx->IsBool()) {
                             if (err) *err = "component 'sprite' field 'flipX' must be a bool";
                             return false;
                         }
                         s.flipX = fx->GetBool();
                     }
                     if (const core::Json* fy = data.Get("flipY")) {
                         if (!fy->IsBool()) {
                             if (err) *err = "component 'sprite' field 'flipY' must be a bool";
                             return false;
                         }
                         s.flipY = fy->GetBool();
                     }
                     if (const core::Json* c = data.Get("colorHex")) {
                         if (!c->IsString()) {
                             if (err) *err = "component 'sprite' field 'colorHex' must be a string";
                             return false;
                         }
                         s.colorHex = c->GetString();
                     }
                     world.Add<SceneSprite>(ent, s);
                     return true;
                 });

    reg.Register("zombie",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"row", "delay", "type"},
                                              "zombie", err))
                         return false;
                     SceneZombie z;
                     if (const core::Json* r = data.Get("row")) {
                         if (!r->IsNumber()) {
                             if (err) *err = "component 'zombie' field 'row' must be a number";
                             return false;
                         }
                         z.row = static_cast<int>(r->GetNumber());
                     }
                     if (const core::Json* d = data.Get("delay")) {
                         if (!d->IsNumber()) {
                             if (err) *err = "component 'zombie' field 'delay' must be a number";
                             return false;
                         }
                         z.delay = static_cast<float>(d->GetNumber());
                     }
                     if (const core::Json* t = data.Get("type")) {
                         if (!t->IsString()) {
                             if (err) *err = "component 'zombie' field 'type' must be a string";
                             return false;
                         }
                         z.type = t->GetString();
                     }
                     world.Add<SceneZombie>(ent, z);
                     return true;
                 });

    reg.Register("script",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"backend", "path", "vars"}, "script", err)) return false;
                     SceneScript s;
                     if (const core::Json* b = data.Get("backend")) {
                         if (!b->IsString()) {
                             if (err) *err = "component 'script' field 'backend' must be a string";
                             return false;
                         }
                         s.backend = b->GetString();
                     }
                     if (const core::Json* p = data.Get("path")) {
                         if (!p->IsString()) {
                             if (err) *err = "component 'script' field 'path' must be a string";
                             return false;
                         }
                         s.path = p->GetString();
                     }
                     if (const core::Json* v = data.Get("vars")) {
                         if (!v->IsObject()) {
                             if (err) *err = "component 'script' field 'vars' must be an object";
                             return false;
                         }
                         s.vars = *v;
                     }
                     world.Add<SceneScript>(ent, s);
                     return true;
                 });

    reg.Register("scripts",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     const core::Json* arr = data.Get("items");
                     if (!arr || !arr->IsArray()) {
                         if (err) *err = "component 'scripts' requires an 'items' array";
                         return false;
                     }
                     SceneScripts out;
                     for (size_t i = 0; i < arr->Size(); ++i) {
                         const core::Json* item = arr->At(i);
                         if (!item || !item->IsObject() ||
                             !CheckComponentShape(*item, {"backend", "path", "vars"},
                                                  "scripts.items[" + std::to_string(i) + "]",
                                                  err))
                             return false;
                         SceneScript s;
                         if (const core::Json* b = item->Get("backend")) s.backend = b->GetString();
                         if (const core::Json* p = item->Get("path")) s.path = p->GetString();
                         if (const core::Json* v = item->Get("vars")) s.vars = *v;
                         if (s.path.empty()) {
                             if (err)
                                 *err = "component 'scripts' item " + std::to_string(i) +
                                        " requires a non-empty 'path'";
                             return false;
                         }
                         out.items.push_back(std::move(s));
                     }
                     if (out.items.empty()) {
                         if (err) *err = "component 'scripts' must not be empty";
                         return false;
                     }
                     world.Add<SceneScripts>(ent, out);
                     return true;
                 });

    reg.Register("rigidbody",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(
                             data,
                             {"shape", "radius", "halfExtents", "dynamic", "mass",
                              "restitution", "friction", "damping", "gravityScale",
                              "layer", "mask"},
                             "rigidbody", err))
                         return false;
                     SceneRigidBody r;
                     if (const core::Json* s = data.Get("shape")) {
                         if (!s->IsString()) {
                             if (err)
                                 *err = "component 'rigidbody' field 'shape' must be "
                                        "'sphere' or 'box'";
                             return false;
                         }
                         const std::string& v = s->GetString();
                         if (v != "sphere" && v != "box") {
                             // Tolerant: an empty/invalid shape (e.g. created by
                             // an older editor build) falls back to sphere so the
                             // scene still plays; the inspector shows the combo.
                             NEON_LOG_CAT(core::LogCategory::Scene, core::LogLevel::Warn,
                                          "scene: rigidbody shape '%s' invalid, "
                                          "defaulting to 'sphere'",
                                          v.c_str());
                             r.shape = "sphere";
                         } else {
                             r.shape = v;
                         }
                     }
                     if (!RequireNumber(data, "radius", "rigidbody", r.radius, err)) return false;
                     if (!ReadVec3(data, "halfExtents", "rigidbody", r.halfExtents, err))
                         return false;
                     if (const core::Json* d = data.Get("dynamic")) {
                         if (!d->IsBool()) {
                             if (err) *err = "component 'rigidbody' field 'dynamic' must be a bool";
                             return false;
                         }
                         r.dynamic = d->GetBool();
                     }
                     if (!RequireNumber(data, "mass", "rigidbody", r.mass, err)) return false;
                     if (!RequireNumber(data, "restitution", "rigidbody", r.restitution, err))
                         return false;
                     if (!RequireNumber(data, "friction", "rigidbody", r.friction, err))
                         return false;
                     if (!RequireNumber(data, "damping", "rigidbody", r.linearDamping, err))
                         return false;
                     if (!RequireNumber(data, "gravityScale", "rigidbody", r.gravityScale, err))
                         return false;
                     if (const core::Json* l = data.Get("layer")) {
                         if (!l->IsNumber()) {
                             if (err) *err = "component 'rigidbody' field 'layer' must be a number";
                             return false;
                         }
                         r.layer = static_cast<uint32_t>(l->GetNumber());
                     }
                     if (const core::Json* m = data.Get("mask")) {
                         if (!m->IsNumber()) {
                             if (err) *err = "component 'rigidbody' field 'mask' must be a number";
                             return false;
                         }
                         r.mask = static_cast<uint32_t>(m->GetNumber());
                     }
                     world.Add<SceneRigidBody>(ent, r);
                     return true;
                 });

    reg.Register("character",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data,
                                              {"radius", "halfHeight", "layer", "mask"},
                                              "character", err))
                         return false;
                     SceneCharacter c;
                     if (!RequireNumber(data, "radius", "character", c.radius, err))
                         return false;
                     if (!RequireNumber(data, "halfHeight", "character", c.halfHeight, err))
                         return false;
                     if (const core::Json* l = data.Get("layer")) {
                         if (!l->IsNumber()) {
                             if (err) *err = "component 'character' field 'layer' must be a number";
                             return false;
                         }
                         c.layer = static_cast<uint32_t>(l->GetNumber());
                     }
                     if (const core::Json* m = data.Get("mask")) {
                         if (!m->IsNumber()) {
                             if (err) *err = "component 'character' field 'mask' must be a number";
                             return false;
                         }
                         c.mask = static_cast<uint32_t>(m->GetNumber());
                     }
                     world.Add<SceneCharacter>(ent, c);
                     return true;
                 });

    reg.Register("behaviorTree",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"tree"}, "behaviorTree", err)) return false;
                     const core::Json* tree = data.Get("tree");
                     if (!tree || !tree->IsString()) {
                         if (err) *err = "component 'behaviorTree' requires a 'tree' string";
                         return false;
                     }
                     SceneBehaviorTree b;
                     b.treeJson = tree->GetString();
                     world.Add<SceneBehaviorTree>(ent, b);
                     return true;
                 });

    reg.Register("name",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"name"}, "name", err)) return false;
                     const core::Json* n = data.Get("name");
                     if (!n || !n->IsString()) {
                         if (err) *err = "component 'name' requires a 'name' string";
                         return false;
                     }
                     SceneName nm;
                     nm.name = n->GetString();
                     world.Add<SceneName>(ent, nm);
                     return true;
                 });

    reg.Register("groups",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"groups"}, "groups", err)) return false;
                     const core::Json* g = data.Get("groups");
                     SceneGroups out;
                     if (g && g->IsArray()) {
                         for (const core::Json& item : g->Items()) {
                             if (item.IsString() && !item.GetString().empty())
                                 out.groups.push_back(item.GetString());
                         }
                     } else if (g && g->IsString()) {
                         // Editor-friendly: comma-separated list.
                         std::string s = g->GetString();
                         size_t start = 0;
                         while (start <= s.size()) {
                             size_t comma = s.find(',', start);
                             if (comma == std::string::npos) comma = s.size();
                             std::string name = s.substr(start, comma - start);
                             // trim whitespace
                             size_t b = name.find_first_not_of(" \t\r\n");
                             size_t e = name.find_last_not_of(" \t\r\n");
                             if (b != std::string::npos) {
                                 name = name.substr(b, e - b + 1);
                                 if (!name.empty()) out.groups.push_back(name);
                             }
                             if (comma == s.size()) break;
                             start = comma + 1;
                         }
                     }
                     world.Add<SceneGroups>(ent, std::move(out));
                     return true;
                 });

    reg.Register("type",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"value"}, "type", err)) return false;
                     SceneNodeType t;
                     if (const core::Json* v = data.Get("value")) {
                         if (!v->IsString()) {
                             if (err) *err = "component 'type' field 'value' must be a string";
                             return false;
                         }
                         t.value = v->GetString();
                     }
                     world.Add<SceneNodeType>(ent, std::move(t));
                     return true;
                 });

    reg.Register("camera",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"fov", "ortho"}, "camera", err))
                         return false;
                     SceneCamera c;
                     if (!RequireNumber(data, "fov", "camera", c.fov, err)) return false;
                     if (const core::Json* o = data.Get("ortho")) {
                         if (!o->IsBool()) {
                             if (err) *err = "component 'camera' field 'ortho' must be a bool";
                             return false;
                         }
                         c.ortho = o->GetBool();
                     }
                     world.Add<SceneCamera>(ent, c);
                     return true;
                 });
}

// --- Instantiate -------------------------------------------------------------

core::Result<int> Instantiate(ecs::World& world, const SceneFile& scene,
                              const PrefabLibrary& prefs, const ComponentRegistry& reg,
                              std::vector<ecs::Entity>* outEntities) {
    std::vector<ecs::Entity> created;
    created.reserve(scene.entities.size());
    if (outEntities) outEntities->clear();
    const core::Json kNull;

    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const EntityDef& def = scene.entities[i];
        const std::string label =
            def.name.empty() ? ("entity " + std::to_string(i)) : ("entity '" + def.name + "'");

        // Effective components = prefab expanded then instance overridden.
        std::map<std::string, core::Json> effective;
        if (!def.prefab.empty()) {
            auto pre = prefs.Get(def.prefab);
            if (!pre.Ok()) {
                for (ecs::Entity e : created) world.Destroy(e);
                return core::Result<int>::Err("scene: " + label + ": " + pre.Error());
            }
            for (const auto& [name, data] : pre.Value()->Members()) effective[name] = data;
        }
        for (const ComponentDef& c : def.components) {
            auto it = effective.find(c.name);
            if (it != effective.end()) {
                DeepMerge(it->second, c.data);
            } else {
                effective[c.name] = c.data;
            }
        }

        if (!effective.count("transform")) {
            for (ecs::Entity e : created) world.Destroy(e);
            return core::Result<int>::Err("scene: " + label + ": missing 'transform' component");
        }

        ecs::Entity ent = world.Create();
        created.push_back(ent);
        if (!def.name.empty() && !effective.count("name"))
            world.Add<SceneName>(ent, SceneName{def.name});

        for (const auto& [name, data] : effective) {
            if (!reg.Has(name)) {
                // Components without a factory are legitimate scene DATA (e.g.
                // a 2D game's "plant"/"zombie" layout entities read by the
                // project script), so this is debug-level noise, not a warning.
                NEON_LOG_DEBUG("scene: %s: data component '%s' (no factory, kept as data)",
                               label.c_str(), name.c_str());
                continue;
            }
            const ComponentFactory& fn = reg.All().at(name);
            const core::Json* instanceData = nullptr;
            for (const ComponentDef& c : def.components) {
                if (c.name == name) {
                    instanceData = &c.data;
                    break;
                }
            }
            std::string err;
            if (!fn(world, ent, data, instanceData ? *instanceData : kNull, &err)) {
                for (ecs::Entity e : created) world.Destroy(e);
                return core::Result<int>::Err("scene: " + label + ": " + err);
            }
        }
    }

    // Scene tree: resolve transform.parent names after every entity exists.
    for (ecs::Entity child : created) {
        const SceneTransform* t = world.Get<SceneTransform>(child);
        if (!t || t->parent.empty()) continue;
        ecs::Entity parent;
        auto names = world.ViewAll<SceneName>();
        for (size_t i = 0; i < names.Size(); ++i) {
            ecs::Entity cand = world.EntityAt<SceneName>(i);
            const SceneName* n = world.Get<SceneName>(cand);
            if (n && n->name == t->parent) {
                parent = cand;
                break;
            }
        }
        if (!parent.IsValid()) {
            for (ecs::Entity e : created) world.Destroy(e);
            return core::Result<int>::Err("scene: entity '" + t->parent +
                                          "' referenced by 'parent' not found");
        }
        world.Add<SceneParentLink>(child, SceneParentLink{parent});
    }

    if (outEntities) *outEntities = created;
    return core::Result<int>::Ok(static_cast<int>(created.size()));
}

} // namespace neon::scene

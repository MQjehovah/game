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

core::Json MakeArray() {
    core::Json j;
    j.type_ = core::Json::Type::Array;
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

core::Json MakeBool(bool v) {
    core::Json j;
    j.type_ = core::Json::Type::Bool;
    j.bool_ = v;
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
        if (const core::Json* id = e->Get("id")) {
            if (!id->IsNumber() || id->GetInt(0) <= 0)
                return core::Result<SceneFile>::Err(
                    "scene: entity 'id' must be a positive number");
            def.id = id->GetInt(0);
        }
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
        // G5-4: hierarchy is entity-level (parentId/parent beside id/name). Read
        // it top-level first, then fall back to the legacy components.transform
        // placement so old scenes keep working.
        if (const core::Json* pid = e->Get("parentId")) {
            if (!pid->IsNumber())
                return core::Result<SceneFile>::Err(
                    "scene: entity 'parentId' must be a number");
            def.parentId = pid->GetInt(0);
        }
        if (const core::Json* pn = e->Get("parent")) {
            if (!pn->IsString())
                return core::Result<SceneFile>::Err("scene: entity 'parent' must be a string");
            def.parent = pn->GetString();
        }
        const core::Json* comps = e->Get("components");
        if (!comps || !comps->IsObject())
            return core::Result<SceneFile>::Err(
                "scene: entity '" + def.name + "' requires a 'components' object");
        // Legacy placement: parent/parentId inside components.transform.
        if (def.parentId == 0 && def.parent.empty()) {
            if (const core::Json* tf = comps->Get("transform")) {
                if (const core::Json* pid = tf->Get("parentId"))
                    if (pid->IsNumber()) def.parentId = pid->GetInt(0);
                if (const core::Json* pn = tf->Get("parent"))
                    if (pn->IsString()) def.parent = pn->GetString();
            }
        }
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
        if (def.id != 0) e.object_["id"] = MakeNumber(def.id);
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
                                               const std::string& parent,
                                               int parentId,
                                               int id) {
    if (name.empty())
        return core::Result<core::Json>::Err("scene: exported entity name must not be empty");
    if (meshKey.empty())
        return core::Result<core::Json>::Err("scene: exported entity '" + name +
                                             "' has an empty meshKey");

    core::Json e = MakeObject();
    e.object_["name"] = MakeString(name);
    if (id != 0) e.object_["id"] = MakeNumber(id);
    // G5-4: hierarchy is entity-level (beside id/name), not in transform.
    if (!parent.empty()) e.object_["parent"] = MakeString(parent);
    if (parentId != 0) e.object_["parentId"] = MakeNumber(parentId);

    core::Json tf = MakeObject();
    tf.object_["pos"] = MakeVec3(pos);
    tf.object_["rot"] = MakeQuat(rot);
    tf.object_["scale"] = MakeVec3(scale);

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

core::Result<core::Json> SceneFile::MakeSpriteEntity(const std::string& name,
                                                     const math::Vec3& pos,
                                                     const math::Quat& rot,
                                                     const math::Vec3& scale,
                                                     const std::string& texture,
                                                     bool flipX, bool flipY,
                                                     const std::string& colorHex, float hp,
                                                     float maxHp, const std::string& parent,
                                                     int parentId, int id) {
    if (name.empty())
        return core::Result<core::Json>::Err("scene: exported entity name must not be empty");
    if (texture.empty())
        return core::Result<core::Json>::Err("scene: exported sprite '" + name +
                                              "' has an empty texture");

    core::Json e = MakeObject();
    e.object_["name"] = MakeString(name);
    if (id != 0) e.object_["id"] = MakeNumber(id);
    // G5-4: hierarchy is entity-level (beside id/name), not in transform.
    if (!parent.empty()) e.object_["parent"] = MakeString(parent);
    if (parentId != 0) e.object_["parentId"] = MakeNumber(parentId);

    core::Json tf = MakeObject();
    tf.object_["pos"] = MakeVec3(pos);
    tf.object_["rot"] = MakeQuat(rot);
    tf.object_["scale"] = MakeVec3(scale);

    core::Json sp = MakeObject();
    sp.object_["texture"] = MakeString(texture);
    if (flipX) sp.object_["flipX"] = MakeBool(true);
    if (flipY) sp.object_["flipY"] = MakeBool(true);
    sp.object_["colorHex"] = MakeString(colorHex.empty() ? "#FFFFFF" : colorHex);

    core::Json comps = MakeObject();
    comps.object_["transform"] = std::move(tf);
    comps.object_["sprite"] = std::move(sp);

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
                     // G5-4: parent/parentId moved to the ENTITY level (EntityDef).
                     // Kept in the allowed set so legacy scenes (parentId inside
                     // transform) still parse; the values are extracted by Parse.
                     if (!CheckComponentShape(data, {"pos", "rot", "scale", "parent", "parentId"},
                                              "transform", err))
                         return false;
                     SceneTransform t;
                     if (!ReadVec3(data, "pos", "transform", t.pos, err)) return false;
                     if (!ReadQuat(data, "rot", "transform", t.rot, err)) return false;
                     if (!ReadVec3(data, "scale", "transform", t.scale, err)) return false;
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

    reg.Register("audio",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"sound", "volume", "radius"}, "audio", err))
                         return false;
                     SceneAudioSource a;
                     if (!a.FromJson(data, err)) return false;
                     world.Add<SceneAudioSource>(ent, a);
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
                     if (!CheckComponentShape(data, {"fov", "ortho", "orthoSize"}, "camera", err))
                         return false;
                     SceneCamera c;
                     if (!RequireNumber(data, "fov", "camera", c.fov, err)) return false;
                     if (const core::Json* n = data.Get("orthoSize"))
                         c.orthoSize = static_cast<float>(n->GetNumber());
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

    reg.Register("light",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                      if (!CheckComponentShape(data,
                                              {"type", "sunDir", "color", "intensity", "radius",
                                                "ambientStrength"},
                                              "light", err))
                         return false;
                     SceneLight l;
                     if (const core::Json* t = data.Get("type")) l.type = t->GetString();
                     auto readVec3 = [&](const char* key, math::Vec3& out) {
                         const core::Json* c = data.Get(key);
                         if (!c || !c->IsArray()) return;
                         float v[3] = {0.0f, 0.0f, 0.0f};
                         size_t n = 0;
                         for (const core::Json& vv : c->Items()) {
                             if (n < 3) v[n++] = static_cast<float>(vv.GetNumber());
                         }
                         out.x = v[0];
                         out.y = v[1];
                         out.z = v[2];
                     };
                     auto readColor = [&](const char* key, gfx::Color& out) {
                         const core::Json* c = data.Get(key);
                         if (!c || !c->IsArray()) return;
                         float v[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                         size_t n = 0;
                         for (const core::Json& vv : c->Items()) {
                             if (n < 4) v[n++] = static_cast<float>(vv.GetNumber());
                         }
                         out.r = v[0];
                         out.g = v[1];
                         out.b = v[2];
                         out.a = v[3];
                     };
                     readVec3("sunDir", l.sunDir);
                     readColor("color", l.color);
                     if (const core::Json* n = data.Get("intensity"))
                         l.intensity = static_cast<float>(n->GetNumber());
                      if (const core::Json* n = data.Get("radius"))
                          l.radius = static_cast<float>(n->GetNumber());
                      if (const core::Json* n = data.Get("ambientStrength"))
                          l.ambientStrength = static_cast<float>(n->GetNumber());
                     world.Add<SceneLight>(ent, l);
                     return true;
                 });

    reg.Register("sortOrder",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"z"}, "sortOrder", err)) return false;
                     SceneSortOrder s;
                     if (!RequireNumber(data, "z", "sortOrder", s.z, err)) return false;
                     world.Add<SceneSortOrder>(ent, s);
                     return true;
                 });

    reg.Register("terrain",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"segments", "size", "heightScale",
                                                     "heights",
                                                     "chunkGridDiv", "chunkLodLevels",
                                                     "chunkBaseSubdiv", "vegMeshKey",
                                                     "vegCount", "vegSeed", "vegSize",
                                                     "vegImpostorDistance", "vegMinHeight",
                                                     "vegMaxHeight", "vegMaxSlope"},
                                              "terrain", err))
                         return false;
                     SceneTerrain t;
                     float seg = 48.0f;
                     if (!RequireNumber(data, "segments", "terrain", seg, err)) return false;
                     t.segments = static_cast<int>(seg);
                     if (!RequireNumber(data, "size", "terrain", t.size, err)) return false;
                     if (!RequireNumber(data, "heightScale", "terrain", t.heightScale, err))
                         return false;
                     if (const core::Json* h = data.Get("heights")) {
                         if (h->IsArray()) {
                             for (const core::Json& v : h->Items())
                                 t.heights.push_back(static_cast<float>(v.GetNumber()));
                         }
                     }
                     // G2-3 optional chunked-LOD + vegetation knobs (defaults
                     // keep the classic single-mesh terrain when absent).
                     if (const core::Json* n = data.Get("chunkGridDiv")) t.chunkGridDiv = static_cast<int>(n->GetNumber());
                     if (const core::Json* n = data.Get("chunkLodLevels")) t.chunkLodLevels = static_cast<int>(n->GetNumber());
                     if (const core::Json* n = data.Get("chunkBaseSubdiv")) t.chunkBaseSubdiv = static_cast<int>(n->GetNumber());
                     if (const core::Json* s = data.Get("vegMeshKey")) t.vegMeshKey = s->GetString();
                     if (const core::Json* n = data.Get("vegCount")) t.vegCount = static_cast<uint32_t>(n->GetNumber());
                     if (const core::Json* n = data.Get("vegSeed")) t.vegSeed = static_cast<uint32_t>(n->GetNumber());
                     if (const core::Json* n = data.Get("vegSize")) t.vegSize = static_cast<float>(n->GetNumber());
                     if (const core::Json* n = data.Get("vegImpostorDistance")) t.vegImpostorDistance = static_cast<float>(n->GetNumber());
                     if (const core::Json* n = data.Get("vegMinHeight")) t.vegMinHeight = static_cast<float>(n->GetNumber());
                     if (const core::Json* n = data.Get("vegMaxHeight")) t.vegMaxHeight = static_cast<float>(n->GetNumber());
                     if (const core::Json* n = data.Get("vegMaxSlope")) t.vegMaxSlope = static_cast<float>(n->GetNumber());
                     world.Add<SceneTerrain>(ent, std::move(t));
                     return true;
                 });

    reg.Register("tilemap",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"cols", "rows", "cellSize", "tiles"},
                                              "tilemap", err))
                         return false;
                     SceneTilemap t;
                     float cols = 8.0f, rows = 5.0f;
                     if (!RequireNumber(data, "cols", "tilemap", cols, err)) return false;
                     if (!RequireNumber(data, "rows", "tilemap", rows, err)) return false;
                     if (!RequireNumber(data, "cellSize", "tilemap", t.cellSize, err))
                         return false;
                     t.cols = static_cast<int>(cols);
                     t.rows = static_cast<int>(rows);
                     if (const core::Json* tls = data.Get("tiles")) {
                         if (tls->IsArray()) {
                             for (const core::Json& v : tls->Items())
                                 t.tiles.push_back(v.IsString() ? v.GetString() : "");
                         }
                     }
                     t.tiles.resize(static_cast<size_t>(t.cols) * t.rows);
                     world.Add<SceneTilemap>(ent, std::move(t));
                     return true;
                 });

    reg.Register("decal",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (!CheckComponentShape(data, {"texture", "size", "alpha"}, "decal",
                                              err))
                         return false;
                     SceneDecal d;
                     if (const core::Json* tex = data.Get("texture")) {
                         if (!tex->IsString()) {
                             if (err) *err = "component 'decal' field 'texture' must be a string";
                             return false;
                         }
                         d.texture = tex->GetString();
                     }
                     if (!RequireNumber(data, "size", "decal", d.size, err)) return false;
                     if (!RequireNumber(data, "alpha", "decal", d.alpha, err)) return false;
                     world.Add<SceneDecal>(ent, std::move(d));
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
                // a 2D game's "plant" layout entities or a plugin's custom
                // "inventory" component). They are stored verbatim in a
                // generic SceneData component so runtime scripts/plugins can
                // read them via the EntityComponent binding.
                if (!world.Has<SceneData>(ent)) world.Add<SceneData>(ent);
                if (SceneData* sd = world.Get<SceneData>(ent))
                    sd->components.emplace_back(name, data);
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

    // Scene tree: resolve transform.parentId (stable id, preferred) or the
    // legacy transform.parent name after every entity exists.
    // id -> created entity (created[i] matches scene.entities[i] by index).
    // Ids must be unique when present.
    std::map<int, ecs::Entity> byId;
    for (size_t i = 0; i < scene.entities.size() && i < created.size(); ++i) {
        const int id = scene.entities[i].id;
        if (id == 0) continue;
        if (byId.count(id) != 0) {
            for (ecs::Entity e : created) world.Destroy(e);
            return core::Result<int>::Err("scene: duplicate entity id " + std::to_string(id));
        }
        byId[id] = created[i];
        world.Add<SceneId>(created[i], SceneId{id}); // G2-2: preserve the stable id
    }

    // G5-4: hierarchy is entity-level (EntityDef.parentId/parent) — resolved
    // into SceneParentLink after every entity exists. created[i] matches
    // scene.entities[i] by index.
    for (size_t i = 0; i < created.size() && i < scene.entities.size(); ++i) {
        const EntityDef& def = scene.entities[i];
        if (def.parentId == 0 && def.parent.empty()) continue;
        ecs::Entity child = created[i];
        ecs::Entity parent;
        if (def.parentId != 0) {
            const auto it = byId.find(def.parentId);
            if (it != byId.end()) {
                parent = it->second;
            } else {
                for (ecs::Entity e : created) world.Destroy(e);
                return core::Result<int>::Err("scene: entity id " +
                                              std::to_string(def.parentId) +
                                              " referenced by 'parentId' not found");
            }
        } else if (!def.parent.empty()) {
            // Legacy name fallback (first match, as before).
            auto names = world.ViewAll<SceneName>();
            for (size_t k = 0; k < names.Size(); ++k) {
                ecs::Entity cand = world.EntityAt<SceneName>(k);
                const SceneName* n = world.Get<SceneName>(cand);
                if (n && n->name == def.parent) {
                    parent = cand;
                    break;
                }
            }
            if (!parent.IsValid()) {
                for (ecs::Entity e : created) world.Destroy(e);
                return core::Result<int>::Err("scene: entity '" + def.parent +
                                              "' referenced by 'parent' not found");
            }
        }
        world.Add<SceneParentLink>(child, SceneParentLink{parent});
    }

    // Cycle guard: with ids (or duplicate names), a scene can express parent
    // cycles / self-parenting. Walk each entity's chain up; revisiting the
    // start means a cycle. Bounded by the entity count.
    for (ecs::Entity start : created) {
        const SceneParentLink* link = world.Get<SceneParentLink>(start);
        if (!link) continue;
        ecs::Entity cur = link->parent;
        size_t steps = 0;
        while (cur.IsValid() && steps <= created.size()) {
            if (cur == start) {
                const SceneName* sn = world.Get<SceneName>(start);
                for (ecs::Entity e : created) world.Destroy(e);
                return core::Result<int>::Err(
                    "scene: parent cycle involving '" +
                    (sn ? sn->name : std::string("<unnamed>")) + "'");
            }
            const SceneParentLink* l = world.Get<SceneParentLink>(cur);
            cur = l ? l->parent : ecs::Entity{};
            ++steps;
        }
    }

    if (outEntities) *outEntities = created;
    return core::Result<int>::Ok(static_cast<int>(created.size()));
}

// G2-2: serialize an ecs::World back to the scene-file JSON format — the
// reverse of Instantiate. Every factory's component is emitted with the exact
// field names it reads, so Parse(FromWorld(w)) re-Instantiates an equivalent
// World. Stable ids round-trip via SceneId; generic components live in
// SceneData. The editor generates play/save output from the World it hosts.
core::Result<core::Json> SceneFile::FromWorld(ecs::World& world) {
    core::Json root = MakeObject();
    core::Json arr;
    arr.type_ = core::Json::Type::Array;

    // Enumerate entities via their transform (every scene entity has one);
    // build id -> entity so parentId references resolve.
    std::map<int, ecs::Entity> byId;
    auto tr = world.ViewAll<SceneTransform>();
    std::vector<ecs::Entity> ents;
    ents.reserve(tr.Size());
    for (size_t i = 0; i < tr.Size(); ++i) {
        ecs::Entity e = world.EntityAt<SceneTransform>(i);
        ents.push_back(e);
        if (const SceneId* id = world.Get<SceneId>(e))
            if (id->id != 0) byId[id->id] = e;
    }

    for (ecs::Entity e : ents) {
        core::Json ent = MakeObject();
        if (const SceneName* n = world.Get<SceneName>(e))
            ent.object_["name"] = MakeString(n->name);
        if (const SceneId* id = world.Get<SceneId>(e))
            if (id->id != 0) ent.object_["id"] = MakeNumber(id->id);
        core::Json comps = MakeObject();

        if (const SceneTransform* t = world.Get<SceneTransform>(e)) {
            core::Json tf = MakeObject();
            tf.object_["pos"] = MakeVec3(t->pos);
            tf.object_["rot"] = MakeQuat(t->rot);
            tf.object_["scale"] = MakeVec3(t->scale);
            comps.object_["transform"] = std::move(tf);
        }
        // G5-4: hierarchy is entity-level — emit parentId from the resolved
        // SceneParentLink (the parent entity's stable SceneId).
        if (const SceneParentLink* link = world.Get<SceneParentLink>(e)) {
            if (const SceneId* pid = world.Get<SceneId>(link->parent))
                if (pid->id != 0) ent.object_["parentId"] = MakeNumber(pid->id);
        }
        if (const SceneMesh* m = world.Get<SceneMesh>(e)) {
            core::Json mesh = MakeObject();
            mesh.object_["meshKey"] = MakeString(m->meshKey);
            core::Json mat = MakeObject();
            mat.object_["metallic"] = MakeNumber(m->metallic);
            mat.object_["roughness"] = MakeNumber(m->roughness);
            if (!m->colorHex.empty()) mat.object_["colorHex"] = MakeString(m->colorHex);
            mat.object_["ao"] = MakeNumber(m->ao);
            mat.object_["emissiveIntensity"] = MakeNumber(m->emissiveIntensity);
            if (!m->albedoTex.empty()) mat.object_["albedoTex"] = MakeString(m->albedoTex);
            if (!m->mrTex.empty()) mat.object_["mrTex"] = MakeString(m->mrTex);
            if (!m->aoTex.empty()) mat.object_["aoTex"] = MakeString(m->aoTex);
            if (!m->emissiveTex.empty()) mat.object_["emissiveTex"] = MakeString(m->emissiveTex);
            mesh.object_["material"] = std::move(mat);
            if (!m->lod.empty()) {
                core::Json lodArr;
                lodArr.type_ = core::Json::Type::Array;
                for (const LodEntry& entry : m->lod) {
                    core::Json item = MakeObject();
                    item.object_["distance"] = MakeNumber(entry.distance);
                    item.object_["meshKey"] = MakeString(entry.meshKey);
                    lodArr.array_.push_back(std::move(item));
                }
                mesh.object_["lod"] = std::move(lodArr);
            }
            comps.object_["mesh"] = std::move(mesh);
        }
        if (const SceneSprite* s = world.Get<SceneSprite>(e)) {
            core::Json sp = MakeObject();
            sp.object_["texture"] = MakeString(s->texture);
            if (s->flipX) sp.object_["flipX"] = MakeBool(true);
            if (s->flipY) sp.object_["flipY"] = MakeBool(true);
            sp.object_["colorHex"] = MakeString(s->colorHex.empty() ? "#FFFFFF" : s->colorHex);
            comps.object_["sprite"] = std::move(sp);
        }
        if (const SceneHealth* h = world.Get<SceneHealth>(e)) {
            core::Json health = MakeObject();
            health.object_["hp"] = MakeNumber(h->hp);
            health.object_["maxHp"] = MakeNumber(h->maxHp);
            comps.object_["health"] = std::move(health);
        }
        if (const SceneScripts* scripts = world.Get<SceneScripts>(e)) {
            core::Json items;
            items.type_ = core::Json::Type::Array;
            for (const SceneScript& sc : scripts->items) {
                core::Json item = MakeObject();
                item.object_["backend"] = MakeString(sc.backend.empty() ? "lua" : sc.backend);
                item.object_["path"] = MakeString(sc.path);
                if (sc.vars.IsObject()) item.object_["vars"] = sc.vars;
                items.array_.push_back(std::move(item));
            }
            core::Json sc = MakeObject();
            sc.object_["items"] = std::move(items);
            comps.object_["scripts"] = std::move(sc);
        } else if (const SceneScript* sc = world.Get<SceneScript>(e)) {
            core::Json item = MakeObject();
            item.object_["backend"] = MakeString(sc->backend.empty() ? "lua" : sc->backend);
            item.object_["path"] = MakeString(sc->path);
            if (sc->vars.IsObject()) item.object_["vars"] = sc->vars;
            core::Json items;
            items.type_ = core::Json::Type::Array;
            items.array_.push_back(std::move(item));
            core::Json scs = MakeObject();
            scs.object_["items"] = std::move(items);
            comps.object_["scripts"] = std::move(scs);
        }
        if (const SceneBehaviorTree* b = world.Get<SceneBehaviorTree>(e)) {
            core::Json bt = MakeObject();
            bt.object_["tree"] = MakeString(b->treeJson);
            comps.object_["behaviorTree"] = std::move(bt);
        }
        if (const SceneGroups* g = world.Get<SceneGroups>(e)) {
            core::Json groups;
            groups.type_ = core::Json::Type::Array;
            for (const std::string& grp : g->groups)
                groups.array_.push_back(MakeString(grp));
            core::Json gc = MakeObject();
            gc.object_["groups"] = std::move(groups);
            comps.object_["groups"] = std::move(gc);
        }
        if (const SceneNodeType* t = world.Get<SceneNodeType>(e)) {
            core::Json tc = MakeObject();
            tc.object_["value"] = MakeString(t->value);
            comps.object_["type"] = std::move(tc);
        }
        if (const SceneCamera* c = world.Get<SceneCamera>(e)) {
            core::Json cam = MakeObject();
            cam.object_["fov"] = MakeNumber(c->fov);
            if (c->ortho) cam.object_["ortho"] = MakeBool(true);
            cam.object_["orthoSize"] = MakeNumber(c->orthoSize);
            comps.object_["camera"] = std::move(cam);
        }
        if (const SceneLight* l = world.Get<SceneLight>(e)) {
            core::Json li = MakeObject();
            li.object_["type"] = MakeString(l->type);
            li.object_["sunDir"] = MakeVec3(l->sunDir);
            core::Json col = MakeArray();
            col.array_ = {MakeNumber(l->color.r), MakeNumber(l->color.g), MakeNumber(l->color.b),
                          MakeNumber(l->color.a)};
            li.object_["color"] = std::move(col);
            li.object_["intensity"] = MakeNumber(l->intensity);
            li.object_["radius"] = MakeNumber(l->radius);
            li.object_["ambientStrength"] = MakeNumber(l->ambientStrength);
            comps.object_["light"] = std::move(li);
        }
        if (const SceneSortOrder* so = world.Get<SceneSortOrder>(e)) {
            core::Json s = MakeObject();
            s.object_["z"] = MakeNumber(so->z);
            comps.object_["sortOrder"] = std::move(s);
        }
        if (const SceneTerrain* t = world.Get<SceneTerrain>(e)) {
            core::Json te = MakeObject();
            te.object_["segments"] = MakeNumber(t->segments);
            te.object_["size"] = MakeNumber(t->size);
            te.object_["heightScale"] = MakeNumber(t->heightScale);
            core::Json hArr;
            hArr.type_ = core::Json::Type::Array;
            for (float h : t->heights) hArr.array_.push_back(MakeNumber(h));
            te.object_["heights"] = std::move(hArr);
            te.object_["chunkGridDiv"] = MakeNumber(t->chunkGridDiv);
            te.object_["chunkLodLevels"] = MakeNumber(t->chunkLodLevels);
            te.object_["chunkBaseSubdiv"] = MakeNumber(t->chunkBaseSubdiv);
            if (!t->vegMeshKey.empty()) te.object_["vegMeshKey"] = MakeString(t->vegMeshKey);
            te.object_["vegCount"] = MakeNumber(t->vegCount);
            te.object_["vegSeed"] = MakeNumber(t->vegSeed);
            te.object_["vegSize"] = MakeNumber(t->vegSize);
            te.object_["vegImpostorDistance"] = MakeNumber(t->vegImpostorDistance);
            te.object_["vegMinHeight"] = MakeNumber(t->vegMinHeight);
            te.object_["vegMaxHeight"] = MakeNumber(t->vegMaxHeight);
            te.object_["vegMaxSlope"] = MakeNumber(t->vegMaxSlope);
            comps.object_["terrain"] = std::move(te);
        }
        if (const SceneTilemap* t = world.Get<SceneTilemap>(e)) {
            core::Json tlm = MakeObject();
            tlm.object_["cols"] = MakeNumber(t->cols);
            tlm.object_["rows"] = MakeNumber(t->rows);
            tlm.object_["cellSize"] = MakeNumber(t->cellSize);
            core::Json tiles;
            tiles.type_ = core::Json::Type::Array;
            for (const std::string& tile : t->tiles) tiles.array_.push_back(MakeString(tile));
            tlm.object_["tiles"] = std::move(tiles);
            comps.object_["tilemap"] = std::move(tlm);
        }
        if (const SceneDecal* d = world.Get<SceneDecal>(e)) {
            core::Json dc = MakeObject();
            dc.object_["texture"] = MakeString(d->texture);
            dc.object_["size"] = MakeNumber(d->size);
            dc.object_["alpha"] = MakeNumber(d->alpha);
            comps.object_["decal"] = std::move(dc);
        }
        if (const SceneRigidBody* rb = world.Get<SceneRigidBody>(e)) {
            core::Json r = MakeObject();
            r.object_["shape"] = MakeString(rb->shape);
            r.object_["radius"] = MakeNumber(rb->radius);
            r.object_["halfExtents"] = MakeVec3(rb->halfExtents);
            if (rb->dynamic) r.object_["dynamic"] = MakeBool(true);
            r.object_["mass"] = MakeNumber(rb->mass);
            r.object_["restitution"] = MakeNumber(rb->restitution);
            r.object_["friction"] = MakeNumber(rb->friction);
            r.object_["damping"] = MakeNumber(rb->linearDamping);
            r.object_["gravityScale"] = MakeNumber(rb->gravityScale);
            r.object_["layer"] = MakeNumber(rb->layer);
            r.object_["mask"] = MakeNumber(rb->mask);
            comps.object_["rigidbody"] = std::move(r);
        }
        if (const SceneCharacter* c = world.Get<SceneCharacter>(e)) {
            core::Json ch = MakeObject();
            ch.object_["radius"] = MakeNumber(c->radius);
            ch.object_["halfHeight"] = MakeNumber(c->halfHeight);
            ch.object_["layer"] = MakeNumber(c->layer);
            ch.object_["mask"] = MakeNumber(c->mask);
            comps.object_["character"] = std::move(ch);
        }
        if (const SceneAudioSource* a = world.Get<SceneAudioSource>(e)) {
            comps.object_["audio"] = a->ToJson();
        }
        if (const SceneAnimOverride* ao = world.Get<SceneAnimOverride>(e)) {
            core::Json an = MakeObject();
            an.object_["clip"] = MakeString(ao->clip);
            if (ao->loop) an.object_["loop"] = MakeBool(true);
            an.object_["speed"] = MakeNumber(ao->speed);
            an.object_["crossFade"] = MakeNumber(ao->crossFade);
            comps.object_["anim"] = std::move(an);
        }
        // Generic components (plant/zombie/plugin data...) from SceneData.
        if (const SceneData* sd = world.Get<SceneData>(e)) {
            for (const auto& [cname, cdata] : sd->components)
                comps.object_[cname] = cdata;
        }

        ent.object_["components"] = std::move(comps);
        arr.array_.push_back(std::move(ent));
    }

    root.object_["entities"] = std::move(arr);
    return core::Result<core::Json>::Ok(std::move(root));
}

} // namespace neon::scene

#include "neon/scene/scene_file.hpp"

#include <algorithm>

#include "neon/assets/asset_manager.hpp"
#include "neon/core/log.hpp"

namespace neon::scene {
namespace {

bool HasUnknown(const core::Json& data, const std::vector<std::string>& allowed,
                const std::string& comp, std::string* err) {
    for (const auto& [key, val] : data.Members()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            if (err) *err = "component '" + comp + "' has unknown field '" + key + "'";
            return true;
        }
    }
    return false;
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
    return core::Result<SceneFile>::Ok(std::move(out));
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
    return root;
}

// --- PrefabLibrary -----------------------------------------------------------

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
        prefs_[name] = *comps;
    } else {
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
                     if (HasUnknown(data, {"pos", "rot", "scale"}, "transform", err)) return false;
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
                     if (HasUnknown(data,
                                    {"meshKey", "material", "metallic", "roughness", "colorHex"},
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
                     if (const core::Json* mat = data.Get("material")) {
                         if (!mat->IsObject()) {
                             if (err) *err = "component 'mesh' field 'material' must be an object";
                             return false;
                         }
                         if (HasUnknown(*mat, {"metallic", "roughness", "colorHex"},
                                        "mesh.material", err))
                             return false;
                         if (!RequireNumber(*mat, "metallic", "mesh.material", m.metallic, err))
                             return false;
                         if (!RequireNumber(*mat, "roughness", "mesh.material", m.roughness, err))
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
                     if (HasUnknown(data, {"hp", "maxHp"}, "health", err)) return false;
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

    reg.Register("script",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (HasUnknown(data, {"backend", "path", "vars"}, "script", err)) return false;
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

    reg.Register("behaviorTree",
                 [](ecs::World& world, ecs::Entity ent, const core::Json& data,
                    const core::Json&, std::string* err) {
                     if (HasUnknown(data, {"tree"}, "behaviorTree", err)) return false;
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
                     if (HasUnknown(data, {"name"}, "name", err)) return false;
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
}

// --- Instantiate -------------------------------------------------------------

core::Result<int> Instantiate(ecs::World& world, const SceneFile& scene,
                              const PrefabLibrary& prefs, const ComponentRegistry& reg) {
    std::vector<ecs::Entity> created;
    created.reserve(scene.entities.size());
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
                NEON_LOG_WARN("scene: %s: skipping component '%s' (no factory registered)",
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

    return core::Result<int>::Ok(static_cast<int>(created.size()));
}

} // namespace neon::scene

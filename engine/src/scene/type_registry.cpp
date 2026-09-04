#include "neon/scene/type_registry.hpp"

#include <utility>

#include "neon/scene/render_stack.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/scene/data_table.hpp"

namespace neon::scene {

std::vector<std::pair<std::string, TypeInfo>>& TypeRegistry::Mutate() {
    static std::vector<std::pair<std::string, TypeInfo>> g_registry;
    return g_registry;
}

const TypeInfo* TypeRegistry::Find(const std::string& name) {
    for (const auto& [n, info] : Mutate())
        if (n == name) return &info;
    return nullptr;
}

std::vector<TypeInfo> TypeRegistry::All() {
    std::vector<TypeInfo> out;
    out.reserve(Mutate().size());
    for (auto& [n, info] : Mutate()) out.push_back(info);
    return out;
}

bool TypeRegistry::Has(const std::string& name) { return Find(name) != nullptr; }

void TypeRegistry::Clear() { Mutate().clear(); }

void RegisterBuiltinReflectedTypes() {
    // Symmetric re-registration is a no-op (last wins); the call is idempotent
    // so tests can Clear() and re-register. ONLY components with a runtime
    // factory (or a pre-existing editor schema) are exposed as editor-addable
    // here; e.g. SceneAnimOverride declares kFields but has no factory yet, so
    // it is intentionally NOT registered (would add a non-functional option).
    TypeRegistry::Register<SceneHealth>("health", "生命");
    TypeRegistry::Register<SceneAudioSource>("audio", "音频源");
    TypeRegistry::Register<SceneSortOrder>("sortOrder", "排序");
    TypeRegistry::Register<SceneDecal>("decal", "贴花");
    TypeRegistry::Register<SceneCharacter>("character", "角色控制器");
    TypeRegistry::Register<SceneRigidBody>("rigidbody", "刚体");
    TypeRegistry::Register<SceneTransform>("transform", "变换");
    TypeRegistry::Register<SceneGroups>("groups", "组");
    TypeRegistry::Register<SceneNodeType>("type", "类型");
    TypeRegistry::Register<SceneCamera>("camera", "相机");
    TypeRegistry::Register<SceneLight>("light", "光照");
    TypeRegistry::Register<SceneScript>("script", "脚本");
    TypeRegistry::Register<RenderStack>("renderstack", "渲染栈");
    // B2: a reflected game-data row type (skills). A project registers its own
    // data rows here (or a data component) and loads `[{...}]` JSON via
    // LoadDataTable so content is schema-checked + editor-editable.
    TypeRegistry::Register<SkillData>("skill", "技能");
    TypeRegistry::Register<ItemData>("item", "物品");
}

} // namespace neon::scene

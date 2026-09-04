#pragma once

// NeonEngine type registry (Godot ClassDB / EnTT meta-style, G2-2). A
// reflected component types' `kFields` list is registered ONCE under a name;
// the registry then answers everything a consumer needs from that one
// declaration:
//   * editor ComponentSchema (G2-1)                      -> Schema()
//   * type-erased JSON serialize / deserialize (C6)      -> ToJson()/FromJson()
//   * clone for undo / deterministic snapshots           -> Clone()
//
// Non-reflected components (data components stored as SceneData) are not
// registered; the built-in ComponentRegistry / hand-written BuildSchemas table
// continues to serve them (the drift they cause is exactly what reflection is
// removing component-by-component).
//
// Usage:
//   struct MyComp { ... inline static const auto kFields = scene::ReflectFields(...); };
//   scene::TypeRegistry::Register<MyComp>("mycomp", "我的组件");
//   if (const auto* info = scene::TypeRegistry::Find("mycomp")) { ... info->schema ... }

#include <functional>
#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/scene/component_reflect.hpp"
#include "neon/scene/component_schema.hpp"

namespace neon::scene {

// One registered component type: name/label plus reflection-derived metadata.
struct TypeInfo {
    std::string name;
    std::string label;
    std::vector<FieldSchema> fields; // editor schema (Transient omitted)

    // Type-erased codecs (nullptr for a type with no kFields-less registration).
    std::function<core::Json(const void*)> toJson;
    std::function<bool(const core::Json&, void*, std::string*)> fromJson;
    std::function<void(const void*, void*)> clone;
    // B2: validates + normalizes a row JSON through the registry (construct a T,
    // FromJson into it, then ToJson back). Returns the normalized JSON, or a
    // null Json when the row is invalid. Lets a DataTable loader validate rows
    // without knowing sizeof(T) / a factory for T's storage.
    std::function<core::Json(const core::Json&)> normalize;
};

// Registry of the reflected component types. Static, single source of truth for
// the reflected subset; the editor schema + JSON coders always read from here.
class TypeRegistry {
public:
    // Register a reflected component (T must declare a static `kFields`).
    // Idempotent: re-registering a name replaces the previous entry, so
    // RegisterBuiltinReflectedTypes can be called any number of times.
    template <typename T>
    static void Register(const char* name, const char* label) {
        static_assert(IsReflected_v<T>, "TypeRegistry::Register<T> requires T::kFields");
        TypeInfo info;
        info.name = name ? name : "";
        info.label = label ? label : "";
        info.fields = T::kFields.Schemas();
        info.toJson = [](const void* p) -> core::Json {
            return T::kFields.ToJson(*static_cast<const T*>(p));
        };
        info.fromJson = [](const core::Json& j, void* p, std::string* e) -> bool {
            return T::kFields.FromJson(j, *static_cast<T*>(p), e);
        };
        info.clone = [](const void* src, void* dst) {
            T::kFields.Clone(*static_cast<const T*>(src), *static_cast<T*>(dst));
        };
        info.normalize = [](const core::Json& j) -> core::Json {
            T row;
            std::string err;
            if (!T::kFields.FromJson(j, row, &err)) return core::Json{};
            return T::kFields.ToJson(row);
        };
        auto& reg = Mutate();
        for (auto& [n, existing] : reg) {
            if (n == (name ? name : "")) {
                existing = std::move(info);
                return;
            }
        }
        reg.emplace_back(name ? name : "", std::move(info));
    }

    static const TypeInfo* Find(const std::string& name);
    static std::vector<TypeInfo> All();
    static bool Has(const std::string& name);
    static void Clear(); // test tear-down

private:
    static std::vector<std::pair<std::string, TypeInfo>>& Mutate();
};

// Registers the built-in reflected scene components (idempotent). Today:
// SceneHealth, SceneAudioSource. Called by RegisterBuiltinComponentSchemas so the
// editor finds their schema through the reflection path rather than hand-written
// duplicates. New components join here as they adopt kFields.
void RegisterBuiltinReflectedTypes();

} // namespace neon::scene

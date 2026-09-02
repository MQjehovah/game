#pragma once

#include <string>
#include <vector>

#include "neon/core/json.hpp"

namespace neon::scene {

// Editor-facing component metadata (Godot @export / Unreal UPROPERTY style).
// A ComponentSchema describes a component's fields so the inspector can render
// an editor for ANY component - built-in (transform/mesh/health/script) or
// data components (plant/zombie/...). The runtime keeps using ComponentFactory
// registrations; schemas are editor tooling that lives beside them.
enum class FieldType { Number, Int, Bool, String, Vec3, Color, Enum, Resource, Json,
                       Array, Vec4, Struct };

struct FieldSchema {
    std::string key;        // JSON field name
    std::string label;      // Chinese editor label
    FieldType type = FieldType::Number;
    double def = 0.0;
    double min = 0.0;
    double max = 0.0;
    double step = 0.01;
    const char* const* options = nullptr; // Enum values
    int optionCount = 0;
    const char* resourceKind = nullptr;   // "texture" | "model" | "script" | "scene"
    const char* header = nullptr;         // Godot @export_group / Unity [Header]
    const char* tooltip = nullptr;        // Unity [Tooltip] hover text
    const char* widget = nullptr;         // optional custom inspector widget name
};

struct ComponentSchema {
    std::string name;       // JSON component name ("plant")
    std::string label;      // Chinese editor label ("植物")
    std::vector<FieldSchema> fields;
};

// Schema lookup: returns nullptr for components without registered metadata
// (the inspector then falls back to raw JSON).
const ComponentSchema* FindComponentSchema(const std::string& name);
const std::vector<ComponentSchema>& AllComponentSchemas();

// Registers schemas for the built-in + known data components (idempotent).
void RegisterBuiltinComponentSchemas();

} // namespace neon::scene

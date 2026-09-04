#pragma once

// B2: reflection-driven data tables (Godot Resource / Unity ScriptableObject
// style, but data-driven JSON + the engine's own reflection). Game content such
// as skills / items / quest rows lives in a data asset (a JSON array of row
// objects) instead of being hard-coded in a script. Each row's shape is an
// engine-reflected struct (declares a `kFields` list, registered in the
// TypeRegistry under a name), so:
//   * the editor's property inspector can author/edit a row automatically
//     (the same schema the type_registry exposes for components),
//   * scripts load the table by type name and get typed rows back
//     (the LoadDataTable binding),
//   * a malformed row fails loudly (the type-erased FromJson validates it).
//
// Lives in the scene layer (uses the scene reflection system). It does NOT bake
// any specific game's row structs into the engine — a project registers its own
// reflected row type via TypeRegistry::Register<T>("skill", ...) and loads
// assets through DataTable. This keeps the engine game-content-free while giving
// designers a data-driven, editor-editable content path.

#include <string>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/core/result.hpp"
#include "neon/scene/type_registry.hpp"

namespace neon::scene {

// A loaded data table: a type name (registered in the TypeRegistry) plus the
// rows, kept as JSON so the consumer (a script binding / the editor) reads the
// typed fields through the registry codec rather than raw hand-written parsing.
struct DataTable {
    std::string typeName;
    // One row per source array element, validated against the registry's
    // `normalize` (construct a row, FromJson into it, ToJson back). Rows that
    // fail validation are dropped (and the load returns an error listing how
    // many were rejected), so a single bad row never poisons the table.
    std::vector<core::Json> rows;

    bool Valid() const { return !typeName.empty(); }
    size_t Count() const { return rows.size(); }
};

// Loads + validates a data table from JSON whose root is an array of row
// objects. `typeName` must be registered in the TypeRegistry. Returns Ok with
// the validated (normalized) rows, or Err (bad JSON / unknown type / N rows
// rejected). See data_table.cpp.
core::Result<DataTable> LoadDataTable(const std::string& typeName,
                                      const std::string& jsonText);

// A generic reflected game-data ROW a project registers + loads as a data table.
// Not engine gameplay logic — just a schema the TypeRegistry can validate and
// the editor can inspect. A project defines its own row structs (with `kFields`)
// and registers them, then loads `[{...}]` JSON via LoadDataTable. SkillData is
// shipped as a reference example/entry point (skills = name + cooldown/mana +
// sfx), registered under "skill" in RegisterBuiltinReflectedTypes.
struct SkillData {
    std::string id;        // stable key the script looks rows up by
    std::string label;     // display name
    float cooldown = 0.0f; // seconds
    float cost = 0.0f;     // resource cost (mana) per cast
    std::string sfx;       // audio cue name
    std::string tag;       // optional gameplay tag ("frost"/"aoe"/...)

    inline static const auto kFields = ReflectFields(
        Field("id", "ID", FieldType::String, &SkillData::id),
        Field("label", "名称", FieldType::String, &SkillData::label),
        Field("cooldown", "冷却", FieldType::Number, &SkillData::cooldown, 0, 0, 600, 0.1),
        Field("cost", "消耗", FieldType::Number, &SkillData::cost, 0, 0, 100000, 1),
        Field("sfx", "音效", FieldType::String, &SkillData::sfx),
        Field("tag", "标签", FieldType::String, &SkillData::tag));
    static scene::ComponentSchema Schema() { return {"skill", "技能", kFields.Schemas()}; }
    core::Json ToJson() const { return kFields.ToJson(*this); }
    bool FromJson(const core::Json& j, std::string* err = nullptr) {
        return kFields.FromJson(j, *this, err);
    }
};

// A generic item row (B2), the counterpart to SkillData for loot/props (herbs,
// currency, gear). A project loads `[{...}]` via LoadDataTable("item", ...) so
// collectible content is data-driven + editor-editable, registered as "item".
struct ItemData {
    std::string id;    // stable key ("herb", "silver", ...)
    std::string label; // display name
    float value = 0.0f; // value / weight / healing amount
    std::string tag;   // optional gameplay tag ("collectible"/"currency"/...)
    std::string icon;  // optional sprite path (HUD/gather pickup)

    inline static const auto kFields = ReflectFields(
        Field("id", "ID", FieldType::String, &ItemData::id),
        Field("label", "名称", FieldType::String, &ItemData::label),
        Field("value", "数值", FieldType::Number, &ItemData::value, 0, 0, 1e7, 1),
        Field("tag", "标签", FieldType::String, &ItemData::tag),
        Field("icon", "图标", FieldType::String, &ItemData::icon));
    static scene::ComponentSchema Schema() { return {"item", "物品", kFields.Schemas()}; }
    core::Json ToJson() const { return kFields.ToJson(*this); }
    bool FromJson(const core::Json& j, std::string* err = nullptr) {
        return kFields.FromJson(j, *this, err);
    }
};

} // namespace neon::scene

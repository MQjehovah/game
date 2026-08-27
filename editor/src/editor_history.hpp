#pragma once

// Concrete editor commands: reversible scene edits against the editor's
// `std::vector<SceneEntity>`. Each command captures an entity INDEX plus the
// original/new values, so Undo/Redo replay deterministically. Index stability
// is guaranteed by the HistoryManager's LIFO undo / FIFO redo ordering: a
// command is only executed while the vector is in the exact layout it was
// created in, so `insert/erase at index_` always lands on the right entity.
// Selection is deliberately NOT part of undo (kept simple); callers clamp
// `selected_` after history operations.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "editor.hpp"
#include "history.hpp"
#include "script_panel_model.hpp"

namespace neon::editor {

namespace {
inline bool Vec3Eq(const math::Vec3& a, const math::Vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
inline bool QuatEq(const math::Quat& a, const math::Quat& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
inline gfx::Color HexToColor(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return gfx::Color::White;
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
        return 255u;
    };
    auto byte = [&](char hi, char lo) {
        return static_cast<float>(((nibble(hi) << 4) | nibble(lo)) / 255.0);
    };
    return {byte(hex[1], hex[2]), byte(hex[3], hex[4]), byte(hex[5], hex[6]), 1.0f};
}
} // namespace

// Insert a resolved entity at a recorded position. Undo erases it; redo
// re-inserts the copy.
class AddEntityCommand : public Command {
public:
    AddEntityCommand(std::vector<SceneEntity>* entities, const SceneEntity& entity, size_t index)
        : entities_(entities), entity_(entity), index_(index) {}

    void Apply() override { entities_->insert(entities_->begin() + static_cast<ptrdiff_t>(index_), entity_); }
    void Undo() override { entities_->erase(entities_->begin() + static_cast<ptrdiff_t>(index_)); }

private:
    std::vector<SceneEntity>* entities_;
    SceneEntity entity_;
    size_t index_;
};

// Remove an entity; undo restores the recorded copy at its recorded index.
class DeleteEntityCommand : public Command {
public:
    DeleteEntityCommand(std::vector<SceneEntity>* entities, size_t index)
        : entities_(entities), index_(index), entity_((*entities_)[index]) {}

    void Apply() override { entities_->erase(entities_->begin() + static_cast<ptrdiff_t>(index_)); }
    void Undo() override { entities_->insert(entities_->begin() + static_cast<ptrdiff_t>(index_), entity_); }

private:
    std::vector<SceneEntity>* entities_;
    size_t index_;
    SceneEntity entity_;
};

// Duplicate the entity at `sourceIndex` (appends a renamed, nudged copy).
class DuplicateEntityCommand : public Command {
public:
    DuplicateEntityCommand(std::vector<SceneEntity>* entities, size_t sourceIndex)
        : entities_(entities), insertAt_(entities->size()) {
        copy_ = (*entities)[sourceIndex];
        copy_.name += "_副本";
        copy_.pos.z += 0.5f;
    }

    void Apply() override { entities_->insert(entities_->begin() + static_cast<ptrdiff_t>(insertAt_), copy_); }
    void Undo() override { entities_->erase(entities_->begin() + static_cast<ptrdiff_t>(insertAt_)); }

private:
    std::vector<SceneEntity>* entities_;
    size_t insertAt_;
    SceneEntity copy_;
};

// Move the entity at `from_` to `to_` (scene-panel up/down). Undo moves it
// back, so a "move A from i to j" is exactly reversed by "move A from j to i".
class ReorderEntityCommand : public Command {
public:
    ReorderEntityCommand(std::vector<SceneEntity>* entities, size_t from, size_t to)
        : entities_(entities), from_(from), to_(to) {}

    void Apply() override { Move(from_, to_); }
    void Undo() override { Move(to_, from_); }

private:
    void Move(size_t a, size_t b) {
        if (a == b) return;
        SceneEntity e = std::move((*entities_)[a]);
        entities_->erase(entities_->begin() + static_cast<ptrdiff_t>(a));
        entities_->insert(entities_->begin() + static_cast<ptrdiff_t>(b), std::move(e));
    }

    std::vector<SceneEntity>* entities_;
    size_t from_;
    size_t to_;
};

// Full scene-tree reorder (the panel's "按名称排序" button): stores the exact
// pre-sort entity list and the target index permutation, so one undo step
// restores the whole tree order. Apply snapshots the current list before
// rebuilding it from the permutation (the history alternates Apply/Undo from
// the state this command was constructed on, so the snapshot is always the
// pre-reorder state).
class SortSceneTreeCommand : public Command {
public:
    SortSceneTreeCommand(std::vector<SceneEntity>* entities, std::vector<size_t> newOrder)
        : entities_(entities), newOrder_(std::move(newOrder)) {
        oldEntities_ = *entities_;
    }

    void Apply() override {
        oldEntities_ = *entities_;
        std::vector<SceneEntity> reordered;
        reordered.reserve(newOrder_.size());
        for (size_t i : newOrder_)
            reordered.push_back(std::move((*entities_)[i]));
        *entities_ = std::move(reordered);
    }
    void Undo() override { std::swap(*entities_, oldEntities_); }

private:
    std::vector<SceneEntity>* entities_;
    std::vector<SceneEntity> oldEntities_;
    std::vector<size_t> newOrder_;
};

// pos/rot/scale edit used by BOTH the gizmo write-back and the inspector.
// Merges only when the incoming command edits the SAME fields (bitmask), so an
// inspector position drag cannot swallow a gizmo rotation. Frames within one
// drag coalesce into a single undo step (merge adopts the incoming CURRENT
// values while keeping the ORIGINAL); the editor calls Seal() when a drag
// ends so a LATER drag starts its own undo step.
class EditTransformCommand : public Command {
public:
    enum Flags : int { kPos = 1, kRot = 2, kScale = 4, kAll = kPos | kRot | kScale };

    EditTransformCommand(std::vector<SceneEntity>* entities, int index,
                         const math::Vec3& origPos, const math::Quat& origRot,
                         const math::Vec3& origScale, const math::Vec3& newPos,
                         const math::Quat& newRot, const math::Vec3& newScale,
                         int fields = kAll)
        : entities_(entities), index_(index), fields_(fields), origPos_(origPos),
          curPos_(newPos), origRot_(origRot), curRot_(newRot), origScale_(origScale),
          curScale_(newScale) {}

    void Apply() override {
        SceneEntity& e = (*entities_)[static_cast<size_t>(index_)];
        e.pos = curPos_;
        e.rot = curRot_;
        e.scale = curScale_;
    }
    void Undo() override {
        SceneEntity& e = (*entities_)[static_cast<size_t>(index_)];
        e.pos = origPos_;
        e.rot = origRot_;
        e.scale = origScale_;
    }
    bool Merge(const Command& incoming) override {
        const EditTransformCommand* other = dynamic_cast<const EditTransformCommand*>(&incoming);
        if (!other || !mergeable_ || other->index_ != index_ || other->fields_ != fields_)
            return false;
        // Value-chain guard (like EditPropertyCommand): only merge when the
        // incoming ORIGINAL equals our CURRENT values, i.e. the edits form one
        // continuous drag. A separate edit that starts from a different value
        // (e.g. a programmatic set between two inspector drags) opens a new
        // undo step instead of coalescing.
        if (!Vec3Eq(other->origPos_, curPos_) || !QuatEq(other->origRot_, curRot_) ||
            !Vec3Eq(other->origScale_, curScale_))
            return false;
        curPos_ = other->curPos_;
        curRot_ = other->curRot_;
        curScale_ = other->curScale_;
        return true;
    }
    bool IsNoop() const override {
        return Vec3Eq(origPos_, curPos_) && Vec3Eq(origScale_, curScale_) &&
               QuatEq(origRot_, curRot_);
    }
    // Stop accepting further merges (called when the drag that produced this
    // command ends, so the next drag becomes its own undo step).
    void Seal() { mergeable_ = false; }
    bool Matches(int index, int fields) const { return index_ == index && fields_ == fields; }

private:
    std::vector<SceneEntity>* entities_;
    int index_;
    int fields_;
    bool mergeable_ = true;
    math::Vec3 origPos_, curPos_;
    math::Quat origRot_, curRot_;
    math::Vec3 origScale_, curScale_;
};

// Property write-back helpers (also keep the PBR material in sync where the
// inspector did). Used as function pointers by EditPropertyCommand.
inline void ApplyColorProp(SceneEntity& e, const gfx::Color& v) {
    e.tint = v;
    e.material.tint = v;
    e.spriteMaterial.tint = v;
}
inline void ApplyMetallicProp(SceneEntity& e, const float& v) {
    e.metallic = v;
    e.material.metallic = v;
}
inline void ApplyRoughnessProp(SceneEntity& e, const float& v) {
    e.roughness = v;
    e.material.roughness = v;
}
inline void ApplyAOProp(SceneEntity& e, const float& v) {
    e.ao = v;
    e.material.aoStrength = v;
}
inline void ApplyEmissiveIntensityProp(SceneEntity& e, const float& v) {
    e.emissiveIntensity = v;
    e.material.emissiveIntensity = v;
}
inline void ApplyNameProp(SceneEntity& e, const std::string& v) { e.name = v; }
inline void ApplyParentProp(SceneEntity& e, const int& v) { e.parentId = v; }
inline void ApplyNodeTypeProp(SceneEntity& e, const std::string& v) { e.nodeType = v; }
inline void ApplyCameraFovProp(SceneEntity& e, const float& v) { e.cameraFov = v; }
inline void ApplyCameraOrthoProp(SceneEntity& e, const bool& v) { e.cameraOrtho = v; }
inline void ApplyShaderPathProp(SceneEntity& e, const std::string& v) { e.shaderPath = v; }
inline void ApplyZOrderProp(SceneEntity& e, const float& v) { e.zOrder = v; }
inline void ApplyDecalTexProp(SceneEntity& e, const std::string& v) { e.decalTex = v; }
inline void ApplyDecalSizeProp(SceneEntity& e, const float& v) { e.decalSize = v; }
inline void ApplyDecalAlphaProp(SceneEntity& e, const float& v) { e.decalAlpha = v; }

// 2D sprite mirroring: flips both axes in one undo step.
struct SpriteFlipValue {
    bool flipX = false;
    bool flipY = false;
};
inline void ApplySpriteFlip(SceneEntity& e, const SpriteFlipValue& v) {
    e.spriteFlipX = v.flipX;
    e.spriteFlipY = v.flipY;
}
inline bool ValuesEqual(const SpriteFlipValue& a, const SpriteFlipValue& b) {
    return a.flipX == b.flipX && a.flipY == b.flipY;
}

// Script mounts are a flat list like any other component list: add/remove
// replaces the whole list in one undo step. There is deliberately no
// "primary" script concept - every entry is an equal mounted script.
inline void ApplyScriptList(SceneEntity& e, const std::vector<SceneScriptFields>& v) {
    e.scripts = v;
}
inline bool ValuesEqual(const std::vector<SceneScriptFields>& a,
                        const std::vector<SceneScriptFields>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].backend != b[i].backend || a[i].path != b[i].path ||
            core::JsonWriter::Write(a[i].vars) != core::JsonWriter::Write(b[i].vars))
            return false;
    }
    return true;
}

// A single-field edit inside one mounted script block (index into
// SceneEntity::scripts), so backend/path/vars edits undo exactly like the
// schema-driven component field edits.
struct ScriptFieldEdit {
    size_t index = 0;
    std::string field; // "backend" | "path" | "vars"
    core::Json value;
};

// P2-editor UX: delete several entities in one undo step (indices sorted).
class MultiDeleteEntityCommand : public Command {
public:
    MultiDeleteEntityCommand(std::vector<SceneEntity>* entities, std::vector<int> indices)
        : entities_(entities), indices_(std::move(indices)) {
        std::sort(indices_.begin(), indices_.end());
        for (int i : indices_)
            removed_.push_back((*entities_)[static_cast<size_t>(i)]);
    }
    void Apply() override {
        for (auto it = indices_.rbegin(); it != indices_.rend(); ++it)
            entities_->erase(entities_->begin() + *it);
    }
    void Undo() override {
        for (size_t k = 0; k < indices_.size(); ++k)
            entities_->insert(entities_->begin() + indices_[k], removed_[k]);
    }

private:
    std::vector<SceneEntity>* entities_;
    std::vector<int> indices_;
    std::vector<SceneEntity> removed_;
};

// Duplicate several entities (appended together, renamed + nudged).
class MultiDuplicateEntityCommand : public Command {
public:
    MultiDuplicateEntityCommand(std::vector<SceneEntity>* entities, std::vector<int> indices)
        : entities_(entities), insertAt_(entities->size()) {
        std::sort(indices.begin(), indices.end());
        for (int i : indices) {
            SceneEntity c = (*entities)[static_cast<size_t>(i)];
            c.name += "_副本";
            c.pos.z += 0.5f;
            copies_.push_back(std::move(c));
        }
    }
    void Apply() override {
        for (size_t k = 0; k < copies_.size(); ++k)
            entities_->insert(entities_->begin() + static_cast<ptrdiff_t>(insertAt_ + k),
                              copies_[k]);
    }
    void Undo() override {
        for (size_t k = 0; k < copies_.size(); ++k)
            entities_->erase(entities_->begin() + static_cast<ptrdiff_t>(insertAt_));
    }

private:
    std::vector<SceneEntity>* entities_;
    size_t insertAt_;
    std::vector<SceneEntity> copies_;
};

// Set the parent of several entities in one undo step.
class MultiSetParentCommand : public Command {
public:
    MultiSetParentCommand(std::vector<SceneEntity>* entities, std::vector<int> indices,
                          int parentId)
        : entities_(entities), indices_(std::move(indices)), parentId_(parentId) {
        for (int i : indices_)
            oldParents_.push_back((*entities_)[static_cast<size_t>(i)].parentId);
    }
    void Apply() override {
        for (size_t k = 0; k < indices_.size(); ++k)
            (*entities_)[static_cast<size_t>(indices_[k])].parentId = parentId_;
    }
    void Undo() override {
        for (size_t k = 0; k < indices_.size(); ++k)
            (*entities_)[static_cast<size_t>(indices_[k])].parentId = oldParents_[k];
    }

private:
    std::vector<SceneEntity>* entities_;
    std::vector<int> indices_;
    std::vector<int> oldParents_;
    int parentId_;
};

// Batch transform for multi-selection gizmo drags. Like EditTransformCommand,
// frames within one drag merge into a single undo step; Seal() ends the merge.
// Transform3 lives in editor.hpp (editor_history.hpp includes it).
class BatchTransformCommand : public Command {
public:
    BatchTransformCommand(std::vector<SceneEntity>* entities, std::vector<int> indices,
                          std::vector<Transform3> from, std::vector<Transform3> to)
        : entities_(entities), indices_(std::move(indices)), from_(std::move(from)),
          to_(std::move(to)) {}
    void Apply() override { Write(to_); }
    void Undo() override { Write(from_); }
    bool Merge(const Command& incoming) override {
        const BatchTransformCommand* other =
            dynamic_cast<const BatchTransformCommand*>(&incoming);
        if (!other || !mergeable_ || other->indices_ != indices_) return false;
        // Continuous drag chain: the incoming ORIGINAL must equal our CURRENT.
        if (other->from_.size() != to_.size()) return false;
        for (size_t k = 0; k < to_.size(); ++k) {
            if (!Vec3Eq(other->from_[k].pos, to_[k].pos) ||
                !Vec3Eq(other->from_[k].scale, to_[k].scale) ||
                !QuatEq(other->from_[k].rot, to_[k].rot))
                return false;
        }
        to_ = other->to_;
        return true;
    }
    void Seal() { mergeable_ = false; }
    // Merges a continuation of the same drag: keep the original `from`, adopt
    // the newer `to`.
    void MergeTo(std::vector<Transform3> to) { to_ = std::move(to); }

private:
    void Write(const std::vector<Transform3>& v) {
        for (size_t k = 0; k < indices_.size() && k < v.size(); ++k) {
            SceneEntity& e = (*entities_)[static_cast<size_t>(indices_[k])];
            e.pos = v[k].pos;
            e.rot = v[k].rot;
            e.scale = v[k].scale;
        }
    }
    std::vector<SceneEntity>* entities_;
    std::vector<int> indices_;
    std::vector<Transform3> from_;
    std::vector<Transform3> to_;
    bool mergeable_ = true;
};
inline void ApplyScriptField(SceneEntity& e, const ScriptFieldEdit& v) {
    if (v.index >= e.scripts.size()) return;
    SceneScriptFields& f = e.scripts[v.index];
    if (v.field == "backend") f.backend = v.value.GetString("lua");
    else if (v.field == "path") f.path = v.value.GetString();
    else if (v.field == "vars") f.vars = v.value;
}
inline bool ValuesEqual(const ScriptFieldEdit& a, const ScriptFieldEdit& b) {
    return a.index == b.index && a.field == b.field &&
           core::JsonWriter::Write(a.value) == core::JsonWriter::Write(b.value);
}

// Health component (flattened hp/maxHp): one undo step for attach/detach.
struct HealthValue {
    float hp = 0.0f;
    float maxHp = 0.0f;
};
inline void ApplyHealth(SceneEntity& e, const HealthValue& v) {
    e.hp = v.hp;
    e.maxHp = v.maxHp;
}
inline bool ValuesEqual(const HealthValue& a, const HealthValue& b) {
    return a.hp == b.hp && a.maxHp == b.maxHp;
}
inline void ApplyHpProp(SceneEntity& e, const float& v) { e.hp = v; }
inline void ApplyMaxHpProp(SceneEntity& e, const float& v) { e.maxHp = v; }

// A texture slot edit: the new path plus the texture handle already resolved
// through the AssetManager (resolved at command-construction time in the
// inspector, which has access to it). Empty path + invalid handle clears the
// slot.
struct TextureSlotValue {
    std::string path;
    gfx::TextureHandle handle;
};

inline void ApplyAlbedoTexSlot(SceneEntity& e, const TextureSlotValue& v) {
    e.albedoTex = v.path;
    e.material.albedo = v.handle;
}
inline void ApplyMRTexSlot(SceneEntity& e, const TextureSlotValue& v) {
    e.mrTex = v.path;
    e.material.metallicRoughness = v.handle;
}
inline void ApplyAOTexSlot(SceneEntity& e, const TextureSlotValue& v) {
    e.aoTex = v.path;
    e.material.occlusion = v.handle;
}
inline void ApplyEmissiveTexSlot(SceneEntity& e, const TextureSlotValue& v) {
    e.emissiveTex = v.path;
    e.material.emissive = v.handle;
}

// A material-ball asset applied to an entity: the reference path plus the
// expanded params (Unity .mat / Godot Material style).
struct MaterialAssetValue {
    std::string ref;          // materials/<name>.mat.json ("" = embedded)
    std::string colorHex;     // "#RRGGBB"
    float metallic = 0.0f;
    float roughness = 0.8f;
    float ao = 1.0f;
    float emissiveIntensity = 1.0f;
    std::string albedoTex, mrTex, aoTex, emissiveTex;
};

inline void ApplyMaterialAssetProp(SceneEntity& e, const MaterialAssetValue& v) {
    e.materialRef = v.ref;
    e.tint = HexToColor(v.colorHex);
    e.metallic = v.metallic;
    e.roughness = v.roughness;
    e.ao = v.ao;
    e.emissiveIntensity = v.emissiveIntensity;
    e.albedoTex = v.albedoTex;
    e.mrTex = v.mrTex;
    e.aoTex = v.aoTex;
    e.emissiveTex = v.emissiveTex;
}

namespace {
inline bool ValuesEqual(const float& a, const float& b) { return a == b; }
inline bool ValuesEqual(const std::string& a, const std::string& b) { return a == b; }
inline bool ValuesEqual(const MaterialAssetValue& a, const MaterialAssetValue& b) {
    return a.ref == b.ref && a.colorHex == b.colorHex && a.metallic == b.metallic &&
           a.roughness == b.roughness && a.ao == b.ao &&
           a.emissiveIntensity == b.emissiveIntensity && a.albedoTex == b.albedoTex &&
           a.mrTex == b.mrTex && a.aoTex == b.aoTex && a.emissiveTex == b.emissiveTex;
}
inline bool ValuesEqual(const TextureSlotValue& a, const TextureSlotValue& b) {
    return a.path == b.path;
}
inline bool ValuesEqual(const gfx::Color& a, const gfx::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
inline bool ValuesEqual(const SceneScriptFields& a, const SceneScriptFields& b) {
    return ScriptFieldsEqual(a, b);
}
inline bool ValuesEqual(const core::Json& a, const core::Json& b) {
    return core::JsonWriter::Write(a) == core::JsonWriter::Write(b);
}
} // namespace

// Single-field property edit (color / metallic / roughness / name). Merges
// consecutive edits of the SAME field whose value chain is continuous
// (incoming.old == current applied value), so one slider drag collapses into a
// single undo step. `mergeable=false` (name InputText) keeps every keystroke
// its own undo step, matching text-editor behavior.
template <typename T>
class EditPropertyCommand : public Command {
public:
    EditPropertyCommand(std::vector<SceneEntity>* entities, int index,
                        void (*apply)(SceneEntity&, const T&), T oldValue, T newValue,
                        bool mergeable = true)
        : entities_(entities), index_(index), apply_(apply), old_(std::move(oldValue)),
          cur_(std::move(newValue)), mergeable_(mergeable) {}

    void Apply() override { apply_((*entities_)[static_cast<size_t>(index_)], cur_); }
    void Undo() override { apply_((*entities_)[static_cast<size_t>(index_)], old_); }
    bool Merge(const Command& incoming) override {
        if (!mergeable_) return false;
        const EditPropertyCommand* other = dynamic_cast<const EditPropertyCommand*>(&incoming);
        if (!other || other->index_ != index_ || other->apply_ != apply_) return false;
        if (!ValuesEqual(other->old_, cur_)) return false; // not a consecutive chain
        cur_ = other->cur_;
        return true;
    }
    bool IsNoop() const override { return ValuesEqual(old_, cur_); }

private:
    std::vector<SceneEntity>* entities_;
    int index_;
    void (*apply_)(SceneEntity&, const T&);
    T old_;
    T cur_;
    bool mergeable_;
};

// Forward declaration of the canonical single-field setter (defined below with
// the rest of the G5-4-4 bridge); EditComponentCommand routes through it.
inline void SetComponentField(SceneEntity& e, const std::string& name,
                              const std::string& fieldKey, const core::Json& v);

// Schema-driven component field edit: writes one field of the component's
// canonical JSON (dedicated built-ins map to flattened fields, data components
// to extraComponents), routed through the bridge below. Consecutive edits of
// the same field merge into a single undo step (like EditPropertyCommand).
class EditComponentCommand : public Command {
public:
    EditComponentCommand(std::vector<SceneEntity>* entities, int index,
                         std::string component, std::string fieldKey, core::Json oldValue,
                         core::Json newValue)
        : entities_(entities), index_(index), component_(std::move(component)),
          fieldKey_(std::move(fieldKey)), old_(std::move(oldValue)), cur_(std::move(newValue)) {}

    void Apply() override { Set(cur_); }
    void Undo() override { Set(old_); }
    bool Merge(const Command& incoming) override {
        const EditComponentCommand* other =
            dynamic_cast<const EditComponentCommand*>(&incoming);
        if (!other || other->index_ != index_ || other->component_ != component_ ||
            other->fieldKey_ != fieldKey_)
            return false;
        if (!ValuesEqual(other->old_, cur_)) return false; // not a consecutive chain
        cur_ = other->cur_;
        return true;
    }
    bool IsNoop() const override { return ValuesEqual(old_, cur_); }

private:
    void Set(const core::Json& v) {
        SceneEntity& e = (*entities_)[static_cast<size_t>(index_)];
        // G5-4-4: route through the canonical bridge so dedicated components
        // (health/groups/...) and data components (extraComponents) share the
        // same single-field undo path.
        SetComponentField(e, component_, fieldKey_, v);
    }

    std::vector<SceneEntity>* entities_;
    int index_;
    std::string component_;
    std::string fieldKey_;
    core::Json old_;
    core::Json cur_;
};

// Adds or removes a whole extra component on one entity (undo restores the
// component's previous JSON / absence). Used by the inspector's 添加组件 /
// 移除 buttons so component edits are first-class undoable scene edits.
class AddComponentCommand : public Command {
public:
    AddComponentCommand(std::vector<SceneEntity>* entities, int index,
                        std::string component, core::Json data, bool remove)
        : entities_(entities), index_(index), component_(std::move(component)),
          data_(std::move(data)), remove_(remove) {}

    void Apply() override {
        SceneEntity& e = (*entities_)[static_cast<size_t>(index_)];
        if (remove_)
            e.extraComponents.erase(component_);
        else
            e.extraComponents[component_] = data_;
    }
    void Undo() override {
        SceneEntity& e = (*entities_)[static_cast<size_t>(index_)];
        if (remove_)
            e.extraComponents[component_] = data_;
        else
            e.extraComponents.erase(component_);
    }
    bool IsNoop() const override { return false; }

private:
    std::vector<SceneEntity>* entities_;
    int index_;
    std::string component_;
    core::Json data_;
    bool remove_;
};

// Mesh-key edit: swaps an entity's mesh and re-resolves it (mesh + material
// stay in sync on both undo and redo). Needs the editor app for ResolveMesh.
class EditMeshKeyCommand : public Command {
public:
    EditMeshKeyCommand(EditorApp* app, std::vector<SceneEntity>* entities, int index,
                       std::string oldKey, std::string newKey)
        : app_(app), entities_(entities), index_(index), old_(std::move(oldKey)),
          cur_(std::move(newKey)) {}

    void Apply() override { Set(cur_); }
    void Undo() override { Set(old_); }
    bool Merge(const Command&) override { return false; }
    bool IsNoop() const override { return old_ == cur_; }

private:
    void Set(const std::string& key) {
        SceneEntity& e = (*entities_)[static_cast<size_t>(index_)];
        e.meshKey = key;
        if (key.empty()) {
            // Mesh component removed: drop the resolved mesh so the viewport
            // stops rendering it (sprite/logical entities re-resolve below).
            e.mesh = gfx::Mesh{};
            e.material = gfx::Material{};
        }
        if (app_ && app_->ResolveMesh(e)) app_->ApplyMaterialParams(e);
    }

    EditorApp* app_;
    std::vector<SceneEntity>* entities_;
    int index_;
    std::string old_;
    std::string cur_;
};

// G5-4-4: canonical component-JSON bridge. Every component — built-in
// (health/groups/sortOrder/camera/type/transform) or data (extraComponents) —
// is addressed as {name, fieldKey} JSON, so the inspector renders ALL of them
// through the SAME schema-driven field editor (one renderer, one undo path).
// Built-in components map to the editor's flattened fields; data components
// map to SceneEntity::extraComponents.

namespace {
inline core::Json JsonNum(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}
inline core::Json JsonBool(bool b) {
    core::Json j;
    j.type_ = core::Json::Type::Bool;
    j.bool_ = b;
    return j;
}
inline core::Json JsonVec3(const math::Vec3& v) {
    core::Json j;
    j.type_ = core::Json::Type::Array;
    j.array_ = {JsonNum(v.x), JsonNum(v.y), JsonNum(v.z)};
    return j;
}
inline math::Vec3 Vec3From(const core::Json* a) {
    if (a && a->IsArray() && a->Size() == 3) {
        return {static_cast<float>(a->At(0)->GetNumber()),
                static_cast<float>(a->At(1)->GetNumber()),
                static_cast<float>(a->At(2)->GetNumber())};
    }
    return {};
}
} // namespace

// Whether the component is currently attached to this entity.
inline bool ComponentPresent(const SceneEntity& e, const std::string& name) {
    if (name == "health") return e.maxHp > 0.0f;
    if (name == "sortOrder") return true; // zOrder always exists (2D sort)
    if (name == "camera") return e.nodeType == "Camera3D";
    if (name == "transform") return true;
    return e.extraComponents.count(name) != 0;
}

// Current component JSON (the shape its ComponentSchema declares).
inline core::Json ComponentJson(const SceneEntity& e, const std::string& name) {
    if (name == "health")
        return core::Json::Parse("{\"hp\":" + std::to_string(static_cast<double>(e.hp)) +
                                 ",\"maxHp\":" +
                                 std::to_string(static_cast<double>(e.maxHp)) + "}");
    if (name == "sortOrder") {
        core::Json j;
        j.type_ = core::Json::Type::Object;
        j.object_["z"] = JsonNum(e.zOrder);
        return j;
    }
    if (name == "camera") {
        core::Json j;
        j.type_ = core::Json::Type::Object;
        j.object_["fov"] = JsonNum(e.cameraFov);
        j.object_["ortho"] = JsonBool(e.cameraOrtho);
        j.object_["orthoSize"] = JsonNum(e.cameraOrthoSize);
        return j;
    }
    if (name == "transform") {
        core::Json j;
        j.type_ = core::Json::Type::Object;
        j.object_["pos"] = JsonVec3(e.pos);
        j.object_["scale"] = JsonVec3(e.scale);
        const math::Vec3 euler = e.rot.ToEulerRad();
        j.object_["rot"] = JsonVec3({euler.x * math::kRadToDeg, euler.y * math::kRadToDeg,
                                     euler.z * math::kRadToDeg});
        return j;
    }
    auto it = e.extraComponents.find(name);
    return it != e.extraComponents.end() ? it->second : core::Json{};
}

// Applies a whole component JSON (dedicated -> flattened fields; empty object
// resets a dedicated component; data -> extraComponents, empty erases).
inline void ApplyComponentJson(SceneEntity& e, const std::string& name,
                               const core::Json& data) {
    if (name == "health") {
        if (!data.IsObject()) {
            e.hp = 0.0f;
            e.maxHp = 0.0f;
            return;
        }
        if (const core::Json* v = data.Get("hp")) e.hp = static_cast<float>(v->GetNumber());
        if (const core::Json* v = data.Get("maxHp")) e.maxHp = static_cast<float>(v->GetNumber());
        return;
    }
    if (name == "sortOrder") {
        if (const core::Json* v = data.Get("z")) e.zOrder = static_cast<float>(v->GetNumber());
        return;
    }
    if (name == "camera") {
        if (const core::Json* v = data.Get("fov")) e.cameraFov = static_cast<float>(v->GetNumber());
        if (const core::Json* v = data.Get("ortho")) e.cameraOrtho = v->GetBool();
        if (const core::Json* v = data.Get("orthoSize"))
            e.cameraOrthoSize = static_cast<float>(v->GetNumber());
        return;
    }
    if (name == "transform") {
        if (const core::Json* v = data.Get("pos")) e.pos = Vec3From(v);
        if (const core::Json* v = data.Get("scale")) e.scale = Vec3From(v);
        if (const core::Json* v = data.Get("rot")) {
            const math::Vec3 deg = Vec3From(v);
            e.rot = math::Quat::FromEuler(deg.x * math::kDegToRad, deg.y * math::kDegToRad,
                                          deg.z * math::kDegToRad);
        }
        return;
    }
    // Data component.
    if (data.IsObject() && data.object_.empty())
        e.extraComponents.erase(name);
    else
        e.extraComponents[name] = data;
}

// Single-field set (used by EditComponentCommand undo/redo).
inline void SetComponentField(SceneEntity& e, const std::string& name,
                              const std::string& fieldKey, const core::Json& v) {
    if (name == "health") {
        if (fieldKey == "hp") e.hp = static_cast<float>(v.GetNumber());
        if (fieldKey == "maxHp") e.maxHp = static_cast<float>(v.GetNumber());
        return;
    }
    if (name == "sortOrder") {
        if (fieldKey == "z") e.zOrder = static_cast<float>(v.GetNumber());
        return;
    }
    if (name == "camera") {
        if (fieldKey == "fov") e.cameraFov = static_cast<float>(v.GetNumber());
        if (fieldKey == "ortho") e.cameraOrtho = v.GetBool();
        if (fieldKey == "orthoSize") e.cameraOrthoSize = static_cast<float>(v.GetNumber());
        return;
    }
    if (name == "transform") {
        if (fieldKey == "pos") e.pos = Vec3From(&v);
        if (fieldKey == "scale") e.scale = Vec3From(&v);
        if (fieldKey == "rot") {
            const math::Vec3 deg = Vec3From(&v);
            e.rot = math::Quat::FromEuler(deg.x * math::kDegToRad, deg.y * math::kDegToRad,
                                          deg.z * math::kDegToRad);
        }
        return;
    }
    e.extraComponents[name].object_[fieldKey] = v;
}

// Applies a whole component JSON as an undoable edit (dedicated components
// include removable ones — ApplyComponentJson(e, name, {}) detaches).
class ComponentJsonCommand : public Command {public:
    ComponentJsonCommand(std::vector<SceneEntity>* entities, int index, std::string name,
                         core::Json oldData, core::Json newData)
        : entities_(entities), index_(index), name_(std::move(name)),
          old_(std::move(oldData)), new_(std::move(newData)) {}

    void Apply() override { ApplyComponentJson((*entities_)[static_cast<size_t>(index_)], name_, new_); }
    void Undo() override { ApplyComponentJson((*entities_)[static_cast<size_t>(index_)], name_, old_); }
    bool Merge(const Command&) override { return false; }
    bool IsNoop() const override { return ValuesEqual(old_, new_); }

private:
    std::vector<SceneEntity>* entities_;
    int index_;
    std::string name_;
    core::Json old_;
    core::Json new_;
};

// G5-4-4(项1): replaces a whole entity (used by "重置为预制体" — reverting an
// instance's component overrides back to the prefab template).
class ReplaceEntityCommand : public Command {
public:
    ReplaceEntityCommand(std::vector<SceneEntity>* entities, int index, SceneEntity newEntity)
        : entities_(entities), index_(index),
          old_((*entities)[static_cast<size_t>(index_)]), new_(std::move(newEntity)) {}

    void Apply() override { (*entities_)[static_cast<size_t>(index_)] = new_; }
    void Undo() override { (*entities_)[static_cast<size_t>(index_)] = old_; }
    bool Merge(const Command&) override { return false; }
    bool IsNoop() const override { return false; }

private:
    std::vector<SceneEntity>* entities_;
    int index_;
    SceneEntity old_;
    SceneEntity new_;
};

} // namespace neon::editor

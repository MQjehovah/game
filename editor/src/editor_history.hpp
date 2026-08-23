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
inline void ApplyParentProp(SceneEntity& e, const std::string& v) { e.parent = v; }

// Script component edit: attaches/replaces/clears the entity's script
// (backend/path/vars) in one undo step. A detach is just an edit whose new
// fields are all empty. Defined here (not in script_panel_model.hpp) because it
// needs the full SceneEntity type.
inline void ApplyScriptFields(SceneEntity& e, const SceneScriptFields& v) {
    e.scriptBackend = v.backend;
    e.scriptPath = v.path;
    e.scriptVars = v.vars;
}

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

namespace {
inline bool ValuesEqual(const float& a, const float& b) { return a == b; }
inline bool ValuesEqual(const std::string& a, const std::string& b) { return a == b; }
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

// Schema-driven component field edit: writes one field of
// SceneEntity::extraComponents[component][fieldKey]. Consecutive edits of the
// same field merge into a single undo step (like EditPropertyCommand).
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
        e.extraComponents[component_].object_[fieldKey_] = v;
    }

    std::vector<SceneEntity>* entities_;
    int index_;
    std::string component_;
    std::string fieldKey_;
    core::Json old_;
    core::Json cur_;
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
        if (app_ && app_->ResolveMesh(e)) app_->ApplyMaterialParams(e);
    }

    EditorApp* app_;
    std::vector<SceneEntity>* entities_;
    int index_;
    std::string old_;
    std::string cur_;
};

} // namespace neon::editor

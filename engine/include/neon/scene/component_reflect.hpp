#pragma once

// G2-1 lightweight reflection: a component's fields are declared ONCE (as
// pointer-to-member entries) and templates derive the editor ComponentSchema,
// JSON serialization and JSON parsing from that single list. Renaming a struct
// member breaks the Field() entry at compile time, so schema and runtime data
// cannot drift apart — no hand-written FieldSchema push_backs or field readers.
//
// Usage (declare the list next to the struct, e.g. SceneAudioSource):
//   struct Inventory {
//       int slots = 10;
//       float weight = 0.5f;
//       bool usable = true;
//       static const scene::FieldList<Inventory> kFields;
//       static scene::ComponentSchema Schema() { return {"inventory", "背包",
//           kFields.Schemas()}; }
//   };
//   const scene::FieldList<Inventory> Inventory::kFields = scene::ReflectFields(
//       scene::Field("slots",  "栏位", FieldType::Int,    &Inventory::slots,  10, 1,  100),
//       scene::Field("weight", "负重", FieldType::Number, &Inventory::weight, 0.5, 0, 1000),
//       scene::Field("usable", "可用", FieldType::Bool,   &Inventory::usable, 1,  0,  1));
//   // JSON round-trip for the runtime/script layer:
//   core::Json j = Inventory::kFields.ToJson(inst);
//   Inventory out; std::string err;
//   Inventory::kFields.FromJson(j, out, &err);

#include <cstddef>
#include <tuple>
#include <utility>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/gfx/color.hpp"
#include "neon/math/math.hpp"
#include "neon/scene/component_schema.hpp"

namespace neon::scene {

// A single field entry: the schema metadata plus the member pointer used to
// read/write the value on any instance of `Owner`.
template <typename Owner, typename T>
struct FieldRef {
    const char* key;
    const char* label;
    FieldType type;
    double def;
    double min;
    double max;
    double step;
    T Owner::* member;
};

template <typename Owner, typename T>
FieldRef<Owner, T> Field(const char* key, const char* label, FieldType type, T Owner::* member,
                         double def = 0.0, double min = 0.0, double max = 0.0,
                         double step = 0.01) {
    return {key, label, type, def, min, max, step, member};
}

namespace detail {

// JSON value from a C++ scalar/bool/string.
template <typename T>
core::Json ScalarToJson(T v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = static_cast<double>(v);
    return j;
}
inline core::Json ScalarToJson(bool v) {
    core::Json j;
    j.type_ = core::Json::Type::Bool;
    j.bool_ = v;
    return j;
}
inline core::Json ScalarToJson(const std::string& v) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = v;
    return j;
}
// C6: Vec3 -> JSON array [x,y,z]; Color -> JSON array [r,g,b,a].
inline core::Json ScalarToJson(const math::Vec3& v) {
    core::Json j;
    j.type_ = core::Json::Type::Array;
    j.array_.push_back(ScalarToJson(v.x));
    j.array_.push_back(ScalarToJson(v.y));
    j.array_.push_back(ScalarToJson(v.z));
    return j;
}
inline core::Json ScalarToJson(const gfx::Color& c) {
    core::Json j;
    j.type_ = core::Json::Type::Array;
    j.array_.push_back(ScalarToJson(c.r));
    j.array_.push_back(ScalarToJson(c.g));
    j.array_.push_back(ScalarToJson(c.b));
    j.array_.push_back(ScalarToJson(c.a));
    return j;
}
// C6: Quat -> JSON array [x,y,z,w] (row-vector convention used by transforms).
inline core::Json ScalarToJson(const math::Quat& q) {
    core::Json j;
    j.type_ = core::Json::Type::Array;
    j.array_.push_back(ScalarToJson(q.x));
    j.array_.push_back(ScalarToJson(q.y));
    j.array_.push_back(ScalarToJson(q.z));
    j.array_.push_back(ScalarToJson(q.w));
    return j;
}

template <typename T>
bool ScalarFromJson(const core::Json& j, T& out) {
    if (!j.IsNumber()) return false;
    out = static_cast<T>(j.GetNumber());
    return true;
}
inline bool ScalarFromJson(const core::Json& j, bool& out) {
    if (!j.IsBool() && !j.IsNumber()) return false;
    out = j.IsBool() ? j.GetBool() : j.GetNumber() != 0.0;
    return true;
}
inline bool ScalarFromJson(const core::Json& j, std::string& out) {
    if (!j.IsString()) return false;
    out = j.GetString();
    return true;
}
// C6: array-form Vec3/Color; out-of-range keeps the value and errors.
inline bool ScalarFromJson(const core::Json& j, math::Vec3& out) {
    if (!j.IsArray() || j.Size() < 3) return false;
    const core::Json* x = j.At(0);
    const core::Json* y = j.At(1);
    const core::Json* z = j.At(2);
    if (!x->IsNumber() || !y->IsNumber() || !z->IsNumber()) return false;
    out = {static_cast<float>(x->GetNumber()), static_cast<float>(y->GetNumber()),
           static_cast<float>(z->GetNumber())};
    return true;
}
inline bool ScalarFromJson(const core::Json& j, gfx::Color& out) {
    if (!j.IsArray() || j.Size() < 4) return false;
    const core::Json* r = j.At(0);
    const core::Json* g = j.At(1);
    const core::Json* b = j.At(2);
    const core::Json* a = j.At(3);
    if (!r->IsNumber() || !g->IsNumber() || !b->IsNumber() || !a->IsNumber()) return false;
    out = {static_cast<float>(r->GetNumber()), static_cast<float>(g->GetNumber()),
           static_cast<float>(b->GetNumber()), static_cast<float>(a->GetNumber())};
    return true;
}
// C6: array-form Quat [x,y,z,w].
inline bool ScalarFromJson(const core::Json& j, math::Quat& out) {
    if (!j.IsArray() || j.Size() < 4) return false;
    const core::Json* x = j.At(0);
    const core::Json* y = j.At(1);
    const core::Json* z = j.At(2);
    const core::Json* w = j.At(3);
    if (!x->IsNumber() || !y->IsNumber() || !z->IsNumber() || !w->IsNumber()) return false;
    out = {static_cast<float>(x->GetNumber()), static_cast<float>(y->GetNumber()),
           static_cast<float>(z->GetNumber()), static_cast<float>(w->GetNumber())};
    return true;
}

// Reads one reflected field; absent fields keep their current value.
template <typename Owner, typename T>
void ReflectReadField(const core::Json& json, const FieldRef<Owner, T>& f, Owner& obj, bool& ok,
                      std::string* err) {
    const core::Json* v = json.Get(f.key);
    if (!v) return;
    if (!detail::ScalarFromJson(*v, obj.*(f.member))) {
        if (err) *err = "field '" + std::string(f.key) + "' has the wrong type";
        ok = false;
    }
}

} // namespace detail

// The reflected field list for one component type: one source of truth for the
// editor schema and the JSON (de)serializers. Iteration expands the pack via an
// initializer list (MSVC-robust; fold expressions over std::get trigger C1001).
template <typename Owner, typename... Fs>
struct FieldList {
    std::tuple<Fs...> refs;

    template <std::size_t... I>
    std::vector<FieldSchema> SchemasImpl(std::index_sequence<I...>) const {
        std::vector<FieldSchema> out;
        out.reserve(sizeof...(Fs));
        using expander = int[];
        (void)expander{0,
                       (out.push_back(FieldSchema{std::get<I>(refs).key, std::get<I>(refs).label,
                                                  std::get<I>(refs).type, std::get<I>(refs).def,
                                                  std::get<I>(refs).min, std::get<I>(refs).max,
                                                  std::get<I>(refs).step}),
                        0)...};
        return out;
    }

    template <std::size_t... I>
    core::Json ToJsonImpl(const Owner& obj, std::index_sequence<I...>) const {
        core::Json out;
        out.type_ = core::Json::Type::Object;
        using expander = int[];
        (void)expander{0,
                       (out.object_[std::get<I>(refs).key] =
                            detail::ScalarToJson(obj.*(std::get<I>(refs).member)),
                        0)...};
        return out;
    }

    template <std::size_t... I>
    bool FromJsonImpl(const core::Json& json, Owner& obj, std::index_sequence<I...>,
                      std::string* err) const {
        if (!json.IsObject()) {
            if (err) *err = "component data must be a JSON object";
            return false;
        }
        bool ok = true;
        using expander = int[];
        (void)expander{0, (detail::ReflectReadField(json, std::get<I>(refs), obj, ok, err), 0)...};
        return ok;
    }

    std::vector<FieldSchema> Schemas() const { return SchemasImpl(std::index_sequence_for<Fs...>{}); }
    core::Json ToJson(const Owner& obj) const { return ToJsonImpl(obj, std::index_sequence_for<Fs...>{}); }
    bool FromJson(const core::Json& json, Owner& obj, std::string* err) const {
        return FromJsonImpl(json, obj, std::index_sequence_for<Fs...>{}, err);
    }
};

// CTAD helper: builds the FieldList from FieldRef entries. `Fs` here is the
// per-field VALUE type (int/float/...); the result stores the FieldRef pack.
template <typename Owner, typename... Fs>
FieldList<Owner, FieldRef<Owner, Fs>...> ReflectFields(FieldRef<Owner, Fs>... fields) {
    return {std::make_tuple(fields...)};
}

} // namespace neon::scene

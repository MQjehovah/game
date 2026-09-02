#pragma once

// NeonEngine reflection (G2-1 / C6 / C7). A component's fields are declared
// ONCE (as pointer-to-member entries) and templates derive:
//   * the editor ComponentSchema (G2-1), and
//   * the JSON serializer/parser (C6)
// from that single list �?so schema, serialization and runtime data cannot
// drift apart. Renaming a struct member breaks the Field() entry at compile
// time (the pointer-to-member will not resolve), which is exactly the drift
// guard the hand-written FieldSchema push_backs / field readers lacked.
//
// Design notes (compare Godot _bind_methods / Unity [SerializeField]+attributes
// / EnTT meta / RTTR):
//   * Non-intrusive: reflect a struct by declaring a `kFields` list beside it.
//     No reflection macros inside the struct body, no UHT-style codegen tool.
//   * Type-driven: `ReflectTraits<T>` knows how to (de)serialize any value and
//     describe it to the editor. Add a `ReflectTraits<T, void>` specialization
//     for a new value type (the <T, void> overload set is unambiguous).
//   * Field categories (Unity SerializeField / Godot export): Serialize
//     (runtime + editor + JSON), EditorOnly (editor only, never serialized),
//     Transient (runtime state, e.g. bodyId: neither serialized nor edited).
//   * Enums use magic_enum (vendored, header-only) for value<->name<->options.
//
// Usage:
//   struct Inventory {
//       int slots = 10;
//       float weight = 0.5f;
//       bool usable = true;
//       inline static const auto kFields = scene::ReflectFields(
//           scene::Field("slots", "栏位", FieldType::Int,    &Inventory::slots,  10, 1, 100),
//           scene::Field("weight", "负重", FieldType::Number, &Inventory::weight, 0.5, 0, 1000));
//       core::Json ToJson() const { return kFields.ToJson(*this); }
//   };

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "neon/core/json.hpp"
#include "neon/scene/component_schema.hpp"
#include "neon/scene/enum_reflect.hpp"
#include "neon/gfx/color.hpp"
#include "neon/math/quat.hpp"
#include "neon/math/vec2.hpp"
#include "neon/math/vec3.hpp"
#include "neon/math/vec4.hpp"

namespace neon::scene {

// ---- Field categories -------------------------------------------------------
// Serialize: runtime data, serialized to JSON, editable in the inspector.
// EditorOnly: inspector-only metadata, never serialized (gameplay ignores it).
// Transient:  runtime state only (e.g. physics bodyId); neither serialized nor edited.
enum class FieldCategory : uint8_t { Serialize, EditorOnly, Transient };

// ---- Reflection value kinds -------------------------------------------------
enum class ValueKind : uint8_t {
    Bool, Int, UInt, Float, Double, String,
    Vec2, Vec3, Vec4, Quat, Color, Enum, Array, Struct, Json,
};

namespace reflect_detail {

// True when a type declares a `kFields` member (a reflected aggregate).
template <typename T, typename = void>
struct HasReflectedFields : std::false_type {};
template <typename T>
struct HasReflectedFields<T, std::void_t<decltype(T::kFields)>> : std::true_type {};

} // namespace reflect_detail

// ---- ReflectTraits: the per-type value codec --------------------------------
// The primary template is intentionally undefined: reflecting a type without a
// specialization (or a `kFields` aggregate) is a compile error, catching
// "forgot to reflect this field" at build time instead of at runtime.
template <typename T, typename Enable = void>
struct ReflectTraits;

// Integral scalars (int, uint, ...). bool is handled below.
template <typename T>
struct ReflectTraits<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                                         !std::is_enum_v<T>>> {
    static constexpr ValueKind kKind = std::is_signed_v<T> ? ValueKind::Int : ValueKind::UInt;
    static core::Json ToJson(const T& v) {
        core::Json j;
        j.type_ = core::Json::Type::Number;
        j.number_ = static_cast<double>(v);
        return j;
    }
    static bool FromJson(const core::Json& j, T& out, std::string*) {
        if (!j.IsNumber()) return false;
        out = static_cast<T>(j.GetNumber());
        return true;
    }
    static T Default() { return T{}; }
};
// Floating-point scalars.
template <typename T>
struct ReflectTraits<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    static constexpr ValueKind kKind = std::is_same_v<T, float> ? ValueKind::Float : ValueKind::Double;
    static core::Json ToJson(const T& v) {
        core::Json j;
        j.type_ = core::Json::Type::Number;
        j.number_ = static_cast<double>(v);
        return j;
    }
    static bool FromJson(const core::Json& j, T& out, std::string*) {
        if (!j.IsNumber()) return false;
        out = static_cast<T>(j.GetNumber());
        return true;
    }
    static T Default() { return T{}; }
};
template <>
struct ReflectTraits<bool, void> {
    static constexpr ValueKind kKind = ValueKind::Bool;
    static core::Json ToJson(const bool& v) {
        core::Json j;
        j.type_ = core::Json::Type::Bool;
        j.bool_ = v;
        return j;
    }
    static bool FromJson(const core::Json& j, bool& out, std::string*) {
        if (j.IsBool()) { out = j.GetBool(); return true; }
        if (j.IsNumber()) { out = j.GetNumber() != 0.0; return true; }
        return false;
    }
    static constexpr bool Default() { return false; }
};
template <>
struct ReflectTraits<std::string, void> {
    static constexpr ValueKind kKind = ValueKind::String;
    static core::Json ToJson(const std::string& v) {
        core::Json j;
        j.type_ = core::Json::Type::String;
        j.string_ = v;
        return j;
    }
    static bool FromJson(const core::Json& j, std::string& out, std::string*) {
        if (!j.IsString()) return false;
        out = j.string_;
        return true;
    }
    static std::string Default() { return {}; }
};

namespace reflect_detail {
inline core::Json NumberJson(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}
inline core::Json JsonFromArray(std::vector<core::Json> items) {
    core::Json j;
    j.type_ = core::Json::Type::Array;
    j.array_ = std::move(items);
    return j;
}
inline bool Get3(const core::Json& j, double& x, double& y, double& z) {
    if (!j.IsArray() || j.Size() < 3) return false;
    const core::Json* a = j.At(0);
    const core::Json* b = j.At(1);
    const core::Json* c = j.At(2);
    if (!a->IsNumber() || !b->IsNumber() || !c->IsNumber()) return false;
    x = a->GetNumber(); y = b->GetNumber(); z = c->GetNumber();
    return true;
}
inline bool Get4(const core::Json& j, double& x, double& y, double& z, double& w) {
    if (!j.IsArray() || j.Size() < 4) return false;
    const core::Json* a = j.At(0);
    const core::Json* b = j.At(1);
    const core::Json* c = j.At(2);
    const core::Json* d = j.At(3);
    if (!a->IsNumber() || !b->IsNumber() || !c->IsNumber() || !d->IsNumber()) return false;
    x = a->GetNumber(); y = b->GetNumber(); z = c->GetNumber(); w = d->GetNumber();
    return true;
}
} // namespace reflect_detail

// Vec2 -> [x,y]; Vec3 -> [x,y,z]; Vec4/Quat -> [x,y,z,w]; Color -> [r,g,b,a].
template <>
struct ReflectTraits<math::Vec2, void> {
    static constexpr ValueKind kKind = ValueKind::Vec2;
    static core::Json ToJson(const math::Vec2& v) {
        return reflect_detail::JsonFromArray({reflect_detail::NumberJson(v.x),
                                              reflect_detail::NumberJson(v.y)});
    }
    static bool FromJson(const core::Json& j, math::Vec2& out, std::string*) {
        double x, y;
        if (!reflect_detail::Get3(j, x, y, x)) return false; // reuses 3 for first two
        out = {static_cast<float>(x), static_cast<float>(y)};
        return true;
    }
    static math::Vec2 Default() { return {}; }
};
template <>
struct ReflectTraits<math::Vec3, void> {
    static constexpr ValueKind kKind = ValueKind::Vec3;
    static core::Json ToJson(const math::Vec3& v) {
        return reflect_detail::JsonFromArray({reflect_detail::NumberJson(v.x),
                                              reflect_detail::NumberJson(v.y),
                                              reflect_detail::NumberJson(v.z)});
    }
    static bool FromJson(const core::Json& j, math::Vec3& out, std::string*) {
        double x, y, z;
        if (!reflect_detail::Get3(j, x, y, z)) return false;
        out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
        return true;
    }
    static math::Vec3 Default() { return {}; }
};
template <>
struct ReflectTraits<math::Vec4, void> {
    static constexpr ValueKind kKind = ValueKind::Vec4;
    static core::Json ToJson(const math::Vec4& v) {
        return reflect_detail::JsonFromArray({reflect_detail::NumberJson(v.x),
                                              reflect_detail::NumberJson(v.y),
                                              reflect_detail::NumberJson(v.z),
                                              reflect_detail::NumberJson(v.w)});
    }
    static bool FromJson(const core::Json& j, math::Vec4& out, std::string*) {
        double x, y, z, w;
        if (!reflect_detail::Get4(j, x, y, z, w)) return false;
        out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
               static_cast<float>(w)};
        return true;
    }
    static math::Vec4 Default() { return {}; }
};
template <>
struct ReflectTraits<math::Quat, void> {
    static constexpr ValueKind kKind = ValueKind::Quat;
    static core::Json ToJson(const math::Quat& v) {
        return reflect_detail::JsonFromArray({reflect_detail::NumberJson(v.x),
                                              reflect_detail::NumberJson(v.y),
                                              reflect_detail::NumberJson(v.z),
                                              reflect_detail::NumberJson(v.w)});
    }
    static bool FromJson(const core::Json& j, math::Quat& out, std::string*) {
        double x, y, z, w;
        if (!reflect_detail::Get4(j, x, y, z, w)) return false;
        out = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
               static_cast<float>(w)};
        return true;
    }
    static math::Quat Default() { return {}; }
};
template <>
struct ReflectTraits<gfx::Color, void> {
    static constexpr ValueKind kKind = ValueKind::Color;
    static core::Json ToJson(const gfx::Color& v) {
        return reflect_detail::JsonFromArray({reflect_detail::NumberJson(v.r),
                                              reflect_detail::NumberJson(v.g),
                                              reflect_detail::NumberJson(v.b),
                                              reflect_detail::NumberJson(v.a)});
    }
    static bool FromJson(const core::Json& j, gfx::Color& out, std::string*) {
        double r, g, b, a;
        if (!reflect_detail::Get4(j, r, g, b, a)) return false;
        out = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b),
               static_cast<float>(a)};
        return true;
    }
    static gfx::Color Default() { return {}; }
};
template <>
struct ReflectTraits<core::Json, void> {
    static constexpr ValueKind kKind = ValueKind::Json;
    static core::Json ToJson(const core::Json& v) { return v; }
    static bool FromJson(const core::Json& j, core::Json& out, std::string*) {
        out = j;
        return true;
    }
    static core::Json Default() { return {}; }
};

namespace reflect_detail {

// Stable editor combo options for a reflected enum (see enum_reflect.hpp): the
// `const char* const*` view for FieldSchema::options, alive for the schema run.
template <typename E>
struct EnumOptions {
    static const EnumOptions& Get() {
        static EnumOptions inst;
        return inst;
    }
    EnumOptions()
        : names(EnumSpecs<E>::Names()), count_(EnumSpecs<E>::Count()) {}
    int count() const { return count_; }
    const char* const* names;
    int count_;
};

} // namespace reflect_detail

// Enum -> string (see enum_reflect.hpp). Schema options come from NEO_ENUM.
template <typename T>
struct ReflectTraits<T, std::enable_if_t<std::is_enum_v<T> && EnumSpecs<T>::Enabled>> {
    static constexpr ValueKind kKind = ValueKind::Enum;
    static core::Json ToJson(const T& v) {
        core::Json j;
        j.type_ = core::Json::Type::String;
        j.string_ = EnumSpecs<T>::ToString(v);
        return j;
    }
    static bool FromJson(const core::Json& j, T& out, std::string* err = nullptr) {
        if (!j.IsString()) return false;
        if (!EnumSpecs<T>::FromString(j.string_.c_str(), out)) {
            out = static_cast<T>(0);
            if (err) *err = "invalid enum value";
            return false;
        }
        return true;
    }
    static T Default() { return static_cast<T>(0); }
    static const reflect_detail::EnumOptions<T>& Options() {
        return reflect_detail::EnumOptions<T>::Get();
    }
};

// std::vector<T> -> JSON array (any reflectable element).
template <typename T>
struct ReflectTraits<std::vector<T>, void> {
    static constexpr ValueKind kKind = ValueKind::Array;
    using Elem = ReflectTraits<T>;
    static core::Json ToJson(const std::vector<T>& v) {
        core::Json j;
        j.type_ = core::Json::Type::Array;
        for (const T& e : v) j.array_.push_back(Elem::ToJson(e));
        return j;
    }
    static bool FromJson(const core::Json& j, std::vector<T>& out, std::string* err = nullptr) {
        if (!j.IsArray()) return false;
        out.clear();
        out.reserve(j.Size());
        for (size_t i = 0; i < j.Size(); ++i) {
            T e{};
            if (!Elem::FromJson(*j.At(i), e, err)) return false;
            out.push_back(std::move(e));
        }
        return true;
    }
    static std::vector<T> Default() { return {}; }
};

// A nested reflected aggregate (any type declaring a static `kFields`).
template <typename T>
struct ReflectTraits<T, std::enable_if_t<reflect_detail::HasReflectedFields<T>::value>> {
    static constexpr ValueKind kKind = ValueKind::Struct;
    static core::Json ToJson(const T& v) { return T::kFields.ToJson(v); }
    static bool FromJson(const core::Json& j, T& out, std::string* err = nullptr) {
        return T::kFields.FromJson(j, out, err);
    }
    static T Default() { return T{}; }
};

// ---- Field description --------------------------------------------------------
// One reflected field: schema metadata + the member pointer used to read/write
// any instance of `Owner`. `type` is the editor FieldType; a string field shown
// as an enum (e.g. shape="sphere"|"box") passes FieldType::Enum (+ options).
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
    FieldCategory category = FieldCategory::Serialize;
    const char* header = nullptr;
    const char* tooltip = nullptr;
    const char* resourceKind = nullptr;
    const char* widget = nullptr;
    const char* const* options = nullptr;
    int optionCount = 0;
};

// Optional metadata (category + editor hints) for the rich Field() overload.
struct FieldMeta {
    FieldCategory category = FieldCategory::Serialize;
    const char* header = nullptr;
    const char* tooltip = nullptr;
    const char* resourceKind = nullptr;
    const char* widget = nullptr;
    const char* const* options = nullptr;
    int optionCount = 0;
};

// Primary factory (keeps the historical positional signature, trailing defaults
// so existing callers that used only 8 args are unchanged).
template <typename Owner, typename T>
FieldRef<Owner, T> Field(const char* key, const char* label, FieldType type, T Owner::* member,
                         double def = 0.0, double min = 0.0, double max = 0.0,
                         double step = 0.01) {
    return {key, label, type, def, min, max, step, member};
}

// Rich variant with category + editor hints (defaults def/min/max/step).
template <typename Owner, typename T>
FieldRef<Owner, T> Field(const char* key, const char* label, FieldType type, T Owner::* member,
                         const FieldMeta& meta) {
    return {key, label, type, 0.0, 0.0, 0.0, 0.01, member, meta.category, meta.header,
            meta.tooltip, meta.resourceKind, meta.widget, meta.options, meta.optionCount};
}

// Rich variant with category + editor hints + explicit def/min/max/step.
template <typename Owner, typename T>
FieldRef<Owner, T> Field(const char* key, const char* label, FieldType type, T Owner::* member,
                         double def, double min, double max, double step, const FieldMeta& meta) {
    return {key, label, type, def, min, max, step, member, meta.category, meta.header,
            meta.tooltip, meta.resourceKind, meta.widget, meta.options, meta.optionCount};
}

namespace reflect_detail {

template <typename Owner, typename T>
FieldSchema MakeSchema(const FieldRef<Owner, T>& f) {
    FieldSchema s;
    s.key = f.key;
    s.label = f.label;
    s.type = f.type;
    s.def = f.def;
    s.min = f.min;
    s.max = f.max;
    s.step = f.step;
    s.resourceKind = f.resourceKind;
    s.widget = f.widget;
    s.header = f.header;
    s.tooltip = f.tooltip;
    s.options = f.options;
    s.optionCount = f.optionCount;

    // A genuine enum member (not a string shown as an enum) gets its option list
    // from NEO_ENUM, so the inspector combo needs no hand-written arrays. The
    // outer `if constexpr` keeps the Options() call from being instantiated for
    // non-enum field types.
    if constexpr (std::is_enum_v<T> && EnumSpecs<T>::Enabled) {
        if (f.type == FieldType::Enum && f.optionCount == 0) {
            auto& opt = ReflectTraits<T>::Options();
            s.options = opt.names;
            s.optionCount = opt.count();
        }
    }
    return s;
}

} // namespace reflect_detail

// The reflected field list for one component type (the Godot _bind_methods
// analog): one source of truth for the editor schema and the JSON coders.
template <typename Owner, typename... Fs>
struct FieldList {
    std::tuple<Fs...> refs;

    template <std::size_t... I>
    std::vector<FieldSchema> SchemasImpl(std::index_sequence<I...>) const {
        std::vector<FieldSchema> out;
        out.reserve(sizeof...(Fs));
        (void)std::initializer_list<int>{
            0, (PushSchema(out, std::get<I>(refs)), 0)...};
        return out;
    }
    template <typename T>
    void PushSchema(std::vector<FieldSchema>& out, const FieldRef<Owner, T>& f) const {
        if (f.category != FieldCategory::Serialize) return;
        out.push_back(reflect_detail::MakeSchema(f));
    }

    // Editor schema for this component (G2-1). Omits Transient fields.
    std::vector<FieldSchema> Schemas() const {
        return SchemasImpl(std::index_sequence_for<Fs...>{});
    }

    // JSON object of the serializable fields (C6 serializer). Omits Transient.
    template <std::size_t... I>
    core::Json ToJsonImpl(const Owner& obj, std::index_sequence<I...>) const {
        core::Json out;
        out.type_ = core::Json::Type::Object;
        (void)std::initializer_list<int>{
            0, (WriteFieldTo(out, obj, std::get<I>(refs)), 0)...};
        return out;
    }
    template <typename T>
    void WriteFieldTo(core::Json& out, const Owner& obj, const FieldRef<Owner, T>& f) const {
        if (f.category != FieldCategory::Serialize) return;
        out.object_[f.key] = ReflectTraits<T>::ToJson(obj.*(f.member));
    }
    core::Json ToJson(const Owner& obj) const {
        return ToJsonImpl(obj, std::index_sequence_for<Fs...>{});
    }

    template <std::size_t... I>
    bool FromJsonImpl(const core::Json& json, Owner& obj, std::index_sequence<I...>,
                      std::string* err = nullptr) const {
        if (!json.IsObject()) {
            if (err) *err = "component data must be a JSON object";
            return false;
        }
        bool ok = true;
        (void)std::initializer_list<int>{
            0, (ReadField(json, obj, std::get<I>(refs), ok, err), 0)...};
        return ok;
    }
    template <typename T>
    void ReadField(const core::Json& json, Owner& obj, const FieldRef<Owner, T>& f, bool& ok,
                   std::string* err = nullptr) const {
        if (f.category != FieldCategory::Serialize) return;
        const core::Json* v = json.Get(f.key);
        if (!v) return; // absent -> keep current value
        if (!ReflectTraits<T>::FromJson(*v, obj.*(f.member), err)) {
            if (err) *err = "field '" + std::string(f.key) + "' has the wrong type";
            ok = false;
        }
    }
    bool FromJson(const core::Json& json, Owner& obj, std::string* err = nullptr) const {
        return FromJsonImpl(json, obj, std::index_sequence_for<Fs...>{}, err);
    }

    // Clone / equality: used by undo and deterministic snapshots. Clone copies
    // the whole struct (preserves every field, including EditorOnly metadata);
    // Equal compares the serialized (Serialize) fields, so two components with
    // identical runtime data but different transient/editor state compare equal.
    void Clone(const Owner& src, Owner& dst) const { dst = src; }
    bool Equal(const Owner& a, const Owner& b) const {
        return core::JsonEquals(ToJson(a), ToJson(b));
    }

    // Reset to the type's value-initialized defaults (respects field initializers).
    template <std::size_t... I>
    void ApplyDefaultsImpl(Owner& obj, std::index_sequence<I...>) const {
        (void)std::initializer_list<int>{0, (ResetField(obj, std::get<I>(refs)), 0)...};
    }
    template <typename T>
    void ResetField(Owner& obj, const FieldRef<Owner, T>& f) const {
        if (f.category != FieldCategory::Serialize) return;
        obj.*(f.member) = ReflectTraits<T>::Default();
    }
    void ApplyDefaults(Owner& obj) const {
        ApplyDefaultsImpl(obj, std::index_sequence_for<Fs...>{});
    }
};

// CTAD helper: builds the FieldList from Field entries (the pack stores the
// per-field value types).
template <typename Owner, typename... Fs>
FieldList<Owner, FieldRef<Owner, Fs>...> ReflectFields(FieldRef<Owner, Fs>... fields) {
    return {std::make_tuple(fields...)};
}

// True when `Owner` declares a reflectable `kFields` list. Used by the TypeRegistry.
template <typename T>
inline constexpr bool IsReflected_v = reflect_detail::HasReflectedFields<T>::value;

} // namespace neon::scene

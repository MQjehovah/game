#include "helpers.hpp"

// G2-1 / C6 reflection tests (component_reflect.hpp + enum_reflect.hpp +
// type_registry.hpp). These exercise the full value-codec matrix, the field
// categories (Serialize / EditorOnly / Transient), enum reflection via NEO_ENUM,
// nested reflected structs, arrays, and the TypeRegistry single-source lookup.

#include "neon/scene/component_reflect.hpp"
#include "neon/scene/component_schema.hpp"
#include "neon/scene/enum_reflect.hpp"
#include "neon/scene/scene_file.hpp"
#include "neon/scene/type_registry.hpp"

using namespace neon;
using namespace neon::scene;

enum class ArmorKind { None, Cone, Bucket };
NEO_ENUM(ArmorKind, None, Cone, Bucket)

struct Helpers {
    int boxes = 3;
    inline static const auto kFields = ReflectFields(
        Field("boxes", "盒子", FieldType::Int, &Helpers::boxes, 0, 0, 99));
};

struct Sample {
    int hp = 1;
    float speed = 2.0f;
    bool alive = true;
    std::string name = "abc";
    math::Vec3 pos{1, 2, 3};
    gfx::Color col{0.1f, 0.2f, 0.3f, 0.4f};
    ArmorKind kind = ArmorKind::Cone;
    std::vector<float> coeffs{0.5f, 0.25f};
    Helpers helper;             // nested reflected struct
    core::Json extra;           // passthrough
    uint32_t bodyId = 7;        // Transient: runtime-only, not serialized
    int editorTag = 42;         // EditorOnly: editable but not serialized

    inline static const auto kFields = ReflectFields(
        Field("hp", "生命", FieldType::Int, &Sample::hp, 0, 0, 9999),
        Field("speed", "速度", FieldType::Number, &Sample::speed),
        Field("alive", "存活", FieldType::Bool, &Sample::alive),
        Field("name", "名字", FieldType::String, &Sample::name),
        Field("pos", "位置", FieldType::Vec3, &Sample::pos),
        Field("col", "颜色", FieldType::Color, &Sample::col),
        Field("kind", "盔甲", FieldType::Enum, &Sample::kind),
        Field("coeffs", "系数", FieldType::Array, &Sample::coeffs),
        Field("helper", "内部", FieldType::Struct, &Sample::helper),
        Field("extra", "扩展", FieldType::Json, &Sample::extra),
        Field("bodyId", "物理体", FieldType::Int, &Sample::bodyId,
              FieldMeta{FieldCategory::Transient}),
        Field("editorTag", "仅编辑器", FieldType::Int, &Sample::editorTag,
              FieldMeta{FieldCategory::EditorOnly}));
};

TEST(reflect_schema_omits_transient) {
    auto schema = Sample::kFields.Schemas();
    bool sawBodyId = false;
    for (const auto& f : schema)
        if (f.key == "bodyId") sawBodyId = true;
    CHECK(!sawBodyId); // Transient excluded from the editor schema
}

TEST(reflect_schema_enum_options) {
    auto schema = Sample::kFields.Schemas();
    bool sawEnum = false;
    for (const auto& f : schema) {
        if (f.key == "kind") {
            sawEnum = f.type == FieldType::Enum && f.optionCount == 3;
        }
    }
    CHECK(sawEnum);
}

TEST(reflect_json_roundtrip) {
    core::Json j = Sample::kFields.ToJson(Sample{});
    CHECK(j.IsObject());
    CHECK(j.Get("bodyId") == nullptr);      // Transient: runtime-only, not emitted
    CHECK(j.Get("editorTag") == nullptr);   // EditorOnly: editor metadata, not serialized
    CHECK(j.Get("coeffs")->IsArray());
    CHECK_EQ(j.Get("coeffs")->Size(), (size_t)2);
    CHECK_EQ(j.Get("col")->Size(), (size_t)4); // [r,g,b,a]
    CHECK_EQ(j.Get("pos")->Size(), (size_t)3); // [x,y,z]

    Sample out;
    CHECK(Sample::kFields.FromJson(j, out));
    CHECK_EQ(out.hp, 1);
    CHECK_NEAR(out.pos.z, 3.0f, 1e-6);
    CHECK(out.kind == ArmorKind::Cone);
    CHECK_EQ(out.helper.boxes, 3);
}

TEST(reflect_enum_codec) {
    core::Json j = ReflectTraits<ArmorKind>::ToJson(ArmorKind::Bucket);
    CHECK(j.IsString());
    CHECK_EQ(j.GetString(), "Bucket");
    ArmorKind v = ArmorKind::None;
    CHECK(ReflectTraits<ArmorKind>::FromJson(j, v));
    CHECK(v == ArmorKind::Bucket);
}

TEST(reflect_nested_struct) {
    core::Json j = ReflectTraits<Helpers>::ToJson(Helpers{8});
    CHECK(j.Get("boxes")->GetInt() == 8);
    Helpers h;
    CHECK(ReflectTraits<Helpers>::FromJson(j, h));
    CHECK_EQ(h.boxes, 8);
}

TEST(reflect_vector_array) {
    std::vector<int> vi{2, 4, 6};
    core::Json j = ReflectTraits<std::vector<int>>::ToJson(vi);
    CHECK(j.IsArray());
    CHECK_EQ(j.Size(), (size_t)3);
    std::vector<int> vi2;
    CHECK(ReflectTraits<std::vector<int>>::FromJson(j, vi2));
    CHECK_EQ(vi2.size(), (size_t)3);
    CHECK_EQ(vi2[2], 6);
}

TEST(reflect_type_registry) {
    TypeRegistry::Clear();
    RegisterBuiltinReflectedTypes();
    CHECK(TypeRegistry::Has("health"));
    CHECK(TypeRegistry::Has("audio"));
    const TypeInfo* h = TypeRegistry::Find("health");
    CHECK(h != nullptr);
    if (h) {
        // health schema has hp/maxHp
        bool sawHp = false;
        for (const auto& f : h->fields)
            if (f.key == "hp") sawHp = true;
        CHECK(sawHp);
    }
    // Type-erased serializer round-trips a live SceneHealth.
    SceneHealth live;
    auto j = h->toJson(&live);
    SceneHealth copy;
    CHECK(h->fromJson(static_cast<const core::Json&>(j), &copy, nullptr));
}

#include "neon/scene/component_schema.hpp"

namespace neon::scene {
namespace {

const char* kPlantTypes[] = {"sunflower", "peashooter", "wallnut", "snowpea", "cherry"};
const char* kZombieTypes[] = {"basic", "cone", "bucket"};
// Dual script backends: the editor's script-component dropdown lets users pick
// the language; the runtime routes by this value ("lua" / "js").
const char* kScriptBackends[] = {"lua", "js"};
const char* kRigidBodyShapes[] = {"sphere", "box"};
const char* kNodeTypes[] = {"Node", "MeshInstance3D", "Camera3D",
                            "CharacterBody", "Sprite", "Light3D"};

std::vector<ComponentSchema> BuildSchemas() {
    std::vector<ComponentSchema> out;
    out.push_back({"transform", "变换",
                   {{"pos", "位置", FieldType::Vec3, 0, -100000, 100000, 0.1},
                    {"rot", "旋转 (欧拉角度)", FieldType::Vec3, 0, -360, 360, 0.5},
                    {"scale", "缩放", FieldType::Vec3, 1, 0.01, 1000, 0.05}}});
    out.push_back({"mesh", "网格",
                   {{"meshKey", "网格键", FieldType::Resource, 0, 0, 0, 0, nullptr, 0,
                     "model"},
                    {"colorHex", "颜色", FieldType::Color, 0, 0, 0, 0},
                    {"metallic", "金属度", FieldType::Number, 0, 0, 1, 0.01},
                    {"roughness", "粗糙度", FieldType::Number, 0.8, 0, 1, 0.01},
                    {"ao", "环境光遮蔽", FieldType::Number, 1, 0, 1, 0.01},
                    {"emissiveIntensity", "自发光强度", FieldType::Number, 1, 0, 5, 0.05},
                    {"albedoTex", "漫反射贴图", FieldType::Resource, 0, 0, 0, 0, nullptr, 0,
                     "texture"},
                    {"mrTex", "金属度/粗糙度贴图", FieldType::Resource, 0, 0, 0, 0, nullptr, 0,
                     "texture"},
                    {"aoTex", "AO 贴图", FieldType::Resource, 0, 0, 0, 0, nullptr, 0, "texture"},
                    {"emissiveTex", "自发光贴图", FieldType::Resource, 0, 0, 0, 0, nullptr, 0,
                     "texture"}}});
    out.push_back({"health", "生命",
                   {{"hp", "当前生命", FieldType::Number, 0, 0, 1e9, 1},
                    {"maxHp", "最大生命", FieldType::Number, 0, 0, 1e9, 1}}});
    out.push_back({"script", "脚本",
                     {{"backend", "后端", FieldType::Enum, 0, 0, 0, 0, kScriptBackends, 2},
                    {"path", "脚本路径", FieldType::Resource, 0, 0, 0, 0, nullptr, 0, "script"},
                    {"vars", "变量", FieldType::Json, 0, 0, 0, 0}}});
    out.push_back({"behaviorTree", "行为树",
                   {{"path", "行为树路径", FieldType::String, 0, 0, 0, 0}}});
    out.push_back({"name", "名称", {{"value", "值", FieldType::String, 0, 0, 0, 0}}});
    out.push_back({"groups", "组", {{"groups", "组 (逗号分隔)", FieldType::String, 0, 0, 0, 0}}});
    out.push_back({"type", "类型",
                   {{"value", "类型", FieldType::Enum, 0, 0, 0, 0, kNodeTypes, 6}}});
    out.push_back({"camera", "相机",
                   {{"fov", "视野 (度)", FieldType::Number, 60, 20, 120, 1},
                    {"ortho", "正交", FieldType::Bool, 0, 0, 1, 0}}});
    out.push_back({"sortOrder", "排序",
                   {{"z", "Z 排序 (小在前)", FieldType::Number, 0, -10000, 10000, 0.1}}});
    out.push_back({"tilemap", "2D 地图",
                   {{"cols", "列", FieldType::Int, 8, 1, 64, 1},
                    {"rows", "行", FieldType::Int, 5, 1, 64, 1},
                    {"cellSize", "格大小", FieldType::Number, 80, 1, 512, 1},
                    {"tiles", "格 (纹理路径数组)", FieldType::String, 0, 0, 0, 0}}});
    out.push_back({"decal", "贴花",
                   {{"texture", "贴图", FieldType::Resource, 0, 0, 0, 0, nullptr, 0,
                     "texture"},
                    {"size", "尺寸", FieldType::Number, 2, 0.1, 100, 0.1},
                    {"alpha", "不透明度", FieldType::Number, 1, 0, 1, 0.01}}});
    out.push_back({"rigidbody", "刚体",
                   {{"shape", "形状", FieldType::Enum, 0, 0, 0, 0, kRigidBodyShapes, 2},
                    {"radius", "半径", FieldType::Number, 0.5, 0.01, 100, 0.1},
                    {"halfExtents", "半尺寸", FieldType::Vec3, 0.5, 0.01, 100, 0.1},
                    {"dynamic", "动态", FieldType::Bool, 1, 0, 1, 0},
                    {"mass", "质量 (0=自动)", FieldType::Number, 0, 0, 1e6, 0.5},
                    {"restitution", "弹性", FieldType::Number, 0, 0, 1, 0.01},
                    {"friction", "摩擦", FieldType::Number, 0.4, 0, 1, 0.01},
                    {"damping", "线性阻尼", FieldType::Number, 0, 0, 10, 0.01},
                    {"gravityScale", "重力缩放", FieldType::Number, 1, 0, 10, 0.1},
                    {"layer", "碰撞层", FieldType::Int, 1, 0, 255, 1},
                    {"mask", "碰撞掩码", FieldType::Int, 0xFFFFFFFF, 0, 0xFFFFFFFF, 1}}});
    out.push_back({"character", "角色控制器",
                   {{"radius", "半径", FieldType::Number, 0.4, 0.01, 10, 0.05},
                    {"halfHeight", "半高", FieldType::Number, 0.9, 0.1, 50, 0.1},
                    {"layer", "碰撞层", FieldType::Int, 1, 0, 255, 1},
                    {"mask", "碰撞掩码", FieldType::Int, 0xFFFFFFFF, 0, 0xFFFFFFFF, 1}}});
    out.push_back({"plant", "植物",
                   {{"row", "行", FieldType::Int, 0, 0, 4, 1},
                    {"col", "列", FieldType::Int, 0, 0, 8, 1},
                    {"type", "类型", FieldType::Enum, 0, 0, 0, 0, kPlantTypes, 5}}});
    out.push_back({"zombie", "僵尸",
                   {{"row", "行", FieldType::Int, 0, 0, 4, 1},
                    {"delay", "延迟 (秒)", FieldType::Number, 8, 0, 3600, 0.5},
                    {"type", "类型", FieldType::Enum, 0, 0, 0, 0, kZombieTypes, 3}}});
    return out;
}

} // namespace

const ComponentSchema* FindComponentSchema(const std::string& name) {
    const auto& all = AllComponentSchemas();
    for (const ComponentSchema& s : all)
        if (s.name == name) return &s;
    return nullptr;
}

const std::vector<ComponentSchema>& AllComponentSchemas() {
    static const std::vector<ComponentSchema> kSchemas = BuildSchemas();
    return kSchemas;
}

void RegisterBuiltinComponentSchemas() {
    // Schemas are a static table; this call exists for symmetry with the
    // runtime component registry and as a future extension point.
    AllComponentSchemas();
}

} // namespace neon::scene

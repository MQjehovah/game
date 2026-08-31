#pragma once
#include <functional>
#include <string>
#include <vector>

#include "neon/ecs/world.hpp"
#include "neon/math/vec3.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::scene {

// 预制体系统：从项目 assets/prefabs/*.json 加载预制体并实例化。纯机械拆分自
// GameRuntime 的 prefs_/LoadPrefabs/SpawnPrefab（C1）。实例化回调由 GameRuntime
// 注入（负责 ecs::World + 组件工厂 + 脚本附加等运行时内部逻辑），本类不反向
// 依赖 GameRuntime 的任何内部状态。
class PrefabSystem {
public:
    // 实例化回调：接收解析好的单实体场景（引用预制体 + transform 覆盖），由注入
    // 方用 ecs::World + ComponentRegistry 展开组件并附加脚本，返回创建的实体。
    using InstantiateFn = std::function<ecs::Entity(const SceneFile&)>;
    // 目录枚举回调：dir -> 该目录下（递归）的文件路径列表。路径风格与 readFile
    // 约定一致即可（VFS 虚拟路径或磁盘绝对路径，FileStem 取最后一段为注册名）。
    using ListFilesFn = std::function<std::vector<std::string>(const std::string& dir)>;
    // 文件读取回调：path -> 文件文本（读不到返回空串）。
    using ReadFileFn = std::function<std::string(const std::string& path)>;

    void SetInstantiate(InstantiateFn fn) { instantiate_ = std::move(fn); }
    // 扫描 <scriptBaseDir>/assets/prefabs/*.json（递归）注册进 prefs_。读取/枚举
    // 方式由回调注入（磁盘目录、VFS pack、readScript 覆盖均由 GameRuntime 决定）。
    void Load(const std::string& scriptBaseDir, const ListFilesFn& listFiles,
              const ReadFileFn& readFile);
    // 按名实例化一个预制体到注入方 world_（经 instantiate_ 回调）。
    ecs::Entity Spawn(const std::string& name, const math::Vec3& pos);
    void Clear() { prefs_ = PrefabLibrary{}; }
    size_t Count() const { return prefs_.Size(); }
    const PrefabLibrary& Library() const { return prefs_; }

private:
    PrefabLibrary prefs_; // prefab 组件模板库（assets/prefabs/*.json）
    InstantiateFn instantiate_;
};

} // namespace neon::scene

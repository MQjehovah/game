-- ===========================================================================
-- lc_flowkit.lua — 引擎级通用玩法节点包（与具体游戏完全解耦）
--
-- 任何项目只要加载 lc_flow + lc_flowkit，就能用可视化流程图编排：
--   音效/音乐/飘字/粒子/预制体生成/全局变量/信号/切场景/子图调用……
-- 全部映射到引擎的脚本绑定（bindings.cpp 注册的通用能力），不含任何
-- 项目特定逻辑。项目自己的玩法节点照旧用 LC.flow.register 追加。
--
-- 节点一览（可视化编辑器按文末 describe 自动发现）：
--   game/sfx        播放音效          PlaySfx(name, volume)
--   game/music      播放音乐          PlayMusic(name, volume)
--   game/floattext  世界飘字          SpawnFloatText(x,y,z, text)
--   game/particles  粒子爆发          EmitParticles(cfg表)
--   game/spawn      生成预制体        SpawnPrefab(name, {x,y,z})
--   game/gamevar    全局变量赋值      SetVar(name, value)
--   game/signal     引擎信号广播      SignalEmit(name)
--   game/scene      切换场景          ChangeScene(name)
--   flow/call       调用子图          LC.flow.call(graph, signal)
-- ===========================================================================
LC = LC or {}
LC.flow = LC.flow or {}

-- --- 音效 ---------------------------------------------------------------
LC.flow.register("game/sfx", function(node)
  PlaySfx(tostring(node.name or ""), node.volume or 1.0)
  return "exec"
end)

-- --- 音乐 ---------------------------------------------------------------
LC.flow.register("game/music", function(node)
  PlayMusic(tostring(node.name or ""), node.volume or 0.6)
  return "exec"
end)

-- --- 世界飘字 -----------------------------------------------------------
LC.flow.register("game/floattext", function(node)
  SpawnFloatText({ x = node.x or 0, y = node.y or 2, z = node.z or 0 },
                 tostring(node.text or "..."))
  return "exec"
end)

-- --- 粒子爆发 -----------------------------------------------------------
LC.flow.register("game/particles", function(node)
  EmitParticles({
    pos = { x = node.x or 0, y = node.y or 1, z = node.z or 0 },
    count = node.count or 24,
    speed = node.speed or 4,
    life = node.life or 0.8,
    size = node.size or 0.25,
    color = node.color or "ffffff",
  })
  return "exec"
end)

-- --- 预制体生成 ----------------------------------------------------------
LC.flow.register("game/spawn", function(node)
  SpawnPrefab(tostring(node.prefab or ""), { x = node.x or 0, y = node.y or 0, z = node.z or 0 })
  return "exec"
end)

-- --- 全局变量（引擎 GameVar，跨图/跨脚本共享） ---------------------------
LC.flow.register("game/gamevar", function(node)
  SetVar(tostring(node.name or ""), node.value or 0)
  return "exec"
end)

-- --- 引擎信号广播（SignalConnect 的对端） --------------------------------
LC.flow.register("game/signal", function(node)
  SignalEmit(tostring(node.name or ""))
  return "exec"
end)

-- --- 切换场景 ------------------------------------------------------------
LC.flow.register("game/scene", function(node)
  ChangeScene(tostring(node.name or ""))
  return "exec"
end)

-- ===========================================================================
-- 编辑器元数据（可视化流程图面板扫描本文件自动生成调色板项）
-- ===========================================================================
LC.flow.describe("game/sfx", "播放音效", "通用", { "exec" },
                 { { "name", "s" }, { "volume", "n" } })
LC.flow.describe("game/music", "播放音乐", "通用", { "exec" },
                 { { "name", "s" }, { "volume", "n" } })
LC.flow.describe("game/floattext", "世界飘字", "通用", { "exec" },
                 { { "x", "n" }, { "y", "n" }, { "z", "n" }, { "text", "s" } })
LC.flow.describe("game/particles", "粒子爆发", "通用", { "exec" },
                 { { "x", "n" }, { "y", "n" }, { "z", "n" }, { "count", "n" },
                   { "speed", "n" }, { "life", "n" }, { "size", "n" }, { "color", "s" } })
LC.flow.describe("game/spawn", "生成预制体", "通用", { "exec" },
                 { { "prefab", "s" }, { "x", "n" }, { "y", "n" }, { "z", "n" } })
LC.flow.describe("game/gamevar", "全局变量", "通用", { "exec" },
                 { { "name", "s" }, { "value", "n" } })
LC.flow.describe("game/signal", "广播信号", "通用", { "exec" }, { { "name", "s" } })
LC.flow.describe("game/scene", "切换场景", "通用", { "exec" }, { { "name", "s" } })

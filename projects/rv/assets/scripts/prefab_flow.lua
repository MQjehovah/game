-- ===========================================================================
-- prefab_flow.lua — 预设体流程图通用适配器
--
-- 任何预设体只要在 components 里加：
--   "flow":  { "graph": "beacon" }          -- 绑定的流程图（assets/flow/xxx.flow.json）
--   "scripts": { "items": [{ "backend": "lua", "path": "assets/scripts/prefab_flow.lua" }] }
-- 生成时自动加载该图并把实体上下文注入图变量（ent_id/ent_x/ent_y/ent_z），
-- 图节点即可用 game/* 原语驱动该实体的行为——模型+动作+逻辑 = 一个自包含预设体。
--
-- 图内可用上下文变量：
--   $ent_id   实体句柄（传给 SetPosition/Despawn 等绑定时转 number）
--   $ent_x/y/z 生成位置
-- ===========================================================================

local me = ...  -- 实体名（AttachOne 以实体名调用 on_start）

function on_start(e)
  -- 读自身 SceneData 的 flow 组件（未注册组件自动进 SceneData）
  local flow = EntityComponentField(e, "flow", "graph")
  if not flow or flow == "" then return end
  -- 加载图（同名实例只加载一次；多实体共享决策层，行为参数走图变量）
  local g = LC.flow.load("assets/flow/" .. flow .. ".flow.json")
  if g then
    -- 注入实体上下文（图节点用 $ent_id 等引用）
    local pos = GetPosition(e)
    if pos then
      LC.flow.setvar(flow, "ent_id", tonumber(e) or 0)
      LC.flow.setvar(flow, "ent_x", pos.x)
      LC.flow.setvar(flow, "ent_y", pos.y)
      LC.flow.setvar(flow, "ent_z", pos.z)
    end
    -- 图的入口由自己的 entry 驱动（signal/timer/tick），此处不主动触发
  end
end

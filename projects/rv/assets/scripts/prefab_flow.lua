-- ===========================================================================
-- prefab_flow.lua — 预设体流程图通用适配器（多实例版）
--
-- 任何预设体只要在 components 里加：
--   "flow":  { "graph": "beacon" }          -- 绑定的流程图（assets/flow/xxx.flow.json）
--   "scripts": { "items": [{ "backend": "lua", "path": "assets/scripts/prefab_flow.lua" }] }
-- 生成时自动加载该图（每个实体一个独立实例：变量/timer 各自独立），
-- 并注入实体上下文。同一种预设体 N 份 = N 个独立行为状态。
--
-- 图内可用上下文变量：
--   $ent_id   实体句柄（传给 SetPosition/Despawn 等绑定时转 number）
--   $ent_x/y/z 生成位置
--   $me       实例键（图名_实体id，供 flow/call 定向）
-- ===========================================================================

function on_start(e)
  local flow = EntityComponentField(e, "flow", "graph")
  if not flow or flow == "" then return end
  -- 每个实体一个独立图实例（key = 图名_实体id）
  local key = flow .. "_" .. tostring(e)
  local g = LC.flow.load("assets/flow/" .. flow .. ".flow.json", key)
  if g then
    local pos = GetPosition(e)
    if pos then
      LC.flow.setvar(key, "ent_id", tonumber(e) or 0)
      LC.flow.setvar(key, "ent_x", pos.x)
      LC.flow.setvar(key, "ent_y", pos.y)
      LC.flow.setvar(key, "ent_z", pos.z)
      LC.flow.setvar(key, "me", key)
    end
  end
end

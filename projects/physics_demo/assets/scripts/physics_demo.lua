-- NeonPhysics demo: 刚体物理(质量/弹性/摩擦/阻尼)与碰撞检测。
-- 场景 JSON 里的 rigidbody 组件注册到物理世界;on_update 每帧把当前刚体
-- 高度和碰撞对数写进 GameVar,方便 --dump-vars 定量验证。
local spawned = {}

function on_start(e)
  SetVar("dropY", GetPosition(e).y)
  SetVar("collisionCount", 0)
  -- 额外动态生成一列小球(不同弹性),落到地面上弹跳。
  for i = 1, 5 do
    local id = PhysicsAddSphere(
      { x = -6 + i * 1.4, y = 7 + i * 0.7, z = 2.5 },
      0.35,
      true,
      { mass = 1, restitution = 0.9, friction = 0.6 })
    spawned[#spawned + 1] = id
  end
  -- 一个不受重力影响的静态观察球(gravityScale=0)验证重力缩放。
  spawned[#spawned + 1] = PhysicsAddSphere(
    { x = 6, y = 3, z = 0 }, 0.5, true,
    { mass = 1, restitution = 0.0, gravityScale = 0 })
end

function on_update(e, dt)
  local p = GetPosition(e)
  SetVar("dropY", p.y)
  local cols = PhysicsCollisions()
  SetVar("collisionCount", #cols)
  if cols[1] ~= nil then
    SetVar("lastCollisionA", cols[1].a)
    SetVar("lastCollisionB", cols[1].b)
  end
end

function on_render()
  local n = GetVar("collisionCount") or 0
  local y = GetVar("dropY") or 0
  DrawText("物理 demo  刚体数量: 场景 5 + 脚本 6   本帧碰撞: " .. n,
           640, 24, 20, 1, 0.95, 0.35, 1, true, false)
  DrawText("报告盒高度: " .. string.format("%.2f", y) .. " (静止应为 1.00)",
           640, 50, 16, 0.8, 0.9, 1, 1, true, false)
end

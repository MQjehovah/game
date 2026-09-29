-- lc_player.lua
-- 第三人称主角控制器：下车/行走/疾跑/动画状态机/持枪位/枪口

function spawnPlayer()
  if player.active then return end
  local bx, bz = rotOf(1.7, -1.0)
  player.x, player.z, player.yaw = bx, bz, rv.yaw + math.pi * 0.5
  player.ent = SpawnPrefab("rv_player", { x = bx, y = 0, z = bz })
  player.active = (player.ent ~= nil)
  player.anim = ""
  player.asm = false
  player.mag = 30
  player.reloadT = 0
  if player.active then
    -- 步枪三件：枪身/枪管/枪托（每帧按持枪位姿摆放）
    player.gunBody = SpawnPrefab("rv_gun_body", { x = bx, y = 1.15, z = bz })
    player.gunBarrel = SpawnPrefab("rv_gun_barrel", { x = bx, y = 1.15, z = bz })
    player.gunStock = SpawnPrefab("rv_gun_stock", { x = bx, y = 1.12, z = bz })
    onFoot = true
  end
end

function despawnPlayer()
  if player.ent ~= nil then Despawn(player.ent) end
  if player.gunBody ~= nil then Despawn(player.gunBody) end
  if player.gunBarrel ~= nil then Despawn(player.gunBarrel) end
  if player.gunStock ~= nil then Despawn(player.gunStock) end
  player.ent, player.gunBody, player.gunBarrel, player.gunStock = nil, nil, nil, nil
  player.active = false
  player.anim = ""
  player.asm = false
  onFoot = false
end

-- 步枪三件贴到持枪位（右手，前向伸出；跑步时枪口下压）
function updatePlayerGun()
  if not onFoot or player.gunBody == nil then return end
  local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
  local rx, rz = -fz * 0.32, fx * 0.32      -- 右手侧偏移（侧影可见）
  local lower = (player.anim == "Idle") and 0.14 or 0
  local bx, bz = player.x + fx * 0.24 + rx, player.z + fz * 0.24 + rz
  local y = 1.1 - lower
  SetPosition(player.gunBody, { x = bx, y = y, z = bz })
  SetRotationY(player.gunBody, player.yaw)
  SetScale(player.gunBody, 0.07, 0.1, 0.46)
  SetPosition(player.gunBarrel, { x = bx + fx * 0.36, y = y + 0.025, z = bz + fz * 0.36 })
  SetRotationY(player.gunBarrel, player.yaw)
  SetScale(player.gunBarrel, 0.035, 0.035, 0.3)
  SetPosition(player.gunStock, { x = bx - fx * 0.2, y = y - 0.025, z = bz - fz * 0.2 })
  SetRotationY(player.gunStock, player.yaw)
  SetScale(player.gunStock, 0.05, 0.09, 0.18)
end

function playerMuzzle()
  if onFoot and player.active then
    local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
    return player.x + fx * 0.5, 1.22, player.z + fz * 0.5
  end
  local turret = hasModule("turret")
  if turret then
    local mx, mz = rotOf(turret.lx, turret.lz)
    return mx, (turret.ly or 2.6) + 0.5, mz
  end
  local mx, mz = rotOf(0, 1.5)
  return mx, 2.35, mz
end

function updatePlayer(dt)
  if not onFoot or player.ent == nil then return end
  -- 坦克式行走：W/S 前后，A/D 转向（与驾驶操作一致）
  local mv = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  local turn = (ActionDown("a") and 1 or 0) - (ActionDown("d") and 1 or 0)
  local sprint = ActionDown("shift") and true or false
  if autoPilot then mv, turn = 1, 0 end  -- 验证钩子：自动直走
  player.yaw = player.yaw + turn * (sprint and 1.9 or 2.4) * dt
  local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
  local sp = ((mv >= 0) and 3.2 or 1.6) * (sprint and 1.65 or 1)
  player.x = player.x + fx * sp * mv * dt
  player.z = player.z + fz * sp * mv * dt
  -- 车内行走：进入车厢范围则夹在 4x14 格内部（不出墙）
  local dx, dz = player.x - rv.x, player.z - rv.z
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  local lx = dx * cy - dz * sy
  local lz = dx * sy + dz * cy
  local inside = (math.abs(lx) < 1.5 and math.abs(lz) < 4.3)
  if inside then
    lx = math.max(-1.0, math.min(1.0, lx))
    lz = math.max(-3.5, math.min(3.5, lz))
    player.x = rv.x + lx * cy + lz * sy
    player.z = rv.z - lx * sy + lz * cy
  end
  SetPosition(player.ent, { x = player.x, y = 0, z = player.z })
  -- 模型网格在 rot 0 朝 -Z，与 yaw 约定（前进 = (sin,cos)，rot 0 朝 +Z）相反，
  -- 加 π 让视觉朝向对齐移动方向（否则人物倒着走、相机对着脸）
  SetRotationY(player.ent, player.yaw + math.pi)
  -- 动画：数据驱动状态机（assets/anim/soldier_asm.json）。脚本变量
  -- （still/walk/run 互斥桶）经 SetAnimParam 喂入，过渡与淡入由规格文件定义，
  -- 脚本不再手写 if 切换。AttachStateMachine 需要蒙皮 draw item 先解析一帧，
  -- 失败逐帧重试；始终失败则回退 PlayAnimation 手动切换（保底）。
  if player.asm then
    SetAnimParam(player.ent, "still", (mv == 0) and 1 or 0)
    SetAnimParam(player.ent, "walk",  (mv ~= 0 and not sprint) and 1 or 0)
    SetAnimParam(player.ent, "run",   (mv ~= 0 and sprint) and 1 or 0)
  else
    player.asm = AttachStateMachine(player.ent, "assets/anim/soldier_asm.json")
    if not player.asm then
      local clip = (mv == 0) and "Idle" or (sprint and "Run" or "Walk")
      if clip ~= player.anim then
        player.anim = clip
        if not PlayAnimation(player.ent, clip, true, 0.15) then
          player.anim = ""  -- 剪辑缺失时回退，避免卡死在状态里
        end
      end
    else
      player.anim = ""
    end
  end
  updatePlayerGun()
  -- 换弹
  if player.reloadT > 0 then
    player.reloadT = player.reloadT - dt
    if player.reloadT <= 0 then player.mag = 30 end
  end
  -- 手雷（Q）：抛物线投掷，落地范围爆炸
  if player.nadeCd > 0 then
    player.nadeCd = player.nadeCd - dt
  end
  if ActionPressed("q") and player.nadeCd <= 0 and #grenades < 4 then
    player.nadeCd = 6.0
    local mp = InputMousePos()
    local aim = PickGround(mp)
    local tx, tz = player.x + fx * 10, player.z + fz * 10
    if aim then
      local adx, adz = aim.x - player.x, aim.z - player.z
      local ad = math.sqrt(adx * adx + adz * adz)
      if ad > 12 then adx, adz, ad = adx / ad * 12, adz / ad * 12, 12 end
      tx, tz = player.x + adx, player.z + adz
    end
    local ent = SpawnPrefab("rv_grenade", { x = player.x + fx * 0.4, y = 1.3, z = player.z + fz * 0.4 })
    if ent ~= nil then
      local n = (tx - player.x)
      local nz = (tz - player.z)
      local flight = 0.9
      grenades[#grenades + 1] = { ent = ent,
          x = player.x + fx * 0.4, y = 1.3, z = player.z + fz * 0.4,
          vx = n / flight, vz = nz / flight,
          vy = (0 - 1.3) / flight + 0.5 * 9.8 * flight,
          fuse = flight + 0.3 }
    end
  end
end

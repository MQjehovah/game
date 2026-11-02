-- lc_player.lua
-- 第一人称主角控制器：鼠标视角 / 行走疾跑 / 动画 / 持枪位（可复用）

EYE_H = 1.62
PLAYER_RADIUS = 0.28

function spawnPlayerAt(x, z, yaw)
  if player.active then return end
  player.x, player.z, player.yaw = x, z, yaw or 0
  player.pitch = 0
  player.ent = SpawnPrefab("rv_player", { x = x, y = 0, z = z })
  player.active = (player.ent ~= nil)
  player.anim = ""
  player.asm = false
  player.mag = 30
  player.reloadT = 0
  if player.active then
    -- 步枪三件：枪身/枪管/枪托（每帧按持枪位姿摆放）
    player.gunBody = SpawnPrefab("rv_gun_body", { x = x, y = 1.15, z = z })
    player.gunBarrel = SpawnPrefab("rv_gun_barrel", { x = x, y = 1.15, z = z })
    player.gunStock = SpawnPrefab("rv_gun_stock", { x = x, y = 1.12, z = z })
    onFoot = true
    -- 第一人称不显示身体（避免头部网格挡住镜头）；射击特效另画
    setPlayerVisible(false)
  end
end

function spawnPlayer()
  -- 停车后站在车右后侧、斜向房车（和开场一致，能同时看到车与路边）
  local lx, lz = 5.2, -7.0
  local bx, bz = rotOf(lx, lz)
  spawnPlayerAt(bx, bz, rv.yaw + dirYaw(-lx, -lz))
  player.pitch = -0.04
end

function setPlayerVisible(vis)
  if player.ent ~= nil then SetVisible(player.ent, vis) end
  if player.gunBody ~= nil then SetVisible(player.gunBody, vis) end
  if player.gunBarrel ~= nil then SetVisible(player.gunBarrel, vis) end
  if player.gunStock ~= nil then SetVisible(player.gunStock, vis) end
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
  local rx, rz = -fz * 0.28, fx * 0.28      -- 右手侧偏移
  local lower = (player.anim == "Idle") and 0.16 or 0
  local bx, bz = player.x + fx * 0.18 + rx, player.z + fz * 0.18 + rz
  local y = 1.32 - lower
  SetPosition(player.gunBody, { x = bx, y = y, z = bz })
  SetRotationY(player.gunBody, player.yaw)
  SetScale(player.gunBody, 0.06, 0.09, 0.42)
  SetPosition(player.gunBarrel, { x = bx + fx * 0.34, y = y + 0.02, z = bz + fz * 0.34 })
  SetRotationY(player.gunBarrel, player.yaw)
  SetScale(player.gunBarrel, 0.03, 0.03, 0.28)
  SetPosition(player.gunStock, { x = bx - fx * 0.18, y = y - 0.02, z = bz - fz * 0.18 })
  SetRotationY(player.gunStock, player.yaw)
  SetScale(player.gunStock, 0.045, 0.08, 0.16)
end

function playerMuzzle()
  if onFoot and player.active then
    local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
    return player.x + fx * 0.5, 1.28, player.z + fz * 0.5
  end
  local turret = hasModule("turret")
  if turret then
    local mx, mz = rotOf(turret.lx, turret.lz)
    return mx, (turret.ly or 2.6) + 0.5, mz
  end
  local mx, mz = rotOf(0, 1.5)
  return mx, 2.35, mz
end

function playerForward()
  local cp = math.cos(player.pitch)
  return math.sin(player.yaw) * cp, math.sin(player.pitch), math.cos(player.yaw) * cp
end

function playerEye()
  return player.x, EYE_H, player.z
end

function clampPlayerInsideRv()
  local dx, dz = player.x - rv.x, player.z - rv.z
  local cy, sy = math.cos(rv.yaw), math.sin(rv.yaw)
  local lx = dx * cy - dz * sy
  local lz = dx * sy + dz * cy
  local inside = (math.abs(lx) < 1.35 and lz > -4.2 and lz < 4.0)
  if inside then
    -- 留出内墙板/车窗的余量，避免第一人称镜头钻进墙里
    lx = math.max(-0.88, math.min(0.88, lx))
    lz = math.max(-4.05, math.min(3.35, lz))
    player.x = rv.x + lx * cy + lz * sy
    player.z = rv.z - lx * sy + lz * cy
  end
end

function isPlayerInsideRv()
  if not (onFoot and player.active) then return false end
  local lx, lz = worldToLocal(player.x, player.z)
  return math.abs(lx) < 1.05 and lz > -3.95 and lz < 3.75
end

function updatePlayer(dt)
  if not onFoot or player.ent == nil then return end

  -- 鼠标视角（引擎 cameraMouseLock 已接管光标）
  -- 限制单帧鼠标增量：窗口捕获/重定位时会产生一个巨大的首帧 delta
  local rawX, rawY = InputMouseX(), InputMouseY()
  local dxm = (math.abs(rawX) > 110) and 0 or math.max(-70, math.min(70, rawX))
  local dym = (math.abs(rawY) > 110) and 0 or math.max(-70, math.min(70, rawY))
  player.yaw = player.yaw - dxm * SENSITIVITY
  player.pitch = math.max(-1.25, math.min(1.25, player.pitch - dym * SENSITIVITY))

  -- WASD：相对视角移动，A/D 横移
  local ix = (ActionDown("d") and 1 or 0) - (ActionDown("a") and 1 or 0)
  local iz = (ActionDown("w") and 1 or 0) - (ActionDown("s") and 1 or 0)
  local sprint = ActionDown("shift") and true or false
  local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
  local rx, rz = math.cos(player.yaw), -math.sin(player.yaw)
  local mx, mz = fx * iz + rx * ix, fz * iz + rz * ix
  local len = math.sqrt(mx * mx + mz * mz)
  if len > 0.001 then mx, mz = mx / len, mz / len end
  local sp = (sprint and 4.3 or 2.7) * ((needs.energy or 100) < 20 and 0.7 or 1)
  local nx = player.x + mx * sp * dt
  local nz = player.z + mz * sp * dt
  nx, nz = collidePoiWalls(player.x, player.z, nx, nz)
  player.x, player.z = nx, nz
  clampPlayerInsideRv()
  -- 脚步声
  if len > 0 then
    player.stepT = (player.stepT or 0) - dt
    if player.stepT <= 0 then
      player.stepT = sprint and 0.30 or 0.46
      if PlaySfx then PlaySfx("step") end
    end
  else
    player.stepT = 0
  end

  SetPosition(player.ent, { x = player.x, y = 0, z = player.z })
  -- 模型网格在 rot 0 朝 -Z，yaw 约定前进 = (sin,cos)，加 π 对齐视觉朝向
  SetRotationY(player.ent, player.yaw + math.pi)
  -- 动画：数据驱动状态机（assets/anim/soldier_asm.json）
  if player.asm then
    SetAnimParam(player.ent, "still", (len == 0) and 1 or 0)
    SetAnimParam(player.ent, "walk",  (len > 0 and not sprint) and 1 or 0)
    SetAnimParam(player.ent, "run",   (len > 0 and sprint) and 1 or 0)
  else
    player.asm = AttachStateMachine(player.ent, "assets/anim/soldier_asm.json")
    if not player.asm then
      local clip = (len == 0) and "Idle" or (sprint and "Run" or "Walk")
      if clip ~= player.anim then
        player.anim = clip
        if not PlayAnimation(player.ent, clip, true, 0.15) then
          player.anim = ""
        end
      end
    else
      player.anim = ""
    end
  end
  updatePlayerGun()
  if player.reloadT > 0 then
    player.reloadT = player.reloadT - dt
    if player.reloadT <= 0 then player.mag = 30 end
  end
end

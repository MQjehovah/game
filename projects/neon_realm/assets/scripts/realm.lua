-- NeonRealm：魔兽世界风格 demo 的数据驱动移植。
-- 玩法与 HUD 全部在脚本里（场景 JSON 摆放村庄/狼群，程序化 mesh key 由
-- 引擎生成）。玩家实体携带本脚本；on_update(e) 里 e 即英雄。

local hero = nil
local wolves = {}     -- { e, home{x,y,z}, dead, respawn, hp, phase, attackCd, bob }
local npcPos = { x = 0, y = 0, z = 0 }
local dialogue = nil  -- { lines = {...}, shown = 0 }
local waveTimer = 18
local saveTimer = 0
local dashTime = 0
local dashCd = 0
local iframes = 0
local yvel = 0
local grounded = true
local facing = 0

-- 加粗描边文字：8 向黑色描边 + 彩色正文，任何背景下都清晰
local function outlined_text(x, y, size, text, r, g, b)
  local offs = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 },
                 { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }
  for _, o in ipairs(offs) do
    DrawText(text, x + o[1], y + o[2], size, 0, 0, 0, 0.9, true, true)
  end
  DrawText(text, x, y, size, r, g, b, 1, true, true)
end

local function vget(k, def)
  local v = GetVar(k)
  if v == nil then return def end
  return v
end

local function vset(k, v) SetVar(k, v) end

local function dist2d(ax, az, bx, bz)
  local dx, dz = ax - bx, az - bz
  return math.sqrt(dx * dx + dz * dz)
end
-- ============ 元素技能 (参照 Elemental Sandbox 的线性/区域施法) ============
-- Q 冰霜新星(以己为中心 AoE 冻结) / R 陨石术(前方落点 AoE) /
-- G 圣光束(面朝方向直线) / T 电击陷阱(前方区域 强减速)。
-- 施法流程: 按键直接施放(带蓝耗+冷却), VFX 全部程序化绘制。
local SKILLS = {
  frost  = { cd = 8,  mana = 30, sfx = "frozen" },
  meteor = { cd = 18, mana = 45, sfx = "explosion" },
  beam   = { cd = 14, mana = 35, sfx = "jalapeno" },
  snare  = { cd = 10, mana = 25, sfx = "siren" },
}
local skillCd = { frost = 0, meteor = 0, beam = 0, snare = 0 }
-- 世界锚定 VFX: 每个特效是一个真实实体 (自发光球 / 贴地环 / 贴花), 到期回收
local vfxEnts, vfxMeteors = {}, {}
local vfxClock = 0
local shakeT = 0

local function spawnBurst(wx, wy, wz, n, speed, col, up)
  for i = 1, n do
    local a = math.random() * math.pi * 2
    local sp = speed * (0.4 + math.random() * 0.6)
    vfxParts[#vfxParts + 1] = {
      wx = wx, wy = wy, wz = wz,
      vx = math.cos(a) * sp, vy = (up or 2.5) + math.random() * 2.5, vz = math.sin(a) * sp,
      g = -9, age = 0, life = 0.5 + math.random() * 0.5,
      size = 3 + math.random() * 3, col = col,
    }
  end
end

local function shake(t) shakeT = math.max(shakeT, t) end

-- 延时调度: after(sec, fn) —— 多节拍技能序列的时间线
local scheduled = {}
local function after(sec, fn) scheduled[#scheduled + 1] = { at = vfxClock + sec, fn = fn } end
-- 全屏冲击闪光: { age, life, col }
local flashes = {}
local function flash(col, life) flashes[#flashes + 1] = { age = 0, life = life, col = col } end

local function wolvesInRadius(x, z, r)
  local out = {}
  for _, w in ipairs(wolves) do
    if not w.dead then
      local wp = GetPosition(w.e)
      if wp ~= nil and dist2d(wp.x, wp.z, x, z) <= r then out[#out + 1] = { e = w.e, wp = wp } end
    end
  end
  return out
end

local function damageWolf(ent, dmg)
  local hp = GetHealth(ent)
  if hp ~= nil then SetHealth(ent, hp - dmg) end
end

-- 沿两点间画线 (画布无旋转矩形 -> 插值小方块)
-- 生成一个 VFX 实体并在 life 秒后自动回收
local function spawnFx(prefab, x, y, z, scale, life)
  local e = SpawnPrefab(prefab, { x = x, y = y, z = z })
  if e == nil then return nil end
  SetScale(e, scale, scale, scale)
  vfxEnts[#vfxEnts + 1] = { ent = e, dieAt = vfxClock + life }
  return e
end

-- 贴地扩张环: 直径 r0 -> r1, 缓出
local function fxRing(prefab, x, z, r0, r1, life)
  local e = SpawnPrefab(prefab, { x = x, y = 0.12, z = z })
  if e == nil then return nil end
  SetScale(e, r0, r0, r0)
  Tween(e, 2, { x = r0, y = r0, z = r0 }, { x = r1, y = r1, z = r1 }, life, 2)
  vfxEnts[#vfxEnts + 1] = { ent = e, dieAt = vfxClock + life + 0.05 }
  return e
end


-- 粒子爆发: 引擎 billboard 粒子系统 (additive 泛光, 尺寸/颜色随生命衰减)
local function burst(x, y, z, n, sp, col, life, grav)
  EmitParticles({
    pos = { x = x, y = y, z = z }, count = n,
    speedMin = sp * 0.35, speedMax = sp,
    lifeMin = life * 0.6, lifeMax = life,
    sizeStart = 0.55, sizeEnd = 0.05,
    color = { r = col[1], g = col[2], b = col[3], a = 1 },
    colorEnd = { r = col[1], g = col[2], b = col[3], a = 0 },
    gravity = grav or -6, additive = true,
  })
end
-- 兼容旧调用点: fxOrbBurst(色族, x, y, z, n, sp, scale, life)
local FAMILIES = {
  ice    = { { 0.62, 0.91, 1.0 } },
  fire   = { { 1.0, 0.62, 0.2 }, { 1.0, 0.85, 0.35 } },
  violet = { { 0.82, 0.55, 1.0 } },
  gold   = { { 1.0, 0.88, 0.45 } },
}
local function fxOrbBurst(family, x, y, z, n, sp, scale, life)
  local cols = FAMILIES[family] or FAMILIES.ice
  local col = cols[math.random(#cols)]
  burst(x, y, z, n, sp, col, life or 0.7)
end

-- 光柱: 从地面拔起 (billboard 竖纹贴图), hold 秒后回收
local function fxPillar(prefab, x, z, w, h, life)
  local e = SpawnPrefab(prefab, { x = x, y = h * 0.5, z = z })
  if e == nil then return nil end
  SetScale(e, w, 0.08, w)
  Tween(e, 2, { x = w, y = 0.08, z = w }, { x = w, y = h, z = w }, 0.16, 1)
  vfxEnts[#vfxEnts + 1] = { ent = e, dieAt = vfxClock + life }
  return e
end

-- 第一人称视角（FPS）状态：lookYaw/lookPitch 由鼠标驱动，
-- 引擎侧通过 cameraMouseLock + cameraYaw/cameraPitch/cameraDist GameVar 接管相机。
local fpsMode = true        -- V 切换第一人称 / 轨道视角
local vDown = false
local lookYaw = 0
local lookPitch = 0.32
local EYE_H = 1.6           -- 眼睛高度（相对脚底）
local LOOK_SENS = 0.003
local CAM_DIST = 2.0        -- 引擎相机最小距离；focus 放到视线前方该距离处，相机即落在眼睛点

local WOLF_MAX = 45
local GROUND_Y = 0.9 -- 英雄站立高度

-- M1 主角动画状态：locomotion（idle/walk/run 按速度）+ 动作一次性播放
local heroAnim = "Idle"     -- 当前 locomotion 状态
local heroAct = nil         -- 一次性动作（"cast"/"punch"），播放期间压制移动动画
local castTime = -1         -- 施法读条进度（<0 = 未在读条）
local CAST_DUR = 0.9        -- 读条时长（对齐 Spellcast_Shoot 前摇）
local fireCd = 0            -- 火球冷却
local FIRE_CD = 1.6
local punchCd = 0

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

-- 攻击方向：FPS 下沿鼠标视线，轨道下沿英雄朝向。
-- 注意 facing 约定是 {sin(facing), -cos(facing)}，而相机 yaw 约定是
-- {-sin(yaw), -cos(yaw)}，两者 x 分量符号相反，不能混用。
-- 保持水平（y=0）：弹道/近战命中判定是水平距离 + 垂直带，带俯仰的弹道
-- 会在远处落到目标脚下而打空。
local function attack_dir()
  if fpsMode then
    return { x = -math.sin(lookYaw), y = 0, z = -math.cos(lookYaw) }
  end
  return { x = math.sin(facing), y = 0, z = -math.cos(facing) }
end


local function load_save()
  local text = ReadText("save.json")
  if text == nil or text == "" then return end
  for line in (text .. "\n"):gmatch("(.-)\n") do
    local k, v = line:match("^([%w_]+)=([%w.]+)$")
    if k and v then
      SetVar(k, tonumber(v) or 0)
    end
  end
end

local function save_game()
  local s = string.format("level=%d\nxp=%d\ngold=%d\n",
                          math.floor(vget("level", 1)),
                          math.floor(vget("xp", 0)),
                          math.floor(vget("gold", 0)))
  WriteText("save.json", s)
end

local function level_up()
  local lv = math.floor(vget("level", 1))
  SetVar("level", lv + 1)
  SetVar("max_hp", 100 + (lv) * 15)
  SetVar("max_mana", 50 + (lv) * 8)
  SetVar("hp", GetVar("max_hp"))
  SetVar("mana", GetVar("max_mana"))
  dialogue = { lines = { "升级！你达到了 " .. tostring(lv + 1) .. " 级。" }, shown = 3 }
  save_game()
end

function on_start(e)
  hero = e
  SetVar("cameraMouseLock", fpsMode and 1 or 0)
  lookYaw = vget("cameraYaw", 0.6) -- 与引擎默认相机朝向衔接，避免初始跳变
  SetVar("level", 1)
  SetVar("xp", 0)
  SetVar("gold", 0)
  SetVar("max_hp", 100)
  SetVar("hp", 100)
  SetVar("max_mana", 50)
  SetVar("mana", 50)
  SetVar("wave", 0)
  SetVar("kills", 0)
  load_save()
  SetVar("hp", GetVar("max_hp"))
  SetVar("mana", GetVar("max_mana"))

  -- M1: 主角是人形 Mage，进入待机
  PlayAnimation(hero, "Idle", true, 0.1)
  SetEntityPlate(hero, nil, -1.0) -- 自己不显示头顶板
  if fpsMode then SetVisible(hero, false) end -- FPS 隐藏自身模型

  -- 收集预摆放的狼（场景里 12 只，脚本管理生死/波次）
  for i = 1, 12 do
    local w = FindNamedEntity("狼_" .. i)
    if w ~= nil then
      local p = GetPosition(w)
      table.insert(wolves, {
        e = w, home = { x = p.x, y = p.y, z = p.z },
        dead = false, respawn = 0, attackCd = 0, phase = i * 0.7,
      })
      SetEntityPlate(w, "野狼", 1.0)
    end
  end

  SetVar("hp", 100)
  SetVar("mana", 50)
end

local function spawn_wave(wave)
  local count = math.min(2 + wave, #wolves)
  local px, pz = 0, 4
  local hp = GetPosition(hero)
  if hp ~= nil then px, pz = hp.x, hp.z end
  for i = 1, count do
    local w = wolves[i]
    local a = (i - 1) * (2 * math.pi / count)
    local r = 15 + (i % 3) * 2
    w.home = { x = px + math.cos(a) * r, y = 0.55, z = pz + math.sin(a) * r }
    w.dead = false
    w.respawn = 0
    w.attackCd = 0
    SetVisible(w.e, true)
    SetHealth(w.e, WOLF_MAX)
    SetPosition(w.e, { x = w.home.x, y = w.home.y, z = w.home.z })
  end
end

local function update_player(dt)
  -- 冷却/回蓝/无敌
  dashCd = math.max(0, dashCd - dt)
  dashTime = math.max(0, dashTime - dt)
  iframes = math.max(0, iframes - dt)
  fireCd = math.max(0, fireCd - dt)
  punchCd = math.max(0, punchCd - dt)
  local mana = vget("mana", 50)
  local maxMana = vget("max_mana", 50)
  SetVar("mana", math.min(maxMana, mana + 3 * dt))

  -- V：切换第一人称 / 轨道视角（轨道模式相机接管鼠标，FPS 模式脚本接管）
  if InputKey("V") > 0 and not vDown then
    vDown = true
    fpsMode = not fpsMode
    SetVar("cameraMouseLock", fpsMode and 1 or 0)
    -- FPS 下隐藏自身模型（相机在眼睛处，避免遮挡视线）；轨道模式恢复
    SetVisible(hero, not fpsMode)
  elseif InputKey("V") <= 0 then
    vDown = false
  end

  -- 相机相对移动（FPS 下用鼠标视角 yaw，轨道下用相机 yaw）
  if fpsMode then
    lookYaw = lookYaw - InputMouseX() * LOOK_SENS
    lookPitch = lookPitch + InputMouseY() * LOOK_SENS
    lookPitch = clamp(lookPitch, -1.2, 1.2)
  end
  local cy = fpsMode and lookYaw or vget("cameraYaw", 0)
  local ix = ActionAxis("move_strafe")
  local iz = ActionAxis("move_forward")
  local fwd = { x = -math.sin(cy), z = -math.cos(cy) }
  local right = { x = math.cos(cy), z = -math.sin(cy) }
  local dx = right.x * ix + fwd.x * iz
  local dz = right.z * ix + fwd.z * iz
  local len = math.sqrt(dx * dx + dz * dz)
  if len > 1 then dx, dz = dx / len, dz / len end

  -- 施法读条：锁移动，完成时发射火球（弹道由引擎管理）
  if castTime >= 0 then
    castTime = castTime + dt
    if castTime >= CAST_DUR then
      castTime = -1
      local pos2 = GetPosition(hero)
      if pos2 ~= nil then
        local origin = { x = pos2.x, y = pos2.y + 1.2, z = pos2.z }
        CastSkill("fireball", origin, attack_dir(), hero)
      end
      heroAct = nil
      PlayAnimation(hero, "Idle", true, 0.25)
    end
    return -- 读条期间不做移动/其他动作
  end

  -- 冲刺（右键）
  if InputMousePressed("right") and dashCd <= 0 and len > 0.01 then
    dashTime = 0.18
    dashCd = 2.5
    iframes = math.max(iframes, 0.35)
  end
  local speed = (dashTime > 0) and 6 * 3.4 or 6

  -- 跳跃（手动重力，地面 y=GROUND_Y）
  local pos = GetPosition(hero)
  if pos == nil then return end
  if ActionDown("jump") and grounded then
    yvel = 8
    grounded = false
  end
  if not grounded then
    yvel = yvel - 20 * dt
    pos.y = pos.y + yvel * dt
    if pos.y <= GROUND_Y then
      pos.y = GROUND_Y
      yvel = 0
      grounded = true
    end
  end
  pos.x = pos.x + dx * speed * dt
  pos.z = pos.z + dz * speed * dt
  pos.x = clamp(pos.x, -48, 48)
  pos.z = clamp(pos.z, -48, 48)
  SetPosition(hero, pos)

  -- 面向：FPS 下英雄始终面向视角方向（攻击/施法对准视线）
  if fpsMode then
    facing = lookYaw
    SetRotationY(hero, facing)
  elseif len > 0.01 then
    facing = math.atan(dx, -dz)
    SetRotationY(hero, facing)
  end

  -- 近战（左键）：出拳一次性动画，命中判定立刻生效
  if InputMousePressed("left") and punchCd <= 0 then
    punchCd = 0.55
    heroAct = "punch"
    PlayAnimation(hero, "Unarmed_Melee_Attack_Punch_A", false, 0.1)
    local origin = { x = pos.x, y = pos.y + 1.2, z = pos.z }
    MeleeAttack(origin, attack_dir(), 2.2, 100, 28)
  end

  -- 火球（1）：进入读条，满条发射（读条动画 Spellcasting 循环 + 发射瞬间 Spellcast_Shoot）
  if ActionPressed("fireball") and fireCd <= 0 then
    local manaNow = vget("mana", 0)
    if manaNow >= 10 then
      SetVar("mana", manaNow - 10)
      fireCd = FIRE_CD
      castTime = 0
      heroAct = "cast"
      PlayAnimation(hero, "Spellcasting", true, 0.15)
      return
    else
      local pos3 = GetPosition(hero)
      if pos3 ~= nil then
        SpawnFloatText(pos3.x, pos3.y + 2.0, pos3.z, "法力不足", false, 1.0)
      end
    end
  end
  if ActionDown("heal") then
    local manaNow = vget("mana", 0)
    if manaNow >= 15 then
      SetVar("mana", manaNow - 15)
      local hp = vget("hp", 100)
      SetVar("hp", math.min(vget("max_hp", 100), hp + 35))
    end
  end

  -- ============ 元素技能 (Q/R/G/T) ============
  local function spendMana(cost)
    local manaNow = vget("mana", 0)
    if manaNow < cost then
      local pp4 = GetPosition(hero)
      if pp4 ~= nil then SpawnFloatText(pp4.x, pp4.y + 2.0, pp4.z, "法力不足", false, 0.8) end
      return false
    end
    SetVar("mana", manaNow - cost)
    return true
  end

  -- Q 冰霜新星: 以英雄为中心的冻结冲击
  if ActionPressed("skill_frost") and skillCd.frost <= 0 and spendMana(SKILLS.frost.mana) then
    skillCd.frost = SKILLS.frost.cd
    local pp = GetPosition(hero)
    if pp ~= nil then
      -- 第 0 拍 (0s): 环绕 8 点的冰粒向内汇聚
      for i = 0, 7 do
        local a = i / 8 * math.pi * 2
        local px, pz = pp.x + math.cos(a) * 3.2, pp.z + math.sin(a) * 3.2
        after(0.0, function()
          EmitParticles({
            pos = { x = px, y = 1.0, z = pz }, count = 6,
            vel = { x = -math.cos(a) * 6, y = 1.0, z = -math.sin(a) * 6 },
            speedMin = 0.3, speedMax = 1.2, lifeMin = 0.3, lifeMax = 0.45,
            sizeStart = 0.4, sizeEnd = 0.03,
            color = { r = 0.65, g = 0.92, b = 1.0, a = 1 },
            colorEnd = { r = 0.65, g = 0.92, b = 1.0, a = 0 },
            additive = true,
          })
        end)
      end
      -- 第 1 拍 (0.4s): 冻结爆发
      after(0.4, function()
        for _, hit in ipairs(wolvesInRadius(pp.x, pp.z, 7)) do
          damageWolf(hit.e, 120)
          ApplyStatus(hit.e, "slow", 3, 0.25)
        end
        fxRing("fx_ring_ice", pp.x, pp.z, 0.5, 7.2, 0.5)
        fxRing("fx_ring_ice", pp.x, pp.z, 0.3, 5.5, 0.7)
        burst(pp.x, pp.y + 1, pp.z, 40, 6, FAMILIES.ice[1], 0.7)
        spawnFx("fx_frost", pp.x, 0.07, pp.z, 14, 2.5)
        for i = 0, 5 do
          local a = i / 6 * math.pi * 2
          fxPillar("fx_pillar_ice", pp.x + math.cos(a) * 5.2, pp.z + math.sin(a) * 5.2, 1.6, 2.6, 1.8)
        end
        fxPillar("fx_pillar_ice", pp.x, pp.z, 2.0, 3.2, 2.0)
        flash({ 0.6, 0.85, 1.0 }, 0.35)
        PlaySfx(SKILLS.frost.sfx)
        shake(0.3)
      end)
      -- 第 2 拍 (0.9s): 余韵碎晶
      after(0.9, function()
        burst(pp.x, pp.y + 1.5, pp.z, 14, 2.5, FAMILIES.ice[1], 0.9)
      end)
    end
  end

  -- R 陨石术: 前方 9m 落点, 短暂指示后轰击
  if ActionPressed("skill_meteor") and skillCd.meteor <= 0 and spendMana(SKILLS.meteor.mana) then
    skillCd.meteor = SKILLS.meteor.cd
    local dir = attack_dir()
    local pp = GetPosition(hero)
    if pp ~= nil then
      local tx, tz = pp.x + dir.x * 9, pp.z + dir.z * 9
      vfxMeteors[#vfxMeteors + 1] = { wx = tx, wz = tz, t = 0, hitDone = false }
      PlayAnimation(hero, "Spellcasting", true, 0.15)
      -- 分层余韵 (引爆时序由 update_vfx 驱动, 这里追加延迟节拍)
      after(1.32, function() burst(tx, pp.y + 0.8, tz, 16, 4, FAMILIES.fire[2], 0.8) end)
      after(1.5, function()
        burst(tx, pp.y + 0.6, tz, 10, 2, { 0.35, 0.2, 0.12 }, 1.1)
        shake(0.18)
      end)
    end
  end

  -- G 圣光束: 面朝方向的灼热直线
  if ActionPressed("skill_beam") and skillCd.beam <= 0 and spendMana(SKILLS.beam.mana) then
    skillCd.beam = SKILLS.beam.cd
    local dir = attack_dir()
    local pp = GetPosition(hero)
    if pp ~= nil then
      local fx, fz = pp.x + dir.x * 1.5, pp.z + dir.z * 1.5
      local tx, tz = pp.x + dir.x * 15, pp.z + dir.z * 15
      for _, w in ipairs(wolves) do
        if not w.dead then
          local wp = GetPosition(w.e)
          if wp ~= nil then
            -- 点到线段距离 (2D)
            local vx, vz = tx - fx, tz - fz
            local len2 = vx * vx + vz * vz
            local t = ((wp.x - fx) * vx + (wp.z - fz) * vz) / (len2 > 0.001 and len2 or 1)
            t = clamp(t, 0, 1)
            local qx, qz = fx + vx * t, fz + vz * t
            if dist2d(wp.x, wp.z, qx, qz) < 1.3 then damageWolf(w.e, 260) end
          end
        end
      end
      for i = 1, 10 do
        local t = i / 10
        burst(fx + (tx - fx) * t, pp.y + 1.1, fz + (tz - fz) * t, 4, 2, FAMILIES.gold[1], 0.5)
      end
      burst(tx, pp.y + 1.1, tz, 24, 7, FAMILIES.fire[1], 0.7)
      PlayAnimation(hero, "Spellcast_Shoot", false, 0.15)
      -- 保持期扫射: 沿线每 0.09s 一处小爆 (0..0.45s)
      for k = 0, 5 do
        after(k * 0.09, function()
          local t = math.random()
          burst(fx + (tx - fx) * t, pp.y + 1.1, fz + (tz - fz) * t, 5, 2.5, FAMILIES.gold[1], 0.4)
        end)
      end
      burst(tx, pp.y + 1.1, tz, 24, 7, FAMILIES.fire[1], 0.7)
      flash({ 1.0, 0.95, 0.7 }, 0.25)
      PlaySfx(SKILLS.beam.sfx)
      shake(0.15)
    end
  end

  -- T 电击陷阱: 前方 6m 区域, 强减速 + 伤害
  if ActionPressed("skill_snare") and skillCd.snare <= 0 and spendMana(SKILLS.snare.mana) then
    skillCd.snare = SKILLS.snare.cd
    local dir = attack_dir()
    local pp = GetPosition(hero)
    if pp ~= nil then
      local tx, tz = pp.x + dir.x * 6, pp.z + dir.z * 6
      for _, hit in ipairs(wolvesInRadius(tx, tz, 5.5)) do
        damageWolf(hit.e, 80)
        ApplyStatus(hit.e, "slow", 2.5, 0.1)
      end
      -- 紫环: 越过半径再回弹 (snap)
      fxRing("fx_ring_volt", tx, tz, 6.4, 5.5, 0.35)
      fxRing("fx_ring_volt", tx, tz, 5.5, 5.5, 0.6)
      -- 第 1 拍 (0.12s): 中心柱爆
      after(0.12, function()
        fxPillar("fx_pillar_volt", tx, tz, 1.6, 5, 1.2)
        burst(tx, pp.y + 1.2, tz, 26, 3, FAMILIES.violet[1], 0.6)
        flash({ 0.8, 0.55, 1.0 }, 0.25)
        shake(0.15)
      end)
      -- 第 2 拍 (0.3s): 半径 8 点环绕 tendrils
      after(0.3, function()
        for i = 0, 7 do
          local a = i / 8 * math.pi * 2
          burst(tx + math.cos(a) * 5.5, pp.y + 0.5, tz + math.sin(a) * 5.5, 4, 1.5,
                FAMILIES.violet[1], 0.45)
        end
      end)
      PlayAnimation(hero, "Spellcast_Shoot", false, 0.15)
      PlaySfx(SKILLS.snare.sfx)
      shake(0.12)
    end
  end

  -- 死亡重生
  if vget("hp", 100) <= 0 then
    SetVar("hp", GetVar("max_hp"))
    SetPosition(hero, { x = 0, y = GROUND_Y, z = 4 })
    dialogue = { lines = { "你倒下了……在村庄中苏醒。" }, shown = 3 }
  end
end

local function update_wolves(dt)
  local pp = GetPosition(hero)
  if pp == nil then return end
  for _, w in ipairs(wolves) do
    if w.dead then
      if w.dieFade ~= nil and w.dieFade > 0 then
        -- 死亡表现：0.6s 内下沉+缩小，随后隐藏
        w.dieFade = w.dieFade - dt
        local t = math.max(0, w.dieFade) / 0.6
        local wp3 = GetPosition(w.e)
        if wp3 ~= nil then
          SetScale(w.e, t * 1.6, t * 0.6 + 0.1, t * 1.6)
          wp3.y = 0.55 * t
          SetPosition(w.e, wp3)
        end
        if w.dieFade <= 0 then SetVisible(w.e, false) end
      else
        w.respawn = w.respawn - dt
        if w.respawn <= 0 then
          w.dead = false
          SetVisible(w.e, true)
          SetScale(w.e, 1.6, 1.6, 1.6)
          SetEntityPlate(w.e, "野狼", 1.0)
          SetHealth(w.e, WOLF_MAX)
          SetPosition(w.e, { x = w.home.x, y = w.home.y, z = w.home.z })
        end
      end
    else
      w.attackCd = math.max(0, w.attackCd - dt)
      w.phase = w.phase + dt * 8
      local wp = GetPosition(w.e)
      if wp == nil then return end
      local hp = GetHealth(w.e)
      if hp ~= nil and hp <= 0 then
        -- 击杀：飘字 + 死亡表现（先缩放塌陷再隐藏，wolf 无死亡动画）
        w.dead = true
        w.respawn = 15
        w.dieFade = 0.6
        local wp2 = GetPosition(w.e)
        if wp2 ~= nil then
          local crit = vget("last_hit_crit", false)
          SpawnFloatText(wp2.x, wp2.y + 1.2, wp2.z, "-" .. tostring(math.floor(WOLF_MAX)), true, 1.2)
        end
        SetEntityPlate(w.e, nil, -1.0)
        SetVar("kills", vget("kills", 0) + 1)
        SetVar("gold", vget("gold", 0) + 5 + math.floor(vget("wave", 0)))
        local xp = vget("xp", 0) + 20
        SetVar("xp", xp)
        if xp >= vget("level", 1) * 100 then
          SetVar("xp", xp - vget("level", 1) * 100)
          level_up()
        end
        save_game()
      else
        local d = dist2d(wp.x, wp.z, pp.x, pp.z)
        local dh = dist2d(wp.x, wp.z, w.home.x, w.home.z)
        local chase = d < 14 and dh < 30
        local mx, mz = 0, 0
        local moving = false
        if chase then
          moving = true
          mx = (pp.x - wp.x) / d
          mz = (pp.z - wp.z) / d
          local vx = mx * 5.5
          local vz = mz * 5.5
          wp.x = wp.x + vx * dt
          wp.z = wp.z + vz * dt
          SetRotationY(w.e, math.atan(mx, mz))
          if d < 1.8 and w.attackCd <= 0 and iframes <= 0 then
            w.attackCd = 1.2
            SetVar("hp", math.max(0, vget("hp", 100) - 8))
            -- 狼扑咬一次性动画（walk 单次代替）
            PlayAnimation(w.e, "04_Idle", false, 0.1)
          end
        elseif dh > 1 then
          moving = true
          mx = (w.home.x - wp.x) / dh
          mz = (w.home.z - wp.z) / dh
          wp.x = wp.x + mx * 3 * dt
          wp.z = wp.z + mz * 3 * dt
          SetRotationY(w.e, math.atan(mx, mz))
        end
        -- 狼 locomotion：追击=跑，回家=走（glb clip 名 01_Run/02_walk）
        local want = chase and "01_Run" or "02_walk"
        if moving and w.animState ~= want then
          w.animState = want
          PlayAnimation(w.e, want, true, 0.25)
        elseif not moving and w.animState ~= "03" then
          w.animState = "03"
          PlayAnimation(w.e, "03_creep", true, 0.4)
        end
        -- 血条板随血量
        SetEntityPlate(w.e, "野狼", math.max(0, hp) / WOLF_MAX)
        wp.y = 0.55 + math.sin(w.phase) * 0.06
        SetPosition(w.e, wp)
      end
    end
  end
end

local function update_waves(dt)
  waveTimer = waveTimer - dt
  if waveTimer <= 0 then
    local wave = vget("wave", 0) + 1
    SetVar("wave", wave)
    spawn_wave(wave)
    waveTimer = 22 + wave * 2
    dialogue = { lines = { "第 " .. tostring(wave) .. " 波狼群来袭！" }, shown = 2.5 }
    save_game()
  end
end

local function update_npc(dt)
  local pp = GetPosition(hero)
  if pp == nil then return end
  if dialogue ~= nil then
    dialogue.shown = dialogue.shown - dt
    if dialogue.shown <= 0 then dialogue = nil end
  end
  if dist2d(pp.x, pp.z, npcPos.x, npcPos.z) < 3 and ActionPressed("interact")
      and dialogue == nil then
    dialogue = {
      lines = {
        "村长：欢迎来到霓虹大陆！",
        "狼群正在威胁村庄，用 WASD 移动、左键近战、1 火球、2 治疗。",
        "鼠标控制视角，按 V 在第一人称/轨道视角间切换。",
        "击败狼群可获得经验与金币。",
      },
      shown = 6,
    }
  end
end

local function update_hero_anim(dt)
  -- 动作一次性播放期间不切 locomotion
  if heroAct ~= nil then
    if AnimationFinished(hero) or AnimationProgress(hero) >= 0.98 then
      heroAct = nil
      heroAnim = "" -- 强制下一帧重选 locomotion
    else
      return
    end
  end
  -- locomotion：读条中=施法姿态（update_player 已设）；否则按输入速度
  if castTime >= 0 then return end
  local ix = ActionAxis("move_strafe")
  local iz = ActionAxis("move_forward")
  local speed = math.abs(ix) + math.abs(iz)
  local want
  if dashTime > 0 or speed > 0.85 then
    want = "Running_A"
  elseif speed > 0.15 then
    want = "Walking_A"
  else
    want = "Idle"
  end
  if want ~= heroAnim then
    heroAnim = want
    PlayAnimation(hero, want, true, 0.25)
  end
end

local function update_camera()
  local pos = GetPosition(hero)
  if pos == nil then return end
  if fpsMode then
    -- 第一人称：引擎相机在 focus + offset*camDist 处，offset 由 cameraYaw/Pitch 决定。
    -- 把 focus 放到视线前方 CAM_DIST 处，相机即精确落在眼睛点 (pos + EYE_H)。
    local cd = math.cos(lookPitch)
    local eyeX, eyeY, eyeZ = pos.x, pos.y + EYE_H, pos.z
    SetVar("cameraFocus", {
      x = eyeX - math.sin(lookYaw) * cd * CAM_DIST,
      y = eyeY - math.sin(lookPitch) * CAM_DIST,
      z = eyeZ - math.cos(lookYaw) * cd * CAM_DIST,
    })
    SetVar("cameraYaw", lookYaw)
    SetVar("cameraPitch", lookPitch)
    SetVar("cameraDist", CAM_DIST)
  else
    -- 轨道视角（第三人称观察位）: 相机绕英雄头顶拉远, 俯角观察特效
    SetVar("cameraFocus", { x = pos.x, y = pos.y + 1.2, z = pos.z })
    SetVar("cameraDist", 7.5)
    SetVar("cameraPitch", 0.42)
  end
  -- 施法震屏: 随 shakeT 衰减的随机焦点抖动
  if shakeT > 0 then
    local f = GetVar("cameraFocus")
    if f ~= nil then
      local m = shakeT * shakeT * 0.6
      SetVar("cameraFocus", { x = f.x + (math.random() - 0.5) * m,
                              y = f.y + (math.random() - 0.5) * m * 0.5,
                              z = f.z + (math.random() - 0.5) * m })
    end
    shakeT = math.max(0, shakeT - 0.016)
  end
end

-- 世界锚定 VFX 推进: 特效到期回收 + 陨石时序 (指示 -> 落体 -> 轰击)
local function update_vfx(dt)
  vfxClock = vfxClock + dt
  -- 多节拍时间线: 到点的延迟节拍出队执行
  for i = #scheduled, 1, -1 do
    if vfxClock >= scheduled[i].at then
      local fn = table.remove(scheduled, i).fn
      fn()
    end
  end
  -- 冲击闪光衰减
  for i = #flashes, 1, -1 do
    flashes[i].age = flashes[i].age + dt
    if flashes[i].age >= flashes[i].life then table.remove(flashes, i) end
  end
  for i = #vfxEnts, 1, -1 do
    if vfxClock >= vfxEnts[i].dieAt then
      Despawn(vfxEnts[i].ent)
      table.remove(vfxEnts, i)
    end
  end
  for i = #vfxMeteors, 1, -1 do
    local mt = vfxMeteors[i]
    mt.t = mt.t + dt
    if not mt.hitDone and mt.t >= 0.9 and not mt.orb then
      mt.orb = SpawnPrefab("fx_meteor", { x = mt.wx, y = 20, z = mt.wz })
      if mt.orb ~= nil then
        SetScale(mt.orb, 0.9, 0.9, 0.9)
        Tween(mt.orb, 0, { x = mt.wx, y = 20, z = mt.wz }, { x = mt.wx, y = 0.8, z = mt.wz }, 0.35, 1)
      end
    end
    if not mt.hitDone and mt.t >= 1.25 then
      mt.hitDone = true
      if mt.orb ~= nil then Despawn(mt.orb) end
      for _, hit in ipairs(wolvesInRadius(mt.wx, mt.wz, 5)) do
        damageWolf(hit.e, 400)
      end
      fxRing("fx_ring_fire", mt.wx, mt.wz, 1, 6.5, 0.45)
      fxRing("fx_ring_fire", mt.wx, mt.wz, 0.5, 4.5, 0.3)
      spawnFx("fx_scorch", mt.wx, 0.06, mt.wz, 9, 3)
      fxOrbBurst("fire", mt.wx, 1, mt.wz, 34, 8, 0.45, 0.8)
      PlaySfx(SKILLS.meteor.sfx)
      shake(0.5)
    end
    if mt.hitDone and mt.t >= 1.4 then table.remove(vfxMeteors, i) end
  end
end

function on_update(e, dt)
  -- 技能冷却推进
  for k, v in pairs(skillCd) do
    if v > 0 then skillCd[k] = math.max(0, v - dt) end
  end
  update_vfx(dt)
  update_player(dt)
  update_hero_anim(dt)
  update_wolves(dt)
  update_waves(dt)
  update_npc(dt)
  saveTimer = saveTimer + dt
  if saveTimer >= 15 then
    saveTimer = 0
    save_game()
  end
  update_camera()
end

local function bar(x, y, w, h, t, cr, cg, cb)
  DrawRect(x, y, w, h, 0.08, 0.08, 0.10, 0.85)
  if t > 0 then
    DrawRect(x, y, w * t, h, cr, cg, cb, 1)
  end
  DrawRectOutline(x, y, w, h, 1, 0.5, 0.5, 0.5, 0.6)
end

function on_render()
  -- 技能栏 (左下角: 键位 + 冷却蒙版)
  local slotW, slotH, gap = 46, 46, 6
  local bx, by = 14, 720 - slotH - 14
  local slots = {
    { k = "Q", id = "frost",  label = "冰" },
    { k = "R", id = "meteor", label = "陨" },
    { k = "G", id = "beam",   label = "束" },
    { k = "T", id = "snare",  label = "阱" },
  }
  for i, sl in ipairs(slots) do
    local x = bx + (i - 1) * (slotW + gap)
    local ready = skillCd[sl.id] <= 0
    local bgc = ready and 0.14 or 0.08
    DrawRect(x, by, slotW, slotH, bgc, bgc + 0.03, bgc + 0.06, 0.9)
    DrawRectOutline(x, by, slotW, slotH, 1.5,
                    ready and 0.5 or 0.3, ready and 0.9 or 0.3, ready and 0.9 or 0.4, 0.9)
    local cdv = skillCd[sl.id]
    if cdv > 0 then
      local frac = math.min(1, cdv / SKILLS[sl.id].cd)
      DrawRect(x, by + slotH * (1 - frac), slotW, slotH * frac, 0, 0, 0, 0.65)
      DrawText(string.format("%.0f", math.ceil(cdv)), x + slotW / 2, by + slotH * 0.5, 16,
               1, 1, 1, 1, true, true)
    else
      DrawText(sl.label, x + slotW / 2, by + slotH * 0.5, 20, 0.95, 0.98, 1, 1, true, true)
    end
    DrawText(sl.k, x + 4, by + 3, 12, 1, 0.85, 0.3, 1, false, false)
  end
  -- 全屏冲击闪光 (各技能色 tint)
  for _, fl in ipairs(flashes) do
    local a = (1 - fl.age / fl.life) * 0.22
    DrawRect(0, 0, 1280, 720, fl.col[1], fl.col[2], fl.col[3], a)
  end

  -- ============ 原有 HUD ============
  -- HUD 面板
  DrawRect(14, 14, 300, 96, 0.10, 0.12, 0.16, 0.82)
  local hp = vget("hp", 100)
  local maxHp = vget("max_hp", 100)
  local mana = vget("mana", 50)
  local maxMana = vget("max_mana", 50)
  local xp = vget("xp", 0)
  local lv = math.floor(vget("level", 1))
  local gold = math.floor(vget("gold", 0))
  local wave = math.floor(vget("wave", 0))
  local kills = math.floor(vget("kills", 0))
  DrawText("霓虹大陆 · 等级 " .. tostring(lv) .. " · 金币 " .. tostring(gold), 26, 22, 18,
           1, 1, 1, 1, false, false)
  bar(26, 46, 276, 14, hp / math.max(1, maxHp), 0.85, 0.2, 0.2)
  bar(26, 64, 276, 12, mana / math.max(1, maxMana), 0.25, 0.5, 1)
  DrawText("HP " .. tostring(math.floor(hp)) .. "/" .. tostring(math.floor(maxHp)),
           308, 46, 13, 1, 0.8, 0.8, 1, true, false)
  bar(26, 80, 276, 8, xp / math.max(1, lv * 100), 0.9, 0.75, 0.2)
  DrawText("经验 " .. tostring(math.floor(xp)) .. "/" .. tostring(lv * 100),
           308, 78, 12, 0.9, 0.8, 0.4, 1, true, false)
  DrawText("第 " .. tostring(wave) .. " 波 · 击杀 " .. tostring(kills), 26, 92, 14,
           0.7, 0.9, 0.7, 1, false, false)

  -- 技能栏（M1 WoW 风格：图标格子 + CD 遮罩 + 读条条）
  local barX, barY = 560, 646
  local cell, gap = 52, 6
  local skills = {
    { key = "1", name = "火球", cd = fireCd, cdMax = FIRE_CD, mana = 10 },
    { key = "2", name = "治疗", cd = 0, cdMax = 1, mana = 15 },
  }
  for i, s in ipairs(skills) do
    local x = barX + (i - 1) * (cell + gap)
    DrawRect(x, barY, cell, cell, 0.10, 0.12, 0.18, 0.92)
    DrawRectOutline(x, barY, cell, cell, 1.5, 0.45, 0.55, 0.75, 0.9)
    -- CD 遮罩（自底向上消退）+ 秒数
    if s.cd > 0 then
      local frac = s.cd / s.cdMax
      DrawRect(x, barY + cell * (1 - frac), cell, cell * frac, 0, 0, 0, 0.72)
      if s.cd > 0.05 then
        DrawText(string.format("%.1f", s.cd), x + cell / 2, barY + cell / 2 - 8, 15,
                 1, 1, 1, 1, true, true)
      end
    end
    DrawText(s.key, x + 4, barY + 2, 11, 0.9, 0.9, 0.9, 0.9)
    DrawText(s.name, x + cell / 2, barY + cell - 15, 11, 0.85, 0.9, 1, 1, true, true)
  end
  -- 左键近战/右键冲刺小格
  local x2 = barX + 2 * (cell + gap)
  DrawRect(x2, barY, cell, cell, 0.10, 0.12, 0.18, 0.92)
  DrawRectOutline(x2, barY, cell, cell, 1.5, 0.45, 0.55, 0.75, 0.9)
  DrawText("LMB", x2 + cell / 2, barY + cell / 2 - 8, 12, 0.9, 0.9, 0.9, 1, true, true)
  DrawText("近战", x2 + cell / 2, barY + cell - 15, 11, 0.85, 0.9, 1, 1, true, true)
  if punchCd > 0 then
    DrawRect(x2, barY + cell * (1 - punchCd / 0.55), cell, cell * (punchCd / 0.55),
             0, 0, 0, 0.72)
  end

  -- 施法读条（屏幕中下）
  if castTime >= 0 then
    local w, h = 240, 16
    local cx, cy = 640 - w / 2, 590
    DrawRect(cx, cy, w, h, 0.05, 0.05, 0.08, 0.85)
    DrawRect(cx + 2, cy + 2, (w - 4) * math.min(1, castTime / CAST_DUR), h - 4,
             1.0, 0.6, 0.15, 0.95)
    DrawRectOutline(cx, cy, w, h, 1.5, 0.9, 0.7, 0.3, 0.9)
    DrawText("火球术", cx + w / 2, cy - 18, 13, 1, 0.85, 0.5, 1, true, true)
  end

  -- 小地图（右上角，世界坐标直接缩放）
  local pp = GetPosition(hero)
  local mx, mz = 0, 0
  if pp ~= nil then mx, mz = pp.x, pp.z end
  local mapX, mapY, mapS = 1090, 20, 170
  DrawRect(mapX, mapY, mapS, mapS, 0.06, 0.10, 0.08, 0.85)
  DrawRectOutline(mapX, mapY, mapS, mapS, 2, 0.5, 0.6, 0.5, 0.8)
  local function mappoint(x, z)
    return mapX + mapS * 0.5 + x * (mapS / 96), mapY + mapS * 0.5 + z * (mapS / 96)
  end
  local px2, pz2 = mappoint(mx, mz)
  DrawRect(px2 - 3, pz2 - 3, 6, 6, 0.3, 0.8, 1, 1)
  local npx, npz = mappoint(npcPos.x, npcPos.z)
  DrawRect(npx - 2, npz - 2, 4, 4, 1, 0.85, 0.3, 1)
  for _, w in ipairs(wolves) do
    if not w.dead then
      local wp = GetPosition(w.e)
      if wp ~= nil then
        local wx, wz = mappoint(wp.x, wp.z)
        DrawRect(wx - 2, wz - 2, 4, 4, 0.9, 0.2, 0.2, 1)
      end
    end
  end

  -- 对话
  if dialogue ~= nil then
    DrawRect(250, 500, 780, 90, 0.05, 0.07, 0.12, 0.92)
    DrawRectOutline(250, 500, 780, 90, 2, 0.6, 0.7, 0.9, 0.7)
    local y = 522
    for _, line in ipairs(dialogue.lines) do
      DrawText(line, 272, y, 17, 1, 1, 1, 1, false, false)
      y = y + 24
    end
  end

  -- M1: 实体头顶板（血条 + 名字）。锚点与铭牌数据由引擎每帧投影/登记。
  local anchors = ScreenAnchors()
  local plates = EntityPlates()
  for i = 1, #anchors do
    local a = anchors[i]
    if a.onscreen then
      local key = string.format("%d_%d", a.entity.id, a.entity.gen)
      local p = plates[key]
      if p ~= nil and p.hp >= 0 then
        local w, h = 56, 5
        DrawRect(a.x - w / 2, a.y - h, w, h, 0.05, 0.05, 0.08, 0.8)
        if p.hp > 0 then
          DrawRect(a.x - w / 2 + 1, a.y - h + 1, (w - 2) * p.hp, h - 2,
                   0.85, 0.2, 0.2, 1)
        end
        outlined_text(a.x, a.y - h - 16, 13, p.name, 1, 0.82, 0.2)
      end
    end
  end

  -- M1: 伤害/提示飘字（世界锚定，上飘淡出）。
  local texts = FloatTexts()
  for i = 1, #texts do
    local f = texts[i]
    local age = f.age / math.max(0.01, f.life)
    local s = WorldToScreen(f.world.x, f.world.y + age * 0.8, f.world.z)
    if s ~= nil and age < 1 then
      local size = f.crit and 22 or 16
      local alpha = 1 - age
      if f.crit then
        DrawText(f.text, s.x, s.y, size, 1, 0.85, 0.3, alpha, true, true)
      else
        DrawText(f.text, s.x, s.y, size, 1, 1, 1, alpha, true, true)
      end
    end
  end
end

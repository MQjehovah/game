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
    -- 轨道视角：相机绕英雄头顶，鼠标拖拽旋转（cameraYaw 由引擎写回）
    SetVar("cameraFocus", { x = pos.x, y = pos.y + 1.5, z = pos.z })
  end
end

function on_update(e, dt)
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

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

local WOLF_MAX = 45
local GROUND_Y = 0.9 -- 英雄站立高度

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

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

  -- 收集预摆放的狼（场景里 12 只，脚本管理生死/波次）
  for i = 1, 12 do
    local w = FindNamedEntity("狼_" .. i)
    if w ~= nil then
      local p = GetPosition(w)
      table.insert(wolves, {
        e = w, home = { x = p.x, y = p.y, z = p.z },
        dead = false, respawn = 0, attackCd = 0, phase = i * 0.7,
      })
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
  local mana = vget("mana", 50)
  local maxMana = vget("max_mana", 50)
  SetVar("mana", math.min(maxMana, mana + 3 * dt))

  -- 相机相对移动
  local cy = vget("cameraYaw", 0)
  local ix = InputAxis("strafe")
  local iz = InputAxis("forward")
  local fwd = { x = -math.sin(cy), z = -math.cos(cy) }
  local right = { x = math.cos(cy), z = -math.sin(cy) }
  local dx = right.x * ix + fwd.x * iz
  local dz = right.z * ix + fwd.z * iz
  local len = math.sqrt(dx * dx + dz * dz)
  if len > 1 then dx, dz = dx / len, dz / len end

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
  if InputKey("space") > 0 and grounded then
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

  -- 面向移动方向
  if len > 0.01 then
    facing = math.atan(dx, -dz)
    SetRotationY(hero, facing)
  end

  -- 近战（左键）
  if InputMousePressed("left") then
    local origin = { x = pos.x, y = pos.y + 1.2, z = pos.z }
    local dir = { x = math.sin(facing), y = 0, z = -math.cos(facing) }
    MeleeAttack(origin, dir, 2.2, 100, 28)
  end

  -- 火球（1）与治疗（2）
  local manaNow = vget("mana", 0)
  if InputKey("1") > 0 and manaNow >= 10 then
    SetVar("mana", manaNow - 10)
    local origin = { x = pos.x, y = pos.y + 1.2, z = pos.z }
    local dir = { x = math.sin(facing), y = 0, z = -math.cos(facing) }
    SpawnProjectile(origin, dir, 16, 30, 2.0, hero)
  end
  if InputKey("2") > 0 and manaNow >= 15 then
    SetVar("mana", manaNow - 15)
    local hp = vget("hp", 100)
    SetVar("hp", math.min(vget("max_hp", 100), hp + 35))
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
      w.respawn = w.respawn - dt
      if w.respawn <= 0 then
        w.dead = false
        SetVisible(w.e, true)
        SetHealth(w.e, WOLF_MAX)
        SetPosition(w.e, { x = w.home.x, y = w.home.y, z = w.home.z })
      end
    else
      w.attackCd = math.max(0, w.attackCd - dt)
      w.phase = w.phase + dt * 8
      local wp = GetPosition(w.e)
      if wp == nil then return end
      local hp = GetHealth(w.e)
      if hp ~= nil and hp <= 0 then
        -- 击杀奖励
        w.dead = true
        w.respawn = 15
        SetVisible(w.e, false)
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
        if chase then
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
          end
        elseif dh > 1 then
          mx = (w.home.x - wp.x) / dh
          mz = (w.home.z - wp.z) / dh
          wp.x = wp.x + mx * 3 * dt
          wp.z = wp.z + mz * 3 * dt
          SetRotationY(w.e, math.atan(mx, mz))
        end
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
  if dist2d(pp.x, pp.z, npcPos.x, npcPos.z) < 3 and InputKey("f") > 0
      and dialogue == nil then
    dialogue = {
      lines = {
        "村长：欢迎来到霓虹大陆！",
        "狼群正在威胁村庄，用 WASD 移动、左键近战、1 火球、2 治疗。",
        "击败狼群可获得经验与金币。",
      },
      shown = 6,
    }
  end
end

function on_update(e, dt)
  update_player(dt)
  update_wolves(dt)
  update_waves(dt)
  update_npc(dt)
  saveTimer = saveTimer + dt
  if saveTimer >= 15 then
    saveTimer = 0
    save_game()
  end
  -- 相机跟随英雄
  local pos = GetPosition(hero)
  if pos ~= nil then
    SetVar("cameraFocus", { x = pos.x, y = pos.y + 1.5, z = pos.z })
  end
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

  -- 技能栏
  DrawText("1 火球 (10)   2 治疗 (15)   左键近战   右键冲刺", 26, 120, 15,
           0.85, 0.9, 1, 1, false, false)

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
end

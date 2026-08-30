-- Warcraft-like RTS (3D 版): 3D 世界 + 俯视角透视相机 + 投影 UI。
-- 系统: 选择/指令/采矿经济/建筑放置/生产队列/敌军 AI 波次/胜负。
-- 拾取: 屏幕空间最近匹配 (选择单位/目标) + 射线-地面求交 (移动落点)。

-- ============ 相机常量 (与 scenes/main.json 的相机严格一致) ============
-- ============ WC3 式相机: 平移 (方向键/屏幕边缘) + 滚轮缩放 ============
-- 朝向固定 (俯视斜角), 与 scenes/main.json 相机一致; 位置每帧驱动相机实体。
local CAM_FWD = { x = 0, y = -0.71934, z = -0.69466 }
local CAM_RIGHT = { x = 1, y = 0, z = 0 }
local CAM_UP = { x = 0, y = 0.69466, z = -0.71934 }
local TAN_Y = 0.5206 -- fov 55°/2
local TAN_X = 0.9254 -- * 16/9
local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
-- 地形高度采样: 返回世界 (x,z) 处的地面 Y (含地形实体偏移; 无地形时 0)。
-- 单位出生/移动/建筑放置都靠它贴地, 避免悬空或陷地。
local function groundY(x, z)
  return GetGroundHeight(x, z)
end
local camX, camZ = 50, 40   -- 视线焦点 (地面)
local camDist = 38.0        -- 相机沿视线轴的距离 (拉近, 单位更清晰)
local camEnt = nil          -- 场景相机实体

-- 框选: 左键空地按下拖拽 → 释放选中屏幕矩形内所有我方单位
local boxSel = { active = false, sx = 0, sy = 0, cx = 0, cy = 0 }

local function camPosNow()
  return { x = camX, y = 0.71934 * camDist, z = camZ + 0.69466 * camDist }
end

-- 屏幕像素 -> 地面 (y=0) 世界点 (用实时相机位置)
local function groundPick(sx, sy)
  local cp = camPosNow()
  local nx = (sx / 1280) * 2 - 1
  local ny = 1 - (sy / 720) * 2
  local dx = CAM_FWD.x + CAM_RIGHT.x * nx * TAN_X + CAM_UP.x * ny * TAN_Y
  local dy = CAM_FWD.y + CAM_RIGHT.y * nx * TAN_X + CAM_UP.y * ny * TAN_Y
  local dz = CAM_FWD.z + CAM_RIGHT.z * nx * TAN_X + CAM_UP.z * ny * TAN_Y
  if dy >= -0.001 then return nil end
  -- 与地形高度场求交: 从相机沿射线步进, 找射线高度降到地形高度之下的第一个点。
  -- 初始 t 用平地近似 (y=0), 再迭代向地形收敛 (起伏地形上选点/落点正确)。
  local t = -cp.y / dy
  for _ = 1, 10 do
    local px = cp.x + dx * t
    local py = cp.y + dy * t
    local pz = cp.z + dz * t
    local g = groundY(px, pz)
    if py <= g then break end
    -- 线性收敛到下个穿越点
    local step = (py - g) / (-dy)
    t = t + step
  end
  local gx = cp.x + dx * t
  local gz = cp.z + dz * t
  return { x = gx, z = gz }
end

local function updateCamera(dt)
  -- 方向键平移 (屏幕对齐: 上=-z 下=+z 左=-x 右=+x), 速度随缩放
  local pan = 30.0 * (camDist / 38.0)
  if ActionDown("cam_up") then camZ = camZ - pan * dt end
  if ActionDown("cam_down") then camZ = camZ + pan * dt end
  if ActionDown("cam_left") then camX = camX - pan * dt end
  if ActionDown("cam_right") then camX = camX + pan * dt end
  -- 屏幕边缘平移 (WC3 招牌操作)
  local m = InputMousePos()
  if m then
    local edge = 10
    if m.x <= edge then camX = camX - pan * dt end
    if m.x >= 1280 - edge then camX = camX + pan * dt end
    if m.y <= edge then camZ = camZ - pan * dt end
    if m.y >= 720 - edge then camZ = camZ + pan * dt end
  end
  camX = clamp(camX, 5, 95)
  camZ = clamp(camZ, 8, 72)
  -- 滚轮缩放 (拉远/拉近)
  camDist = clamp(camDist - MouseWheel() * 5.0, 24, 130)
  -- 驱动场景相机实体
  if camEnt ~= nil then
    SetPosition(camEnt, { x = camX, y = 0.71934 * camDist, z = camZ + 0.69466 * camDist })
  end
end

-- ============ 数据表 ============
-- 兵种属性: armor(护甲类型 light/medium/heavy) + atk(攻击类型 normal/pierce/magic)
-- 克制表 MOD[atk][armor] 决定实际伤害倍率 (见下方 MODIFIER)。
local UNIT_DEFS = {
  peon     = { hp = 90,  dmg = 6,  range = 2.2, cd = 1.0, speed = 5.5, r = 0.7, cost = 75,  bt = 6,  name = "苦工", prefab = "unit_peon",     armor = "light",  atk = "normal" },
  footman  = { hp = 260, dmg = 22, range = 2.4, cd = 1.1, speed = 4.6, r = 0.8, cost = 120, bt = 9,  name = "步兵", prefab = "unit_footman", armor = "medium", atk = "normal" },
  rifleman = { hp = 160, dmg = 21, range = 12,  cd = 1.6, speed = 4.2, r = 0.7, cost = 180, bt = 12, name = "火枪手", ranged = true, prefab = "unit_rifleman", armor = "medium", atk = "pierce" },
  knight   = { hp = 420, dmg = 28, range = 2.4, cd = 1.3, speed = 4.4, r = 0.9, cost = 260, bt = 14, name = "骑士", prefab = "unit_knight", armor = "heavy",  atk = "normal" },
  mage     = { hp = 130, dmg = 34, range = 13,  cd = 1.8, speed = 4.0, r = 0.7, cost = 300, bt = 16, name = "法师", ranged = true, prefab = "unit_mage", armor = "light",  atk = "magic" },
  grunt    = { hp = 200, dmg = 16, range = 2.4, cd = 1.2, speed = 4.8, r = 0.8, name = "兽人步兵", prefab = "unit_grunt", armor = "medium", atk = "normal" },
  hero     = { hp = 500, dmg = 30, range = 10,  cd = 1.4, speed = 4.8, r = 0.9, name = "英雄·法师", ranged = true, prefab = "unit_mage", armor = "medium", atk = "magic" },
}
-- 攻击类型 vs 护甲类型的伤害倍率 (norm/pierce/magic × light/medium/heavy)
local MODIFIER = {
  normal = { light = 1.0, medium = 1.0, heavy = 0.75 },
  pierce = { light = 1.3, medium = 1.0, heavy = 0.85 },
  magic  = { light = 1.0, medium = 1.15, heavy = 1.35 },
}
-- 单位动画剪辑 (Kaykit 骨骼模型内嵌; Wolf/苦工无可用 idle/walk 剪辑则不播)
local CLIPS = {
  footman  = { idle = "Idle", move = "Walking_A", atk = "1H_Melee_Attack_Chop", death = "Death_A" },
  rifleman = { idle = "Idle", move = "Walking_A", atk = "1H_Melee_Attack_Chop", death = "Death_A" },
  knight   = { idle = "Idle", move = "Walking_A", atk = "1H_Melee_Attack_Chop", death = "Death_A" },
  mage     = { idle = "Idle", move = "Walking_A", atk = "1H_Melee_Attack_Chop", death = "Death_A" },
  grunt    = { idle = nil, move = "01_Run_Armature_0", atk = nil, death = nil },
  hero     = { idle = "Idle", move = "Walking_A", atk = "1H_Melee_Attack_Chop", death = "Death_A" },
}
-- 模型朝向修正: 不同来源的模型默认面朝不同 (Kaykit=-Z 正确, CesiumMan=+Z 需翻转 180)
local YAW_FIX = { peon = math.pi, footman = 0, rifleman = 0, knight = 0, mage = 0, grunt = 0, hero = 0 }
local animState = {}   -- unit.id -> 当前循环 clip
local dying = {}       -- unit.id -> 死亡动画剩余时间
-- ============ 状态 ============
local units, buildings = {}, {}
local selected, selectedBuilding = {}, nil
local buildMode = nil
local atkCd = {}
local waveT, waveN = 25, 0
local gameover, won = false, false
local ids = 0
local function dist(x1, z1, x2, z2) return math.sqrt((x1 - x2) ^ 2 + (z1 - z2) ^ 2) end
local function newId() ids = ids + 1; return ids end

-- ============ 科技 (兵营升级) ============
-- 玩家科技等级: 每级全体攻击 +12%、生命 +12% (敌方无科技, 纯玩家成长线)。
local techAtk, techDef = 0, 0
local TECH_COST = 250       -- 每次升级的金币成本 (随等级递增)
local function techAtkMul() return 1.0 + techAtk * 0.12 end
local function techDefMul() return 1.0 + techDef * 0.12 end

-- ============ 英雄技能 ============
-- 选中英雄时屏幕底部显示技能栏; Q=火球 AoE, W=治疗光环。
local skillCd = { fire = 0, heal = 0 }   -- 冷却计时
local SKILL_COOLDOWN = { fire = 6.0, heal = 10.0 }
local function heroCaster()   -- 当前选中的英雄单位
  if #selected == 1 and selected[1].kind == "hero" then return selected[1] end
  return nil
end
local function fireball(h)
  local best, bd = nil, 1e9
  for _, o in ipairs(units) do if o.team ~= h.team and dist(h.x, h.z, o.x, o.z) < 8 then
    if dist(h.x, h.z, o.x, o.z) < bd then best, bd = o, dist(h.x, h.z, o.x, o.z) end end end
  if not best then return end
  local u = UNIT_DEFS[h.kind]
  local dir = { x = (best.x - h.x) / (bd + 0.001), y = -0.10, z = (best.z - h.z) / (bd + 0.001) }
  SpawnProjectile({ x = h.x, y = 2.6, z = h.z }, dir, 26, u.dmg * 1.6, 2.6, h.ent)
  PlaySfx("shoot")
end
local function healHalo(h)
  for _, o in ipairs(units) do
    if o.team == h.team and dist(h.x, h.z, o.x, o.z) < 9 and o.hp > 0 and o.hp < o.maxHp then
      o.hp = math.min(o.maxHp, o.hp + 40)
      SpawnFloatText(o.x, 2.0, o.z, "+40", false, 0.8)
    end
  end
  SpawnFloatText(h.x, 2.2, h.z, "治疗", false, 1.0)
end
-- 技能冷却推进 + 施放入口 (on_update 调用)
local function updateSkills(dt)
  for k, v in pairs(skillCd) do skillCd[k] = math.max(0, v - dt) end
end
local function tryCast(h, key)
  if skillCd[key] > 0 then return end
  skillCd[key] = SKILL_COOLDOWN[key]
  if key == "fire" then fireball(h) else healHalo(h) end
end
-- 英雄技能栏 (选中英雄时, 屏幕右下角两个按钮): Q=火球, W=治疗
local SKILL_SLOTS = {
  { key = "fire", name = "火球(Q)", x = 1030, y = 640, w = 110, h = 46 },
  { key = "heal", name = "治疗(W)", x = 1150, y = 640, w = 110, h = 46 },
}
local function skillHit(mx, my)
  for _, s in ipairs(SKILL_SLOTS) do
    if mx >= s.x and mx <= s.x + s.w and my >= s.y and my <= s.y + s.h then return s end
  end
  return nil
end

local BUILD_DEFS = {
  hall     = { hp = 900, w = 5,  d = 5,  cost = 0,   bt = 0,  name = "大本营", prefab = "bld_hall", trains = { "peon" } },
  farm     = { hp = 340, w = 2.6, d = 2.6, cost = 80,  bt = 8,  name = "农场", prefab = "bld_farm" },
  barracks = { hp = 520, w = 3.6, d = 3.2, cost = 160, bt = 14, name = "兵营", prefab = "bld_barracks", trains = { "footman", "rifleman", "knight" } },
  mine     = { hp = 1200, w = 3.4, d = 3.4, name = "金矿", prefab = "fx_mine" },
  tower    = { hp = 620, w = 1.4, d = 1.4, cost = 220, bt = 14, name = "箭塔", tower = true, range = 14, dmg = 18, cd = 1.4, prefab = "bld_tower" },
}

-- ============ 状态 ============
local MINES = {
  { x = 10, z = 62 },
  { x = 92, z = 14 },
}

-- ============ 战斗特效 ============
-- 命中火花: 加色粒子四溅 (单位受击/攻击命中的打击感)
local function hitFx(x, z, sharp)
  local y = groundY(x, z) + 0.8
  EmitParticles({
    pos = { x = x, y = y, z = z },
    count = sharp and 10 or 6,
    speedMin = 2, speedMax = 5,
    lifeMin = 0.2, lifeMax = 0.45,
    sizeStart = sharp and 0.35 or 0.25, sizeEnd = 0.04,
    color = { 1.0, 0.85, 0.35, 0.9 },
    colorEnd = { 1.0, 0.4, 0.1, 0.0 },
    gravity = -6,
    additive = true,
  })
end
-- 死亡爆炸: 大范围粒子 + 冲击波 (单位被击杀)
local function deathFx(x, z, big)
  local y = groundY(x, z) + 0.6
  EmitParticles({
    pos = { x = x, y = y, z = z },
    count = big and 22 or 12,
    speedMin = 3, speedMax = 8,
    lifeMin = 0.4, lifeMax = 0.9,
    sizeStart = big and 0.6 or 0.4, sizeEnd = 0.05,
    color = { 0.9, 0.4, 0.2, 0.95 },
    colorEnd = { 0.3, 0.2, 0.1, 0.0 },
    gravity = -8,
    additive = true,
  })
end
-- 远程攻击弹道火花 (射手开火)
local function muzzleFx(x, z)
  local y = groundY(x, z) + 1.2
  EmitParticles({
    pos = { x = x, y = y, z = z },
    count = 4, speedMin = 1, speedMax = 3,
    lifeMin = 0.1, lifeMax = 0.25,
    sizeStart = 0.3, sizeEnd = 0.03,
    color = { 1.0, 0.9, 0.4, 0.9 },
    colorEnd = { 1.0, 0.5, 0.1, 0.0 },
    additive = true,
  })
end

-- 单位循环动画: clip 变化时才重触发 (PlayAnimation 每帧调用会重置进度)
local function driveAnim(u, wantClip)
  if u.ent == nil then return end
  if animState[u.id] ~= wantClip then
    animState[u.id] = wantClip
    if wantClip and wantClip ~= "" then
      PlayAnimation(u.ent, wantClip, true, 0.15)
    end
  end
end

-- 攻击一次性动画
local function playAttackAnim(u)
  local c = CLIPS[u.kind]
  if c and c.atk and u.ent ~= nil then PlayAnimation(u.ent, c.atk, false, 0.1) end
end

-- 死亡动画 + 延迟移除 + 击杀奖励 (兽人死亡 = 玩家 +8 金)
local function startDeath(u)
  local c = CLIPS[u.kind]
  if c and c.death and u.ent ~= nil then PlayAnimation(u.ent, c.death, false, 0.1) end
  dying[u.id] = { ent = u.ent, t = (c and c.death) and 1.2 or 0.0 }
  deathFx(u.x, u.z, false)
  if u.team == "enemy" then
    SetVar("gold_player", (GetVar("gold_player") or 0) + 8)
    SpawnFloatText(u.x, 2.0, u.z, "+8", false, 0.8)
  else
    SetVar("gold_enemy", (GetVar("gold_enemy") or 0) + 8)
  end
end

local function spawnUnit(kind, team, x, z)
  local d = UNIT_DEFS[kind]
  local ent = SpawnPrefab(d.prefab, { x = x, y = groundY(x, z), z = z })
  -- 敌方单位转向玩家方向
  if team == "enemy" and ent ~= nil then SetRotationY(ent, math.pi) end
  local u = { id = newId(), ent = ent, kind = kind, team = team, x = x, z = z,
              yawFix = YAW_FIX[kind] or 0,
              hp = d.hp, maxHp = d.hp, r = d.r, state = "idle", cd = 0,
              tx = x, tz = z, carry = 0 }
  -- 英雄单位放大 + 更高选中环 (醒目)
  if ent ~= nil and kind == "hero" then SetScale(ent, 1.35, 1.35, 1.35) end
  -- 玩家科技: 攻击/生命乘科技加成 (敌方单位不享受)
  if team == "player" then
    u.hp = math.floor(u.hp * techDefMul() + 0.5)
    u.maxHp = u.hp
    u.atkMul = techAtkMul()
  else
    u.atkMul = 1.0
  end
  units[#units + 1] = u
  return u
end

local function spawnBuilding(kind, team, x, z, done)
  local d = BUILD_DEFS[kind]
  local ent = SpawnPrefab(d.prefab, { x = x, y = groundY(x, z), z = z })
  local b = { id = newId(), ent = ent, kind = kind, team = team, x = x, z = z,
              hp = done and d.hp or math.max(60, d.hp * 0.15), maxHp = d.hp,
              w = d.w, d2 = d.d, done = done and true or false, progress = 0, queue = {} }
  buildings[#buildings + 1] = b
  return b
end

local function findBuilding(kind, team)
  for _, b in ipairs(buildings) do
    if b.kind == kind and b.team == team and b.done then return b end
  end
  return nil
end

local function killBuilding(idx)
  local b = buildings[idx]
  deathFx(b.x, b.z, true)
  PlaySfx("splat")
  if b.kind == "hall" then
    gameover = true
    won = (b.team == "enemy")
  end
  if b.ent ~= nil then Despawn(b.ent) end
  table.remove(buildings, idx)
end

local function nearestEnemy(u, maxR)
  local best, bestD, isB, isUnit = nil, maxR, false, nil
  for _, o in ipairs(units) do
    if o.team ~= u.team then
      local d = dist(u.x, u.z, o.x, o.z)
      if d < bestD then best, bestD, isB, isUnit = o, d, false, o end
    end
  end
  for _, b in ipairs(buildings) do
    if b.team ~= u.team then
      local d = dist(u.x, u.z, b.x, b.z) - b.w * 0.4
      if d < bestD then best, bestD, isB, isUnit = b, d, true, nil end
    end
  end
  if best then return best, bestD, isB, isUnit end
  return nil
end

local function damageUnit(u, dmg) u.hp = u.hp - dmg end

local function damageBuilding(b, dmg)
  b.hp = b.hp - dmg
  for i, bb in ipairs(buildings) do
    if bb == b and bb.hp <= 0 then killBuilding(i) return end
  end
end

local function attackTick(u, target, isB, dt)
  u.cd = u.cd - dt
  if u.cd > 0 then return end
  u.cd = UNIT_DEFS[u.kind].cd
  local d = UNIT_DEFS[u.kind]
  playAttackAnim(u)
  local px, pz = target.x, target.z
  if d.ranged and u.ent ~= nil then muzzleFx(u.x, u.z) end
  -- 克制伤害: 攻击类型 × 目标护甲类型的倍率
  local dtgt = isB and { armor = "medium" } or UNIT_DEFS[target.kind]
  local mod = (MODIFIER[d.atk] and MODIFIER[d.atk][dtgt.armor]) or 1.0
  local dmg = math.floor(d.dmg * mod * (u.atkMul or 1.0) + 0.5)
  if isB then
    damageBuilding(target, dmg)
    SpawnFloatText(target.x, 2.0, target.z, "-" .. dmg, false, 0.7)
    hitFx(px, pz, d.ranged)
    PlaySfx("splat")
  else
    damageUnit(target, dmg)
    SpawnFloatText(target.x, 2.2, target.z, "-" .. dmg, false, 0.7)
    hitFx(px, pz, d.ranged)
    if d.ranged then
      PlaySfx("shoot")
    else
      PlaySfx("splat")
    end
  end
end

local function moveToward(u, tx, tz, speed, dt)
  local dd = dist(u.x, u.z, tx, tz)
  if dd > 0.05 then
    u.x = u.x + (tx - u.x) / dd * speed * dt
    u.z = u.z + (tz - u.z) / dd * speed * dt
    if u.ent ~= nil then
      SetPosition(u.ent, { x = u.x, y = groundY(u.x, u.z), z = u.z })
      SetRotationY(u.ent, math.atan(tx - u.x, -(tz - u.z)) + (u.yawFix or 0))
    end
  end
end

local function updateUnit(u, dt)
  local d = UNIT_DEFS[u.kind]
  u.cd = math.max(0, u.cd - dt)

  if u.kind == "peon" and u.state == "tomine" then
    -- 敌方没有右击目标: 默认去第二个矿 (己方去第一个)
    local mine = MINES[u.mineIdx or (u.team == "enemy" and 2 or 1)]
    if dist(u.x, u.z, mine.x, mine.z) < 3.2 then
      u.state = "mining"; u.t = 0
    else
      moveToward(u, u.tx, u.tz, d.speed, dt)
    end
    return
  end
  if u.kind == "peon" and u.state == "mining" then
    u.t = (u.t or 0) + dt
    if u.t >= 2.0 then
      local hall = findBuilding("hall", u.team)
      if hall then
        u.state = "return"; u.tx, u.tz = hall.x, hall.z + hall.d2 * 0.5 + 1
        u.carry = 10
      else
        u.state = "idle"
      end
    end
    return
  end
  if u.kind == "peon" and u.state == "return" then
    local hall = findBuilding("hall", u.team)
    if not hall then u.state = "idle" return end
    if dist(u.x, u.z, u.tx, u.tz) < 2.5 then
      local key = "gold_" .. u.team
      SetVar(key, (GetVar(key) or 0) + (u.carry or 10))
      u.carry = 0
      u.state = "tomine"
      local mine = MINES[u.mineIdx or (u.team == "enemy" and 2 or 1)]
      u.tx, u.tz = mine.x + 1.5, mine.z - 1
      SpawnFloatText(u.x, 1.6, u.z, "+10", false, 0.6)
    else
      moveToward(u, u.tx, u.tz, d.speed, dt)
    end
    return
  end

  -- 战斗索敌
  local target, d2, isB = nearestEnemy(u, d.range + 1)
  if target then
    if d2 <= d.range then
      attackTick(u, target, isB, dt)
    else
      moveToward(u, target.x, target.z, d.speed, dt)
    end
    return
  end

  moveToward(u, u.tx, u.tz, d.speed, dt)
  -- 移动/待机动画切换
  local c = CLIPS[u.kind]
  if c then
    local moving = dist(u.x, u.z, u.tx, u.tz) > 0.1
    driveAnim(u, moving and c.move or c.idle)
  end
end

-- 敌军攻击单位: 近处索敌, 否则推进玩家大厅
local function updateAttacker(u, dt)
  local d = UNIT_DEFS[u.kind]
  local target, d2, isB = nearestEnemy(u, 9)
  if target then
    if d2 <= d.range then
      attackTick(u, target, isB, dt)
    else
      moveToward(u, target.x, target.z, d.speed, dt)
    end
    return
  end
  local ph = findBuilding("hall", "player")
  local gx, gz = (ph and ph.x) or 18, (ph and ph.z) or 20
  if dist(u.x, u.z, gx, gz) > d.range then
    moveToward(u, gx, gz, d.speed, dt)
  end
end

local function updateBuildings(dt)
  for i = #buildings, 1, -1 do
    local b = buildings[i]
    if not b.done then
      b.progress = b.progress + dt
      b.hp = math.min(b.maxHp, b.hp + (b.maxHp - 60) * dt / BUILD_DEFS[b.kind].bt)
      if b.progress >= BUILD_DEFS[b.kind].bt then b.done = true end
    end
    if b.queue and #b.queue > 0 and b.done then
      local q = b.queue[1]
      q.t = q.t + dt
      if q.t >= UNIT_DEFS[q.kind].bt then
        spawnUnit(q.kind, b.team, b.x, b.z + b.d2 * 0.5 + 1.2).state = "idle"
        table.remove(b.queue, 1)
      end
    end
    -- 箭塔自动攻击: 塔建好后索敌开火 (塔顶火球, 引擎投射物带拖尾)
    if b.done and BUILD_DEFS[b.kind].tower then
      b.cd = (b.cd or 0) - dt
      if b.cd <= 0 then
        local best, bd = nil, BUILD_DEFS[b.kind].range
        for _, o in ipairs(units) do
          if o.team ~= b.team then
            local d = dist(b.x, b.z, o.x, o.z)
            if d < bd then best, bd = o, d end
          end
        end
        if best then
          b.cd = BUILD_DEFS[b.kind].cd
          local dir = { x = (best.x - b.x) / (bd + 0.001), y = -0.12,
                        z = (best.z - b.z) / (bd + 0.001) }
          SpawnProjectile({ x = b.x, y = 3.2, z = b.z }, dir, 22,
                          BUILD_DEFS[b.kind].dmg, 2.5, nil)
          PlaySfx("shoot")
        end
      end
    end
    if b.hp <= 0 then killBuilding(i) end
  end
end

local function updateWaves(dt)
  waveT = waveT - dt
  if waveT <= 0 then
    waveN = waveN + 1
    waveT = 32
    local hx, hz = 78, 58
    local hall = findBuilding("hall", "enemy")
    if hall then hx, hz = hall.x, hall.z + 4 end
    local count = 1 + waveN
    for i = 1, count do
      local u = spawnUnit("grunt", "enemy", hx + (i - count / 2) * 2, hz)
      u.state = "attack"
    end
    SpawnFloatText(80, 3, 20, "第 " .. waveN .. " 波进攻!", false, 1.2)
    PlaySfx("wave")
  end
end

-- ============ 拾取 (屏幕空间) ============
local function pickUnit(sx, sy, team, maxPx)
  local best, bd, idx = nil, maxPx or 32, nil
  for i, u in ipairs(units) do
    if team == nil or u.team == team then
      local s = WorldToScreen(u.x, 1.1, u.z)
      if s and s.x then
        local d = dist(sx, sy, s.x, s.y)
        if d < bd then best, bd, idx = u, d, i end
      end
    end
  end
  return best, idx
end

local function pickBuilding(sx, sy, team, maxPx)
  local best, bd = nil, maxPx or 42
  for _, b in ipairs(buildings) do
    if team == nil or b.team == team then
      local s = WorldToScreen(b.x, 1.5, b.z)
      if s and s.x then
        local d = dist(sx, sy, s.x, s.y)
        if d < bd then best, bd = b, d end
      end
    end
  end
  return best
end

function on_start()
  camEnt = FindNamedEntity("Main Camera")
  SetVar("gold_player", 350)
  spawnBuilding("hall", "player", 18, 20, true)
  spawnBuilding("mine", "neutral", 10, 62, true)
  spawnBuilding("mine", "neutral", 92, 14, true)
  for i = 1, 3 do spawnUnit("peon", "player", 20 + i * 2, 30) end
  spawnUnit("hero", "player", 26, 34)   -- 英雄·法师: 带主动技能, 玩家主力
  spawnBuilding("hall", "enemy", 82, 62, true)
  spawnUnit("peon", "enemy", 78, 58)
  spawnUnit("grunt", "enemy", 80, 55)
  spawnUnit("grunt", "enemy", 76, 57)
end


function on_update(e, dt)
  updateCamera(dt)
  if GetVar("paused") == true then return end
  if gameover then
    if InputMousePressed(0) then ChangeScene("assets/scenes/main.json") end
    return
  end

  updateBuildings(dt)
  -- 死亡动画到期回收
  for id, d in pairs(dying) do
    d.t = d.t - dt
    if d.t <= 0 then
      if d.ent ~= nil then Despawn(d.ent) end
      dying[id] = nil
    end
  end
  for i = #units, 1, -1 do
    local u = units[i]
    if u.hp <= 0 then
      for j = #selected, 1, -1 do if selected[j] == u then table.remove(selected, j) end end
      -- 死亡动画 (有 death 剪辑的单位播完再消失)
      startDeath(u)
      table.remove(units, i)
    else
      if u.state == "attack" and u.team == "enemy" then
        updateAttacker(u, dt)
      else
        updateUnit(u, dt)
      end
    end
  end
  updateWaves(dt)
  updateSkills(dt)

  if not findBuilding("hall", "player") then gameover = true; won = false end
  if not findBuilding("hall", "enemy") then gameover = true; won = true end

  -- 法力/金刷新
  SetVar("gold_player", GetVar("gold_player") or 300)

  if ActionPressed("cancel") then buildMode = nil end
  if ActionPressed("build_farm") then buildMode = "farm" end
  if ActionPressed("build_barracks") then buildMode = "barracks" end

  local m = InputMousePos()
  if InputMousePressed(0) and m then
    -- 小地图点击: 跳转相机 (左下角 200x130 区域)
    if m.x >= 10 and m.x <= 210 and m.y >= 720 - 140 and m.y <= 720 - 10 then
      camX = clamp((m.x - 10) / 200 * 100, 5, 95)
      camZ = clamp((m.y - (720 - 140)) / 130 * 80, 8, 72)
      return
    end
    local handled = false
    -- 英雄技能栏点击 (选中英雄时, 右下角按钮)
    local hc = heroCaster()
    if hc then
      local slot = skillHit(m.x, m.y)
      if slot then
        tryCast(hc, slot.key)
        handled = true
      end
    end
    if selectedBuilding and selectedBuilding.done and BUILD_DEFS[selectedBuilding.kind].trains then
      for bi, tkind in ipairs(BUILD_DEFS[selectedBuilding.kind].trains) do
        local s = WorldToScreen(selectedBuilding.x, 0.2, selectedBuilding.z)
        if s and s.x then
          local bx, by = s.x + 40, s.y - 20 + (bi - 1) * 34
          if m.x >= bx and m.x <= bx + 170 and m.y >= by and m.y <= by + 30 then
            local cost = UNIT_DEFS[tkind].cost
            local g = GetVar("gold_player") or 0
            if g >= cost and #selectedBuilding.queue < 5 then
              SetVar("gold_player", g - cost)
              selectedBuilding.queue[#selectedBuilding.queue + 1] = { kind = tkind, t = 0 }
              PlaySfx("click")
            end
            handled = true
          end
        end
      end
    end
    -- 兵营升级科技按钮 (在训练按钮下方, 竖排)
    if selectedBuilding and selectedBuilding.done and selectedBuilding.kind == "barracks" then
      local s = WorldToScreen(selectedBuilding.x, 0.2, selectedBuilding.z)
      if s and s.x then
        local bi = #(BUILD_DEFS[selectedBuilding.kind].trains)
        local by = s.y - 20 + bi * 34 + 6
        local bx = s.x + 40
        if m.x >= bx and m.x <= bx + 200 and m.y >= by and m.y <= by + 30 then
          local cost = TECH_COST * (math.max(techAtk, techDef) + 1)
          local g = GetVar("gold_player") or 0
          if g >= cost then
            SetVar("gold_player", g - cost)
            -- 交替提升攻击/防御科技
            if (techAtk + techDef) % 2 == 0 then techAtk = techAtk + 1 else techDef = techDef + 1 end
            PlaySfx("click")
          end
          handled = true
        end
      end
    end
    if not handled and buildMode == nil and #selected == 1 and selected[1].kind == "peon" then
      for bi, bkind in ipairs({ "farm", "barracks" }) do
        local s = WorldToScreen(selected[1].x, 0.2, selected[1].z)
        if s and s.x then
          local bx, by = s.x + 40, s.y - 20 + (bi - 1) * 34
          if m.x >= bx and m.x <= bx + 170 and m.y >= by and m.y <= by + 30 then
            buildMode = bkind
            handled = true
          end
        end
      end
    end
    if not handled and buildMode and m then
      local d = BUILD_DEFS[buildMode]
      local g = GetVar("gold_player") or 0
      local gp = groundPick(m.x, m.y)
      if gp and g >= d.cost then
        spawnBuilding(buildMode, "player", gp.x, gp.z, false)
        SetVar("gold_player", g - d.cost)
        PlaySfx("plant")
      end
      buildMode = nil
      handled = true
    end
    -- 选择: 单位优先, 其次建筑; 空地 → 启动框选
    if not handled and m then
      selected = {}
      selectedBuilding = nil
      local u, idx = pickUnit(m.x, m.y, "player", 34)
      if u then
        if InputKey("Control") > 0 then
          -- Ctrl+点击: 添加/移除到多选
          local found = false
          for j, su in ipairs(selected) do
            if su == u then table.remove(selected, j) found = true break end
          end
          if not found then selected[#selected + 1] = u end
        else
          selected = { u }
        end
      end
      if not u then
        local b = pickBuilding(m.x, m.y, "player", 40)
        if b and b.done then selectedBuilding = b end
      end
      if u or selectedBuilding then
        PlaySfx("click")
      else
        -- 空地按下: 启动框选拖拽
        boxSel.active = true
        boxSel.sx, boxSel.sy = m.x, m.y
        boxSel.cx, boxSel.cy = m.x, m.y
      end
    end
  end

  -- 框选拖拽: 更新当前角点
  if boxSel.active and m then boxSel.cx, boxSel.cy = m.x, m.y end
  -- 左键释放: 选中屏幕矩形内所有我方单位 (含点选兜底)
  if InputMouseReleased(0) and boxSel.active then
    boxSel.active = false
    local x1, x2 = math.min(boxSel.sx, boxSel.cx), math.max(boxSel.sx, boxSel.cx)
    local y1, y2 = math.min(boxSel.sy, boxSel.cy), math.max(boxSel.sy, boxSel.cy)
    -- 拖拽超过阈值才算框选 (小于 6px 视为点空地 = 取消选择)
    if (x2 - x1) > 6 or (y2 - y1) > 6 then
      selected = {}
      for _, u in ipairs(units) do
        if u.team == "player" then
          local s = WorldToScreen(u.x, 1.0, u.z)
          if s and s.x and s.x >= x1 and s.x <= x2 and s.y >= y1 and s.y <= y2 then
            selected[#selected + 1] = u
          end
        end
      end
      if #selected > 0 then PlaySfx("click") end
    end
  end

  -- 右键指令
  if InputMousePressed("right") and m then
    if buildMode then buildMode = nil return end
    if #selected > 0 then
      -- 点金矿 -> 采集 (记录命中的矿, 单位去那个矿而非固定 MINES[1])
      local gp = groundPick(m.x, m.y)
      local mineIdx = nil
      if gp then
        for mi, mn in ipairs(MINES) do
          if dist(gp.x, gp.z, mn.x, mn.z) < 3.4 then mineIdx = mi break end
        end
      end
      -- 点敌对单位 -> 攻击该目标
      local eu, eidx = pickUnit(m.x, m.y, "enemy", 30)
      for _, u in ipairs(selected) do
        if u.kind == "peon" and mineIdx then
          u.state = "tomine"
          u.mineIdx = mineIdx
          local mn = MINES[mineIdx]
          u.tx, u.tz = mn.x + 1.5, mn.z - 1
        elseif eu then
          u.state = "attack"
          u.tx, u.tz = eu.x, eu.z
          u.targetIdx = eidx
        else
          u.state = "moving"
          if gp then u.tx, u.tz = gp.x, gp.z end
        end
      end
      PlaySfx("click")
    end
  end

  -- 攻击目标跟随 (targetIdx 存活则追击)
  for _, u in ipairs(units) do
    if u.targetIdx then
      local tgt = units[u.targetIdx]
      if tgt == nil or tgt.hp <= 0 or tgt.team == u.team then
        u.targetIdx = nil
      else
        u.state = "attack"
      end
    end
  end


  if not findBuilding("hall", "player") then gameover = true; won = false end
  if not findBuilding("hall", "enemy") then gameover = true; won = true end
end

-- ============ 渲染 ============
local function drawHpBar(x, y, w, frac, col)
  DrawRect(x - w * 0.5, y, w, 4, 0.08, 0.08, 0.08, 0.9)
  if frac > 0 then DrawRect(x - w * 0.5, y, w * frac, 4, col[1], col[2], col[3], 1) end
end

local function drawGroundRing(wx, wz, r, col, segs)
  local prev = nil
  for i = 0, (segs or 26) do
    local a = i / (segs or 26) * math.pi * 2
    local s = WorldToScreen(wx + math.cos(a) * r, 0.06, wz + math.sin(a) * r)
    if s and s.x then
      if prev then
        local steps = math.max(1, math.floor(dist(prev.x, prev.y, s.x, s.y) / 12))
        for k = 0, steps do
          local t = k / steps
          DrawRect(prev.x + (s.x - prev.x) * t - 1.5, prev.y + (s.y - prev.y) * t - 1.5, 3, 3,
                   col[1], col[2], col[3], col[4] or 0.9)
        end
      end
      prev = s
    else
      prev = nil
    end
  end
end

function on_render()
  -- 建筑 (真实 3D 实体, 画布只画标注)
  for _, b in ipairs(buildings) do
    local s = WorldToScreen(b.x, 2.2, b.z)
    if s and s.x then
      local label = BUILD_DEFS[b.kind].name
      if b.kind == "mine" then label = "金矿" end
      DrawText(label, s.x, s.y, 13, 1, 1, 1, 0.9, true, false)
      local frac = clamp(b.hp / b.maxHp, 0, 1)
      if frac < 1 or not b.done then
        drawHpBar(s.x, s.y + 8, 44, frac, (b.team == "player") and { 0.3, 0.9, 0.35 } or { 1, 0.35, 0.3 })
        if not b.done then
          local bf = clamp(b.progress / BUILD_DEFS[b.kind].bt, 0, 1)
          DrawText("建造中 " .. math.floor(bf * 100) .. "%", s.x, s.y + 16, 12, 1, 0.85, 0.3, 1, true, false)
        end
      end
      -- 生产队列
      if b.queue and #b.queue > 0 then
        local q = b.queue[1]
        local fq = clamp(q.t / UNIT_DEFS[q.kind].bt, 0, 1)
        DrawRect(s.x - 22, s.y + 20, 44 * fq, 3, 0.4, 0.8, 1, 1)
      end
    end
    -- 选中建筑: 训练按钮 (投影到建筑旁)
    if b == selectedBuilding and b.done and BUILD_DEFS[b.kind].trains then
      local s2 = WorldToScreen(b.x, 0.2, b.z)
      if s2 and s2.x then
        for bi, tkind in ipairs(BUILD_DEFS[b.kind].trains) do
          local bx, by = s2.x + 46, s2.y - 24 + (bi - 1) * 32
          local afford = (GetVar("gold_player") or 0) >= UNIT_DEFS[tkind].cost
          DrawRect(bx, by, 170, 30, 0.1, 0.15, 0.22, 0.95)
          DrawRectOutline(bx, by, 170, 30, 1.5, afford and 0.4 or 0.6, afford and 0.9 or 0.4, afford and 1 or 0.4, 0.9)
          DrawText(UNIT_DEFS[tkind].name .. " " .. UNIT_DEFS[tkind].cost .. "金", bx + 10, by + 15, 13,
                   afford and 1 or 0.6, afford and 1 or 0.6, afford and 1 or 0.6, 1, false, true)
         end
        -- 兵营升级科技按钮 (训练按钮下方): 交替升攻/防
        if b.kind == "barracks" then
          local bi = #(BUILD_DEFS[b.kind].trains)
          local bx, by = s2.x + 46, s2.y - 24 + bi * 32
          local cost = TECH_COST * (math.max(techAtk, techDef) + 1)
          local afford = (GetVar("gold_player") or 0) >= cost
          DrawRect(bx, by, 200, 30, 0.1, 0.15, 0.22, 0.95)
          DrawRectOutline(bx, by, 200, 30, 1.5, afford and 0.4 or 0.6, afford and 0.9 or 0.4, afford and 1 or 0.4, 0.9)
          DrawText(("升级 攻 Lv%d | 防 Lv%d  %d金"):format(techAtk, techDef, cost), bx + 10, by + 15, 13,
                   afford and 1 or 0.6, afford and 1 or 0.6, afford and 1 or 0.6, 1, false, true)
        end
      end
    end
  end

  -- 单位: 选中环 (贴地) + 血条
  for _, u in ipairs(units) do
    local col = (u.team == "player") and { 0.3, 1, 0.4, 0.95 } or { 1, 0.3, 0.3, 0.95 }
    if u.state == "attack" then col = { 1, 0.6, 0.2, 0.95 } end
    drawGroundRing(u.x, u.z, u.r + 0.25, col, 16)
    local s = WorldToScreen(u.x, 2.3, u.z)
    if s and s.x then
      drawHpBar(s.x, s.y, 30, clamp(u.hp / u.maxHp, 0, 1),
                (u.team == "player") and { 0.3, 0.9, 0.35 } or { 1, 0.35, 0.3 })
    end
  end
  for _, u in ipairs(selected) do
    drawGroundRing(u.x, u.z, u.r + 0.55, { 0.3, 1, 0.4, 1 }, 22)
  end
  if selectedBuilding then
    drawGroundRing(selectedBuilding.x, selectedBuilding.z, selectedBuilding.w * 0.75,
                   { 0.3, 1, 0.4, 1 }, 30)
    -- 训练按钮由建筑循环绘制 (上方)
  end

  -- 英雄技能栏 (选中英雄时, 右下角): Q=火球 W=治疗 (带冷却遮罩)
  local hc2 = heroCaster()
  if hc2 then
    for _, s in ipairs(SKILL_SLOTS) do
      local cd = skillCd[s.key]
      local ready = cd <= 0
      DrawRect(s.x, s.y, s.w, s.h, 0.1, 0.15, 0.22, 0.95)
      DrawRectOutline(s.x, s.y, s.w, s.h, 1.5, ready and 0.4 or 0.6, ready and 0.9 or 0.4, ready and 1 or 0.4, 0.9)
      DrawText(s.name, s.x + 10, s.y + 23, 14, ready and 1 or 0.5, ready and 1 or 0.5, ready and 1 or 0.5, 1, false, true)
      if not ready then
        local f = clamp(cd / SKILL_COOLDOWN[s.key], 0, 1)
        DrawRect(s.x, s.y, s.w * f, s.h, 0.0, 0.0, 0.0, 0.55)
      end
    end
  end

  -- 小地图 (左下角): 地图 + 单位/建筑/金矿点 + 相机框
  do
    local mx, my, mw, mh = 10, 720 - 140, 200, 130
    DrawRect(mx, my, mw, mh, 0.05, 0.08, 0.06, 0.85)
    DrawRectOutline(mx, my, mw, mh, 1.5, 0.45, 0.65, 1.0, 0.8)
    local px = function(wx) return mx + wx / 100 * mw end
    local pz = function(wz) return my + wz / 80 * mh end
    for _, b in ipairs(buildings) do
      if b.kind == "mine" then
        DrawRect(px(b.x) - 2, pz(b.z) - 2, 4, 4, 1.0, 0.85, 0.2, 1)
      elseif b.team == "player" then
        DrawRect(px(b.x) - 2, pz(b.z) - 2, 4, 4, 0.3, 0.6, 1.0, 1)
      else
        DrawRect(px(b.x) - 2, pz(b.z) - 2, 4, 4, 1.0, 0.3, 0.3, 1)
      end
    end
    for _, u in ipairs(units) do
      if u.team == "player" then
        DrawRect(px(u.x) - 1.5, pz(u.z) - 1.5, 3, 3, 0.35, 0.85, 0.5, 1)
      else
        DrawRect(px(u.x) - 1.5, pz(u.z) - 1.5, 3, 3, 1.0, 0.4, 0.35, 1)
      end
    end
    -- 相机视野框 (示意)
    DrawRectOutline(px(camX) - 18, pz(camZ) - 12, 36, 24, 1, 1, 1, 1, 0.5)
  end

  -- 框选矩形 (半透明蓝 + 边框)
  if boxSel.active and (math.abs(boxSel.cx - boxSel.sx) > 2 or math.abs(boxSel.cy - boxSel.sy) > 2) then
    local x1 = math.min(boxSel.sx, boxSel.cx)
    local y1 = math.min(boxSel.sy, boxSel.cy)
    local w = math.abs(boxSel.cx - boxSel.sx)
    local h = math.abs(boxSel.cy - boxSel.sy)
    DrawRect(x1, y1, w, h, 0.3, 0.6, 1.0, 0.15)
    DrawRectOutline(x1, y1, w, h, 1.5, 0.4, 0.8, 1.0, 0.9)
  end

  -- 建造幽灵
  local m = InputMousePos()
  if buildMode and m then
    local gp = groundPick(m.x, m.y)
    if gp then
      local s = WorldToScreen(gp.x, 0.4, gp.z)
      if s and s.x then
        local d = BUILD_DEFS[buildMode]
        DrawRect(s.x - 30, s.y - 24, 60, 48, 0.3, 0.8, 0.4, 0.35)
        DrawRectOutline(s.x - 30, s.y - 24, 60, 48, 2, 0.4, 1, 0.5, 0.9)
      end
    end
  end

  -- 资源/提示: WC3 风格顶部资源条 (深色底 + 金色黄金数字 + 底部金线)
  DrawRect(0, 0, 1280, 30, 0.045, 0.06, 0.09, 0.95)
  DrawRect(0, 0, 1280, 1, 0.55, 0.45, 0.15, 0.9)              -- 顶金线
  DrawRect(0, 29, 1280, 1, 0.35, 0.30, 0.10, 0.8)             -- 底金线
  -- 金币图标 (金色小方块) + 数量
  DrawRect(18, 12, 12, 7, 0.95, 0.72, 0.15, 1)
  DrawText(tostring(GetVar("gold_player") or 0), 36, 20, 17, 1, 0.86, 0.25, 1, false, true)
  -- 波次
  DrawText("波次 " .. tostring(waveN), 180, 20, 15, 0.92, 0.92, 0.94, 1, false, true)
  -- 操作提示 (右侧, 弱化)
  DrawText("左键选择 · 右键移动/攻击/采集 (点金矿) · 选中苦工 F 农场 B 兵营",
           1280, 20, 12, 0.65, 0.70, 0.76, 0.9, false, true)

  -- 全屏闪光
  for _, fl in ipairs(flashes or {}) do
    local a = (1 - fl.age / fl.life) * 0.22
    DrawRect(0, 0, 1280, 720, fl.col[1], fl.col[2], fl.col[3], a)
  end

  -- 胜负
  if gameover then
    DrawRect(0, 0, 1280, 720, 0, 0, 0, 0.55)
    if won then
      DrawRect(380, 260, 520, 200, 0.08, 0.32, 0.12, 0.96)
      DrawText("胜利!", 640, 320, 52, 0.95, 1, 0.7, 1, true, true)
      DrawText("敌方大本营已被摧毁", 640, 390, 18, 0.9, 0.95, 1, 1, true, true)
    else
      DrawRect(380, 260, 520, 200, 0.35, 0.06, 0.06, 0.96)
      DrawText("失败", 640, 320, 52, 1, 0.5, 0.5, 1, true, true)
      DrawText("你的大本营已被摧毁", 640, 390, 18, 1, 0.85, 0.85, 1, true, true)
    end
    DrawText("点击任意处重新开始", 640, 430, 15, 0.8, 0.9, 1, 0.9, true, true)
  end
end

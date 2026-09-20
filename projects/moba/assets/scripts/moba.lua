-- NeonMOBA —— League of Legends 风格 1v1 单路 MOBA（全部玩法在 Lua 中，数据驱动）。
-- 地图: 真实召唤师峡谷（蓝方 -Z，红方 +Z，中路沿 Z 轴）。
-- 玩家: 左键移动/锁敌，QWER 技能，自动普攻；滚轮缩放，空格回中；P 商店，B 回城，Tab 记分板。
-- 敌方: AI 英雄 + 定时兵线 + 野区；推掉敌方水晶获胜。
-- ==========================================================================
-- 常量 / 地图
-- ==========================================================================
local BLUE, RED = 1, 2
local TEAM_NAME = { [BLUE] = "蓝方", [RED] = "红方" }
local TEAM_COLOR = { [BLUE] = { 0.30, 0.55, 1.0 }, [RED] = { 1.0, 0.38, 0.30 } }

local MAP = {
    base = { [BLUE] = { x = 0, z = -70 }, [RED] = { x = 0, z = 70 } },
    spawn = { [BLUE] = { x = 0, z = -58 }, [RED] = { x = 0, z = 58 } },
    towers = {
        [BLUE] = { { x = 0, z = -46 }, { x = 0, z = -30 } },
        [RED] = { { x = 0, z = 46 }, { x = 0, z = 30 } },
    },
}

-- 草丛（视野遮挡）。位置为世界坐标近似值。
local BRUSHES = {
    { x = -7, z = -38, r = 3.6 }, { x = 7, z = -22, r = 3.6 },
    { x = -7, z = -4, r = 3.6 }, { x = 7, z = 12, r = 3.6 },
    { x = -7, z = 26, r = 3.6 }, { x = 7, z = 40, r = 3.6 },
}

-- 相机视角（俯角 pitch，Riot Rift 约 55°；位置由脚本每帧驱动 Main Camera 实体，保持鼠标可见）
local CAM = { yaw = math.pi * 0.75, pitch = 0.98, dist = 17, minDist = 11, maxDist = 28 }
local CAM_FOV = 55
local VW, VH = 1280, 720
local FACE_OFF = 0

-- 平衡
local TOWER_HP, NEXUS_HP = 1500, 3200
local MINION_WAVE_PERIOD = 26
local FIRST_WAVE = 8
local MINION_COUNT = 3

-- 出场英雄（AI 对手从 AI_CHAMP 开始）
local AI_CHAMP = "Ashe"

-- ==========================================================================
-- 工具
-- ==========================================================================
local function clamp(v, a, b) if v < a then return a elseif v > b then return b else return v end end
local function dist2(ax, az, bx, bz) local dx, dz = ax - bx, az - bz; return dx * dx + dz * dz end
local function dist(ax, az, bx, bz) return math.sqrt(dist2(ax, az, bx, bz)) end
local function norm(dx, dz) local l = math.sqrt(dx * dx + dz * dz); if l < 1e-6 then return 0, 0 end return dx / l, dz / l end

local ids = 0
local function nid() ids = ids + 1; return ids end
local function fmtKey(e) if e == nil then return "" end return string.format("%d_%d", e.id, e.gen) end

-- ==========================================================================
-- 状态
-- ==========================================================================
local units = {}       -- 所有存活单位
local heroes = {}      -- 英雄
local structures = {}  -- 塔 / 水晶
local projectiles = {}
local groundAoes = {}
local waveTimer = FIRST_WAVE
local waveN = 0
local gameOver, winner = false, nil
local playerHero, enemyHero = nil, nil
local phase = "select"
local SELECT = {}
local selectIndex = 1
local selectTime = 0
local loadingTime = 0
local kills = { [BLUE] = 0, [RED] = 0 }
local elapsed = 0
local ROSTER = {}
local CHAMP_DATA = {}
local ITEMS = {}
local SHOP = {}
local shopOpen = false
local plateTeam = {}
local MINIMAP_IMG = "assets/lol/ui/minimap.png"

-- 音效（按事件触发 + 限流）
local sfxLast = {}
local function sfx(name, cd)
    cd = cd or 0.06
    if (sfxLast[name] or -1) > elapsed - cd then return end
    sfxLast[name] = elapsed
    PlaySfx(name)
end

local function loadRoster()
    local txt = ReadText("assets/data/moba_roster.json")
    if txt == nil or txt == "" then return end
    local t = Json.Parse(txt)
    if t ~= nil then ROSTER = t end
end

local function loadItems()
    local txt = ReadText("assets/data/items.json")
    if txt == nil or txt == "" then return end
    local t = Json.Parse(txt)
    if t == nil then return end
    ITEMS = t
    SHOP = {}
    for id, _ in pairs(t) do SHOP[#SHOP + 1] = id end
    table.sort(SHOP, function(a, b) return (t[a].price or 0) < (t[b].price or 0) end)
end

-- 用 Data Dragon 的真实英雄/技能数据（名称/图标/说明/冷却/消耗）覆盖演示数值。
local function loadAbilityData()
    local txt = ReadText("assets/data/abilities.json")
    if txt == nil or txt == "" then return end
    local t = Json.Parse(txt)
    if t == nil then return end
    CHAMP_DATA = t
    for name, s in pairs(ROSTER) do
        local d = CHAMP_DATA[name]
        if d ~= nil then
            s.title = d.title
            s.portrait = d.portrait
            s.passive = d.passive
            for i, ab in ipairs(s.abilities or {}) do
                local da = d.abilities[i]
                if da ~= nil then
                    ab.name = da.name
                    ab.icon = da.icon
                    ab.desc = da.desc
                    if da.cd and da.cd > 0 then ab.cd = da.cd end
                    if da.cost and da.cost > 0 then ab.cost = da.cost end
                end
            end
        end
    end
end

-- ==========================================================================
-- 相机
-- ==========================================================================
local camFocusX, camFocusZ = 0, 0
local camFollow = true
local camEnt = nil

local function camPos()
    local sp, cp = math.sin(CAM.pitch), math.cos(CAM.pitch)
    local cy, sy = math.cos(CAM.yaw), math.sin(CAM.yaw)
    -- Ground-plane offset (0, -cp*dist) rotated by yaw about the focus, so the
    -- whole map reads at the same angle as the minimap.
    local rx = -sy * cp * CAM.dist
    local rz = -cy * cp * CAM.dist
    return camFocusX + rx, sp * CAM.dist, camFocusZ + rz
end

local function applyCamera()
    if camEnt == nil then return end
    local x, y, z = camPos()
    SetPosition(camEnt, { x = x, y = y, z = z })
    -- 只改位置不会转向：显式把相机朝向焦点（含俯角），yaw 才会真正旋转视角。
    local sp, cp = math.sin(CAM.pitch), math.cos(CAM.pitch)
    local cy, sy = math.cos(CAM.yaw), math.sin(CAM.yaw)
    SetLook(camEnt, sy * cp, -sp, cy * cp)
end

-- 屏幕像素 -> 地面 y=0 的世界点（相机由脚本固定，直接解析求交）。
local function groundPick(sx, sy)
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    local nx = (sx / vw) * 2 - 1
    local ny = 1 - (sy / vh) * 2
    local tanY = math.tan(math.rad(CAM_FOV * 0.5))
    local tanX = tanY * (vw / vh)
    local sp, cp = math.sin(CAM.pitch), math.cos(CAM.pitch)
    local dx = -nx * tanX
    local dy = -sp + cp * ny * tanY
    local dz = cp + sp * ny * tanY
    -- Rotate the camera ray by yaw so picking matches the yawed camera.
    local cyaw, syaw = math.cos(CAM.yaw), math.sin(CAM.yaw)
    local rdx = cyaw * dx + syaw * dz
    local rdz = -syaw * dx + cyaw * dz
    dx, dz = rdx, rdz
    local cx, cy, cz = camPos()
    if dy >= -1e-4 then return nil end
    local t = -cy / dy
    return { x = cx + dx * t, z = cz + dz * t }
end

-- ==========================================================================
-- 生成
-- ==========================================================================
local function baseStats(name)
    local s = ROSTER[name]
    if s == nil then s = { hp = 600, mana = 0, ad = 60, range = 2.0, ms = 340, abilities = {} } end
    return s
end

local function spawnHero(name, team)
    local s = baseStats(name)
    local p = MAP.spawn[team]
    local off = (team == BLUE) and -1.6 or 1.6
    local ent = SpawnPrefab("hero_" .. name, { x = p.x + off, y = 0, z = p.z })
    if ent == nil then return nil end
    local u = {
        id = nid(), ent = ent, kind = "champion", team = team,
        name = s.chinese or name, key = name,
        portrait = s.portrait, title = s.title,
        x = p.x + off, z = p.z, h = 1.8, radius = 0.55,
        hp = s.hp, maxHp = s.hp, mana = s.mana or 0, maxMana = s.mana or 0,
        ad = s.ad or 60, range = s.range or 2.0, speed = (s.ms or 340) * 0.015,
        atkPeriod = (s.range or 2.0) > 3.0 and 1.0 or 1.15, atkTimer = 0,
        ranged = (s.range or 2.0) > 3.0,
        abilities = s.abilities or {}, cds = { 0, 0, 0, 0 },
        buffs = {}, target = nil, moveTarget = nil, state = "idle",
        anim = nil, actionT = 0, yaw = (team == BLUE) and 0 or math.pi,
        channel = 0, dying = 0, dead = false, isHero = true,
        base = { hp = s.hp, hpGrow = s.hpGrow or 78, mana = s.mana or 0,
                 manaGrow = s.manaGrow or 30, ad = s.ad or 60, adGrow = s.adGrow or 4,
                 ms = s.ms or 340 },
        level = 1, xp = 0, gold = 500, cs = 0, kills = 0, deaths = 0, ap = 0,
        bonus = { ad = 0, ap = 0, hp = 0, mana = 0, armor = 0, ms = 0 }, items = {},
    }
    units[#units + 1] = u
    heroes[#heroes + 1] = u
    SetRotationY(ent, u.yaw)
    SetHealth(ent, u.hp)
    return u
end

local MINION_DEFS = {
    melee = { hp = 450, dmg = 22, range = 1.8, speed = 3.2, radius = 0.5, h = 1.1, period = 1.1 },
    caster = { hp = 300, dmg = 26, range = 6.5, speed = 3.0, radius = 0.45, h = 1.1, period = 1.2, ranged = true },
    siege = { hp = 800, dmg = 45, range = 7.5, speed = 2.7, radius = 0.7, h = 1.4, period = 1.6, ranged = true },
}

local function spawnMinion(kind, team, x, z)
    local d = MINION_DEFS[kind]
    local prefab = "minion_" .. kind .. (team == BLUE and "_blue" or "_red")
    local ent = SpawnPrefab(prefab, { x = x, y = 0, z = z })
    if ent == nil then return nil end
    local u = {
        id = nid(), ent = ent, kind = "minion", mkind = kind, team = team,
        name = "小兵", x = x, z = z, h = d.h, radius = d.radius,
        hp = d.hp, maxHp = d.hp, ad = d.dmg, range = d.range, speed = d.speed,
        atkPeriod = d.period, atkTimer = 0, ranged = d.ranged or false,
        buffs = {}, target = nil, moveTarget = { x = MAP.base[3 - team].x, z = MAP.base[3 - team].z },
        state = "push", anim = nil, actionT = 0, yaw = (team == BLUE) and 0 or math.pi,
        dying = 0, dead = false,
    }
    units[#units + 1] = u
    SetRotationY(ent, u.yaw)
    SetHealth(ent, u.hp)
    return u
end

local function spawnStructure(kind, team, x, z)
    local suffix = (team == BLUE) and "blue" or "red"
    local prefab = (kind == "nexus") and ("struct_nexus_" .. suffix) or ("struct_tower_" .. suffix)
    local ent = SpawnPrefab(prefab, { x = x, y = 0, z = z })
    if ent == nil then return nil end
    local u
    if kind == "nexus" then
        u = {
            id = nid(), ent = ent, kind = "nexus", team = team, name = "水晶",
            x = x, z = z, h = 5.2, radius = 2.2, hp = NEXUS_HP, maxHp = NEXUS_HP,
            ad = 0, range = 0, speed = 0, atkPeriod = 1, atkTimer = 0,
            buffs = {}, target = nil, anim = nil, actionT = 0, dead = false,
        }
        SetPosition(ent, { x = x, y = 0, z = z })
    else
        u = {
            id = nid(), ent = ent, kind = "tower", team = team, name = "防御塔",
            x = x, z = z, h = 7.4, radius = 1.3, hp = TOWER_HP, maxHp = TOWER_HP,
            ad = 95, range = 9.0, speed = 0, atkPeriod = 1.25, atkTimer = 0,
            buffs = {}, target = nil, anim = nil, actionT = 0, dead = false, ranged = true,
        }
        SetPosition(ent, { x = x, y = 0, z = z })
    end
    units[#units + 1] = u
    structures[#structures + 1] = u
    SetHealth(ent, u.hp)
    return u
end

-- 野区营地（中立单位，team=0）。
local JUNGLE = {
    { key = "red", name = "红BUFF", prefab = "unit_sru_red", x = -14, z = -22, hp = 2300, ad = 80, range = 2.0, radius = 0.8, h = 1.6, gold = 100, xp = 110, buff = "red" },
    { key = "bluecamp", name = "蓝BUFF", prefab = "unit_sru_gromp", x = -20, z = -34, hp = 1800, ad = 60, range = 2.2, radius = 0.9, h = 1.8, gold = 80, xp = 90, buff = "blue" },
    { key = "wolf", name = "魔沼蛙", prefab = "unit_sru_murkwolf", x = -11, z = -30, hp = 1300, ad = 45, range = 2.0, radius = 0.6, h = 1.2, gold = 60, xp = 70, buff = "" },
    { key = "crab", name = "迅捷蟹", prefab = "unit_sru_crab", x = -9, z = 0, hp = 1200, ad = 40, range = 2.0, radius = 0.6, h = 1.0, gold = 55, xp = 60, buff = "" },
    { key = "dragon", name = "巨龙", prefab = "unit_sru_dragon", x = 13, z = -2, hp = 3800, ad = 110, range = 6.0, radius = 1.6, h = 3.0, gold = 150, xp = 200, buff = "dragon" },
    { key = "baron", name = "纳什男爵", prefab = "unit_sru_baron", x = -13, z = 3, hp = 5200, ad = 140, range = 6.0, radius = 1.8, h = 3.4, gold = 200, xp = 280, buff = "baron" },
}

local function spawnNeutral(cfg)
    local ent = SpawnPrefab(cfg.prefab, { x = cfg.x, y = 0, z = cfg.z })
    if ent == nil then return nil end
    local u = {
        id = nid(), ent = ent, kind = "neutral", team = 0, name = cfg.name or cfg.key,
        x = cfg.x, z = cfg.z, h = cfg.h, radius = cfg.radius,
        hp = cfg.hp, maxHp = cfg.hp, ad = cfg.ad, range = cfg.range, speed = 0,
        atkPeriod = 1.6, atkTimer = 0, buffs = {}, target = nil,
        home = { x = cfg.x, z = cfg.z }, gold = cfg.gold, xpReward = cfg.xp, campBuff = cfg.buff,
        anim = nil, actionT = 0, dying = 0, dead = false,
    }
    units[#units + 1] = u
    SetRotationY(ent, math.pi)
    SetHealth(ent, u.hp)
    return u
end

local function spawnJungle()
    for _, cfg in ipairs(JUNGLE) do spawnNeutral(cfg) end
end

-- ==========================================================================
-- 伤害 / 奖励 / 死亡
-- ==========================================================================
local function inOwnBase(u)
    local b = MAP.base[u.team]
    return dist(u.x, u.z, b.x, b.z) < 12
end

local function floatAt(u, text, crit)
    SpawnFloatText({ x = u.x, y = u.h + 0.4, z = u.z }, text, crit or false, 0.85)
end

local function recomputeDerived(u)
    local b = u.base
    if b == nil then return end
    u.maxHp = b.hp + b.hpGrow * (u.level - 1) + u.bonus.hp
    u.maxMana = b.mana + b.manaGrow * (u.level - 1) + u.bonus.mana
    u.ad = b.ad + b.adGrow * (u.level - 1) + u.bonus.ad
    u.ap = u.bonus.ap
    u.speed = (b.ms + u.bonus.ms) * 0.015
end

local function xpForLevel(l) return 100 + (l - 1) * 80 end

local function grantXp(u, amount)
    if u == nil or not u.isHero then return end
    u.xp = u.xp + amount
    while u.xp >= xpForLevel(u.level) and u.level < 18 do
        u.xp = u.xp - xpForLevel(u.level)
        u.level = u.level + 1
        local oldHp, oldMana = u.maxHp, u.maxMana
        recomputeDerived(u)
        u.hp = u.hp + (u.maxHp - oldHp)
        u.mana = u.mana + (u.maxMana - oldMana)
        SetHealth(u.ent, math.min(u.hp, u.maxHp))
        floatAt(u, "升级!", true)
        sfx("levelup", 0.3)
        EmitParticles({ pos = { x = u.x, y = 1, z = u.z }, count = 20, vel = { x = 0, y = 1, z = 0 },
            speedMin = 0.5, speedMax = 2, lifeMin = 0.4, lifeMax = 0.8, sizeStart = 0.6, sizeEnd = 0.05,
            color = { r = 1, g = 0.85, b = 0.3, a = 0.9 },
            colorEnd = { r = 1, g = 1, b = 0, a = 0 }, additive = true })
    end
end

local GOLD_MINION = { melee = 21, caster = 14, siege = 60 }
local XP_MINION = { melee = 32, caster = 28, siege = 55 }

local function updateEconomy(dt)
    for i = 1, #heroes do
        local h = heroes[i]
        if not h.dead then h.gold = h.gold + 2.0 * dt end
    end
end

local function buyItem(h, iid)
    local it = ITEMS[iid]
    if it == nil or h == nil then return end
    if #h.items >= 6 then return end
    if h.gold < (it.price or 0) then floatAt(h, "金币不足", false); return end
    h.gold = h.gold - (it.price or 0)
    h.items[#h.items + 1] = iid
    h.bonus.ad = h.bonus.ad + (it.ad or 0)
    h.bonus.ap = h.bonus.ap + (it.ap or 0)
    h.bonus.hp = h.bonus.hp + (it.hp or 0)
    h.bonus.mana = h.bonus.mana + (it.mana or 0)
    h.bonus.armor = h.bonus.armor + (it.armor or 0)
    h.bonus.ms = h.bonus.ms + (it.ms or 0)
    local oldHp, oldMana = h.maxHp, h.maxMana
    recomputeDerived(h)
    h.hp = h.hp + (h.maxHp - oldHp)
    h.mana = h.mana + (h.maxMana - oldMana)
    SetHealth(h.ent, math.min(h.hp, h.maxHp))
    floatAt(h, it.name, false)
    sfx("buy", 0.1)
end

local function updateShop(dt)
    if ActionPressed("shop") then shopOpen = not shopOpen end
    if not shopOpen then return end
    local h = playerHero
    if h == nil then return end
    local m = InputMousePos()
    if m == nil or not InputMousePressed("left") then return end
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    local cols, cell, gap = 5, 74, 10
    local n = #SHOP
    local rows = math.ceil(n / cols)
    local totalW = cols * cell + (cols - 1) * gap
    local totalH = rows * cell + (rows - 1) * gap
    local ox = vw * 0.5 - totalW * 0.5
    local oy = vh * 0.5 - totalH * 0.5
    for i = 1, n do
        local r = math.floor((i - 1) / cols)
        local c = (i - 1) % cols
        local x = ox + c * (cell + gap)
        local y = oy + r * (cell + gap)
        if m.x >= x and m.x <= x + cell and m.y >= y and m.y <= y + cell then
            buyItem(h, SHOP[i])
            return
        end
    end
end

local function killUnit(u, source)
    if u.dead then return end
    u.dead = true
    u.hp = 0
    SetHealth(u.ent, 0)
    -- 塔被拆掉后放行该处的导航障碍，后续兵线可直推。
    if u.kind == "tower" then NavBlock(u.x, u.z, 2.0, true) end
    if u.isHero then
        sfx("kill", 0.4)
    elseif u.kind == "minion" and playerHero ~= nil and dist(u.x, u.z, playerHero.x, playerHero.z) < 22 then
        sfx("minion_die", 0.12)
    end
    if u.isHero then
        u.dying = 5.0 + (u.deaths or 0) * 1.5
        u.deaths = (u.deaths or 0) + 1
    elseif u.kind == "nexus" then
        u.dying = 0
    else
        u.dying = (u.kind == "minion") and 0.7 or 1.4
    end
    if u.ent ~= nil and u.kind ~= "tower" and u.kind ~= "nexus" then
        PlayAnimation(u.ent, "death", false, 0.1)
    end

    if u.kind == "minion" then
        if source ~= nil then
            kills[source.team] = (kills[source.team] or 0) + 1
            if source.isHero then
                local g = GOLD_MINION[u.mkind] or 20
                source.gold = source.gold + g
                source.cs = source.cs + 1
                floatAt(source, "+" .. tostring(g), false)
            end
        end
        for i = 1, #heroes do
            local h = heroes[i]
            if not h.dead and h.team ~= u.team and dist(h.x, h.z, u.x, u.z) < 13 then
                grantXp(h, XP_MINION[u.mkind] or 30)
            end
        end
    elseif u.isHero then
        if source ~= nil and source.isHero and source.team ~= u.team then
            source.kills = (source.kills or 0) + 1
            source.gold = source.gold + 300
            grantXp(source, 150)
            floatAt(source, "+300", true)
        end
    elseif u.kind == "neutral" then
        sfx("monster_die", 0.15)
        if source ~= nil and source.isHero then
            local g = u.gold or 60
            source.gold = source.gold + g
            grantXp(source, u.xpReward or 60)
            floatAt(source, "+" .. tostring(g), false)
            if u.campBuff == "red" then
                applyStatusLua(source, "adbuff", 120, 1.15, source)
            elseif u.campBuff == "blue" then
                applyStatusLua(source, "msbuff", 120, 1.1, source)
            elseif u.campBuff == "dragon" then
                applyStatusLua(source, "adbuff", 600, 1.08, source)
            elseif u.campBuff == "baron" then
                applyStatusLua(source, "adbuff", 180, 1.2, source)
                applyStatusLua(source, "msbuff", 180, 1.2, source)
            end
        end
    end

    if u.kind == "nexus" and not gameOver then
        gameOver = true
        winner = source and source.team or (3 - u.team)
        sfx(winner == BLUE and "victory" or "defeat", 999)
    end
end

local function damage(target, amount, source)
    if target == nil or target.dead or amount <= 0 then return end
    local buf = target.buffs
    if buf and buf.shield and buf.shield.t > 0 and buf.shield.pool > 0 then
        local absorbed = math.min(buf.shield.pool, amount)
        buf.shield.pool = buf.shield.pool - absorbed
        amount = amount - absorbed
    end
    if amount > 0 then
        target.hp = target.hp - amount
        SetHealth(target.ent, math.max(0, target.hp))
        floatAt(target, tostring(math.floor(amount + 0.5)), amount >= 90)
        if amount >= 40 then sfx("hit", 0.1) end
        if amount >= 25 then
            EmitParticles({ pos = { x = target.x, y = target.h * 0.6, z = target.z }, count = 5,
                vel = { x = 0, y = 1, z = 0 }, speedMin = 1, speedMax = 3, lifeMin = 0.15, lifeMax = 0.3,
                sizeStart = 0.3, sizeEnd = 0.02, color = { r = 1, g = 0.9, b = 0.5, a = 0.8 },
                colorEnd = { r = 1, g = 0.3, b = 0.1, a = 0 }, additive = true })
        end
    end
    if target.hp <= 0 then killUnit(target, source) end
end

-- ==========================================================================
-- 目标选择
-- ==========================================================================
local function nearestEnemy(u, range, preferMinion)
    local best, bestScore = nil, nil
    local r2 = range * range
    for i = 1, #units do
        local o = units[i]
        if not o.dead and o.team ~= u.team then
            local d = dist2(u.x, u.z, o.x, o.z)
            if d <= r2 then
                local score = d
                if preferMinion and o.kind == "minion" then score = score - 1e9 end
                if bestScore == nil or score < bestScore then bestScore = score; best = o end
            end
        end
    end
    return best
end

-- ==========================================================================
-- 移动 / 动画
-- ==========================================================================
local function faceTo(u, tx, tz)
    local dx, dz = tx - u.x, tz - u.z
    if dx * dx + dz * dz < 1e-6 then return end
    -- 实测约定: SetRotationY(θ) 使模型 +Z 前向映射到 (sinθ, cosθ)
    u.yaw = math.atan(dx, dz) + FACE_OFF
    SetRotationY(u.ent, u.yaw)
end

local function moveToward(u, tx, tz, dt)
    local dx, dz = tx - u.x, tz - u.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 0.05 then return true end
    local mult = 1.0
    if u.buffs and u.buffs.slow and u.buffs.slow.t > 0 then mult = mult * (1.0 - u.buffs.slow.mag) end
    if u.buffs and u.buffs.msbuff and u.buffs.msbuff.t > 0 then mult = mult * u.buffs.msbuff.mul end
    if u.buffs and u.buffs.stun and u.buffs.stun.t > 0 then mult = 0 end
    local step = u.speed * dt * mult
    if step > d then step = d end
    u.x = u.x + dx / d * step
    u.z = u.z + dz / d * step
    SetPosition(u.ent, { x = u.x, y = 0, z = u.z })
    faceTo(u, tx, tz)
    return d <= 0.2
end

-- 导航寻路：沿场景导航网格（level.navgrid）的 A* 路点移动，绕开地图障碍。
-- 无网格 / 无路时优雅回退直线（beeline），行为与旧版一致。
local function repath(u, tx, tz)
    local p = NavFindPath({ x = u.x, y = 0, z = u.z }, { x = tx, y = 0, z = tz })
    if p ~= nil and #p > 0 then
        u.navPath = p
        u.navIdx = 1
        u.navGoal = { x = tx, z = tz }
    else
        u.navPath = nil
        u.navGoal = nil
    end
end

-- 返回是否已抵达最终目标（<1.0）。移动目标用节流重算，固定目标只算一次。
local function stepMove(u, tx, tz, dt)
    if u.dead then return true end
    u.navCd = (u.navCd or 0) - dt
    local exhausted = (u.navPath == nil) or (u.navIdx > #u.navPath)
    local goalMoved = (u.navGoal == nil) or dist(u.navGoal.x, u.navGoal.z, tx, tz) > 2.5
    if (exhausted or goalMoved) and u.navCd <= 0 then
        u.navCd = 0.35
        repath(u, tx, tz)
    end
    local wx, wz = tx, tz
    if u.navPath ~= nil then
        local wp = u.navPath[u.navIdx]
        while wp ~= nil and dist(u.x, u.z, wp.x, wp.z) < 0.7 do
            u.navIdx = u.navIdx + 1
            wp = u.navPath[u.navIdx]
        end
        if wp ~= nil then
            wx, wz = wp.x, wp.z
        else
            u.navPath = nil
        end
    end
    moveToward(u, wx, wz, dt)
    return dist(u.x, u.z, tx, tz) < 1.0
end

local function setLoop(u, clip, fade)
    if (u.actionT or 0) > 0 then return end
    if u.anim == clip then return end
    u.anim = clip
    if u.ent ~= nil then PlayAnimation(u.ent, clip, true, fade or 0.15) end
end

local function playAction(u, clip, dur)
    u.anim = clip
    u.actionT = dur or 0.4
    if u.ent ~= nil then PlayAnimation(u.ent, clip, false, 0.08) end
end

-- 施法动作：不同英雄的技能剪辑命名不一（Yasuo 是 spell1a/1b/1c），逐级回退
local function playSpell(u, idx)
    u.actionT = 0.5
    if u.ent == nil then return end
    for _, clip in ipairs({ "spell" .. idx, "spell" .. idx .. "a", "spell" .. idx .. "_cast", "attack1" }) do
        if PlayAnimation(u.ent, clip, false, 0.08) then
            u.anim = clip
            return
        end
    end
    u.anim = "spell" .. idx
end

-- ==========================================================================
-- 状态（Lua 侧）
-- ==========================================================================
function applyStatusLua(u, kind, dur, mag, src)
    u.buffs = u.buffs or {}
    if kind == "slow" then
        u.buffs.slow = { t = dur, mag = mag }
    elseif kind == "burning" or kind == "poison" then
        u.buffs.burn = { t = dur, dps = mag, acc = 0, src = src }
    elseif kind == "stun" then
        u.buffs.stun = { t = dur }
    elseif kind == "shield" then
        u.buffs.shield = { t = dur, pool = mag }
    elseif kind == "msbuff" then
        u.buffs.msbuff = { t = dur, mul = mag }
    elseif kind == "adbuff" then
        u.buffs.adbuff = { t = dur, mul = mag }
    end
end

local function tickBuffs(u, dt)
    local b = u.buffs
    if b == nil then return end
    if b.burn and b.burn.t > 0 then
        b.burn.t = b.burn.t - dt
        b.burn.acc = b.burn.acc + dt
        if b.burn.acc >= 0.5 then
            b.burn.acc = b.burn.acc - 0.5
            if u.hp > 0 then
                u.hp = u.hp - b.burn.dps * 0.5
                SetHealth(u.ent, math.max(0, u.hp))
                if u.hp <= 0 then killUnit(u, b.burn.src) end
            end
        end
        if b.burn.t <= 0 then b.burn = nil end
    end
    for _, k in ipairs({ "slow", "stun", "shield", "msbuff", "adbuff" }) do
        if b[k] and b[k].t > 0 then
            b[k].t = b[k].t - dt
            if b[k].t <= 0 then b[k] = nil end
        end
    end
end

local function adMul(u)
    if u.buffs and u.buffs.adbuff and u.buffs.adbuff.t > 0 then return u.buffs.adbuff.mul end
    return 1.0
end

-- ==========================================================================
-- 投射物 / AoE
-- ==========================================================================
local function spawnProjectile(owner, x, z, dirx, dirz, opts)
    local col = opts.color or TEAM_COLOR[owner.team]
    projectiles[#projectiles + 1] = {
        team = owner.team, owner = owner, x = x, z = z,
        dx = dirx, dz = dirz, speed = opts.speed or 16,
        dmg = opts.dmg or 60, life = opts.life or 2.0,
        radius = opts.radius or 0.8, range = opts.range or 12,
        traveled = 0, pierce = opts.pierce or false,
        status = opts.status, statusDur = opts.statusDur or 2, statusMag = opts.statusMag or 0.3,
        color = col, trail = 0,
        showTrail = opts.trail ~= false,
    }
    -- 枪口/施法闪光（队伍色），让每次出手都有起手反馈
    EmitParticles({ pos = { x = x + dirx * 0.5, y = 1.1, z = z + dirz * 0.5 }, count = 6,
        vel = { x = dirx, y = 0.2, z = dirz }, speedMin = 2, speedMax = 5,
        lifeMin = 0.07, lifeMax = 0.14, sizeStart = 0.5, sizeEnd = 0.05,
        color = { r = col[1], g = col[2], b = col[3], a = 0.9 },
        colorEnd = { r = 1, g = 1, b = 1, a = 0 }, additive = true })
    -- 引擎侧火球（自发光球体 + 拖尾 + 命中爆裂），damage=0 仅作视觉；伤害由上面的 Lua 投射物结算
    if owner.ent ~= nil then
        SpawnProjectile({ x = x, y = 1.1, z = z }, { x = dirx, y = 0, z = dirz },
            opts.speed or 16, 0, opts.life or 2.0, owner.ent, opts.range or 12, 0.8,
            nil, { r = col[1], g = col[2], b = col[3], a = 1 })
    end
end

local function aoeDamage(source, cx, cz, radius, dmg, status, dur, mag)
    for i = 1, #units do
        local o = units[i]
        if not o.dead and o.team ~= source.team and dist(o.x, o.z, cx, cz) <= radius + o.radius then
            damage(o, dmg, source)
            if status then applyStatusLua(o, status, dur, mag, source) end
        end
    end
    EmitParticles({ pos = { x = cx, y = 0.5, z = cz }, count = 40, vel = { x = 0, y = 1, z = 0 },
        speedMin = 2, speedMax = 6, lifeMin = 0.3, lifeMax = 0.7,
        sizeStart = 1.0, sizeEnd = 0.05,
        color = { r = TEAM_COLOR[source.team][1], g = TEAM_COLOR[source.team][2], b = TEAM_COLOR[source.team][3], a = 0.9 },
        colorEnd = { r = 1, g = 1, b = 1, a = 0 }, gravity = 2.0, additive = true })
    -- 冲击波：贴地扩散的亮环
    EmitParticles({ pos = { x = cx, y = 0.15, z = cz }, count = 30, vel = { x = 0, y = 0.2, z = 0 },
        speedMin = radius * 2.2, speedMax = radius * 3.0, lifeMin = 0.25, lifeMax = 0.4,
        sizeStart = 0.55, sizeEnd = 0.02,
        color = { r = 1, g = 0.9, b = 0.55, a = 1.0 },
        colorEnd = { r = 1, g = 0.45, b = 0.15, a = 0 }, additive = true })
    sfx("spell_hit", 0.1)
end

-- ==========================================================================
-- 技能
-- ==========================================================================
local function castAbility(h, idx, aimX, aimZ)
    local ab = h.abilities[idx]
    if ab == nil or h.dead then return end
    if (h.cds[idx] or 0) > 0 then floatAt(h, "冷却中", false); return end
    local cost = ab.cost or 0
    if h.mana < cost then floatAt(h, "法力不足", false); return end
    h.mana = h.mana - cost
    h.cds[idx] = ab.cd or 6
    playSpell(h, idx)
    if h.isHero then sfx("cast", 0.08) end

    local dx, dz = aimX - h.x, aimZ - h.z
    dx, dz = norm(dx, dz)
    if dx == 0 and dz == 0 then
        local fy = h.yaw or 0
        dx, dz = math.sin(fy), math.cos(fy)
    end
    local t = ab.type
    local dmg = ab.dmg or 0

    if t == "projectile" then
        local count = ab.count or 1
        local spread = math.rad(ab.spread or 0)
        for i = 1, count do
            local a = 0
            if count > 1 then a = -spread * 0.5 + spread * (i - 1) / (count - 1) end
            local ca, sa = math.cos(a), math.sin(a)
            local rx, rz = dx * ca - dz * sa, dx * sa + dz * ca
            spawnProjectile(h, h.x + rx * 0.6, h.z + rz * 0.6, rx, rz, {
                speed = ab.speed or 18, dmg = dmg,
                life = (ab.range or 10) / (ab.speed or 18),
                radius = ab.radius or 0.8, pierce = ab.pierce,
                status = ab.status, statusDur = ab.statusDur, statusMag = ab.statusMag,
            })
        end
    elseif t == "aoe_self" then
        local ticks = ab.ticks or 1
        for _ = 1, ticks do
            aoeDamage(h, h.x + dx * 0.8, h.z + dz * 0.8, ab.radius or 3, dmg / ticks, ab.status, ab.statusDur, ab.statusMag)
        end
    elseif t == "aoe_target" then
        local tl = math.sqrt((aimX - h.x) ^ 2 + (aimZ - h.z) ^ 2)
        local r = ab.range or 10
        if tl > r then aimX = h.x + dx * r; aimZ = h.z + dz * r end
        groundAoes[#groundAoes + 1] = {
            team = h.team, owner = h, x = aimX, z = aimZ,
            delay = ab.delay or 0.4, radius = ab.radius or 3, dmg = dmg,
            status = ab.status, statusDur = ab.statusDur, statusMag = ab.statusMag,
        }
        -- 落点预警圈：沿圆周喷一圈粒子标记范围
        local cr = ab.radius or 3
        for i = 0, 15 do
            local a = i / 16 * math.pi * 2
            EmitParticles({ pos = { x = aimX + math.cos(a) * cr, y = 0.1, z = aimZ + math.sin(a) * cr },
                count = 1, vel = { x = 0, y = 1, z = 0 }, speedMin = 0.1, speedMax = 0.4,
                lifeMin = (ab.delay or 0.4) * 0.8, lifeMax = (ab.delay or 0.4) * 1.1,
                sizeStart = 0.5, sizeEnd = 0.15,
                color = { r = TEAM_COLOR[h.team][1], g = TEAM_COLOR[h.team][2], b = TEAM_COLOR[h.team][3], a = 0.95 },
                colorEnd = { r = 1, g = 0.4, b = 0.2, a = 0 }, additive = true })
        end
    elseif t == "dash" then
        local d = ab.dist or 4
        local steps = 8
        for _ = 1, steps do
            h.x = h.x + dx * d / steps
            h.z = h.z + dz * d / steps
            if ab.radius and ab.radius > 0 then
                for j = 1, #units do
                    local o = units[j]
                    if not o.dead and o.team ~= h.team and dist(o.x, o.z, h.x, h.z) <= ab.radius + o.radius then
                        damage(o, dmg / steps, h)
                    end
                end
            end
        end
        SetPosition(h.ent, { x = h.x, y = 0, z = h.z })
    elseif t == "melee" then
        local r = ab.range or 2.5
        local arc = math.rad(ab.arc or 120)
        local fy = h.yaw or 0
        local fx, fz = math.sin(fy), math.cos(fy)
        for j = 1, #units do
            local o = units[j]
            if not o.dead and o.team ~= h.team then
                local ox, oz = o.x - h.x, o.z - h.z
                local d = math.sqrt(ox * ox + oz * oz)
                if d <= r + o.radius then
                    local ndx, ndz = norm(ox, oz)
                    if ndx * fx + ndz * fz > math.cos(arc * 0.5) or d < 0.6 then
                        damage(o, dmg, h)
                        if ab.status then applyStatusLua(o, ab.status, ab.statusDur or 2, ab.statusMag or 0.3, h) end
                        if ab.stun then applyStatusLua(o, "stun", ab.stun, 0, h) end
                    end
                end
            end
        end
        EmitParticles({ pos = { x = h.x + fx, y = 1, z = h.z + fz }, count = 16, vel = { x = dx, y = 0.4, z = dz },
            speedMin = 1, speedMax = 3, lifeMin = 0.2, lifeMax = 0.4, sizeStart = 0.4, sizeEnd = 0.02,
            color = { r = 1, g = 1, b = 1, a = 0.8 }, colorEnd = { r = 1, g = 1, b = 1, a = 0 }, additive = true })
    elseif t == "buff" then
        if ab.shield and ab.shield > 0 then applyStatusLua(h, "shield", ab.duration or 3, ab.shield, h) end
        if ab.msMul then applyStatusLua(h, "msbuff", ab.duration or 4, ab.msMul, h) end
        if ab.adMul then applyStatusLua(h, "adbuff", ab.duration or 5, ab.adMul, h) end
        EmitParticles({ pos = { x = h.x, y = 1, z = h.z }, count = 24, vel = { x = 0, y = 1, z = 0 },
            speedMin = 0.5, speedMax = 2, lifeMin = 0.4, lifeMax = 0.8, sizeStart = 0.6, sizeEnd = 0.05,
            color = { r = TEAM_COLOR[h.team][1], g = TEAM_COLOR[h.team][2], b = TEAM_COLOR[h.team][3], a = 0.9 },
            colorEnd = { r = 1, g = 1, b = 1, a = 0 }, additive = true })
    elseif t == "heal" then
        h.hp = math.min(h.maxHp, h.hp + (ab.amount or 100))
        SetHealth(h.ent, h.hp)
        floatAt(h, "+" .. tostring(ab.amount or 100), false)
    elseif t == "execute" then
        local tgt = nearestEnemy(h, ab.range or 3)
        if tgt == nil then floatAt(h, "无目标", false); return end
        local missing = 1.0 - tgt.hp / tgt.maxHp
        local total = dmg + (tgt.maxHp * (ab.missingPct or 0.3) * missing)
        damage(tgt, total, h)
    end
end

-- ==========================================================================
-- 更新
-- ==========================================================================
local function autoAttack(u, dt)
    u.atkTimer = u.atkTimer - dt
    if u.range <= 0 then return end
    local tgt = u.target
    if u == playerHero then
        -- 玩家英雄不自动索敌/自动攻击：只打玩家点选（commandTarget）的目标。
        if tgt == nil or tgt.dead then return end
        if dist(u.x, u.z, tgt.x, tgt.z) > u.range + tgt.radius then return end
    else
        if tgt == nil or tgt.dead or dist(u.x, u.z, tgt.x, tgt.z) > u.range + tgt.radius then
            tgt = nearestEnemy(u, u.range + 0.5, u.kind == "tower")
            u.target = tgt
        end
        if tgt == nil then return end
        if dist(u.x, u.z, tgt.x, tgt.z) > u.range + tgt.radius then return end
    end
    if u.atkTimer > 0 then return end
    u.atkTimer = u.atkPeriod
    playAction(u, "attack1", math.min(0.45, u.atkPeriod))
    if u.isHero then sfx("attack", 0.08) elseif u.kind == "tower" then sfx("tower", 0.1) end
    faceTo(u, tgt.x, tgt.z)
    if u.ranged or u.kind == "tower" then
        local dx, dz = norm(tgt.x - u.x, tgt.z - u.z)
        spawnProjectile(u, u.x, u.z, dx, dz, { speed = 26, dmg = u.ad * adMul(u), life = 2.5, radius = 0.8, trail = false })
    else
        damage(tgt, u.ad * adMul(u), u)
    end
end

local function updateNeutral(u, dt)
    if u.dead then return end
    autoAttack(u, dt)
    setLoop(u, "idle1", 0.4)
    if u.hp < u.maxHp then
        u.hp = math.min(u.maxHp, u.hp + u.maxHp * 0.02 * dt)
        SetHealth(u.ent, u.hp)
    end
end

-- ==========================================================================
-- 更新
-- ==========================================================================
local function updateHeroControl(h, dt)
    if h.dead then return end
    if (h.channel or 0) > 0 then
        h.channel = h.channel - dt
        setLoop(h, "recall", 0.2)
        if h.channel <= 0 then
            local b = MAP.spawn[h.team]
            h.x, h.z = b.x, b.z
            SetPosition(h.ent, { x = h.x, y = 0, z = h.z })
            h.hp = h.maxHp
            h.mana = h.maxMana
            SetHealth(h.ent, h.hp)
            h.moveTarget = nil
            h.navPath = nil
            h.attackMove = nil
            h.channel = 0
        end
        return
    end
    -- A 键攻击移动：沿路点前进，遇到射程内敌人停下攻击，敌人没了继续走。
    if h.attackMove then
        local t = nearestEnemy(h, h.range, false)
        h.target = t
        if t == nil and h.moveTarget ~= nil then
            if stepMove(h, h.moveTarget.x, h.moveTarget.z, dt) then
                h.moveTarget = nil
                h.attackMove = nil
                h.navPath = nil
            end
        end
        autoAttack(h, dt)
        setLoop(h, t == nil and "run" or "idle1", 0.2)
        return
    end
    autoAttack(h, dt)
    -- 只有"点击敌人"下达的攻击目标才追击/停手；autoAttack 自动锁定的目标不影响移动。
    if h.commandTarget and (h.target == nil or h.target.dead) then h.commandTarget = nil end
    if h.commandTarget and h.target ~= nil and not h.target.dead then
        local t = h.target
        if dist(h.x, h.z, t.x, t.z) > h.range + t.radius then
            h.moveTarget = { x = t.x, z = t.z }
        else
            h.moveTarget = nil
            h.navPath = nil
        end
    end
    if h.moveTarget ~= nil then
        local arrived = stepMove(h, h.moveTarget.x, h.moveTarget.z, dt)
        if arrived then h.moveTarget = nil; h.navPath = nil end
        setLoop(h, "run", 0.15)
    else
        setLoop(h, "idle1", 0.2)
    end
end

-- 地面圆环 telegraph（技能/攻击范围指示）
local function drawRangeRing(cx, cz, r, cr, cg, cb)
    if r == nil or r <= 0 then return end
    for k = 0, 23 do
        local a = k / 24 * math.pi * 2
        EmitParticles({ pos = { x = cx + math.cos(a) * r, y = 0.08, z = cz + math.sin(a) * r },
            count = 1, vel = { x = 0, y = 0.2, z = 0 }, speedMin = 0.05, speedMax = 0.15,
            lifeMin = 0.06, lifeMax = 0.12, sizeStart = 0.24, sizeEnd = 0.14,
            color = { r = cr, g = cg, b = cb, a = 0.9 },
            colorEnd = { r = 1, g = 1, b = 1, a = 0 }, additive = true })
    end
end

-- 按住 QWER 的地面施法指示器（按技能类型：圆 / 直线）
local function drawSpellIndicator(h, idx, aimX, aimZ)
    local ab = h.abilities[idx]
    if ab == nil then return end
    local col = TEAM_COLOR[h.team]
    local dx, dz = norm(aimX - h.x, aimZ - h.z)
    if dx == 0 and dz == 0 then dx, dz = math.sin(h.yaw or 0), math.cos(h.yaw or 0) end
    if ab.type == "aoe_target" then
        local r = ab.range or 10
        local cx, cz = aimX, aimZ
        if dist(h.x, h.z, cx, cz) > r then cx, cz = h.x + dx * r, h.z + dz * r end
        drawRangeRing(cx, cz, ab.radius or 3, col[1], col[2], col[3])
    elseif ab.type == "aoe_self" then
        drawRangeRing(h.x, h.z, ab.radius or 3, col[1], col[2], col[3])
    else
        local len = ab.range or (ab.dist or 6)
        local steps = math.max(4, math.floor(len))
        for k = 1, steps do
            local t = k / steps * len
            EmitParticles({ pos = { x = h.x + dx * t, y = 0.08, z = h.z + dz * t },
                count = 1, vel = { x = 0, y = 0.2, z = 0 }, speedMin = 0.05, speedMax = 0.15,
                lifeMin = 0.06, lifeMax = 0.12, sizeStart = 0.22, sizeEnd = 0.14,
                color = { r = col[1], g = col[2], b = col[3], a = 0.85 },
                colorEnd = { r = 1, g = 1, b = 1, a = 0 }, additive = true })
        end
    end
end

local function updatePlayer(dt)
    local h = playerHero
    if h == nil or h.dead then return end
    -- 游戏渲染区域（设计坐标）。点击落在视野外（编辑器面板/黑边）一律忽略。
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    local m = InputMousePos()
    local inView = m ~= nil and m.x >= 0 and m.y >= 0 and m.x <= vw and m.y <= vh

    -- A：攻击移动准备；G：信号标记准备。二者都用下一次左键确认。
    if ActionPressed("attack") then h.attackArmed = true; h.pingArmed = nil end
    if ActionPressed("ping") then h.pingArmed = true; h.attackArmed = nil end

    local left = InputMousePressed("left")
    local right = InputMousePressed("right")
    if inView and (left or right) then
        -- 拾取点击处附近的敌人
        local best, bd = nil, 44
        for i = 1, #units do
            local o = units[i]
            if not o.dead and o.team ~= h.team then
                local s = WorldToScreen(o.x, o.h * 0.5, o.z)
                if s ~= nil then
                    local d = dist(s.x, s.y, m.x, m.y)
                    if d < bd then bd = d; best = o end
                end
            end
        end
        local g = groundPick(m.x, m.y)

        if left and h.pingArmed then
            -- G + 左键：地面信号标记
            if g ~= nil then
                EmitParticles({ pos = { x = g.x, y = 0.1, z = g.z }, count = 36,
                    vel = { x = 0, y = 1, z = 0 }, speedMin = 4, speedMax = 8,
                    lifeMin = 0.4, lifeMax = 0.7, sizeStart = 0.8, sizeEnd = 0.05,
                    color = { r = 1, g = 0.85, b = 0.25, a = 1 },
                    colorEnd = { r = 1, g = 0.3, b = 0, a = 0 }, gravity = 1.2, additive = true })
                SpawnFloatText({ x = g.x, y = 2.0, z = g.z }, "!", true, 1.2)
            end
            sfx("cast", 0.2)
            h.pingArmed = nil
        elseif right or (left and h.attackArmed) then
            -- 右键：移动/攻击；A + 左键：攻击移动
            if best ~= nil then
                h.target = best
                h.commandTarget = true
                h.attackMove = nil
            elseif g ~= nil then
                h.target = nil
                h.commandTarget = false
                h.moveTarget = { x = g.x, z = g.z }
                h.attackMove = left and true or nil
            end
            h.attackArmed = nil
        end
        -- 左键未配合 A/G：不做任何移动（左键默认不移动）
        if left then h.pingArmed = nil end
    end

    local g = inView and groundPick(m.x, m.y) or nil
    local fy = h.yaw or 0
    local aimX = g and g.x or (h.x + math.sin(fy) * 6)
    local aimZ = g and g.z or (h.z + math.cos(fy) * 6)
    -- 按住显示施法指示器，松开在该处释放（快速施法+指示）。
    for i = 1, 4 do
        if ActionDown("spell" .. i) then drawSpellIndicator(h, i, aimX, aimZ) end
        if ActionReleased("spell" .. i) then castAbility(h, i, aimX, aimZ) end
    end
    -- 按住 A：显示攻击范围圈。
    if ActionDown("attack") then drawRangeRing(h.x, h.z, h.range, 0.95, 0.9, 0.35) end
    if ActionPressed("recall") then h.channel = 1.4 end
    updateHeroControl(h, dt)
end

local function updateAI(h, dt)
    if h.dead then return end
    local hpFrac = h.hp / h.maxHp
    if hpFrac < 0.28 and not inOwnBase(h) then
        local b = MAP.base[h.team]
        h.target = nil
        h.moveTarget = { x = b.x, z = b.z }
        setLoop(h, "run", 0.15)
        stepMove(h, b.x, b.z, dt)
        return
    end
    local tgt, bestScore = nil, nil
    for i = 1, #units do
        local o = units[i]
        if not o.dead and o.team ~= h.team then
            local d = dist(h.x, h.z, o.x, o.z)
            local score = d
            if o.isHero then score = score - 20 end
            if o.kind == "nexus" then score = score + 15 end
            if bestScore == nil or score < bestScore then bestScore = score; tgt = o end
        end
    end
    if tgt == nil then return end
    h.target = tgt
    local d = dist(h.x, h.z, tgt.x, tgt.z)
    for i = 1, 4 do
        local ab = h.abilities[i]
        if ab and (h.cds[i] or 0) <= 0 and h.mana >= (ab.cost or 0) then
            local useRange = ab.range or ((ab.type == "aoe_self" or ab.type == "buff") and 3.5) or 6
            if d <= math.max(useRange, 5) and math.random() < 0.7 then
                castAbility(h, i, tgt.x, tgt.z)
            end
        end
    end
    autoAttack(h, dt)
    if d > h.range + tgt.radius then
        stepMove(h, tgt.x, tgt.z, dt)
        setLoop(h, "run", 0.15)
    else
        setLoop(h, "idle1", 0.2)
    end
end

local function updateMinion(u, dt)
    if u.dead then return end
    local tgt = nearestEnemy(u, 8.0, false)
    if tgt ~= nil and dist(u.x, u.z, tgt.x, tgt.z) <= u.range + tgt.radius then
        u.target = tgt
        autoAttack(u, dt)
        setLoop(u, "idle1", 0.3)
        return
    end
    if u.moveTarget ~= nil then
        stepMove(u, u.moveTarget.x, u.moveTarget.z, dt)
        setLoop(u, "run", 0.25)
    end
    autoAttack(u, dt)
end

local function updateProjectiles(dt)
    local i = 1
    while i <= #projectiles do
        local p = projectiles[i]
        local step = p.speed * dt
        p.x = p.x + p.dx * step
        p.z = p.z + p.dz * step
        p.traveled = p.traveled + step
        p.life = p.life - dt
        p.trail = p.trail + dt
        if p.showTrail and p.trail >= 0.055 then
            p.trail = 0
            EmitParticles({ pos = { x = p.x, y = 1.0, z = p.z }, count = 1, vel = { x = 0, y = 0.25, z = 0 },
                speedMin = 0.05, speedMax = 0.2, lifeMin = 0.05, lifeMax = 0.09,
                sizeStart = 0.32, sizeEnd = 0.05,
                color = { r = p.color[1], g = p.color[2], b = p.color[3], a = 0.9 },
                colorEnd = { r = 1, g = 1, b = 1, a = 0 }, additive = true })
        end
        local hit = false
        for j = 1, #units do
            local o = units[j]
            if not o.dead and o.team ~= p.team and dist(o.x, o.z, p.x, p.z) <= p.radius + o.radius then
                damage(o, p.dmg, p.owner)
                if p.status then applyStatusLua(o, p.status, p.statusDur, p.statusMag, p.owner) end
                hit = true
                if not p.pierce then break end
            end
        end
        if hit and not p.pierce then
            -- 命中爆点（技能/普攻都可见）
            EmitParticles({ pos = { x = p.x, y = 1.0, z = p.z }, count = 12, vel = { x = 0, y = 1, z = 0 },
                speedMin = 2, speedMax = 5.5, lifeMin = 0.12, lifeMax = 0.28,
                sizeStart = 0.55, sizeEnd = 0.04,
                color = { r = p.color[1], g = p.color[2], b = p.color[3], a = 1 },
                colorEnd = { r = 1, g = 0.9, b = 0.5, a = 0 }, additive = true })
            table.remove(projectiles, i)
        elseif p.life <= 0 or p.traveled >= p.range then
            table.remove(projectiles, i)
        else
            i = i + 1
        end
    end
end

local function updateGroundAoes(dt)
    local i = 1
    while i <= #groundAoes do
        local a = groundAoes[i]
        a.delay = a.delay - dt
        if a.delay <= 0 then
            aoeDamage(a.owner, a.x, a.z, a.radius, a.dmg, a.status, a.statusDur, a.statusMag)
            table.remove(groundAoes, i)
        else
            i = i + 1
        end
    end
end

local function respawnHero(u)
    u.dead = false
    u.hp = u.maxHp
    u.mana = u.maxMana
    u.buffs = {}
    u.target = nil
    u.moveTarget = nil
    u.anim = nil
    u.actionT = 0
    u.atkTimer = 0
    local b = MAP.spawn[u.team]
    u.x, u.z = b.x, b.z
    SetPosition(u.ent, { x = u.x, y = 0, z = u.z })
    SetHealth(u.ent, u.hp)
    SetRotationY(u.ent, (u.team == BLUE) and 0 or math.pi)
    u.navPath = nil
    u.navGoal = nil
end

local function cleanupUnits(dt)
    local i = 1
    while i <= #units do
        local u = units[i]
        if u.dead then
            u.dying = u.dying - dt
            if u.dying <= 0 then
                if u.isHero then
                    respawnHero(u)
                    i = i + 1
                else
                    if u.ent ~= nil then Despawn(u.ent) end
                    units[i] = units[#units]
                    units[#units] = nil
                end
            else
                i = i + 1
            end
        else
            i = i + 1
        end
    end
end

local function updateWaves(dt)
    if gameOver then return end
    waveTimer = waveTimer - dt
    if waveTimer > 0 then return end
    waveTimer = MINION_WAVE_PERIOD
    waveN = waveN + 1
    for _, team in ipairs({ BLUE, RED }) do
        local p = MAP.spawn[team]
        for k = 1, MINION_COUNT do
            local off = (k - (MINION_COUNT + 1) / 2) * 1.6
            spawnMinion("melee", team, p.x + off, p.z)
        end
        for k = 1, MINION_COUNT do
            local off = (k - (MINION_COUNT + 1) / 2) * 1.6
            spawnMinion("caster", team, p.x + off, p.z - (team == BLUE and 2 or -2))
        end
    end
    if waveN % 3 == 0 then
        for _, team in ipairs({ BLUE, RED }) do
            local p = MAP.spawn[team]
            spawnMinion("siege", team, p.x, p.z)
        end
    end
end

local function updateCameraFollow(dt)
    local h = playerHero
    -- Y：锁定/解锁视角。锁定＝英雄居中；解锁＝可边缘平移。
    if ActionPressed("camera") then camFollow = not camFollow end
    if camFollow and h ~= nil and not h.dead then
        camFocusX = camFocusX + (h.x - camFocusX) * math.min(1, dt * 10)
        camFocusZ = camFocusZ + (h.z - camFocusZ) * math.min(1, dt * 10)
    end
    if ActionPressed("center") then
        camFollow = true
        if h ~= nil then camFocusX, camFocusZ = h.x, h.z end
    end
    if not camFollow then
        -- 屏幕边缘平移（沿相机 yaw 的屏幕方向，手感与画面一致）。
        local m = InputMousePos()
        local vp = GetViewportSize()
        local vw = (vp and vp.w) or VW
        local vh = (vp and vp.h) or VH
        if m ~= nil then
            local edge = 0.04 * math.min(vw, vh) + 6
            local pan = 26 * dt
            local cy, sy = math.cos(CAM.yaw), math.sin(CAM.yaw)
            local rx, rz = -cy, sy   -- 屏幕右方向（地面）
            local ux, uz = sy, cy    -- 屏幕上前方向（地面）
            if m.x < edge then camFocusX = camFocusX - rx * pan; camFocusZ = camFocusZ - rz * pan end
            if m.x > vw - edge then camFocusX = camFocusX + rx * pan; camFocusZ = camFocusZ + rz * pan end
            if m.y < edge then camFocusX = camFocusX + ux * pan; camFocusZ = camFocusZ + uz * pan end
            if m.y > vh - edge then camFocusX = camFocusX - ux * pan; camFocusZ = camFocusZ - uz * pan end
            camFocusX = clamp(camFocusX, -70, 70)
            camFocusZ = clamp(camFocusZ, -70, 70)
        end
    end
    CAM.dist = clamp(CAM.dist - MouseWheel() * 3.0, CAM.minDist, CAM.maxDist)
    applyCamera()
end

local function updateVision()
    for i = 1, #units do
        local u = units[i]
        local hidden = false
        if not u.dead and u.team == RED then
            local brush = false
            for _, b in ipairs(BRUSHES) do
                if dist2(u.x, u.z, b.x, b.z) <= b.r * b.r then brush = true; break end
            end
            if brush then
                local seen = false
                for j = 1, #units do
                    local o = units[j]
                    if not o.dead and o.team == BLUE and dist(o.x, o.z, u.x, u.z) < 9 then
                        seen = true; break
                    end
                end
                hidden = not seen
            end
        end
        u.hidden = hidden
        if u.ent ~= nil then SetVisible(u.ent, not hidden) end
    end
end

local function updatePlates()
    for i = 1, #units do
        local u = units[i]
        if u.ent ~= nil then
            if u.hidden then
                SetEntityPlate(u.ent, u.name, -1)
            else
                SetEntityPlate(u.ent, u.name, u.dead and -1 or (u.hp / u.maxHp))
            end
            plateTeam[fmtKey(u.ent)] = u.team
        end
    end
end

-- ==========================================================================
-- 生命周期
-- ==========================================================================
function on_start(e)
    loadRoster()
    loadAbilityData()
    loadItems()
    camEnt = FindNamedEntity("Main Camera")
    SELECT = {}
    for name, _ in pairs(ROSTER) do SELECT[#SELECT + 1] = name end
    table.sort(SELECT)
    selectIndex = 1
    selectTime = 0
    phase = "select"
    camFocusX, camFocusZ = 0, 8
    applyCamera()
    for _, team in ipairs({ BLUE, RED }) do
        spawnStructure("nexus", team, MAP.base[team].x, MAP.base[team].z)
        for _, t in ipairs(MAP.towers[team]) do
            spawnStructure("tower", team, t.x, t.z)
        end
    end
    -- 塔/水晶是脚本生成的结构体（不在 sr_map.glb 里），单独写进导航网格当障碍。
    for _, team in ipairs({ BLUE, RED }) do
        NavBlock(MAP.base[team].x, MAP.base[team].z, 3.0, false)
        for _, t in ipairs(MAP.towers[team]) do
            NavBlock(t.x, t.z, 2.0, false)
        end
    end
    waveTimer = FIRST_WAVE
end

local function chooseChampion(name)
    playerHero = spawnHero(name, BLUE)
    local pick = AI_CHAMP
    if pick == name then pick = (name == "Ashe") and "Garen" or "Ashe" end
    enemyHero = spawnHero(pick, RED)
    spawnJungle()
    phase = "loading"
    loadingTime = 0
end

local function updateLoading(dt)
    loadingTime = loadingTime + dt
    if loadingTime > 1.8 then
        phase = "play"
        if playerHero ~= nil then camFocusX, camFocusZ = playerHero.x, playerHero.z end
        PlayMusic("ambience", 0.30)
    end
end

local function updateSelect()
    local n = #SELECT
    if n == 0 then phase = "play"; return end
    selectTime = selectTime + 1
    if selectTime > 480 then chooseChampion(SELECT[selectIndex]); return end
    if InputKey("Left") == 1 or InputKey("A") == 1 then selectIndex = ((selectIndex - 2) % n) + 1 end
    if InputKey("Right") == 1 or InputKey("D") == 1 then selectIndex = (selectIndex % n) + 1 end
    for i = 1, math.min(9, n) do
        if InputKey(tostring(i)) == 1 then selectIndex = i end
    end
    local m = InputMousePos()
    if m ~= nil then
        local vp = GetViewportSize()
        local vw = (vp and vp.w) or VW
        local vh = (vp and vp.h) or VH
        local cols, cell, gap = 5, 120, 16
        local rows = math.ceil(n / cols)
        local totalW = cols * cell + (cols - 1) * gap
        local totalH = rows * cell + (rows - 1) * gap
        local ox = (vw - totalW) * 0.5
        local oy = (vh - totalH) * 0.5 + 20
        for i = 1, n do
            local r = math.floor((i - 1) / cols)
            local c = (i - 1) % cols
            local x = ox + c * (cell + gap)
            local y = oy + r * (cell + gap)
            if m.x >= x and m.x <= x + cell and m.y >= y and m.y <= y + cell then
                selectIndex = i
            end
        end
    end
    if InputMousePressed("left") or InputKey("Return") == 1 or InputKey("Enter") == 1 or ActionPressed("spell1") then
        chooseChampion(SELECT[selectIndex])
    end
end

function on_update(e, dt)
    if phase == "select" then
        updateSelect()
        updateCameraFollow(dt)
        return
    end
    if phase == "loading" then
        updateLoading(dt)
        updateCameraFollow(dt)
        return
    end
    if gameOver then
        if ActionPressed("center") or InputKey("Return") == 1 or InputKey("Enter") == 1 then
            ChangeScene("assets/scenes/moba.json")
        end
        return
    end
    elapsed = elapsed + dt
    for i = 1, #heroes do
        local h = heroes[i]
        for k = 1, 4 do
            if (h.cds[k] or 0) > 0 then h.cds[k] = h.cds[k] - dt end
        end
    end
    updatePlayer(dt)
    updateEconomy(dt)
    updateShop(dt)
    if enemyHero ~= nil then updateAI(enemyHero, dt) end
    for i = 1, #units do
        local u = units[i]
        if not u.dead then
            if (u.actionT or 0) > 0 then u.actionT = math.max(0, u.actionT - dt) end
            tickBuffs(u, dt)
            if u.kind == "minion" then updateMinion(u, dt)
            elseif u.kind == "neutral" then updateNeutral(u, dt)
            elseif u.kind == "tower" then autoAttack(u, dt) end
        end
    end
    updateProjectiles(dt)
    updateGroundAoes(dt)
    updateWaves(dt)
    updateCameraFollow(dt)
    updateVision()
    updatePlates()
    cleanupUnits(dt)
end

-- ==========================================================================
-- HUD (on_render, 1280x720 设计坐标)
-- ==========================================================================
local function bar(x, y, w, h, frac, r, g, b)
    DrawRect(x, y, w, h, 0.05, 0.05, 0.07, 0.85)
    if frac > 0 then DrawRect(x + 1, y + 1, (w - 2) * clamp(frac, 0, 1), h - 2, r, g, b, 1) end
    DrawRectOutline(x, y, w, h, 1, 0, 0, 0, 0.9)
end

-- 目标选中框：点选/锁定敌人时在其头顶画方框
local function drawTargetMarker()
    local h = playerHero
    if h == nil or h.target == nil or h.target.dead then return end
    local s = WorldToScreen(h.target.x, h.target.h * 0.5, h.target.z)
    if s ~= nil then
        DrawRectOutline(s.x - 18, s.y - 18, 36, 36, 2, 0.95, 0.45, 0.3, 1)
    end
end

local function drawWorldPlates()
    local anchors = ScreenAnchors()
    local plates = EntityPlates()
    for i = 1, #anchors do
        local a = anchors[i]
        if a.onscreen then
            local p = plates[fmtKey(a.entity)]
            if p ~= nil and p.hp >= 0 then
                local w, h = 50, 5
                DrawRect(a.x - w / 2, a.y - h, w, h, 0.04, 0.04, 0.06, 0.8)
                local key = fmtKey(a.entity)
                local team = plateTeam[key]
                local col
                if team == 0 then
                    col = { 0.9, 0.78, 0.2 }
                else
                    col = (team and TEAM_COLOR[team]) or { 0.85, 0.2, 0.2 }
                end
                if p.hp > 0 then DrawRect(a.x - w / 2 + 1, a.y - h + 1, (w - 2) * p.hp, h - 2, col[1], col[2], col[3], 1) end
                DrawText(p.name, a.x, a.y - h - 14, 12, 1, 1, 1, 0.9, true, true)
            end
        end
    end
end

local function drawFloatTexts()
    local texts = FloatTexts()
    for i = 1, #texts do
        local f = texts[i]
        local age = f.age / math.max(0.01, f.life)
        local s = WorldToScreen(f.world.x, f.world.y + age * 0.8, f.world.z)
        if s ~= nil and age < 1 then
            DrawText(f.text, s.x, s.y, f.crit and 20 or 15, 1, 1, 1, 1 - age, true, true)
        end
    end
end

local function drawHud()
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    local cx = vw * 0.5
    local mm = math.floor(elapsed / 60)
    local ss = math.floor(elapsed) % 60
    DrawRect(cx - 70, 6, 140, 26, 0.05, 0.05, 0.08, 0.75)
    DrawText(string.format("%02d:%02d", mm, ss), cx, 19, 17, 1, 1, 1, 1, true, true)
    DrawText(TEAM_NAME[BLUE] .. " 击杀 " .. kills[BLUE], cx - 200, 19, 15, TEAM_COLOR[BLUE][1], TEAM_COLOR[BLUE][2], TEAM_COLOR[BLUE][3], 1, true, true)
    DrawText(TEAM_NAME[RED] .. " 击杀 " .. kills[RED], cx + 200, 19, 15, TEAM_COLOR[RED][1], TEAM_COLOR[RED][2], TEAM_COLOR[RED][3], 1, true, true)

    local h = playerHero
    if h == nil then return end
    local bw, bh = 240, 16
    local bx, by = cx - bw / 2, vh - 96
    DrawText(string.format("%s  Lv.%d", h.name, h.level), bx, by - 22, 16, 1, 1, 1, 1, false, true)
    bar(bx, by, bw, bh, h.hp / h.maxHp, 0.2, 0.8, 0.25)
    bar(bx, by + bh + 3, bw, bh - 4, h.maxMana > 0 and h.mana / h.maxMana or 0, 0.25, 0.45, 0.95)
    DrawText(string.format("%d / %d", math.max(0, math.floor(h.hp)), math.floor(h.maxHp)), bx + bw / 2, by + bh / 2, 12, 1, 1, 1, 1, true, true)
    if h.portrait and h.portrait ~= "" then
        DrawSprite(h.portrait, bx - 64, by - 12, 58, 58, 1, 1, 1, 1)
        DrawRectOutline(bx - 64, by - 12, 58, 58, 2, 0.85, 0.72, 0.35, 1)
    end

    local keys = { "Q", "W", "E", "R" }
    local slot = 48
    local total = slot * 4 + 3 * 8
    local sx = cx - total / 2
    local sy = vh - slot - 8
    for i = 1, 4 do
        local x = sx + (i - 1) * (slot + 8)
        local ab = h.abilities[i]
        local cost = (ab and ab.cost) or 0
        local ready = (h.cds[i] or 0) <= 0 and h.mana >= cost
        if ab ~= nil and ab.icon and ab.icon ~= "" then
            DrawSprite(ab.icon, x, sy, slot, slot, 1, 1, 1, 1)
        else
            DrawRect(x, sy, slot, slot, 0.08, 0.08, 0.12, 0.9)
        end
        DrawRectOutline(x, sy, slot, slot, 2, ready and 0.9 or 0.3, ready and 0.78 or 0.3, 0.2, 1)
        DrawText(keys[i], x + 7, sy + 8, 13, 1, 1, 1, 0.95, false, false)
        if ab ~= nil then
            local cd = h.cds[i] or 0
            if cd > 0 then
                local frac = clamp(cd / (ab.cd or 1), 0, 1)
                DrawRect(x, sy + slot * (1 - frac), slot, slot * frac, 0, 0, 0, 0.72)
                DrawText(string.format("%.0f", math.ceil(cd)), x + slot / 2, sy + slot / 2, 20, 1, 1, 1, 1, true, true)
            end
            if cost > 0 then
                DrawRect(x, sy + slot - 14, slot, 14, 0, 0, 0, 0.5)
                DrawText(tostring(cost), x + slot / 2, sy + slot - 7, 11, 0.55, 0.78, 1, 1, true, true)
            end
        end
    end

    local infoY = sy - 22
    DrawText(string.format("金币 %d", math.floor(h.gold)), bx, infoY, 15, 0.95, 0.82, 0.3, 1, false, true)
    DrawText(string.format("补刀 %d", h.cs), bx + 86, infoY, 15, 0.9, 0.9, 0.9, 1, false, true)
    DrawText(string.format("KDA %d/%d", h.kills, h.deaths), bx + 166, infoY, 15, 0.9, 0.9, 0.9, 1, false, true)
    DrawRect(bx - 64, by + 48, 58, 8, 0.05, 0.05, 0.08, 0.9)
    DrawRect(bx - 63, by + 49, 56 * clamp(h.xp / xpForLevel(h.level), 0, 1), 6, 0.85, 0.7, 0.25, 1)
    DrawText("Lv." .. h.level, bx - 35, by + 52, 11, 1, 1, 1, 1, true, true)

    if h.dead then
        DrawRect(0, 0, vw, vh, 0, 0, 0, 0.45)
        DrawText(string.format("复活中 %.1fs", math.max(0, h.dying)), cx, vh / 2, 30, 1, 0.3, 0.3, 1, true, true)
    end

    if gameOver then
        DrawRect(0, 0, vw, vh, 0, 0, 0, 0.6)
        local win = winner == BLUE
        DrawText(win and "胜利" or "失败", cx, vh / 2 - 20, 48,
            win and 0.3 or 1, win and 0.9 or 0.3, win and 0.4 or 0.3, 1, true, true)
        DrawText("按 Enter / 空格 重新开始", cx, vh / 2 + 30, 20, 1, 1, 1, 1, true, true)
    end
end

local function drawMinimap(vw)
    local w, h = 150, 150
    local x, y = vw - w - 12, 40
    DrawSprite(MINIMAP_IMG, x, y, w, h, 1, 1, 1, 0.92)
    DrawRectOutline(x, y, w, h, 2, 0.7, 0.6, 0.3, 0.9)
    -- 世界坐标 -> 小地图图像坐标：地图实体绕 Y 旋转 -45°，标记做同样旋转，
    -- 使小地图上的位置/朝向与图像和 3D 视角一致。
    local rc, rs = math.cos(math.pi * 0.25), math.sin(math.pi * 0.25)
    local function mapX(wx, wz) return x + w * 0.5 + ((rc * wx + rs * wz) / 140) * (w * 0.42) end
    local function mapY(wx, wz) return y + h * 0.5 - ((-rs * wx + rc * wz) / 140) * (h * 0.42) end
    for i = 1, #units do
        local u = units[i]
        if not u.dead and not u.hidden then
            local col = TEAM_COLOR[u.team] or { 0.6, 0.6, 0.6 }
            local s = (u.kind == "champion") and 5 or (u.kind == "minion" and 2 or 4)
            DrawRect(mapX(u.x, u.z) - s / 2, mapY(u.x, u.z) - s / 2, s, s, col[1], col[2], col[3], 1)
        end
    end
end

local function drawScoreboard()
    if not ActionDown("scoreboard") then return end
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    DrawRect(0, 0, vw, vh, 0, 0, 0, 0.5)
    DrawRect(vw * 0.5 - 360, 110, 720, 310, 0.04, 0.05, 0.08, 0.96)
    DrawRectOutline(vw * 0.5 - 360, 110, 720, 310, 2, 0.6, 0.5, 0.2, 1)
    DrawText("记分板  (Tab)", vw * 0.5, 88, 22, 0.95, 0.82, 0.35, 1, true, true)
    local rows = { { playerHero, TEAM_COLOR[BLUE], "蓝方" }, { enemyHero, TEAM_COLOR[RED], "红方" } }
    for i, r in ipairs(rows) do
        local h = r[1]
        if h ~= nil then
            local y = 150 + (i - 1) * 130
            DrawText(r[3], vw * 0.5 - 330, y + 8, 16, r[2][1], r[2][2], r[2][3], 1, false, true)
            if h.portrait then DrawSprite(h.portrait, vw * 0.5 - 330, y + 28, 64, 64, 1, 1, 1, 1) end
            DrawText(string.format("%s  Lv.%d", h.name, h.level), vw * 0.5 - 250, y + 36, 20, 1, 1, 1, 1, false, true)
            DrawText(string.format("KDA %d/%d    补刀 %d    金币 %d", h.kills, h.deaths, h.cs, math.floor(h.gold)),
                vw * 0.5 - 250, y + 66, 15, 0.9, 0.9, 0.9, 1, false, true)
            DrawText("装备", vw * 0.5 + 30, y + 8, 14, 0.9, 0.85, 0.6, 1, false, true)
            for k = 1, #h.items do
                local it = ITEMS[h.items[k]]
                if it and it.icon ~= "" then
                    DrawSprite(it.icon, vw * 0.5 + 30 + (k - 1) * 42, y + 26, 38, 38, 1, 1, 1, 1)
                end
            end
        end
    end
end

local function drawShop()
    if not shopOpen or playerHero == nil then return end
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    DrawRect(0, 0, vw, vh, 0, 0, 0, 0.55)
    local cols, cell, gap = 5, 74, 10
    local n = #SHOP
    local rows = math.ceil(n / cols)
    local totalW = cols * cell + (cols - 1) * gap
    local totalH = rows * cell + (rows - 1) * gap
    local ox = vw * 0.5 - totalW * 0.5
    local oy = vh * 0.5 - totalH * 0.5
    local h = playerHero
    DrawText("装备商店", vw * 0.5, oy - 48, 24, 0.95, 0.82, 0.35, 1, true, true)
    DrawText("点击购买，P 关闭", vw * 0.5, oy - 24, 14, 0.85, 0.85, 0.85, 1, true, true)
    for i = 1, n do
        local it = ITEMS[SHOP[i]]
        if it ~= nil then
            local r = math.floor((i - 1) / cols)
            local c = (i - 1) % cols
            local x = ox + c * (cell + gap)
            local y = oy + r * (cell + gap)
            DrawRect(x, y, cell, cell, 0.1, 0.1, 0.14, 0.95)
            if it.icon and it.icon ~= "" then DrawSprite(it.icon, x + 4, y + 4, cell - 8, cell - 8, 1, 1, 1, 1) end
            local afford = h ~= nil and h.gold >= (it.price or 0)
            DrawRectOutline(x, y, cell, cell, 2, afford and 0.9 or 0.4, afford and 0.75 or 0.35, 0.2, 1)
            DrawText(tostring(it.price), x + cell / 2, y + cell - 7, 11, 0.95, 0.82, 0.3, 1, true, true)
            DrawText(it.name, x + cell / 2, y - 10, 11, 1, 1, 1, 0.9, true, true)
        end
    end
end

local function drawSelect()
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    DrawRect(0, 0, vw, vh, 0.02, 0.03, 0.05, 0.82)
    DrawText("选择你的英雄", vw * 0.5, 60, 30, 0.95, 0.82, 0.35, 1, true, true)
    DrawText("方向键 / 数字键 / 鼠标点击选择，Enter 或 Q 确认", vw * 0.5, 92, 15, 0.85, 0.85, 0.85, 1, true, true)
    local n = #SELECT
    local cols, cell, gap = 5, 120, 16
    local rows = math.ceil(n / cols)
    local totalW = cols * cell + (cols - 1) * gap
    local totalH = rows * cell + (rows - 1) * gap
    local ox = (vw - totalW) * 0.5
    local oy = (vh - totalH) * 0.5 + 20
    for i = 1, n do
        local name = SELECT[i]
        local d = CHAMP_DATA[name]
        local r = math.floor((i - 1) / cols)
        local c = (i - 1) % cols
        local x = ox + c * (cell + gap)
        local y = oy + r * (cell + gap)
        if d ~= nil and d.portrait then
            DrawSprite(d.portrait, x, y, cell, cell, 1, 1, 1, 1)
        else
            DrawRect(x, y, cell, cell, 0.15, 0.15, 0.2, 1)
        end
        local sel = (i == selectIndex)
        DrawRectOutline(x, y, cell, cell, sel and 4 or 2,
            sel and 0.95 or 0.3, sel and 0.8 or 0.3, 0.2, 1)
        DrawText((ROSTER[name] and ROSTER[name].chinese) or name, x + cell / 2, y + cell + 12, 14,
            1, 1, 1, 1, true, true)
    end
end

local function drawLoading()
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    DrawRect(0, 0, vw, vh, 0.02, 0.03, 0.05, 1)
    local ph, eh = playerHero, enemyHero
    local ih = vh * 0.78
    local iw = ih * 0.55
    if ph ~= nil then
        DrawSprite("assets/lol/icons/loading/" .. ph.key .. ".jpg", vw * 0.5 - iw - 24, (vh - ih) * 0.5, iw, ih, 1, 1, 1, 1)
    end
    if eh ~= nil then
        DrawSprite("assets/lol/icons/loading/" .. eh.key .. ".jpg", vw * 0.5 + 24, (vh - ih) * 0.5, iw, ih, 1, 1, 1, 1)
    end
    DrawText((ph and ph.name or "") .. "   VS   " .. (eh and eh.name or ""), vw * 0.5, vh - 74, 22, 1, 1, 1, 1, true, true)
    DrawText("加载中…", vw * 0.5, vh - 44, 15, 0.9, 0.85, 0.6, 1, true, true)
    local prog = clamp(loadingTime / 1.8, 0, 1)
    DrawRect(vw * 0.5 - 160, vh - 26, 320, 8, 0.1, 0.1, 0.12, 1)
    DrawRect(vw * 0.5 - 159, vh - 25, 318 * prog, 6, 0.85, 0.7, 0.25, 1)
end

function on_render()
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    if phase == "select" then
        drawSelect()
        return
    end
    if phase == "loading" then
        drawLoading()
        return
    end
    drawWorldPlates()
    drawTargetMarker()
    drawFloatTexts()
    drawHud()
    drawMinimap(vw)
    drawScoreboard()
    drawShop()
end

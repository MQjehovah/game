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
    inhibitors = {
        [BLUE] = { { x = 0, z = -62 } },
        [RED] = { { x = 0, z = 62 } },
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
local CAM_FOV = 52
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
local neutralRespawns = {}                 -- 野怪刷新计时 { cfg, t }
local inhibDown = { [BLUE] = false, [RED] = false } -- 兵营被破 -> 对方出超级兵
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
local killFeed = {} -- 击杀播报 {killer, victim, color, t}
local elapsed = 0
local netStateTimer = 0
local netHudTimer = 0
local chooseChampion -- forward (defined below; used by lobby/match-start)
-- 联机大厅（client 角色）：等待对手 / 房号 / 房间列表
local LOBBY = { phase = "lobby", room = "", players = 0, max = 2, host = 0, rooms = {},
                side = nil, code = "", msg = "", refreshT = 0, keysPrev = {},
                click = false, clickX = 0, clickY = 0 }
local ROSTER = {}
local CHAMP_DATA = {}
local ITEMS = {}
local SHOP = {}
local shopOpen = false
local plateTeam = {}
local plateMana = {}  -- 头顶蓝条（champion）
local plateLevel = {} -- 头顶等级（champion）
local MINIMAP_IMG = "assets/lol/ui/minimap.png"
-- 大厅 UI 素材（ComfyUI/Qwen-Image 生成，见 assets/ui/）
local UI_BG = "assets/ui/lobby_bg.jpg"
local UI_PANEL = "assets/ui/panel.jpg"
local UI_BTN = "assets/ui/button.jpg"
local UI_BTN_ACCENT = "assets/ui/button_accent.jpg"

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
local camShake = 0 -- 受击/命中震屏强度
local camEnt = nil

local function camPos()
    local sp, cp = math.sin(CAM.pitch), math.cos(CAM.pitch)
    local cy, sy = math.cos(CAM.yaw), math.sin(CAM.yaw)
    -- Ground-plane offset (0, -cp*dist) rotated by yaw about the focus, so the
    -- whole map reads at the same angle as the minimap.
    local rx = -sy * cp * CAM.dist
    local rz = -cy * cp * CAM.dist
    -- 轻微震屏（命中/受击）：沿屏幕方向抖动焦点
    local shx, shz = 0, 0
    if camShake > 0 then
        local amp = camShake * 0.35
        shx = math.sin(elapsed * 47.0) * amp
        shz = math.cos(elapsed * 53.0) * amp
    end
    return camFocusX + rx + shx, sp * CAM.dist, camFocusZ + rz + shz
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
        baseAtkPeriod = (s.range or 2.0) > 3.0 and 1.0 or 1.15,
        ranged = (s.range or 2.0) > 3.0,
        armor = 30, mr = 30,
        abilities = s.abilities or {}, cds = { 0, 0, 0, 0 },
        ranks = { 0, 0, 0, 0 }, skillPoints = 1,
        buffs = {}, target = nil, moveTarget = nil, state = "idle",
        anim = nil, actionT = 0, yaw = (team == BLUE) and 0 or math.pi,
        channel = 0, dying = 0, dead = false, isHero = true,
        base = { hp = s.hp, hpGrow = s.hpGrow or 78, mana = s.mana or 0,
                 manaGrow = s.manaGrow or 30, ad = s.ad or 60, adGrow = s.adGrow or 4,
                 ms = s.ms or 340, armor = 30, mr = 30 },
        level = 1, xp = 0, gold = 500, cs = 0, kills = 0, deaths = 0, ap = 0,
        bonus = { ad = 0, ap = 0, hp = 0, mana = 0, armor = 0, mr = 0, ms = 0, as = 0 },
        items = {},
    }
    units[#units + 1] = u
    heroes[#heroes + 1] = u
    SetRotationY(ent, u.yaw)
    SetHealth(ent, u.hp)
    return u
end

local MINION_DEFS = {
    super = { hp = 1300, dmg = 65, range = 2.2, speed = 2.6, radius = 0.7, h = 1.5, period = 1.4 },
    melee = { hp = 450, dmg = 22, range = 1.8, speed = 3.2, radius = 0.5, h = 1.1, period = 1.1 },
    caster = { hp = 300, dmg = 26, range = 6.5, speed = 3.0, radius = 0.45, h = 1.1, period = 1.2, ranged = true },
    siege = { hp = 800, dmg = 45, range = 7.5, speed = 2.7, radius = 0.7, h = 1.4, period = 1.6, ranged = true },
}

local function spawnMinion(kind, team, x, z)
    -- 僵持局小兵会无限累积，separateUnits 是 O(n²)：设上限避免帧率崩塌。
    if #units >= 140 then return nil end
    local d = MINION_DEFS[kind]
    -- 超级兵复用攻城兵模型（没有单独的 super 资产）。
    local model = (kind == "super") and "siege" or kind
    local prefab = "minion_" .. model .. (team == BLUE and "_blue" or "_red")
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
    local prefab
    if kind == "nexus" then prefab = "struct_nexus_" .. suffix
    elseif kind == "inhibitor" then prefab = "struct_inhibitor_" .. suffix
    else prefab = "struct_tower_" .. suffix end
    local ent = SpawnPrefab(prefab, { x = x, y = 0, z = z })
    if ent == nil then return nil end
    local u
    if kind == "nexus" then
        u = {
            id = nid(), ent = ent, kind = "nexus", team = team, name = "水晶",
            x = x, z = z, h = 5.2, radius = 2.2, hp = NEXUS_HP, maxHp = NEXUS_HP,
            ad = 0, range = 0, speed = 0, atkPeriod = 1, atkTimer = 0,
            armor = 20, mr = 20,
            buffs = {}, target = nil, anim = nil, actionT = 0, dead = false,
        }
        SetPosition(ent, { x = x, y = 0, z = z })
    elseif kind == "inhibitor" then
        u = {
            id = nid(), ent = ent, kind = "inhibitor", team = team, name = "兵营",
            x = x, z = z, h = 3.2, radius = 1.5, hp = 1500, maxHp = 1500,
            ad = 0, range = 0, speed = 0, atkPeriod = 1, atkTimer = 0,
            armor = 20, mr = 20,
            buffs = {}, target = nil, anim = nil, actionT = 0, dead = false,
        }
        SetPosition(ent, { x = x, y = 0, z = z })
    else
        u = {
            id = nid(), ent = ent, kind = "tower", team = team, name = "防御塔",
            x = x, z = z, h = 7.4, radius = 1.3, hp = TOWER_HP, maxHp = TOWER_HP,
            ad = 95, range = 9.0, speed = 0, atkPeriod = 1.25, atkTimer = 0,
            armor = 40, mr = 40,
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
    { key = "red", name = "红BUFF", prefab = "unit_sru_red", x = -14, z = -22, hp = 2300, ad = 80, range = 2.0, radius = 0.8, h = 1.6, gold = 100, xp = 110, buff = "red", respawn = 120 },
    { key = "bluecamp", name = "蓝BUFF", prefab = "unit_sru_gromp", x = -20, z = -34, hp = 1800, ad = 60, range = 2.2, radius = 0.9, h = 1.8, gold = 80, xp = 90, buff = "blue", respawn = 120 },
    { key = "wolf", name = "魔沼蛙", prefab = "unit_sru_murkwolf", x = -11, z = -30, hp = 1300, ad = 45, range = 2.0, radius = 0.6, h = 1.2, gold = 60, xp = 70, buff = "", respawn = 120 },
    { key = "crab", name = "迅捷蟹", prefab = "unit_sru_crab", x = -9, z = 0, hp = 1200, ad = 40, range = 2.0, radius = 0.6, h = 1.0, gold = 55, xp = 60, buff = "", respawn = 120 },
    { key = "dragon", name = "巨龙", prefab = "unit_sru_dragon", x = 13, z = -2, hp = 3800, ad = 110, range = 6.0, radius = 1.6, h = 3.0, gold = 150, xp = 200, buff = "dragon", respawn = 300 },
    { key = "baron", name = "纳什男爵", prefab = "unit_sru_baron", x = -13, z = 3, hp = 5200, ad = 140, range = 6.0, radius = 1.8, h = 3.4, gold = 200, xp = 280, buff = "baron", respawn = 360 },
}

local function spawnNeutral(cfg)
    local ent = SpawnPrefab(cfg.prefab, { x = cfg.x, y = 0, z = cfg.z })
    if ent == nil then return nil end
    local u = {
        id = nid(), ent = ent, kind = "neutral", team = 0, name = cfg.name or cfg.key,
        x = cfg.x, z = cfg.z, h = cfg.h, radius = cfg.radius,
        hp = cfg.hp, maxHp = cfg.hp, ad = cfg.ad, range = cfg.range, speed = 0,
        atkPeriod = 1.6, atkTimer = 0, armor = 15, mr = 15, buffs = {}, target = nil,
        home = { x = cfg.x, z = cfg.z }, gold = cfg.gold, xpReward = cfg.xp, campBuff = cfg.buff,
        campKey = cfg.key, cfg = cfg, respawn = cfg.respawn or 120,
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

local function floatAt(u, text, crit, cr, cg, cb)
    SpawnFloatText({ x = u.x, y = u.h + 0.4, z = u.z }, text, crit or false, 0.85,
        cr or 1.0, cg or 1.0, cb or 1.0)
end

-- 统一的金币入账：玩家金币增加时头顶飘 "+N 金币"（击杀/补刀/被动收入都用它）。
local function gainGold(u, amount)
    if u == nil or amount == nil or amount <= 0 then return end
    u.gold = (u.gold or 0) + amount
    if u == playerHero then
        floatAt(u, "+" .. tostring(math.floor(amount + 0.5)) .. " 金币", false, 1.0, 0.86, 0.30)
    end
end

local function recomputeDerived(u)
    local b = u.base
    if b == nil then return end
    u.maxHp = b.hp + b.hpGrow * (u.level - 1) + u.bonus.hp
    u.maxMana = b.mana + b.manaGrow * (u.level - 1) + u.bonus.mana
    u.ad = b.ad + b.adGrow * (u.level - 1) + u.bonus.ad
    u.ap = u.bonus.ap
    u.armor = (b.armor or 0) + (u.bonus.armor or 0)
    u.mr = (b.mr or 0) + (u.bonus.mr or 0)
    u.speed = (b.ms + u.bonus.ms) * 0.015
    -- 攻速：基础攻击间隔 / (1 + 攻速加成)。
    u.atkPeriod = (u.baseAtkPeriod or 1.15) / (1.0 + (u.bonus.as or 0))
end

-- 技能加点：R 上限 3，其余 5。玩家手动加（Ctrl+QWER），AI 自动加。
local RANK_MAX = { 5, 5, 5, 3 }

local function spendPoint(h, idx)
    if h == nil or h.ranks == nil then return false end
    if (h.skillPoints or 0) <= 0 then return false end
    if h.ranks[idx] >= RANK_MAX[idx] then return false end
    h.ranks[idx] = h.ranks[idx] + 1
    h.skillPoints = h.skillPoints - 1
    return true
end

local function autoLevel(h)
    if h == nil or h.ranks == nil then return end
    while (h.skillPoints or 0) > 0 do
        local spent = false
        for i = 1, 4 do
            if h.ranks[i] < RANK_MAX[i] and spendPoint(h, i) then spent = true end
        end
        if not spent then break end
    end
end

local function xpForLevel(l) return 100 + (l - 1) * 80 end

local function grantXp(u, amount)
    if u == nil or not u.isHero then return end
    u.xp = u.xp + amount
    while u.xp >= xpForLevel(u.level) and u.level < 18 do
        u.xp = u.xp - xpForLevel(u.level)
        u.level = u.level + 1
        u.skillPoints = (u.skillPoints or 0) + 1
        if u ~= playerHero then autoLevel(u) end
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
        if not h.dead then
            -- 被动金币攒够 10 再入账一次，玩家能看到 "+10 金币" 漂字而不是每帧刷屏。
            h.goldAccum = (h.goldAccum or 0) + 2.0 * dt
            if h.goldAccum >= 10 then
                local g = math.floor(h.goldAccum)
                h.goldAccum = h.goldAccum - g
                gainGold(h, g)
            end
        end
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
    h.bonus.mr = h.bonus.mr + (it.mr or 0)
    h.bonus.as = h.bonus.as + (it.as or 0)
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
    -- 塔/兵营被拆掉后放行该处的导航障碍，后续兵线可直推。
    if u.kind == "tower" then NavBlock(u.x, u.z, 2.0, true) end
    if u.kind == "inhibitor" then
        if NavBlock ~= nil then NavBlock(u.x, u.z, 1.6, true) end
        -- 兵营被破：对方基地开始出超级兵。
        inhibDown[u.team] = true
        sfx("tower", 0.3)
        killFeed[#killFeed + 1] = { killer = (source ~= nil and source.name) or "?",
            victim = u.name, col = TEAM_COLOR[3 - u.team], t = elapsed, notice = true }
        if #killFeed > 6 then table.remove(killFeed, 1) end
    end
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
                source.cs = source.cs + 1
                gainGold(source, g)
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
            source.streak = (source.streak or 0) + 1
            local bounty = 300 + math.min(source.streak - 1, 5) * 50
            grantXp(source, 150)
            gainGold(source, bounty)
        end
        u.streak = 0
        local kname = (source ~= nil and source.name) or "?"
        local kcol = TEAM_COLOR[(source ~= nil and source.team) or (3 - u.team)]
        killFeed[#killFeed + 1] = { killer = kname, victim = u.name, col = kcol, t = elapsed }
        if #killFeed > 6 then table.remove(killFeed, 1) end
    elseif u.kind == "neutral" then
        sfx("monster_die", 0.15)
        if u.cfg ~= nil then neutralRespawns[#neutralRespawns + 1] = { cfg = u.cfg, t = u.respawn or 120 } end
        if source ~= nil and source.isHero then
            local g = u.gold or 60
            grantXp(source, u.xpReward or 60)
            gainGold(source, g)
            source.buffTimers = source.buffTimers or {}
            local dur = 120
            if u.campBuff == "red" then
                applyStatusLua(source, "adbuff", 120, 1.15, source)
            elseif u.campBuff == "blue" then
                applyStatusLua(source, "msbuff", 120, 1.1, source)
            elseif u.campBuff == "dragon" then
                dur = 120
                applyStatusLua(source, "adbuff", dur, 1.08, source)
            elseif u.campBuff == "baron" then
                dur = 180
                applyStatusLua(source, "adbuff", dur, 1.2, source)
                applyStatusLua(source, "msbuff", dur, 1.2, source)
            else
                dur = 0
            end
            if dur > 0 then source.buffTimers[u.campBuff] = dur end
        end
        -- 大龙/小龙击杀播报
        if u.campKey == "dragon" or u.campKey == "baron" then
            killFeed[#killFeed + 1] = { killer = (source ~= nil and source.name) or "?",
                victim = u.name, col = { 1.0, 0.85, 0.35 }, t = elapsed, notice = true }
            if #killFeed > 6 then table.remove(killFeed, 1) end
        end
    end

    if u.kind == "nexus" and not gameOver then
        gameOver = true
        winner = source and source.team or (3 - u.team)
        sfx(winner == BLUE and "victory" or "defeat", 999)
    end
end

local function damage(target, amount, source, kind)
    if target == nil or target.dead or amount <= 0 then return end
    kind = kind or "physical"
    -- 抗性减伤（真实伤害除外）：护甲吃物理，魔抗吃魔法。def 100 => 减半。
    if kind ~= "true" then
        local def = (kind == "magic") and (target.mr or 0) or (target.armor or 0)
        amount = amount * (100.0 / (100.0 + math.max(0, def)))
    end
    local buf = target.buffs
    if buf and buf.shield and buf.shield.t > 0 and buf.shield.pool > 0 then
        local absorbed = math.min(buf.shield.pool, amount)
        buf.shield.pool = buf.shield.pool - absorbed
        amount = amount - absorbed
    end
    if amount > 0 then
        target.hp = target.hp - amount
        SetHealth(target.ent, math.max(0, target.hp))
        -- 伤害数字按类型上色：物理橙、魔法紫、真实白（大额加粗）。
        local cr, cg, cb
        if kind == "magic" then cr, cg, cb = 0.72, 0.55, 1.0
        elseif kind == "true" then cr, cg, cb = 1.0, 1.0, 1.0
        else cr, cg, cb = 1.0, 0.9, 0.35 end
        if amount >= 90 then cr, cg, cb = 1.0, 0.5, 0.15 end
        floatAt(target, tostring(math.floor(amount + 0.5)), amount >= 90, cr, cg, cb)
        if amount >= 40 then sfx("hit", 0.1) end
        if amount >= 25 then
            EmitParticles({ pos = { x = target.x, y = target.h * 0.6, z = target.z }, count = 5,
                vel = { x = 0, y = 1, z = 0 }, speedMin = 1, speedMax = 3, lifeMin = 0.15, lifeMax = 0.3,
                sizeStart = 0.3, sizeEnd = 0.02, color = { r = 1, g = 0.9, b = 0.5, a = 0.8 },
                colorEnd = { r = 1, g = 0.3, b = 0.1, a = 0 }, additive = true })
        end
        -- 回城被打断：读条中受到任意伤害立即取消
        if target.isHero and (target.channel or 0) > 0 then
            target.channel = 0
            floatAt(target, "回城被打断", false)
        end
        -- 轻重震屏：附近的大额伤害
        if amount >= 50 and playerHero ~= nil and
            dist(target.x, target.z, playerHero.x, playerHero.z) < 30 then
            camShake = math.min(1.2, camShake + amount * 0.004)
        end
        -- 防御塔仇恨：英雄攻击敌方英雄后，进入该方塔范围会被转火
        if source ~= nil and source.isHero and target.isHero and source.team ~= target.team then
            for i = 1, #units do
                local tw = units[i]
                if tw.kind == "tower" and tw.team == target.team and not tw.dead and
                    dist(tw.x, tw.z, source.x, source.z) <= tw.range then
                    tw.towerTarget = source
                    tw.towerAggroT = 3.0
                end
            end
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

-- 把不可走的点吸附到最近的可行走格：A* 在端点不可走时会直接返回空路径，
-- 而小兵的目标是敌方基地中心（被 NavBlock 标成障碍），不吸附就会退化成
-- 直线移动、正好撞进沿途防御塔卡住。
local function nearestWalkable(x, z)
    if type(NavWalkable) ~= "function" then return x, z end
    if NavWalkable(x, z) then return x, z end
    for r = 1, 14 do
        local n = 8 * r
        for k = 0, n - 1 do
            local a = k / n * math.pi * 2
            local nx, nz = x + math.cos(a) * r, z + math.sin(a) * r
            if NavWalkable(nx, nz) then return nx, nz end
        end
    end
    return x, z
end

-- 导航寻路：沿场景导航网格（level.navgrid）的 A* 路点移动，绕开地图障碍。
-- 无网格 / 无路时优雅回退直线（beeline），行为与旧版一致。
local function repath(u, tx, tz)
    local sx, sz = nearestWalkable(u.x, u.z)
    local gx, gz = nearestWalkable(tx, tz)
    local p = NavFindPath({ x = sx, y = 0, z = sz }, { x = gx, y = 0, z = gz })
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

-- 技能特效小工具：让不同技能有不同形状，而不是清一色的同一团粒子。
local function fxBurst(cx, cy, cz, count, sMin, sMax, lMin, lMax, sz0, sz1, col, colEnd, vy, grav)
    EmitParticles({ pos = { x = cx, y = cy, z = cz }, count = count,
        vel = { x = 0, y = vy or 1, z = 0 }, speedMin = sMin, speedMax = sMax,
        lifeMin = lMin, lifeMax = lMax, sizeStart = sz0, sizeEnd = sz1,
        color = col, colorEnd = colEnd, gravity = grav, additive = true })
end

-- 沿世界圆喷一圈（地面预警/光环），dir=false 时粒子只向上飘。
local function fxRing(cx, cz, r, count, col, colEnd, y, sMin, sMax, lMin, lMax, sz0, sz1, vy)
    for k = 0, count - 1 do
        local a = k / count * math.pi * 2
        EmitParticles({ pos = { x = cx + math.cos(a) * r, y = y or 0.1, z = cz + math.sin(a) * r },
            count = 1, vel = { x = 0, y = vy or 1, z = 0 }, speedMin = sMin, speedMax = sMax,
            lifeMin = lMin, lifeMax = lMax, sizeStart = sz0, sizeEnd = sz1,
            color = col, colorEnd = colEnd, additive = true })
    end
end

-- 沿朝向的扇形挥砍（近战弧线），沿弧线布点形成月牙。
local function fxSlash(h, range, arc, col)
    local fy = h.yaw or 0
    for k = 0, 16 do
        local a = -arc * 0.5 + arc * k / 16
        local ax, az = math.sin(fy + a), math.cos(fy + a)
        EmitParticles({ pos = { x = h.x + ax * range * 0.65, y = 0.95, z = h.z + az * range * 0.65 },
            count = 2, vel = { x = ax, y = 0.35, z = az }, speedMin = 3.5, speedMax = 8,
            lifeMin = 0.12, lifeMax = 0.26, sizeStart = 0.5, sizeEnd = 0.02,
            color = { r = 1, g = 1, b = 1, a = 0.85 },
            colorEnd = { r = col[1], g = col[2], b = col[3], a = 0 }, additive = true })
    end
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
    -- 野怪增益计时（HUD 显示剩余时间）
    if u.buffTimers ~= nil then
        for k, v in pairs(u.buffTimers) do
            v = v - dt
            if v <= 0 then u.buffTimers[k] = nil else u.buffTimers[k] = v end
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
        target = opts.target, dmgKind = opts.dmgKind or "magic",
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

local function aoeDamage(source, cx, cz, radius, dmg, status, dur, mag, stun, kind)
    for i = 1, #units do
        local o = units[i]
        if not o.dead and o.team ~= source.team and dist(o.x, o.z, cx, cz) <= radius + o.radius then
            damage(o, dmg, source, kind)
            if status then applyStatusLua(o, status, dur, mag, source) end
            if stun and stun > 0 then applyStatusLua(o, "stun", stun, 0, source) end
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
    local rank = (h.ranks and h.ranks[idx]) or 0
    -- 失败反馈只对玩家显示：AI 每帧都会尝试施法，否则漂字会堆满敌方头顶。
    local me = (h == playerHero)
    if rank <= 0 then if me then floatAt(h, "未学习技能", false) end; return end
    if (h.cds[idx] or 0) > 0 then if me then floatAt(h, "冷却中", false) end; return end
    local cost = ab.cost or 0
    if h.mana < cost then if me then floatAt(h, "法力不足", false) end; return end
    h.mana = h.mana - cost
    h.cds[idx] = (ab.cd or 6) * (1 - 0.05 * (rank - 1))
    playSpell(h, idx)
    if h.isHero then sfx("cast", 0.08) end

    local dx, dz = aimX - h.x, aimZ - h.z
    dx, dz = norm(dx, dz)
    if dx == 0 and dz == 0 then
        local fy = h.yaw or 0
        dx, dz = math.sin(fy), math.cos(fy)
    end
    local t = ab.type
    local col = TEAM_COLOR[h.team]
    -- 伤害类型：近战/处决为物理，投射物/AoE 为魔法（吃对应抗性）。
    local kind = (t == "melee" or t == "execute") and "physical" or "magic"
    -- 法强加成：魔法技能默认 0.4 系数，让 AP 装备真正生效。
    local apRatio = ab.apRatio or ((kind == "magic") and 0.4 or 0.0)
    local dmg = (ab.dmg or 0) * (0.6 + 0.4 * rank) + (h.ap or 0) * apRatio

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
                dmgKind = kind,
            })
        end
        -- 枪口能量环（区别于普攻）
        fxRing(h.x + dx * 0.7, h.z + dz * 0.7, 0.35, 10,
            { r = col[1], g = col[2], b = col[3], a = 0.9 }, { r = 1, g = 1, b = 1, a = 0 },
            1.0, 2, 5, 0.1, 0.2, 0.4, 0.05, 0.2)
    elseif t == "aoe_self" then
        local ticks = ab.ticks or 1
        for _ = 1, ticks do
            aoeDamage(h, h.x + dx * 0.8, h.z + dz * 0.8, ab.radius or 3, dmg / ticks,
                ab.status, ab.statusDur, ab.statusMag, ab.stun, kind)
        end
        fxRing(h.x, h.z, ab.radius or 3, 24, { r = col[1], g = col[2], b = col[3], a = 0.9 },
            { r = 1, g = 1, b = 1, a = 0 }, 0.1, 1.5, 4, 0.3, 0.6, 0.6, 0.05, 0.6)
    elseif t == "aoe_target" then
        local tl = math.sqrt((aimX - h.x) ^ 2 + (aimZ - h.z) ^ 2)
        local r = ab.range or 10
        if tl > r then aimX = h.x + dx * r; aimZ = h.z + dz * r end
        groundAoes[#groundAoes + 1] = {
            team = h.team, owner = h, x = aimX, z = aimZ,
            delay = ab.delay or 0.4, radius = ab.radius or 3, dmg = dmg,
            status = ab.status, statusDur = ab.statusDur, statusMag = ab.statusMag,
            stun = ab.stun, kind = kind,
        }
        -- 落点预警圈（cast 时一次性粒子 + on_render 脉冲地面圈，见 drawGroundAoes）
        local cr = ab.radius or 3
        fxRing(aimX, aimZ, cr, 20, { r = col[1], g = col[2], b = col[3], a = 0.95 },
            { r = 1, g = 0.4, b = 0.2, a = 0 }, 0.1, 0.1, 0.4,
            (ab.delay or 0.4) * 0.8, (ab.delay or 0.4) * 1.1, 0.5, 0.15, 1.2)
    elseif t == "dash" then
        local d = ab.dist or 4
        local steps = 8
        for _ = 1, steps do
            local nx = h.x + dx * d / steps
            local nz = h.z + dz * d / steps
            if type(NavWalkable) ~= "function" or NavWalkable(nx, nz) then
                h.x, h.z = nx, nz
            end
            -- 残影
            fxBurst(h.x, 1.0, h.z, 4, 0.2, 0.8, 0.2, 0.4, 0.5, 0.05,
                { r = col[1], g = col[2], b = col[3], a = 0.7 }, { r = 1, g = 1, b = 1, a = 0 }, 0.5)
            if ab.radius and ab.radius > 0 then
                for j = 1, #units do
                    local o = units[j]
                    if not o.dead and o.team ~= h.team and dist(o.x, o.z, h.x, h.z) <= ab.radius + o.radius then
                        damage(o, dmg / steps, h, kind)
                    end
                end
            end
        end
        SetPosition(h.ent, { x = h.x, y = 0, z = h.z })
    elseif t == "melee" then
        local r = ab.range or 2.5
        local arc = math.rad(ab.arc or 120)
        -- 朝施法点转身（原来只用 h.yaw，鼠标指向侧面/背后会打空）。
        if aimX ~= nil and aimZ ~= nil and (math.abs(aimX - h.x) + math.abs(aimZ - h.z)) > 0.05 then
            faceTo(h, aimX, aimZ)
        end
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
                        damage(o, dmg, h, kind)
                        if ab.status then applyStatusLua(o, ab.status, ab.statusDur or 2, ab.statusMag or 0.3, h) end
                        if ab.stun then applyStatusLua(o, "stun", ab.stun, 0, h) end
                    end
                end
            end
        end
        fxSlash(h, r, arc, col)
    elseif t == "buff" then
        if ab.shield and ab.shield > 0 then applyStatusLua(h, "shield", ab.duration or 3, ab.shield, h) end
        if ab.msMul then applyStatusLua(h, "msbuff", ab.duration or 4, ab.msMul, h) end
        if ab.adMul then applyStatusLua(h, "adbuff", ab.duration or 5, ab.adMul, h) end
        -- 双环上浮光环（区别于伤害类）
        fxRing(h.x, h.z, 0.7, 20, { r = col[1], g = col[2], b = col[3], a = 0.9 },
            { r = 1, g = 1, b = 1, a = 0 }, 0.1, 0.1, 0.4, 0.5, 0.9, 0.5, 0.05, 1.6)
        fxRing(h.x, h.z, 1.3, 24, { r = 1, g = 1, b = 1, a = 0.7 },
            { r = col[1], g = col[2], b = col[3], a = 0 }, 0.1, 0.1, 0.4, 0.5, 0.9, 0.4, 0.05, 1.4)
        fxBurst(h.x, 0.6, h.z, 18, 0.4, 1.6, 0.4, 0.8, 0.55, 0.05,
            { r = col[1], g = col[2], b = col[3], a = 0.9 }, { r = 1, g = 1, b = 1, a = 0 }, 1.6)
    elseif t == "heal" then
        h.hp = math.min(h.maxHp, h.hp + (ab.amount or 100))
        SetHealth(h.ent, h.hp)
        floatAt(h, "+" .. tostring(ab.amount or 100), false, 0.4, 1.0, 0.5)
        fxBurst(h.x, 0.3, h.z, 20, 0.3, 1.2, 0.4, 0.8, 0.45, 0.05,
            { r = 0.4, g = 1.0, b = 0.5, a = 0.9 }, { r = 0.8, g = 1, b = 1, a = 0 }, 2.2, -0.6)
    elseif t == "execute" then
        -- 处决优先锁英雄（原来会砸到最近的小兵）。
        local rng = ab.range or 3
        local tgt, best = nil, nil
        for j = 1, #units do
            local o = units[j]
            if not o.dead and o.team ~= h.team and dist(o.x, o.z, h.x, h.z) <= rng + o.radius then
                local score = dist(o.x, o.z, h.x, h.z) - (o.isHero and 1000 or 0)
                if best == nil or score < best then best = score; tgt = o end
            end
        end
        if tgt == nil then floatAt(h, "无目标", false); return end
        local missing = 1.0 - tgt.hp / tgt.maxHp
        local total = dmg + (tgt.maxHp * (ab.missingPct or 0.3) * missing)
        damage(tgt, total, h, kind)
        camShake = math.min(1.5, camShake + 0.7)
        fxBurst(tgt.x, 1.0, tgt.z, 30, 3, 9, 0.25, 0.5, 0.8, 0.05,
            { r = 1, g = 0.85, b = 0.3, a = 1 }, { r = 1, g = 0.2, b = 0.1, a = 0 }, 0.5)
        fxRing(tgt.x, tgt.z, 1.0, 24, { r = 1, g = 0.9, b = 0.5, a = 0.9 },
            { r = 1, g = 0.3, b = 0, a = 0 }, 0.15, 4, 9, 0.25, 0.5, 0.7, 0.05, 0.5)
        SpawnFloatText({ x = tgt.x, y = tgt.h + 0.6, z = tgt.z }, "处决!", true, 1.0, 1, 0.6, 0.2)
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
    if u.kind == "minion" then u.lungeT = 0.18 end -- 静态模型：攻击前冲作为出手反馈
    if u.isHero then sfx("attack", 0.08) elseif u.kind == "tower" then sfx("tower", 0.1) end
    faceTo(u, tgt.x, tgt.z)
    if u.isHero then
        -- 普攻前摇：起手后 ~0.15/0.22s 才结算，手感更有重量
        u.swing = u.ranged and 0.22 or 0.15
        u.swingTarget = tgt
    elseif u.ranged or u.kind == "tower" then
        local dx, dz = norm(tgt.x - u.x, tgt.z - u.z)
        spawnProjectile(u, u.x, u.z, dx, dz, { speed = 30, dmg = u.ad * adMul(u), life = 2.5,
            radius = 0.8, trail = false, target = tgt, dmgKind = "physical" })
    else
        damage(tgt, u.ad * adMul(u), u)
    end
end

-- 普攻前摇结算（英雄）：到点后在射程内出手
local function updateSwing(u, dt)
    if u.swing == nil then return end
    u.swing = u.swing - dt
    if u.swing > 0 then return end
    local tgt = u.swingTarget
    u.swing = nil
    u.swingTarget = nil
    if u.dead or tgt == nil or tgt.dead then return end
    if dist(u.x, u.z, tgt.x, tgt.z) > u.range + tgt.radius + 1.0 then return end
    faceTo(u, tgt.x, tgt.z)
    if u.ranged then
        local dx, dz = norm(tgt.x - u.x, tgt.z - u.z)
        spawnProjectile(u, u.x, u.z, dx, dz, { speed = 30, dmg = u.ad * adMul(u), life = 2.5,
            radius = 0.8, trail = false, target = tgt, dmgKind = "physical" })
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

-- 防御塔：小兵优先；有英雄攻击了我方英雄则短暂转火该英雄。
local function updateTower(u, dt)
    if u.dead then return end
    if (u.towerAggroT or 0) > 0 then
        u.towerAggroT = u.towerAggroT - dt
        local t = u.towerTarget
        if t == nil or t.dead or dist(u.x, u.z, t.x, t.z) > u.range then
            u.towerTarget = nil
            u.towerAggroT = 0
        end
    end
    if u.towerTarget ~= nil then
        u.target = u.towerTarget
    else
        u.target = nearestEnemy(u, u.range, true)
    end
    autoAttack(u, dt)
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

-- 施法/攻击的地面 telegraph 现在统一走 on_render 的透视正确多边形（见
-- drawSkillOverlay + groundRing/groundLine），不再用屏幕空间 DrawCircle 或
-- 粒子圈：屏幕圆会随透视“越走越大”，粒子圈则不够清晰。

-- ==========================================================================
-- 联机（A）：服务器权威消费指令，客户端发指令
-- ==========================================================================
local function netRole()
    local r = GetVar("netRole")
    if type(r) == "string" then return r end
    return nil
end

local function netIndexUnit(id)
    for i = 1, #units do
        if units[i].id == id then return units[i] end
    end
    return nil
end

-- 联机：客户端分配（服务器在每个客户端加入时调用 on_player_join）
local netJoins = {}
local function assignNetClients()
    if playerHero ~= nil and playerHero.netClient == nil and netJoins[1] ~= nil then
        playerHero.netClient = netJoins[1]
    end
    if enemyHero ~= nil and enemyHero.netClient == nil and netJoins[2] ~= nil then
        enemyHero.netClient = netJoins[2]
    end
end

function on_player_join(clientId)
    netJoins[#netJoins + 1] = clientId
    assignNetClients()
end

-- 服务器：把客户端发来的指令应用到玩家英雄。客户端输入不可信，字段必须校验
-- （畸形 JSON 曾能让 moveTarget 变成 {x=nil,z=nil} 从而崩掉权威服务器）。
local function isNum(v) return type(v) == "number" and v == v and v > -1e9 and v < 1e9 end
local function validPoint(x, z)
    return isNum(x) and isNum(z) and math.abs(x) <= 120 and math.abs(z) <= 120
end
local function validAbility(i)
    return isNum(i) and i >= 1 and i <= 4
end

local function applyNetCommand(cmd)
    -- 指令按 clientId 路由到对应英雄（由 on_player_join 分配；无人操控则回退）
    local h = nil
    for i = 1, #heroes do
        if heroes[i].netClient == cmd.client then h = heroes[i]; break end
    end
    if h == nil then h = playerHero end
    if h == nil or h.dead then return end
    if cmd.name ~= "moba_cmd" then return end
    local a = nil
    if type(cmd.args) == "string" and cmd.args ~= "" then a = Json.Parse(cmd.args) end
    if type(a) ~= "table" then return end
    local t = a.type
    if t == "move" or t == "attackMove" then
        if not validPoint(a.x, a.z) then return end
        h.target = nil; h.commandTarget = false
        h.moveTarget = { x = a.x, z = a.z }
        h.attackMove = (t == "attackMove") and true or nil
        h.navPath = nil
    elseif t == "attackTarget" then
        if not isNum(a.id) then return end
        local u = netIndexUnit(a.id)
        if u ~= nil and not u.dead and u.team ~= h.team then
            h.target = u; h.commandTarget = true; h.attackMove = nil
        end
    elseif t == "cast" then
        if not validAbility(a.ability) then return end
        local x, z = h.x, h.z
        if validPoint(a.x, a.z) then x, z = a.x, a.z end
        castAbility(h, math.floor(a.ability), x, z)
    elseif t == "recall" then
        h.channel = 1.4
    elseif t == "levelup" then
        if not validAbility(a.ability) then return end
        spendPoint(h, math.floor(a.ability))
    end
end

-- 客户端：把服务器广播的 moba_state 应用到本地英雄（供 HUD 显示）
local function applyNetState(argsJson)
    local s = nil
    if type(argsJson) == "string" and argsJson ~= "" then s = Json.Parse(argsJson) end
    if s == nil or s.heroes == nil then return end
    local myId = GetVar("myClientId")
    for i = 1, #s.heroes do
        local e = s.heroes[i]
        local h = (e.team == BLUE) and playerHero or enemyHero
        if h ~= nil and not h.dead then
            -- 位置：服务器权威坐标，客户端插值到本地（moba_state ~10Hz）。
            if e.x ~= nil then h.nx = e.x; h.nz = e.z; h.nyaw = e.yaw or 0 end
        end
    end
end

-- 客户端：HUD 数值（低频小消息，仅本地英雄需要）
local function applyNetHud(a)
    if a == nil then return end
    local myId = GetVar("myClientId")
    if myId == nil or a.client ~= myId then return end
    local h = (a.team == BLUE) and playerHero or enemyHero
    if h == nil or h.dead then return end
    h.hp = a.hp or h.hp
    h.maxHp = a.maxHp or h.maxHp
    h.mana = a.mana or h.mana
    h.level = a.level or h.level
    h.gold = a.gold or h.gold
    h.cs = a.cs or h.cs
    h.kills = a.kills or h.kills
    h.deaths = a.deaths or h.deaths
    if a.cd1 ~= nil then h.cds = { a.cd1, a.cd2, a.cd3, a.cd4 } end
    if a.r1 ~= nil then h.ranks = { a.r1, a.r2, a.r3, a.r4 } end
end

-- 客户端大厅事件
local function lobbyHandle(name, argsJson)
    local a = nil
    if type(argsJson) == "string" and argsJson ~= "" then a = Json.Parse(argsJson) end
    if name == "room.joined" then
        if a ~= nil then
            if a.ok == false then
                LOBBY.msg = a.error or "加入失败"
            else
                LOBBY.room = a.room or LOBBY.room
                LOBBY.players = a.players or 1
                LOBBY.max = a.max or 2
                LOBBY.host = a.host or LOBBY.host
                LOBBY.msg = ""
            end
        end
    elseif name == "lobby" then
        if a ~= nil and LOBBY.room ~= "" and a.room == LOBBY.room then
            LOBBY.players = a.players or LOBBY.players
            LOBBY.max = a.max or LOBBY.max
            LOBBY.host = a.host or LOBBY.host
        end
    elseif name == "room.list" then
        LOBBY.rooms = (a and a.rooms) or {}
    elseif name == "match.start" then
        LOBBY.side = (a and a.side) or "blue"
        LOBBY.phase = "match"
        if playerHero == nil and #SELECT > 0 then chooseChampion(SELECT[1]) end
        phase = "play"
    elseif name == "net.reset" then
        -- Reconnected to the server: our old room membership is gone.
        LOBBY.phase = "lobby"
        LOBBY.room = ""
        LOBBY.players = 0
        LOBBY.host = 0
        LOBBY.rooms = {}
        LOBBY.msg = "已重新连接服务器"
        phase = "play"
    end
end

-- 客户端大厅键盘：房号输入（0-9A-Z / Backspace / Enter 加入）
local function lobbyKeyInput()
    local letters = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    for i = 1, #letters do
        local k = letters:sub(i, i)
        local down = InputKey(k) == 1
        if down and not LOBBY.keysPrev[k] and #LOBBY.code < 6 then
            LOBBY.code = LOBBY.code .. k
        end
        LOBBY.keysPrev[k] = down
    end
    local bs = InputKey("Backspace") == 1
    if bs and not LOBBY.keysPrev["Backspace"] then
        LOBBY.code = LOBBY.code:sub(1, math.max(0, #LOBBY.code - 1))
    end
    LOBBY.keysPrev["Backspace"] = bs
    local en = (InputKey("Return") == 1) or (InputKey("Enter") == 1)
    if en and not LOBBY.keysPrev["Return"] and #LOBBY.code > 0 then
        Rpc("room.join", { room = LOBBY.code })
        LOBBY.code = ""
    end
    LOBBY.keysPrev["Return"] = en
end

-- 客户端：读本地输入 -> 发 moba_cmd，不改本地状态（位置由服务器快照驱动）
local function clientSendCommands(h)
    -- 先排空服务器状态广播
    local cmd = NetCommand()
    while cmd ~= nil do
        if cmd.name == "moba_state" then applyNetState(cmd.args)
        elseif cmd.name == "moba_hud" then applyNetHud(Json.Parse(cmd.args)) end
        cmd = NetCommand()
    end
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    local m = InputMousePos()
    local inView = m ~= nil and m.x >= 0 and m.y >= 0 and m.x <= vw and m.y <= vh
    if ActionPressed("attack") then h.attackArmed = true end
    local left = InputMousePressed("left")
    local right = InputMousePressed("right")
    if inView and (left or right) then
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
        if best ~= nil then
            Rpc("moba_cmd", { type = "attackTarget", id = best.id })
        elseif g ~= nil then
            Rpc("moba_cmd", { type = (left and h.attackArmed) and "attackMove" or "move",
                              x = g.x, z = g.z })
        end
        if left then h.attackArmed = nil end
    end
    local g = inView and groundPick(m.x, m.y) or nil
    local fy = h.yaw or 0
    local aimX = g and g.x or (h.x + math.sin(fy) * 6)
    local aimZ = g and g.z or (h.z + math.cos(fy) * 6)
    local ctrl = InputKey("ctrl") == 1
    h.spellArmed = h.spellArmed or {}
    for i = 1, 4 do
        if ActionPressed("spell" .. i) then
            if ctrl then
                h.spellArmed[i] = false
                Rpc("moba_cmd", { type = "levelup", ability = i })
            else
                h.spellArmed[i] = true
            end
        elseif ActionReleased("spell" .. i) then
            if h.spellArmed[i] then
                Rpc("moba_cmd", { type = "cast", ability = i, x = aimX, z = aimZ })
            end
            h.spellArmed[i] = nil
        end
    end
    if ActionPressed("recall") then Rpc("moba_cmd", { type = "recall" }) end
    -- 调试驱动：定期发一条移动指令，验证 客户端->服务器->快照 链路。
    if GetVar("mobaAutostart") == 1 then
        if (LOBBY.autoMoveT or 0) <= elapsed then
            LOBBY.autoMoveT = elapsed + 1.5
            Rpc("moba_cmd", { type = "move", x = h.x + 25, z = h.z + 25 })
        end
    end
end

local function updatePlayer(dt)
    local h = playerHero
    if h == nil or h.dead then return end
    local role = netRole()
    if role == "server" then
        local cmd = NetCommand()
        while cmd ~= nil do
            applyNetCommand(cmd)
            cmd = NetCommand()
        end
        updateHeroControl(h, dt)
        return
    elseif role == "client" then
        -- 服务器权威坐标 -> 本地插值（moba_state ~10Hz），驱动模型/相机/小地图。
        local function lerpNet(u)
            if u == nil or u.dead or u.nx == nil then return end
            local k = math.min(1, dt * 14)
            u.x = u.x + (u.nx - u.x) * k
            u.z = u.z + (u.nz - u.z) * k
            u.yaw = u.nyaw or u.yaw
            if u.ent ~= nil then SetPosition(u.ent, { x = u.x, y = 0, z = u.z }) end
        end
        lerpNet(playerHero)
        lerpNet(enemyHero)
        clientSendCommands(h)
        return
    end
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
    -- Ctrl+QWER 加点；否则按住显示施法指示器、松开在该处释放。
    -- 用 spellArmed 记录“这次按下是施法还是加点”，避免 Ctrl+Q 松开时误放技能。
    local ctrl = InputKey("ctrl") == 1
    h.spellArmed = h.spellArmed or {}
    for i = 1, 4 do
        if ActionPressed("spell" .. i) then
            if ctrl then
                h.spellArmed[i] = false
                if spendPoint(h, i) then
                    floatAt(h, "技能升级", false)
                    sfx("levelup", 0.2)
                end
            else
                h.spellArmed[i] = true
            end
        elseif ActionReleased("spell" .. i) then
            if h.spellArmed[i] then castAbility(h, i, aimX, aimZ) end
            h.spellArmed[i] = nil
        end
    end
    -- 按住 A：显示攻击范围圈（on_render 的世界空间地面圈，见 drawSkillOverlay）。
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
        -- 只施放已加点的技能（ranks[i] > 0），否则会走 castAbility 的失败分支。
        if ab and (h.ranks[i] or 0) > 0 and (h.cds[i] or 0) <= 0 and h.mana >= (ab.cost or 0) then
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
        -- 追踪弹（普攻）：锁定目标后每步转向，移动目标不会“擦肩而过”。
        if p.target ~= nil then
            if p.target.dead then
                p.target = nil
            else
                p.dx, p.dz = norm(p.target.x - p.x, p.target.z - p.z)
            end
        end
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
                damage(o, p.dmg, p.owner, p.dmgKind)
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
            aoeDamage(a.owner, a.x, a.z, a.radius, a.dmg, a.status, a.statusDur, a.statusMag, a.stun, a.kind)
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
    -- 复活时清掉可能残留的命令/前摇状态，否则 attackMove 等会让新一命失去控制。
    u.attackMove = nil
    u.commandTarget = false
    u.attackArmed = nil
    u.pingArmed = nil
    u.swing = nil
    u.swingTarget = nil
    u.channel = 0
    u.spellArmed = nil
    u.dying = 0
    u.navCd = 0
    local b = MAP.spawn[u.team]
    u.x, u.z = b.x, b.z
    SetPosition(u.ent, { x = u.x, y = 0, z = u.z })
    SetHealth(u.ent, u.hp)
    SetRotationY(u.ent, (u.team == BLUE) and 0 or math.pi)
    u.navPath = nil
    u.navGoal = nil
end

-- 小兵模型是静态网格（无骨骼动画）：用程序化起伏/摆动/攻击前冲让它“动起来”。
local function minionWalk(u, dt)
    u.bobT = (u.bobT or 0) + dt
    local attacking = (u.actionT or 0) > 0
    local bob, sway
    if attacking then
        bob = 0.04 + math.sin(u.bobT * 3.0) * 0.03
        sway = math.sin(u.bobT * 3.0) * 0.05
    else
        bob = math.abs(math.sin(u.bobT * 9.0)) * 0.16 + 0.02
        sway = math.sin(u.bobT * 9.0) * 0.11
    end
    local px, pz = u.x, u.z
    if (u.lungeT or 0) > 0 then
        u.lungeT = u.lungeT - dt
        local f = math.sin((1 - math.max(0, u.lungeT) / 0.18) * math.pi) * 0.3
        px = px + math.sin(u.yaw or 0) * f
        pz = pz + math.cos(u.yaw or 0) * f
    end
    return px, bob, pz, (u.yaw or 0) + sway
end

-- 单位分离：避免英雄/小兵/野怪互相重叠（结构体不动，只推开单位）。
local function separateUnits(dt)
    local n = #units
    for i = 1, n do
        local a = units[i]
        if not a.dead then
            for j = i + 1, n do
                local b = units[j]
                if not b.dead then
                    local aStatic = (a.kind == "tower" or a.kind == "nexus" or a.kind == "inhibitor")
                    local bStatic = (b.kind == "tower" or b.kind == "nexus" or b.kind == "inhibitor")
                    if not (aStatic and bStatic) then
                        local dx, dz = b.x - a.x, b.z - a.z
                        local d2 = dx * dx + dz * dz
                        local rr = a.radius + b.radius
                        if d2 < rr * rr and d2 > 1e-6 then
                            local d = math.sqrt(d2)
                            local push = (rr - d) * 0.5
                            local nx, nz = dx / d, dz / d
                            if not aStatic then a.x = a.x - nx * push; a.z = a.z - nz * push end
                            if not bStatic then b.x = b.x + nx * push; b.z = b.z + nz * push end
                        end
                    end
                end
            end
        end
    end
    for i = 1, n do
        local u = units[i]
        if not u.dead and u.ent ~= nil then
            local px, py, pz, yaw = u.x, 0, u.z, u.yaw or 0
            if u.kind == "minion" then
                px, py, pz, yaw = minionWalk(u, dt)
            end
            SetPosition(u.ent, { x = px, y = py, z = pz })
            SetRotationY(u.ent, yaw)
        end
    end
end

local function cleanupUnits(dt)
    local i = 1
    while i <= #units do
        local u = units[i]
        if u.dead then
            u.dying = u.dying - dt
            -- 小兵无死亡动画：压扁下沉作为死亡表现。
            if u.kind == "minion" and u.ent ~= nil and u.dying > 0 then
                local f = clamp(u.dying / 0.7, 0, 1)
                SetScale(u.ent, 0.01, 0.01 * f * f, 0.01)
            end
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
    -- 对方兵营被破 -> 我方每波额外出一只超级兵。
    for _, team in ipairs({ BLUE, RED }) do
        if inhibDown[3 - team] then
            local p = MAP.spawn[team]
            spawnMinion("super", team, p.x, p.z)
        end
    end
end

-- 野怪刷新：击杀后按 respawn 时间重新出现（红/蓝/河蟹 120s，小龙 300s，大龙 360s）。
local function updateJungle(dt)
    local i = 1
    while i <= #neutralRespawns do
        local r = neutralRespawns[i]
        r.t = r.t - dt
        if r.t <= 0 then
            spawnNeutral(r.cfg)
            table.remove(neutralRespawns, i)
        else
            i = i + 1
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
    if camShake > 0 then camShake = math.max(0, camShake - dt * 2.5) end
    applyCamera()
    -- 独立播放器(neon_game)：用脚本相机模式复刻 MOBA 视角（编辑器走场景相机实体）。
    SetVar("cameraMode", "script")
    SetVar("cameraYaw", CAM.yaw)
    SetVar("cameraPitch", CAM.pitch)
    SetVar("cameraDist", CAM.dist)
    SetVar("cameraFocus", { x = camFocusX, y = 0, z = camFocusZ })
end

-- 战争迷雾：粗网格 + 导航网格视线遮挡。fogSeen=探索过，fogVis=当前可见。
local FOG_CELL = 8
local FOG_MIN = -96
local FOG_COLS = math.floor((96 - FOG_MIN) / FOG_CELL) + 1
local fogSeen, fogVis = {}, {}
for i = 1, FOG_COLS * FOG_COLS do fogSeen[i] = 0; fogVis[i] = 0 end
local VISION_R = { champion = 16, minion = 11, tower = 22, nexus = 22 }

local function fogIndex(x, z)
    local cx = math.floor((x - FOG_MIN) / FOG_CELL)
    local cz = math.floor((z - FOG_MIN) / FOG_CELL)
    if cx < 0 or cz < 0 or cx >= FOG_COLS or cz >= FOG_COLS then return nil end
    return cz * FOG_COLS + cx + 1
end

-- 视线：两点之间沿途采样是否都可行走（墙挡视野）
local function lineClear(x0, z0, x1, z1)
    if type(NavWalkable) ~= "function" then return true end -- 无导航网格时不遮挡视野
    local d = dist(x0, z0, x1, z1)
    local steps = math.max(1, math.floor(d / (FOG_CELL * 0.5)))
    for i = 1, steps - 1 do
        local t = i / steps
        if not NavWalkable(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t) then return false end
    end
    return true
end

local function addVision(x, z, r)
    local c0 = math.floor((x - r - FOG_MIN) / FOG_CELL)
    local c1 = math.floor((x + r - FOG_MIN) / FOG_CELL)
    local r0 = math.floor((z - r - FOG_MIN) / FOG_CELL)
    local r1 = math.floor((z + r - FOG_MIN) / FOG_CELL)
    for cz = r0, r1 do
        for cx = c0, c1 do
            if cx >= 0 and cz >= 0 and cx < FOG_COLS and cz < FOG_COLS then
                local idx = cz * FOG_COLS + cx + 1
                local wx = FOG_MIN + (cx + 0.5) * FOG_CELL
                local wz = FOG_MIN + (cz + 0.5) * FOG_CELL
                if fogVis[idx] ~= 1 and dist(x, z, wx, wz) <= r and lineClear(x, z, wx, wz) then
                    fogVis[idx] = 1
                    fogSeen[idx] = 1
                end
            end
        end
    end
end

local function inBrush(x, z)
    for _, b in ipairs(BRUSHES) do
        if dist2(x, z, b.x, b.z) <= b.r * b.r then return true end
    end
    return false
end

local function updateVision()
    for i = 1, #fogVis do fogVis[i] = 0 end
    -- 蓝方（观察者）单位提供视野
    for i = 1, #units do
        local u = units[i]
        if not u.dead and u.team == BLUE then
            addVision(u.x, u.z, VISION_R[u.kind] or 10)
        end
    end
    for i = 1, #units do
        local u = units[i]
        if not u.dead and u.team == RED then
            local idx = fogIndex(u.x, u.z)
            local hidden = (idx == nil) or (fogVis[idx] ~= 1)
            -- 草丛内的敌人：除非蓝方贴脸（4.5），否则隐藏
            if not hidden and inBrush(u.x, u.z) then
                local near = false
                for j = 1, #units do
                    local o = units[j]
                    if not o.dead and o.team == BLUE and dist(o.x, o.z, u.x, u.z) < 4.5 then
                        near = true; break
                    end
                end
                if not near then hidden = true end
            end
            u.hidden = hidden
            if u.ent ~= nil then SetVisible(u.ent, not hidden) end
        elseif u.ent ~= nil and not u.dead then
            SetVisible(u.ent, true)
        end
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
            if u.isHero then
                local k = fmtKey(u.ent)
                plateMana[k] = (u.maxMana > 0) and (u.mana / u.maxMana) or 0
                plateLevel[k] = u.level or 1
            end
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
        for _, b in ipairs(MAP.inhibitors[team]) do
            spawnStructure("inhibitor", team, b.x, b.z)
        end
    end
    -- 塔/水晶/兵营是脚本生成的结构体（不在 sr_map.glb 里），单独写进导航网格当障碍。
    for _, team in ipairs({ BLUE, RED }) do
        NavBlock(MAP.base[team].x, MAP.base[team].z, 3.0, false)
        for _, t in ipairs(MAP.towers[team]) do
            NavBlock(t.x, t.z, 2.0, false)
        end
        for _, b in ipairs(MAP.inhibitors[team]) do
            NavBlock(b.x, b.z, 1.6, false)
        end
    end
    waveTimer = FIRST_WAVE
end

function chooseChampion(name)
    playerHero = spawnHero(name, BLUE)
    local pick = AI_CHAMP
    if pick == name then pick = (name == "Ashe") and "Garen" or "Ashe" end
    enemyHero = spawnHero(pick, RED)
    if enemyHero ~= nil then autoLevel(enemyHero) end
    assignNetClients()
    spawnJungle()
    phase = "loading"
    loadingTime = 0
end

-- 服务器：大厅开始一局（blue 客户端；red 客户端，0=AI）
function on_match_start(blueClientId, redClientId)
    netJoins = {}
    if blueClientId ~= nil and blueClientId > 0 then netJoins[1] = blueClientId end
    if redClientId ~= nil and redClientId > 0 then netJoins[2] = redClientId end
    if playerHero == nil then chooseChampion(SELECT[1] or AI_CHAMP) end
    if playerHero ~= nil and netJoins[1] ~= nil then playerHero.netClient = netJoins[1] end
    if enemyHero ~= nil then
        enemyHero.netClient = netJoins[2] or nil
    end
    phase = "play"
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
    if netRole() == "server" then return end -- 服务器等大厅开局，不自动选人
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
    -- 联机客户端：先处理大厅（等待对手），未开局不进入玩法
    if netRole() == "client" then
        local cmd = NetCommand()
        while cmd ~= nil do
        if cmd.name == "moba_state" then applyNetState(cmd.args)
        elseif cmd.name == "moba_hud" then applyNetHud(Json.Parse(cmd.args))
            else lobbyHandle(cmd.name, cmd.args) end
            cmd = NetCommand()
        end
        if LOBBY.phase ~= "match" then
            -- 大厅自带整屏 UI：隐藏引擎的调试/状态叠加层，避免文字重叠。
            SetVar("debugOverlay", 0)
            -- 未连上服务器时不要发请求（Rpc 会被丢弃），显示“连接中”。
            if GetVar("netConnected") ~= 1 then
                updateCameraFollow(dt)
                return
            end
            LOBBY.refreshT = LOBBY.refreshT - dt
            if LOBBY.refreshT <= 0 then LOBBY.refreshT = 1.0; Rpc("room.list") end
            lobbyKeyInput()
            -- Mouse clicks are edge-triggered and the edge is cleared by
            -- IInput::EndTick at the END of every tick, so it is already dead
            -- inside on_render (where the lobby is drawn). Latch it here and let
            -- drawLobby consume it this frame instead of polling in the renderer.
            if InputMousePressed("left") then
                local mp = InputMousePos()
                if mp ~= nil then
                    LOBBY.click = true
                    LOBBY.clickX = mp.x
                    LOBBY.clickY = mp.y
                end
            end
            -- 调试驱动（--moba-autostart）：自动建房 -> 开局，便于无鼠标下测试。
            if GetVar("mobaAutostart") == 1 then
                LOBBY.autoT = (LOBBY.autoT or 0) + dt
                if LOBBY.autoT > 1.0 and not LOBBY.autoCreated then
                    LOBBY.autoCreated = true
                    Rpc("room.create")
                elseif LOBBY.autoT > 2.5 and not LOBBY.autoStarted then
                    LOBBY.autoStarted = true
                    Rpc("room.start")
                end
            end
            updateCameraFollow(dt)
            return
        end
    end
    SetVar("debugOverlay", 1)
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
    -- 服务器：周期广播英雄位置（10Hz，消息保持较小以稳妥送达）
    if netRole() == "server" then
        netStateTimer = netStateTimer - dt
        if netStateTimer <= 0 then
            netStateTimer = 0.1
            local arr = {}
            for i = 1, #heroes do
                local x = heroes[i]
                arr[i] = { client = x.netClient or 0, team = x.team, x = x.x, z = x.z, yaw = x.yaw or 0 }
            end
            Rpc("moba_state", { heroes = arr })
        end
        -- HUD 数值改动慢：分开、低频、每英雄一条小消息。
        netHudTimer = netHudTimer - dt
        if netHudTimer <= 0 then
            netHudTimer = 0.3
            for i = 1, #heroes do
                local x = heroes[i]
                Rpc("moba_hud", { client = x.netClient or 0, team = x.team,
                    hp = math.floor(x.hp), maxHp = math.floor(x.maxHp),
                    mana = math.floor(x.mana), level = x.level,
                    kills = x.kills, deaths = x.deaths, cs = x.cs, gold = math.floor(x.gold),
                    cd1 = x.cds[1], cd2 = x.cds[2], cd3 = x.cds[3], cd4 = x.cds[4],
                    r1 = x.ranks[1], r2 = x.ranks[2], r3 = x.ranks[3], r4 = x.ranks[4] })
            end
        end
    end
    for i = 1, #heroes do
        local h = heroes[i]
        for k = 1, 4 do
            if (h.cds[k] or 0) > 0 then h.cds[k] = h.cds[k] - dt end
        end
    end
    updatePlayer(dt)
    updateEconomy(dt)
    updateShop(dt)
    if enemyHero ~= nil and enemyHero.netClient == nil then updateAI(enemyHero, dt) end
    for i = 1, #units do
        local u = units[i]
        if not u.dead then
            if (u.actionT or 0) > 0 then u.actionT = math.max(0, u.actionT - dt) end
            tickBuffs(u, dt)
            if u.isHero then updateSwing(u, dt) end
            if u.kind == "minion" then updateMinion(u, dt)
            elseif u.kind == "neutral" then updateNeutral(u, dt)
            elseif u.kind == "tower" then updateTower(u, dt) end
        end
    end
    updateProjectiles(dt)
    updateGroundAoes(dt)
    updateWaves(dt)
    updateJungle(dt)
    updateCameraFollow(dt)
    updateVision()
    updatePlates()
    separateUnits(dt)
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

-- 选中/锁定目标：用 SetEntityHighlight 让敌方 mesh 边缘发光（替代原来的头顶方框）。
local lastTargetEnt = nil
local function updateTargetHighlight()
    local h = playerHero
    local t = (h ~= nil and h.target ~= nil and not h.target.dead) and h.target or nil
    local ent = t and t.ent or nil
    if lastTargetEnt ~= nil and lastTargetEnt ~= ent then
        SetEntityHighlight(lastTargetEnt, 0, 0, 0, 0)
    end
    lastTargetEnt = ent
    if ent ~= nil and t ~= nil then
        if h ~= nil and t.team ~= h.team then
            SetEntityHighlight(ent, 1.0, 0.30, 0.18, 1.9) -- 敌方：红橙描边
        else
            SetEntityHighlight(ent, 0.30, 1.0, 0.45, 1.4) -- 友方：绿色描边
        end
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
                -- 英雄：血条下方蓝条 + 左侧等级
                if plateMana[key] ~= nil then
                    local mw, mh = w, 3
                    DrawRect(a.x - mw / 2, a.y + 1, mw, mh, 0.04, 0.04, 0.06, 0.8)
                    DrawRect(a.x - mw / 2 + 1, a.y + 2, (mw - 2) * plateMana[key], mh - 2, 0.25, 0.45, 0.95, 1)
                    DrawText("Lv." .. tostring(plateLevel[key] or 1), a.x - w / 2 - 4, a.y - h + 2, 11,
                        1, 1, 1, 0.95, false, true)
                end
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
            DrawText(f.text, s.x, s.y, f.crit and 20 or 15, f.r, f.g, f.b, 1 - age, true, true)
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
    DrawText(string.format("%s  Lv.%d", h.name, h.level), bx, by - 40, 16, 1, 1, 1, 1, false, true)
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
        local rank = (h.ranks and h.ranks[i]) or 0
        local ready = rank > 0 and (h.cds[i] or 0) <= 0 and h.mana >= cost
        if ab ~= nil and ab.icon and ab.icon ~= "" then
            DrawSprite(ab.icon, x, sy, slot, slot, 1, 1, 1, rank > 0 and 1 or 0.42)
        else
            DrawRect(x, sy, slot, slot, 0.08, 0.08, 0.12, 0.9)
        end
        DrawRectOutline(x, sy, slot, slot, 2, ready and 0.9 or 0.3, ready and 0.78 or 0.3, 0.2, 1)
        DrawText(keys[i], x + 7, sy + 8, 13, 1, 1, 1, 0.95, false, false)
        -- 技能等级角标
        DrawRect(x + slot - 16, sy + slot - 14, 16, 14, 0, 0, 0, 0.6)
        DrawText(tostring(rank), x + slot - 8, sy + slot - 7, 11, 1, 1, 1, 1, true, true)
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
    if (h.skillPoints or 0) > 0 then
        -- 放到顶部计时器下方，避免和血/蓝条、技能栏重叠。
        DrawText("技能点 " .. h.skillPoints .. "   Ctrl+QWER 加点", cx, 46, 14, 1, 0.9, 0.4, 1, true, true)
    end
    -- 回城读条进度
    if (h.channel or 0) > 0 then
        local cw, chh = 220, 14
        local rbx, rby = cx - cw / 2, by - 72
        DrawRect(rbx, rby, cw, chh, 0.05, 0.05, 0.08, 0.85)
        DrawRect(rbx + 1, rby + 1, (cw - 2) * clamp(1 - h.channel / 1.4, 0, 1), chh - 2,
            0.3, 0.7, 1.0, 1)
        DrawText("回城中…", cx, rby + chh / 2, 12, 1, 1, 1, 1, true, true)
    end

    -- 数值行放在血条右侧（原来放在 sy-22 与血/蓝条重叠）。
    local infoY = by + bh * 0.5
    local infoX = bx + bw + 16
    DrawText(string.format("金币 %d", math.floor(h.gold)), infoX, infoY, 15, 0.95, 0.82, 0.3, 1, false, true)
    DrawText(string.format("补刀 %d", h.cs), infoX + 92, infoY, 15, 0.9, 0.9, 0.9, 1, false, true)
    DrawText(string.format("KDA %d/%d", h.kills, h.deaths) ..
        (((h.streak or 0) > 1) and ("   连杀 x" .. h.streak) or ""),
        infoX + 178, infoY, 15, 0.9, 0.9, 0.9, 1, false, true)
    -- 野怪增益剩余时间（红/蓝/小龙/大龙）
    if h.buffTimers ~= nil then
        local bf = { { "red", "红", 1.0, 0.4, 0.35 }, { "blue", "蓝", 0.4, 0.6, 1.0 },
                     { "dragon", "小龙", 1.0, 0.6, 0.2 }, { "baron", "大龙", 0.8, 0.45, 1.0 } }
        for i = 1, #bf do
            local e = bf[i]
            local t = h.buffTimers[e[1]]
            if t ~= nil and t > 0 then
                DrawText(string.format("%s %.0fs", e[2], math.ceil(t)),
                    infoX + (i - 1) * 62, infoY + 18, 12, e[3], e[4], e[5], 1, false, true)
            end
        end
    end
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
            local mx, my = mapX(u.x, u.z), mapY(u.x, u.z)
            DrawRect(mx - s / 2, my - s / 2, s, s, col[1], col[2], col[3], 1)
            -- 英雄：朝向小箭头
            if u.kind == "champion" then
                local dx, dz = math.sin(u.yaw or 0), math.cos(u.yaw or 0)
                local dlx = rc * dx + rs * dz
                local dly = rs * dx - rc * dz
                DrawLine(mx, my, mx + dlx * 7, my + dly * 7, 2.0,
                    col[1], col[2], col[3], 1)
            end
        end
    end
end

-- 击杀播报（右上，小地图下方）
local function drawKillFeed()
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local y = 200
    for i = #killFeed, 1, -1 do
        local f = killFeed[i]
        local age = elapsed - f.t
        if age < 6 then
            local a = math.min(1, (6 - age) / 0.6)
            local w = 210
            local x = vw - w - 12
            DrawRect(x, y, w, 20, 0.05, 0.05, 0.08, 0.65 * a)
            DrawRect(x, y, 3, 20, f.col[1], f.col[2], f.col[3], a)
            DrawText(f.killer .. " 击杀 " .. f.victim, x + w / 2 + 2, y + 10, 13,
                f.col[1], f.col[2], f.col[3], a, true, true)
            y = y + 22
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
    -- 表头列
    DrawText("英雄", vw * 0.5 - 250, 124, 13, 0.75, 0.75, 0.75, 1, false, true)
    DrawText("击杀", vw * 0.5 + 40, 124, 13, 0.75, 0.75, 0.75, 1, false, true)
    DrawText("死亡", vw * 0.5 + 100, 124, 13, 0.75, 0.75, 0.75, 1, false, true)
    DrawText("补刀", vw * 0.5 + 160, 124, 13, 0.75, 0.75, 0.75, 1, false, true)
    DrawText("金币", vw * 0.5 + 220, 124, 13, 0.75, 0.75, 0.75, 1, false, true)
    DrawText("装备", vw * 0.5 + 300, 124, 13, 0.75, 0.75, 0.75, 1, false, true)
    for i, r in ipairs(rows) do
        local h = r[1]
        if h ~= nil then
            local y = 150 + (i - 1) * 130
            DrawText(r[3], vw * 0.5 - 330, y + 8, 16, r[2][1], r[2][2], r[2][3], 1, false, true)
            if h.portrait then DrawSprite(h.portrait, vw * 0.5 - 330, y + 28, 64, 64, 1, 1, 1, 1) end
            DrawText(string.format("%s  Lv.%d", h.name, h.level), vw * 0.5 - 250, y + 36, 20, 1, 1, 1, 1, false, true)
            DrawText(tostring(h.kills), vw * 0.5 + 40, y + 40, 18, 0.95, 0.85, 0.4, 1, false, true)
            DrawText(tostring(h.deaths), vw * 0.5 + 100, y + 40, 18, 0.9, 0.6, 0.5, 1, false, true)
            DrawText(tostring(h.cs), vw * 0.5 + 160, y + 40, 18, 0.9, 0.9, 0.9, 1, false, true)
            DrawText(tostring(math.floor(h.gold)), vw * 0.5 + 220, y + 40, 16, 0.95, 0.82, 0.3, 1, false, true)
            for k = 1, #h.items do
                local it = ITEMS[h.items[k]]
                if it and it.icon ~= "" then
                    DrawSprite(it.icon, vw * 0.5 + 300 + (k - 1) * 42, y + 26, 38, 38, 1, 1, 1, 1)
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

-- 世界半径 -> 屏幕像素（用于把地面范围画成 HUD 圆）
local function worldRadiusPx(wx, wz, r)
    local a = WorldToScreen(wx, wz, 0.2)
    local b = WorldToScreen(wx + r, wz, 0.2)
    if a == nil or b == nil then return nil end
    return dist(a.x, a.y, b.x, b.y)
end

-- 地面圆环：把世界空间圆采样后投影到屏幕，画成透视正确的多边形。
-- 旧实现用 DrawCircle 画屏幕圆，半径随相机透视变化（越走越大/变椭圆），
-- 这里改在世界空间采样，任何相机角度都贴合地面。
local function groundRing(cx, cz, r, cr, cg, cb, ca, filled, seg)
    if r == nil or r <= 0 then return end
    seg = seg or 40
    local pts = {}
    for k = 0, seg do
        local a = k / seg * math.pi * 2
        local s = WorldToScreen(cx + math.cos(a) * r, 0.06, cz + math.sin(a) * r)
        if s == nil then return end
        pts[k + 1] = s
    end
    if filled then
        local c = WorldToScreen(cx, 0.06, cz)
        if c ~= nil then
            for k = 1, seg do
                DrawTri(c.x, c.y, pts[k].x, pts[k].y, pts[k + 1].x, pts[k + 1].y,
                        cr, cg, cb, ca)
            end
        end
    end
    for k = 1, seg do
        local p, q = pts[k], pts[k + 1]
        DrawLine(p.x, p.y, q.x, q.y, 2.0, cr, cg, cb, ca)
    end
end

-- 地面直线（技能轨迹）：沿世界直线采样投影，透视正确（不会像屏幕线一样悬空）。
local function groundLine(x0, z0, x1, z1, cr, cg, cb, ca, width)
    width = width or 2.0
    local steps = 16
    local prev = nil
    for k = 0, steps do
        local t = k / steps
        local s = WorldToScreen(x0 + (x1 - x0) * t, 0.06, z0 + (z1 - z0) * t)
        if s == nil then return end
        if prev ~= nil then DrawLine(prev.x, prev.y, s.x, s.y, width, cr, cg, cb, ca) end
        prev = s
    end
end

-- 待落地 AoE 的地面预警：脉冲圆环 + 内圈，比一次性粒子更易读。
local function drawGroundAoes()
    for i = 1, #groundAoes do
        local a = groundAoes[i]
        local col = TEAM_COLOR[a.team]
        local pulse = 0.5 + 0.4 * math.sin(elapsed * 14)
        groundRing(a.x, a.z, a.radius, col[1], col[2], col[3], 0.22, true)
        groundRing(a.x, a.z, a.radius, 0.95, 0.97, 1.0, pulse, false)
        groundRing(a.x, a.z, a.radius * 0.55, col[1], col[2], col[3], 0.55, false)
    end
end

-- HUD 层技能叠加：世界空间攻击范围圈 + 施法地面指示器（圆/直线）。
local function drawSkillOverlay()
    local h = playerHero
    if h == nil or h.dead then return end
    local heroS = WorldToScreen(h.x, 0.0, h.z)
    if heroS == nil then return end

    -- A：攻击范围（世界空间地面圈）；攻击移动时高亮目标点。
    if ActionDown("attack") then
        groundRing(h.x, h.z, h.range, 0.95, 0.9, 0.4, 0.85, false)
    end
    if h.attackMove and h.moveTarget ~= nil then
        groundRing(h.moveTarget.x, h.moveTarget.z, 1.0, 0.95, 0.55, 0.2, 0.30, true)
        groundRing(h.moveTarget.x, h.moveTarget.z, 1.0, 1.0, 0.85, 0.4, 0.95, false)
    end

    local m = InputMousePos()
    if m == nil then return end
    for i = 1, 4 do
        local ab = h.abilities[i]
        if ab ~= nil and ((h.ranks and h.ranks[i]) or 0) > 0 and ActionDown("spell" .. i)
            and InputKey("ctrl") ~= 1 then
            local col = TEAM_COLOR[h.team]
            local g = groundPick(m.x, m.y)
            local tx = g and g.x or (h.x + math.sin(h.yaw or 0) * 6)
            local tz = g and g.z or (h.z + math.cos(h.yaw or 0) * 6)
            -- 屏幕瞄准线（指向光标，作为辅助读向）
            DrawLine(heroS.x, heroS.y, m.x, m.y, 1.5, col[1], col[2], col[3], 0.5)
            if ab.type == "aoe_target" then
                local r = ab.range or 10
                local d = dist(h.x, h.z, tx, tz)
                if d > r and d > 0.0001 then
                    tx = h.x + (tx - h.x) / d * r
                    tz = h.z + (tz - h.z) / d * r
                end
                groundRing(h.x, h.z, r, col[1], col[2], col[3], 0.30, false)  -- 施法距离
                groundRing(tx, tz, ab.radius or 3, 0.55, 0.85, 1.0, 0.26, true) -- 落点填充
                groundRing(tx, tz, ab.radius or 3, 0.75, 0.92, 1.0, 0.95, false)
            elseif ab.type == "aoe_self" then
                groundRing(h.x, h.z, ab.radius or 3, 0.55, 0.85, 1.0, 0.26, true)
                groundRing(h.x, h.z, ab.radius or 3, 0.75, 0.92, 1.0, 0.95, false)
            else
                local len = ab.range or (ab.dist or 6)
                local d = dist(h.x, h.z, tx, tz)
                if d <= 0.0001 then d = 1 end
                local ex = h.x + (tx - h.x) / d * math.min(d, len)
                local ez = h.z + (tz - h.z) / d * math.min(d, len)
                groundLine(h.x, h.z, ex, ez, col[1], col[2], col[3], 0.30, 9.0) -- 轨迹宽带
                groundLine(h.x, h.z, ex, ez, 0.75, 0.92, 1.0, 0.95, 2.0)        -- 中心亮线
                groundRing(ex, ez, 0.7, 0.75, 0.92, 1.0, 0.9, false)             -- 终点标记
            end
            break
        end
    end
end

-- 战争迷雾遮罩：未探索=近黑，探索过但当前不可见=半透明
local function drawFog()
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    for cz = 0, FOG_COLS - 1 do
        for cx = 0, FOG_COLS - 1 do
            local idx = cz * FOG_COLS + cx + 1
            if fogVis[idx] ~= 1 then
                local wx = FOG_MIN + (cx + 0.5) * FOG_CELL
                local wz = FOG_MIN + (cz + 0.5) * FOG_CELL
                local s = WorldToScreen(wx, 0.0, wz)
                if s ~= nil then
                    local rp = worldRadiusPx(wx, wz, FOG_CELL)
                    -- 远处/近地平线的格子投影会爆表；夹住，避免一块黑盖满全屏。
                    if rp ~= nil then rp = math.min(rp, 40) end
                    if rp ~= nil and s.x > -rp and s.x < vw + rp and s.y > -rp and
                        s.y < vh + rp then
                        local a = (fogSeen[idx] == 1) and 0.5 or 0.96
                        DrawRect(s.x - rp * 1.15, s.y - rp * 1.15, rp * 2.3, rp * 2.3,
                            0.02, 0.02, 0.05, a)
                    end
                end
            end
        end
    end
end

-- 大厅界面（client）：创建/刷新/房号加入/等待/和 AI 开始/离开
local function drawLobby()
    local vp = GetViewportSize()
    local vw = (vp and vp.w) or VW
    local vh = (vp and vp.h) or VH
    local m = InputMousePos()
    local cx, cy = LOBBY.clickX, LOBBY.clickY
    local click = LOBBY.click == true
    LOBBY.click = false

    local function hovered(x, y, w, h)
        return m ~= nil and m.x >= x and m.x <= x + w and m.y >= y and m.y <= y + h
    end
    local function hit(x, y, w, h)
        return click and cx >= x and cx <= x + w and cy >= y and cy <= y + h
    end
    -- 带描边的文字：浓烈背景上也能看清（先画深色描边再填色）。
    local OUT = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } }
    local function T(s, x, y, size, r, g, b, cenx, ceny)
        for i = 1, #OUT do
            DrawText(s, x + OUT[i][1], y + OUT[i][2], size, 0, 0, 0, 0.9, cenx, ceny)
        end
        DrawText(s, x, y, size, r, g, b, 1, cenx, ceny)
    end

    -- AI 生成的奥术大厅背景 + 压暗遮罩
    DrawSprite(UI_BG, 0, 0, vw, vh, 1, 1, 1, 1)
    DrawRect(0, 0, vw, vh, 0.02, 0.03, 0.06, 0.50)
    DrawRect(0, 0, vw, 104, 0.02, 0.03, 0.06, 0.80)
    DrawLine(0, 104, vw, 104, 2, 0.90, 0.72, 0.32, 1)
    T("NeonMOBA", vw * 0.5, 44, 42, 1.0, 0.88, 0.48, true, true)
    T("对战大厅 · 创建或加入房间开始对局", vw * 0.5, 84, 15, 0.86, 0.90, 0.98, true, true)

    local connected = GetVar("netConnected") == 1
    local pname = GetVar("playerName")
    if pname == nil or pname == "" then pname = "玩家" end
    local pillW = math.max(190, 120 + #tostring(pname) * 10)
    local pillX = vw - pillW - 24
    DrawRect(pillX, 34, pillW, 38, 0.04, 0.06, 0.10, 0.92)
    DrawRectOutline(pillX, 34, pillW, 38, 1.5, 0.55, 0.45, 0.22, 1)
    DrawCircle(pillX + 20, 53, 6, 2,
        connected and 0.35 or 0.95, connected and 0.92 or 0.45, connected and 0.55 or 0.4, 1, true)
    T((connected and "已连接   " or "连接中   ") .. tostring(pname),
        pillX + 40, 53, 15, 0.95, 0.97, 1, false, true)

    if not connected then
        T("正在连接服务器…", vw * 0.5, vh * 0.5, 24, 0.95, 0.95, 0.95, true, true)
        return
    end

    -- 更宽松的两栏布局
    local cardW = math.min(460, (vw - 120) * 0.5)
    local cardH = math.min(vh - 210, 560)
    local gap = 28
    local leftX = vw * 0.5 - cardW - gap * 0.5
    local rightX = vw * 0.5 + gap * 0.5
    local cardY = 138
    local pad = math.floor(cardW * 0.14)
    local function card(x, y, w, h, title)
        T(title, x + w * 0.5, y - 20, 20, 1.0, 0.88, 0.48, true, true)
        DrawSprite(UI_PANEL, x, y, w, h, 1, 1, 1, 1)
        DrawRect(x + pad, y + h * 0.075, w - pad * 2, h * 0.85, 0.02, 0.04, 0.08, 0.70)
    end
    local function button(x, y, w, h, label, accent)
        local hov = hovered(x, y, w, h)
        DrawSprite(accent and UI_BTN_ACCENT or UI_BTN, x, y, w, h, 1, 1, 1, 1)
        if hov then DrawRect(x + w * 0.07, y + h * 0.16, w * 0.86, h * 0.68, 1, 1, 1, 0.18) end
        T(label, x + w / 2, y + h / 2 + 1, 16, 1, 1, 1, true, true)
        return hit(x, y, w, h)
    end

    ---------------- 左：房间列表 ----------------
    card(leftX, cardY, cardW, cardH, "房 间 列 表")
    local innerX = leftX + pad
    local innerW = cardW - pad * 2
    local listY = cardY + 66
    local rowH, rowGap = 46, 12
    local maxRows = math.max(1, math.floor((cardH - 150) / (rowH + rowGap)))
    if #LOBBY.rooms == 0 then
        T("暂无房间 — 来创建第一个吧", leftX + cardW * 0.5, listY + 34, 16, 0.70, 0.76, 0.86, true, true)
    end
    for i = 1, math.min(#LOBBY.rooms, maxRows) do
        local r = LOBBY.rooms[i]
        local yy = listY + (i - 1) * (rowH + rowGap)
        local hov = hovered(innerX, yy, innerW, rowH)
        DrawRect(innerX, yy, innerW, rowH,
            hov and 0.18 or 0.10, hov and 0.22 or 0.13, hov and 0.30 or 0.19, 0.96)
        DrawRectOutline(innerX, yy, innerW, rowH, hov and 2 or 1, 0.35, 0.42, 0.55, 1)
        local started = r.started and "  ·  已开始" or ""
        T("房间  " .. tostring(r.room), innerX + 16, yy + rowH * 0.5, 16, 0.95, 0.97, 1, false, true)
        T(string.format("%d/%d%s", r.players or 1, r.max or 2, started),
            innerX + innerW - 78, yy + rowH * 0.5, 15, 0.78, 0.88, 1, true, true)
        T("加入", innerX + innerW - 28, yy + rowH * 0.5, 14, 0.55, 0.98, 0.75, true, true)
        if hit(innerX, yy, innerW, rowH) and not r.started then
            Rpc("room.join", { room = r.room })
        end
    end
    if button(leftX + cardW * 0.5 - 80, cardY + cardH - 56, 160, 40, "刷新列表") then
        Rpc("room.list")
    end

    ---------------- 右：对战房间 ----------------
    card(rightX, cardY, cardW, cardH, "对 战 房 间")
    local myId = GetVar("myClientId")
    if LOBBY.room == "" then
        T("还没有加入房间", rightX + cardW * 0.5, cardY + 108, 17, 0.90, 0.93, 1, true, true)
        T("创建房间后把房号发给好友，", rightX + cardW * 0.5, cardY + 144, 14, 0.72, 0.78, 0.9, true, true)
        T("或点击左侧列表加入。", rightX + cardW * 0.5, cardY + 168, 14, 0.72, 0.78, 0.9, true, true)
        if button(rightX + cardW * 0.5 - 110, cardY + 208, 220, 52, "创建房间", true) then
            Rpc("room.create")
        end
        DrawLine(rightX + pad, cardY + 292, rightX + cardW - pad, cardY + 292, 1, 0.24, 0.28, 0.38, 1)
        T("输入房号加入", rightX + pad, cardY + 320, 15, 0.88, 0.92, 1, false, true)
        local boxW = innerW - 106
        local boxY = cardY + 350
        DrawRect(rightX + pad, boxY, boxW, 50, 0.03, 0.04, 0.08, 1)
        DrawRectOutline(rightX + pad, boxY, boxW, 50, 2, 0.35, 0.55, 0.72, 1)
        if #LOBBY.code == 0 then
            T("0-9 / A-Z", rightX + pad + 16, boxY + 25, 16, 0.5, 0.55, 0.65, false, true)
        else
            T(LOBBY.code, rightX + pad + 16, boxY + 25, 24, 1, 1, 1, false, true)
        end
        if button(rightX + pad + boxW + 14, boxY, 92, 50, "加入") then
            if #LOBBY.code > 0 then
                Rpc("room.join", { room = LOBBY.code })
                LOBBY.code = ""
            end
        end
    else
        local isHost = (myId ~= nil and LOBBY.host ~= 0 and LOBBY.host == myId)
        T("房号", rightX + pad, cardY + 84, 14, 0.72, 0.78, 0.9, false, true)
        T(tostring(LOBBY.room), rightX + pad, cardY + 116, 40, 1.0, 0.86, 0.42, false, true)
        T(string.format("%d / %d 名玩家", LOBBY.players, LOBBY.max),
            rightX + pad, cardY + 172, 16, 0.92, 0.94, 1, false, true)
        T(isHost and "你是房主" or "等待房主开始…",
            rightX + pad, cardY + 204, 15, 0.62, 0.92, 0.72, false, true)
        if isHost then
            if LOBBY.players >= 2 then
                if button(rightX + pad, cardY + 258, 210, 52, "开始对局", true) then Rpc("room.start") end
                if button(rightX + pad, cardY + 322, 210, 42, "踢出对手") then Rpc("room.kick") end
            else
                if button(rightX + pad, cardY + 258, 210, 52, "和 AI 开始", true) then Rpc("room.start") end
                if button(rightX + pad, cardY + 322, 210, 42, "刷新对手") then Rpc("room.list") end
            end
        end
        if button(rightX + cardW * 0.5 - 80, cardY + cardH - 56, 160, 40, "离开房间") then
            Rpc("room.leave")
            LOBBY.room = ""
            LOBBY.players = 0
            LOBBY.host = 0
        end
    end

    if LOBBY.msg ~= "" then
        T(LOBBY.msg, vw * 0.5, vh - 68, 16, 1.0, 0.65, 0.55, true, true)
    end
    T("鼠标点击操作 · 数字/字母输入房号 · Backspace 删除 · Enter 加入",
        vw * 0.5, vh - 36, 14, 0.68, 0.74, 0.85, true, true)
end

function on_render()
    if netRole() == "client" and LOBBY.phase ~= "match" then
        drawLobby()
        return
    end
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
    -- 战争迷雾的逐格黑色遮罩观感很差（硬边黑方块），暂时不画；
    -- 视野/草丛的“敌人隐藏”逻辑仍在（updateVision）。真正的柔和迷雾需要引擎遮罩纹理。
    drawWorldPlates()
    updateTargetHighlight()
    drawGroundAoes()
    drawSkillOverlay()
    drawFloatTexts()
    drawHud()
    drawMinimap(vw)
    drawKillFeed()
    drawScoreboard()
    drawShop()
end

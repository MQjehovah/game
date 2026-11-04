-- NeonOps loadout / preparation bay.
-- Shows the selected primary weapon on a turntable, lets the player switch
-- guns (1-4 / Q / A-D), persists the choice, and deploys to the saved mission.

local SWITCH_TIME = 0.28

local weapons = {}
local selectedIdx = 1
local selectedId = "rifle"
local gunModels = {}
local switchT = 0
local clock = 0
local strafeLatch = false

local settings = { mouseSensitivity = 0.004, musicVolume = 0.7, sfxVolume = 0.8 }
local bestScore = 0
local nextScene = "assets/scenes/level_01.json"

local function clamp(v, lo, hi)
  return math.max(lo, math.min(hi, v))
end

local function read_json(path)
  local text = ReadText(path)
  if text == nil or text == "" then return nil end
  return Json.Parse(text)
end

local function load_save()
  local root = read_json("save.json")
  if root ~= nil then
    if root.settings ~= nil then
      settings.mouseSensitivity = root.settings.mouseSensitivity or settings.mouseSensitivity
      settings.musicVolume = root.settings.musicVolume or settings.musicVolume
      settings.sfxVolume = root.settings.sfxVolume or settings.sfxVolume
    end
    bestScore = math.floor(root.bestScore or 0)
    if type(root.loadout) == "string" and root.loadout ~= "" then
      selectedId = root.loadout
    end
    if type(root.nextScene) == "string" and root.nextScene ~= "" then
      nextScene = root.nextScene
    end
  end
  if nextScene == nil or nextScene == "" then
    nextScene = "assets/scenes/level_01.json"
  end
end

local function save_state()
  local s = settings
  local text = string.format(
    '{"settings":{"mouseSensitivity":%f,"musicVolume":%f,"sfxVolume":%f},"bestScore":%d,"loadout":"%s","nextScene":"%s"}',
    s.mouseSensitivity, s.musicVolume, s.sfxVolume, bestScore, selectedId, nextScene)
  WriteText("save.json", text)
end

local function load_weapons()
  local list = read_json("assets/data/weapons.json")
  if list ~= nil then weapons = list end
end

local function mission_title()
  if string.find(nextScene, "level_01") ~= nil then return "PERIMETER BREACH" end
  if string.find(nextScene, "level_02") ~= nil then return "SECTOR HOLD" end
  if string.find(nextScene, "level_03") ~= nil then return "COMMAND CORE" end
  return "DEPLOYMENT"
end

local function update_weapon_visibility()
  for id, e in pairs(gunModels) do
    SetVisible(e, id == selectedId)
  end
end

local function spawn_weapon_models()
  for _, w in ipairs(weapons) do
    if w.prefab ~= nil and w.prefab ~= "" then
      local e = SpawnPrefab(w.prefab, { x = 0, y = 0, z = 0 })
      if e ~= nil then
        gunModels[w.id] = e
        SetVisible(e, false)
      end
    end
  end
  update_weapon_visibility()
end

local function refresh_ui()
  local w = weapons[selectedIdx]
  if w == nil then return end
  local rpm = math.floor(60.0 / (w.fireRate or 0.12) + 0.5)
  UISetText("WeaponName", w.name or "Weapon")
  UISetText("WeaponRole", w.role or "")
  UISetText("WeaponDesc", w.description or "")
  UISetText("DamageLabel", string.format("DAMAGE  %d", w.damage or 0))
  UISetText("RateLabel", string.format("FIRE RATE  %d RPM", rpm))
  UISetText("MagLabel", string.format("MAGAZINE  %d", w.mag or 0))
  UISetText("RangeLabel", string.format("EFFECTIVE RANGE  %d m", w.range or 0))
  UISetFill("DamageBar", clamp((w.damage or 0) / 120.0, 0.04, 1))
  UISetFill("RateBar", clamp(rpm / 750.0, 0.04, 1))
  UISetFill("MagBar", clamp((w.mag or 0) / 40.0, 0.04, 1))
  UISetFill("RangeBar", clamp((w.range or 0) / 240.0, 0.04, 1))
  UISetText("BestScoreLabel", "BEST SCORE  " .. tostring(bestScore))
  UISetText("MissionLabel", "NEXT: " .. mission_title())
  for i, def in ipairs(weapons) do
    local btn = "Pick_" .. def.id
    if i == selectedIdx then
      UISetColor(btn, 0.09, 0.48, 0.70, 1)
    else
      UISetColor(btn, 0.10, 0.16, 0.24, 1)
    end
  end
end

local function select_index(idx)
  if #weapons == 0 then return end
  local nextIdx = ((idx - 1) % #weapons) + 1
  if nextIdx == selectedIdx then return end
  selectedIdx = nextIdx
  selectedId = weapons[selectedIdx].id
  switchT = SWITCH_TIME
  update_weapon_visibility()
  refresh_ui()
  PlaySfx("click")
end

local function deploy()
  save_state()
  PlaySfx("click")
  ChangeScene(nextScene)
end

-- Prep-bay turntable: the chosen primary floats above the platform and turns
-- slowly so every side of the model (receiver, optic, handguard, stock) reads
-- before the player deploys.
local function update_pose(dt)
  clock = clock + dt
  if switchT > 0 then switchT = math.max(0, switchT - dt) end
  local w = weapons[selectedIdx]
  if w == nil then return end

  local raise = switchT / SWITCH_TIME
  local e = gunModels[w.id]
  if e == nil then return end
  local scale = 1.15
  local yaw = clock * 0.55
  -- Spin around the weapon's visual centre, not the pistol grip, so the
  -- muzzle and stock sweep symmetrically instead of orbiting off to one side.
  local centerZ = -0.28
  if w.id == "smg" then centerZ = -0.12
  elseif w.id == "shotgun" then centerZ = -0.26
  elseif w.id == "sniper" then centerZ = -0.37
  end
  local f = { x = -math.sin(yaw), y = 0.0, z = -math.cos(yaw) }
  local y = 1.42 + math.sin(clock * 1.1) * 0.03 - raise * 0.38
  SetPosition(e, {
    x = 0.0 + f.x * centerZ * scale,
    y = y,
    z = 0.95 + f.z * centerZ * scale,
  })
  SetRotationY(e, yaw)
  SetScale(e, scale, scale, scale)
end

function on_start(e)
  load_save()
  load_weapons()
  for i, w in ipairs(weapons) do
    if w.id == selectedId then
      selectedIdx = i
      break
    end
  end
  selectedId = weapons[selectedIdx] and weapons[selectedIdx].id or "rifle"
  spawn_weapon_models()
  UIShow("assets/ui/loadout.ui.json")
  UISetVisible("Loadout", true)
  refresh_ui()
  PlayMusic("menu", settings.musicVolume)
end

function on_update(e, dt)
  if UIClicked("Deploy") or ActionPressed("restart") then
    deploy()
    return
  end
  if UIClicked("BackLoadout") or ActionPressed("pause") then
    PlaySfx("click")
    ChangeScene("assets/scenes/menu.json")
    return
  end

  for i, w in ipairs(weapons) do
    if UIClicked("Pick_" .. w.id) then select_index(i) end
  end
  if ActionPressed("weapon_1") then select_index(1) end
  if ActionPressed("weapon_2") then select_index(2) end
  if ActionPressed("weapon_3") then select_index(3) end
  if ActionPressed("weapon_4") then select_index(4) end
  if ActionPressed("next_weapon") then select_index(selectedIdx + 1) end

  local strafe = ActionAxis("move_strafe")
  if math.abs(strafe) > 0.5 then
    if not strafeLatch then
      strafeLatch = true
      select_index(selectedIdx + (strafe > 0 and 1 or -1))
    end
  else
    strafeLatch = false
  end

  update_pose(dt)
end

function on_render()
end

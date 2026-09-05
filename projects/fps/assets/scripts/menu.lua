-- NeonOps main menu. Handles mission start, options, persisted settings and
-- routes into the first level scene.

local settings = { mouseSensitivity = 0.004, musicVolume = 0.7, sfxVolume = 0.8 }
local bestScore = 0

local function clamp(v, lo, hi)
  return math.max(lo, math.min(hi, v))
end

local function read_json(path)
  local text = ReadText(path)
  if text == nil or text == "" then return nil end
  return Json.Parse(text)
end

local function save_settings()
  local root = { settings = settings, bestScore = bestScore }
  local text = string.format(
    '{"settings":{"mouseSensitivity":%f,"musicVolume":%f,"sfxVolume":%f},"bestScore":%d}',
    settings.mouseSensitivity, settings.musicVolume, settings.sfxVolume, bestScore)
  WriteText("save.json", text)
end

local function apply_audio()
  SetBusVolume(0, 1.0)
  SetBusVolume(1, settings.sfxVolume)
  SetBusVolume(2, settings.musicVolume)
end

local function load_settings()
  local root = read_json("save.json")
  if root ~= nil then
    if root.settings ~= nil then
      settings.mouseSensitivity = root.settings.mouseSensitivity or settings.mouseSensitivity
      settings.musicVolume = root.settings.musicVolume or settings.musicVolume
      settings.sfxVolume = root.settings.sfxVolume or settings.sfxVolume
    end
    bestScore = root.bestScore or 0
  end
  apply_audio()
end

local function refresh_options()
  UISetText("SensLabel", string.format("Mouse sensitivity: %.2f", settings.mouseSensitivity * 1000))
  UISetText("MusicLabel", string.format("Music: %d%%", math.floor(settings.musicVolume * 100 + 0.5)))
  UISetText("SfxLabel", string.format("SFX: %d%%", math.floor(settings.sfxVolume * 100 + 0.5)))
end

function on_start(e)
  load_settings()
  UIShow("assets/ui/main_menu.ui.json")
  UISetVisible("MainMenu", true)
  UISetVisible("Options", false)
  refresh_options()
  PlayMusic("menu", settings.musicVolume)
end

function on_update(e, dt)
  if UIClicked("Start") then
    PlaySfx("click")
    UISetVisible("MainMenu", false)
    ChangeScene("assets/scenes/level_01.json")
    return
  end

  if UIClicked("Options") then
    PlaySfx("click")
    UISetVisible("MainMenu", false)
    UISetVisible("Options", true)
    refresh_options()
    return
  end

  if UIClicked("Back") then
    PlaySfx("click")
    UISetVisible("Options", false)
    UISetVisible("MainMenu", true)
    return
  end

  local changed = false
  if UIClicked("SensDown") then
    settings.mouseSensitivity = clamp(settings.mouseSensitivity - 0.00025, 0.001, 0.01)
    changed = true
  elseif UIClicked("SensUp") then
    settings.mouseSensitivity = clamp(settings.mouseSensitivity + 0.00025, 0.001, 0.01)
    changed = true
  elseif UIClicked("MusicDown") then
    settings.musicVolume = clamp(settings.musicVolume - 0.1, 0, 1)
    changed = true
  elseif UIClicked("MusicUp") then
    settings.musicVolume = clamp(settings.musicVolume + 0.1, 0, 1)
    changed = true
  elseif UIClicked("SfxDown") then
    settings.sfxVolume = clamp(settings.sfxVolume - 0.1, 0, 1)
    changed = true
  elseif UIClicked("SfxUp") then
    settings.sfxVolume = clamp(settings.sfxVolume + 0.1, 0, 1)
    changed = true
  end

  if changed then
    PlaySfx("click")
    apply_audio()
    refresh_options()
    save_settings()
  end
end

function on_render()
  DrawRect(0, 0, 1280, 720, 0.02, 0.04, 0.07, 1)
  DrawText("BEST SCORE  " .. tostring(bestScore), 640, 614, 16, 0.65, 0.72, 0.82, 1, true, true)
end

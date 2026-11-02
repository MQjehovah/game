-- lc_weather.lua
-- 天气：晴 / 阴 / 雨 / 雾，随游戏时间缓慢变化

WEATHER_NAMES = { clear = "晴", cloudy = "阴", rain = "雨", fog = "雾" }
WEATHER_WEIGHTS = {
  { id = "clear",  w = 46 },
  { id = "cloudy", w = 26 },
  { id = "rain",   w = 18 },
  { id = "fog",    w = 10 },
}

function initWeather()
  weather = "clear"
  weatherT = 0
  weatherNext = 10 + math.random() * 14
end

function setWeather(id)
  weather = id or "clear"
  weatherT = 0
  weatherNext = 9 + math.random() * 14
  if recomputeStats then recomputeStats() end  -- 太阳能随天气变化
  if toast and WEATHER_NAMES[weather] then
    toast.text = "天气：" .. WEATHER_NAMES[weather]
    toast.t = 3.0
  end
end

function rollWeather()
  local total = 0
  for _, e in ipairs(WEATHER_WEIGHTS) do total = total + e.w end
  local r = math.random() * total
  for _, e in ipairs(WEATHER_WEIGHTS) do
    r = r - e.w
    if r <= 0 then return e.id end
  end
  return "clear"
end

function updateWeather(dt)
  weatherT = weatherT + dt * SURVIVAL.timeScale
  if weatherT >= weatherNext then
    local nextWeather = rollWeather()
    if nextWeather ~= weather then setWeather(nextWeather) else weatherT = 0 end
  end
  -- 3D 雨丝（屏幕雨线之外，让雨真正落在世界里）
  if weather == "rain" and onFoot and player.active then
    rainAcc = (rainAcc or 0) + dt * 42
    while rainAcc >= 1 do
      rainAcc = rainAcc - 1
      local a = math.random() * math.pi * 2
      local r = 2 + math.random() * 9
      EmitParticles({
        pos = { x = player.x + math.sin(a) * r, y = 5.5 + math.random() * 2.5,
                z = player.z + math.cos(a) * r },
        count = 1, speedMin = 0, speedMax = 0,
        vel = { x = -0.6, y = -11, z = 0.2 },
        lifeMin = 0.28, lifeMax = 0.42,
        sizeStart = 0.035, sizeEnd = 0.02,
        color = { r = 0.62, g = 0.74, b = 0.88, a = 0.55 },
        colorEnd = { r = 0.62, g = 0.74, b = 0.88, a = 0.0 },
        additive = false, gravity = 0,
      })
    end
  end
end

function weatherName()
  return WEATHER_NAMES[weather] or "晴"
end

function isRainy()
  return weather == "rain" or weather == "fog"
end

function drawWeatherOverlay()
  local vp = GetViewportSize()
  local vw = (vp and vp.w) or 1280
  local vh = (vp and vp.h) or 720
  if weather == "cloudy" then
    DrawRect(0, 0, vw, vh, 0.35, 0.38, 0.42, 0.10)
  elseif weather == "rain" then
    DrawRect(0, 0, vw, vh, 0.03, 0.06, 0.12, 0.20)
    for i = 1, 44 do
      local x = (i * 97 + math.floor(weatherT * 17)) % vw
      local y = (i * 53 + math.floor(weatherT * 230)) % vh
      DrawLine(x, y, x - 7, y + 22, 1, 0.62, 0.72, 0.82, 0.32)
    end
  elseif weather == "fog" then
    DrawRect(0, 0, vw, vh, 0.72, 0.75, 0.79, 0.24)
    DrawRect(0, 0, vw, vh, 0.85, 0.87, 0.89, 0.08)
  end
end

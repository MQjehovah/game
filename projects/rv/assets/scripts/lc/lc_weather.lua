-- lc_weather.lua
-- 天气：晴 / 阴 / 雨 / 雾 / 雪，随游戏时间缓慢变化

WEATHER_NAMES = { clear = "晴", cloudy = "阴", rain = "雨", fog = "雾", snow = "雪" }
WEATHER_WEIGHTS = {
  { id = "clear",  w = 44 },
  { id = "cloudy", w = 24 },
  { id = "rain",   w = 16 },
  { id = "fog",    w = 9 },
  { id = "snow",   w = 7 },
}

-- 天气持续时间（游戏分钟）：1.5–3 游戏小时，约 30–60 现实秒；
-- 太短会让雨/雾一闪而过，也不利于规划下一段路。
function weatherDuration()
  return 90 + math.random() * 90
end

-- 电台曲目：1=radio、2=radio2、3=radio3（天气/短波台）
function radioTrack(station)
  local s = station or 1
  if s == 2 then return "radio2" end
  if s == 3 then return "radio3" end
  return "radio"
end

-- 环境音乐：开车=引擎（可切换车载电台）、雨天步行=雨声、营地收音机=radio、其它=风声
function desiredMusic(phaseValue, weatherValue, radioIsOn, station, carRadio)
  if phaseValue == "driving" then
    if carRadio then
      return radioTrack(station), 0.12
    end
    return "engine", 0.22
  end
  if radioIsOn then
    return radioTrack(station), 0.12
  end
  if weatherValue == "rain" then return "rain", 0.14 end
  return "wind", 0.10
end

function initWeather()
  weather = "clear"
  weatherT = 0
  weatherNext = weatherDuration()
  weatherNextId = rollWeather()
end

function setWeather(id)
  local wasRain = (weather == "rain")
  weather = id or "clear"
  weatherT = 0
  weatherNext = weatherDuration()
  weatherNextId = rollWeather()
  -- 雨停后的白天出现短暂彩虹
  if wasRain and (weather == "clear" or weather == "cloudy") and
     not (isNight ~= nil and isNight()) then
    rainbowT = 10
  end
  if recomputeStats then recomputeStats() end  -- 太阳能随天气变化
  if toast and WEATHER_NAMES[weather] then
    local line = "天气：" .. WEATHER_NAMES[weather]
    local hint = snowChainHint(weather,
      hasItem ~= nil and hasItem("chains", 1))
    if hint ~= nil then line = line .. " · " .. hint end
    toast.text = line
    toast.t = hint ~= nil and 4.2 or 3.0
  end
end

-- 雪天未带防滑链时的提示
function snowChainHint(weatherValue, chained)
  if weatherValue == "snow" and not chained then
    return "可到营地货运板换防滑链（4-9）"
  end
  return nil
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

-- 挡风玻璃水珠数量：仅雨天 + 驾驶席视角
function windshieldDropCount()
  if weather == "rain" and phase == "driving" and driveView == "cab" then
    return 18
  end
  return 0
end

-- 雨刷角度：驾驶席视角下周期性来回摆动（-0.20 ~ 0.42 rad）
function wiperAngle(t)
  local period = 1.8
  local p = ((t or 0) % period) / period
  local sweep = (p < 0.5) and (p * 2) or (2 - p * 2)
  return -0.20 + sweep * 0.62
end

function wipersActive()
  return (weather == "rain" or weather == "snow") and
         phase == "driving" and driveView == "cab"
end

function drawWipers(vw, vh)
  if not wipersActive() then return end
  local t = weatherT or 0
  for k = 0, 1 do
    local ang = wiperAngle(t + k * 0.22) * 1.4
    local cx = vw * (0.34 + k * 0.32)
    local cy = vh * 0.99
    local len = vh * 0.20
    local wx = cx + math.sin(ang) * len
    local wy = cy - math.cos(ang) * len
    DrawLine(cx, cy, wx, wy, 4, 0.06, 0.07, 0.08, 0.55)
    DrawLine(cx, cy, wx, wy, 2, 0.20, 0.21, 0.22, 0.45)
  end
end

-- 路面抓地折减：雨天湿滑、雪天更滑；带防滑链时雨雪抓地提高
function weatherGripScale()
  local base = 1.0
  if weather == "rain" then base = 0.82
  elseif weather == "snow" then base = 0.78 end
  if base < 1.0 and hasItem ~= nil and hasItem("chains", 1) then
    base = math.min(0.95, base + 0.14)
  end
  return base
end

-- 雨雪抓地提示：显示当前抓地系数与是否装备防滑链（路况良好时返回 nil）
function tractionStatusText()
  local grip = weatherGripScale()
  if grip >= 0.99 then return nil end
  local chained = hasItem ~= nil and hasItem("chains", 1)
  return string.format("抓地 %.2f%s", grip, chained and " · 防滑链" or "")
end

-- 夜间萤火虫发射率（/s）：深夜且非雨雪；自检/调参用
function fireflyRate()
  if weather == "rain" or weather == "snow" then return 0 end
  if nightAmount == nil or nightAmount() < 0.6 then return 0 end
  return 6
end

-- 闪电：雨天每隔一段时间触发一次全屏闪光 + 雷声
function triggerLightning()
  lightningT = 0.50
  lightningNext = 8 + math.random() * 12
  lightningCount = (lightningCount or 0) + 1
  if PlaySfx then PlaySfx("thunder") end
end

-- 地面薄雾发射率（/s）：雾天最浓，深夜也有少量；自检/调参用
function fogWispRate()
  if weather == "fog" then return 6 end
  if nightAmount ~= nil and nightAmount() > 0.65 then return 2 end
  return 0
end

function updateWeather(dt)
  weatherT = weatherT + dt * SURVIVAL.timeScale
  if rainbowT ~= nil and rainbowT > 0 then
    rainbowT = math.max(0, rainbowT - dt)
  end
  if lightningT ~= nil and lightningT > 0 then
    lightningT = math.max(0, lightningT - dt)
  end
  if weather == "rain" then
    lightningNext = (lightningNext or (6 + math.random() * 8)) - dt
    if lightningNext <= 0 then triggerLightning() end
  end
  if weatherT >= weatherNext then
    local nextWeather = weatherNextId or rollWeather()
    if nextWeather ~= weather then setWeather(nextWeather) else
      weatherT = 0
      weatherNextId = rollWeather()
    end
  end
  -- 3D 雨丝（屏幕雨线之外，让雨真正落在世界里）：步行围绕玩家，
  -- 驾驶时围绕房车，追逐视角也能看到雨幕。
  if weather == "rain" then
    local cx, cz, cy, rate, rad
    if onFoot and player.active then
      cx, cz, cy = player.x, player.z, 4.6
      rate, rad = 42, 9
    elseif phase == "driving" then
      cx, cz = rv.x, rv.z
      cy = (rv.groundY or 0) + 4.2
      rate, rad = 60, 15
    end
    if cx ~= nil then
      rainAcc = (rainAcc or 0) + dt * rate
      while rainAcc >= 1 do
        rainAcc = rainAcc - 1
        local a = math.random() * math.pi * 2
        local r = 2 + math.random() * rad
        EmitParticles({
          pos = { x = cx + math.sin(a) * r, y = cy + math.random() * 2.5,
                  z = cz + math.cos(a) * r },
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

  -- 地面薄雾：雾天/深夜在玩家或房车周围缓慢漂移，低透明度大颗粒
  local fogRate = fogWispRate()
  if fogRate > 0 then
    local fx, fz, fy
    if onFoot and player.active then
      fx, fz, fy = player.x, player.z, 0.25
    elseif phase == "driving" then
      fx, fz = rv.x, rv.z
      fy = (rv.groundY or 0) + 0.25
    end
    if fx ~= nil then
      fogAcc = (fogAcc or 0) + dt * fogRate
      while fogAcc >= 1 do
        fogAcc = fogAcc - 1
        local a = math.random() * math.pi * 2
        local r = 4 + math.random() * 16
        EmitParticles({
          pos = { x = fx + math.sin(a) * r, y = fy + 0.2 + math.random() * 1.5,
                  z = fz + math.cos(a) * r },
          count = 1, speedMin = 0, speedMax = 0,
          vel = { x = 0.25, y = 0.06, z = 0.15 },
          lifeMin = 3.5, lifeMax = 7.0,
          sizeStart = 2.0, sizeEnd = 4.5,
          color = { r = 0.76, g = 0.79, b = 0.84, a = 0.07 },
          colorEnd = { r = 0.76, g = 0.79, b = 0.84, a = 0.0 },
          additive = false, gravity = 0,
        })
      end
    end
  end
  -- 夜间萤火虫：营地/草甸周围的暖绿微光（加法混合）
  local ffRate = fireflyRate()
  if ffRate > 0 then
    local fx, fz, fy
    if onFoot and player.active then
      fx, fz, fy = player.x, player.z, 0.4
    elseif phase == "driving" then
      fx, fz = rv.x, rv.z
      fy = (rv.groundY or 0) + 0.5
    end
    if fx ~= nil then
      ffAcc = (ffAcc or 0) + dt * ffRate
      while ffAcc >= 1 do
        ffAcc = ffAcc - 1
        local a = math.random() * math.pi * 2
        local r = 3 + math.random() * 10
        EmitParticles({
          pos = { x = fx + math.sin(a) * r, y = fy + 0.3 + math.random() * 1.2,
                  z = fz + math.cos(a) * r },
          count = 1, speedMin = 0.05, speedMax = 0.25,
          vel = { x = 0.1, y = 0.05, z = 0.08 },
          lifeMin = 2.0, lifeMax = 4.0,
          sizeStart = 0.045, sizeEnd = 0.012,
          color = { r = 0.85, g = 0.95, b = 0.45, a = 0.75 },
          colorEnd = { r = 0.75, g = 0.85, b = 0.38, a = 0.0 },
          additive = true, gravity = 0,
        })
      end
    end
  end
end

function weatherName()
  return WEATHER_NAMES[weather] or "晴"
end

-- 当前天气还剩多少游戏小时（HUD 预报用）
function weatherHoursLeft()
  local left = (weatherNext or 0) - (weatherT or 0)
  if left < 0 then left = 0 end
  return left / 60
end

function isRainy()
  return weather == "rain" or weather == "fog" or weather == "snow"
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
    -- 闪电全屏闪光（亮度随时间衰减）
    if lightningT ~= nil and lightningT > 0 then
      local a = math.min(0.35, lightningT * 0.9)
      DrawRect(0, 0, vw, vh, 0.86, 0.90, 1.0, a)
    end
    -- 驾驶席视角：挡风玻璃上的水珠/水痕
    local drops = windshieldDropCount()
    for i = 1, drops do
      local x = (i * 173 + math.floor(weatherT * 9)) % vw
      local y = (i * 89 + math.floor(weatherT * 55)) % vh
      DrawCircle(x, y, 2 + (i % 3), 1, 0.72, 0.80, 0.90, 0.18, false)
      DrawLine(x, y, x + 1, y + 5 + (i % 4), 1, 0.72, 0.80, 0.90, 0.12)
    end
    -- 驾驶席视角：雨刷来回摆动（只在挡风玻璃下沿，避免遮挡视线）
    if wipersActive ~= nil then drawWipers(vw, vh) end
  elseif weather == "snow" then
    DrawRect(0, 0, vw, vh, 0.55, 0.60, 0.68, 0.12)
    -- 雪花：白色小点缓慢飘落，带轻微横向摆动
    for i = 1, 56 do
      local x = (i * 131 + math.floor(weatherT * 11) +
                 math.floor(math.sin(weatherT * 0.7 + i) * 14)) % vw
      local y = (i * 67 + math.floor(weatherT * 90)) % vh
      local r = 1 + (i % 3) * 0.5
      DrawCircle(x, y, r, 1, 0.92, 0.95, 1.0, 0.55, true)
    end
    -- 雪天驾驶席也摆雨刷
    if wipersActive ~= nil then drawWipers(vw, vh) end
  elseif weather == "fog" then
    DrawRect(0, 0, vw, vh, 0.72, 0.75, 0.79, 0.24)
    DrawRect(0, 0, vw, vh, 0.85, 0.87, 0.89, 0.08)
  end
end

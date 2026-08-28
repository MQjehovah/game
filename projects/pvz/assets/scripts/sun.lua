-- 阳光实体 (prefab 动态生成): 缓慢下落, 点击收集, 超时消失。

local timers = {}

function on_start(e)
  local s = EntityComponent(e, "sun")
  local pos = GetPosition(e)
  -- 世界坐标 Y 向上: 阳光向屏幕下方"落" = y 减小, 落到植物下方约 1.3 格。
  timers[e.id] = { t = 0, targetY = math.max(160, pos.y - 130), value = (s and s.value) or 25 }
end

function on_update(e, dt)
  if GetVar("started") ~= true then return end
  if GetVar("paused") == true then return end
  local st = timers[e.id]
  if not st then return end
  local pos = GetPosition(e)
  st.t = st.t + dt

  if pos.y > st.targetY then
    pos.y = math.max(st.targetY, pos.y - 30 * dt)
    SetPosition(e, { x = pos.x, y = pos.y, z = pos.z })
  end
  if st.t > 10 then
    timers[e.id] = nil
    Despawn(e)
    return
  end

  if InputMousePressed(0) then
    local m = InputMousePos()
    -- 鼠标是视口像素; 阳光在世界系(y 向上)。ScreenToWorld 完成换算。
    if m then
      local w = ScreenToWorld(m)
      if w and math.abs(w.x - pos.x) < 32 and math.abs(w.y - pos.y) < 32 then
        local s = GetVar("sun")
        if type(s) ~= "number" then s = 0 end
        SetVar("sun", s + st.value)
        SpawnFloatText({ x = pos.x, y = pos.y + 20, z = pos.z }, "+" .. st.value, false, 0.9)
        timers[e.id] = nil
        PlaySfx("sun")
        Despawn(e)
        return
      end
    end
  end
end

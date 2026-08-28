-- Data-driven sample script: no engine gameplay hooks, just the generic
-- InputAxis/InputKey/GetVar/SetVar/GetPosition/SetPosition API. Every tick it
-- increments the "ticks" GameVar so tests/smoke runs can assert it ran.
function on_start(e)
  SetVar("started", true)
end

function on_update(e, dt)
  local t = GetVar("ticks")
  if t == nil then t = 0 end
  t = t + 1
  SetVar("ticks", t)

  local fwd = InputAxis("forward")
  local strafe = InputAxis("strafe")
  if fwd > 0.5 or strafe > 0.5 then
    SetVar("moving", true)
  end
  if InputKey("space") > 0 then
    SetVar("jump", true)
  end

  local p = GetPosition(e)
  if p ~= nil then
    SetPosition(e, { x = p.x, y = 1.2 + math.sin(t * 0.1) * 0.5, z = p.z })
  end
end

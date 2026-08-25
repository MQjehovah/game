-- M1 animation verification: drives the wolf's clips through PlayAnimation
-- and stamps a plate + float text so the whole chain is visible in playtest.
local wolf = nil
local mode = 0   -- 0 idle loop, 1 run loop, 2 one-shot attack
local timer = 0
local elapsed = 0

function on_start(e)
  wolf = e
  elapsed = 0
  local ok = PlayAnimation(e, "idle", true, 0.3)
  print("ANIMVERIFY start idle=" .. tostring(ok))
  SetEntityPlate(e, "验证狼", 1.0)
end

function on_update(e, dt)
  if wolf == nil then return end
  timer = timer + dt
  elapsed = elapsed + dt
  -- Cycle: 2.5s idle -> 2.5s run -> one-shot attack (wait for finish) -> back.
  if mode == 0 and timer > 2.5 then
    mode = 1
    timer = 0
    local ok = PlayAnimation(wolf, "run", true, 0.25)
    print("ANIMVERIFY -> run ok=" .. tostring(ok) .. " t=" .. string.format("%.1f", elapsed))
  elseif mode == 1 and timer > 2.5 then
    mode = 2
    timer = 0
    local ok = PlayAnimation(wolf, "walk", false, 0.15) -- walk one-shot as "attack" stand-in
    print("ANIMVERIFY -> walk(oneshot) ok=" .. tostring(ok) .. " t=" .. string.format("%.1f", elapsed))
  elseif mode == 2 then
    if AnimationFinished(wolf) then
      mode = 0
      timer = 0
      PlayAnimation(wolf, "idle", true, 0.3)
      local p = GetPosition(wolf)
      SpawnFloatText(p.x, p.y + 1.5, p.z, "动作完成", false, 1.5)
      print("ANIMVERIFY oneshot finished t=" .. string.format("%.1f", elapsed))
    end
  end
  -- Keep the plate hp swinging so the bar is visibly alive.
  local t = (math.sin(elapsed * 2) + 1) * 0.5
  SetEntityPlate(wolf, "验证狼", t)
end

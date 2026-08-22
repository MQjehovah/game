-- Headless server smoke script: a script-spawned "player" entity that the
-- server's controller client drives through InputAxis("forward") (moveY).
-- Every tick also bumps the "ticks" GameVar so smoke runs can assert the
-- world advanced. The spawned entity uses the CTransformBind position
-- component (Spawn/SetPosition), which the server's snapshot replicates.
function on_start(e)
  player = Spawn("player", { x = 0, y = 0, z = 0 })
  SetVar("ticks", 0)
end

function on_update(e, dt)
  local t = GetVar("ticks")
  if t == nil then t = 0 end
  t = t + 1
  SetVar("ticks", t)

  local fwd = InputAxis("forward")
  if fwd > 0.5 then
    local p = GetPosition(player)
    if p ~= nil then
      SetPosition(player, { x = p.x, y = p.y, z = p.z + 1 })
    end
  end
end

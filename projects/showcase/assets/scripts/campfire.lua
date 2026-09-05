-- Campfire: per-frame ember + flame burst from the fire entity.
-- on_update(ent, dt) — ent id not needed (fixed world pos).

local elapsed = 0

function on_update(ent, dt)
  elapsed = elapsed + dt
  -- ~10 bursts/sec so the flame looks alive but stays cheap.
  if elapsed < 0.1 then return end
  elapsed = 0

  local rnd = math.random
  EmitParticles({
    pos = { x = 0, y = 0.5, z = -3.5 },
    count = 6,
    speedMin = 0.2,
    speedMax = 1.1,
    vel = { x = 0, y = 2.2, z = 0 },
    lifeMin = 0.5,
    lifeMax = 1.1,
    sizeStart = 0.55,
    sizeEnd = 0.08,
    color = { r = 1.0, g = 0.62 + rnd() * 0.2, b = 0.18, a = 0.9 },
    colorEnd = { r = 0.9, g = 0.25, b = 0.05, a = 0.0 },
    gravity = -0.6,
    additive = true
  })
  -- Embers: brighter, slower, sparse sparks that drift up then out.
  EmitParticles({
    pos = { x = 0, y = 0.6, z = -3.5 },
    count = 3,
    speedMin = 0.1,
    speedMax = 0.6,
    vel = { x = (rnd() - 0.5) * 0.8, y = 1.4, z = (rnd() - 0.5) * 0.8 },
    lifeMin = 0.8,
    lifeMax = 1.6,
    sizeStart = 0.2,
    sizeEnd = 0.03,
    color = { r = 1.0, g = 0.85, b = 0.45, a = 1.0 },
    colorEnd = { r = 0.5, g = 0.15, b = 0.02, a = 0.0 },
    gravity = -1.2,
    additive = true
  })
end

-- Dusk forest fireflies: sparse warm-green motes drifting near the ground.
local acc = 0

function on_update(ent, dt)
  acc = acc + dt
  if acc < 0.14 then return end
  acc = 0

  local rnd = math.random
  EmitParticles({
    pos = { x = -10 + rnd() * 20, y = 0.7 + rnd() * 3.0, z = -2 + rnd() * 14 },
    count = 2,
    shape = "sphere",
    radius = 0.35,
    vel = { x = (rnd() - 0.5) * 0.25, y = 0.08 + rnd() * 0.16, z = (rnd() - 0.5) * 0.25 },
    lifeMin = 3.0,
    lifeMax = 5.0,
    sizeStart = 0.075,
    sizeEnd = 0.02,
    color = { r = 0.85, g = 1.0, b = 0.45, a = 0.9 },
    colorEnd = { r = 0.4, g = 0.7, b = 0.15, a = 0.0 },
    gravity = 0.02,
    additive = true,
    texture = "assets/sprites/glow.png"
  })
end

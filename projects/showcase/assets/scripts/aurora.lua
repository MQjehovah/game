-- Aurora: slow additive curtains in the northern sky, green fading to blue.
local acc = 0

function on_update(ent, dt)
  acc = acc + dt
  if acc < 0.1 then return end
  acc = 0

  local rnd = math.random
  local x = -20 + rnd() * 40
  local y = 7.0 + rnd() * 5.0
  local z = -18 - rnd() * 18
  EmitParticles({
    pos = { x = x, y = y, z = z },
    count = 3,
    shape = "sphere",
    radius = 1.8,
    vel = { x = (rnd() - 0.5) * 0.35, y = 0.04 + rnd() * 0.05, z = 0.0 },
    lifeMin = 4.5,
    lifeMax = 7.5,
    sizeStart = 3.5 + rnd() * 1.5,
    sizeEnd = 5.0 + rnd() * 2.0,
    color = { r = 0.15, g = 0.9, b = 0.55, a = 0.18 },
    colorEnd = { r = 0.2, g = 0.45, b = 0.95, a = 0.0 },
    gravity = 0.01,
    additive = true,
    texture = "assets/sprites/vfx/glow_soft.png"
  })
end

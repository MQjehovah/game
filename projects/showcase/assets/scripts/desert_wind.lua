-- Desert: low wind-blown sand veils drifting across the dunes.
local acc = 0

function on_update(ent, dt)
  acc = acc + dt
  if acc < 0.1 then return end
  acc = 0

  local rnd = math.random
  EmitParticles({
    pos = { x = -12 + rnd() * 24, y = 0.4 + rnd() * 2.2, z = 4 + rnd() * 14 },
    count = 5,
    shape = "sphere",
    radius = 1.4,
    vel = { x = 0.8 + rnd() * 0.5, y = 0.05, z = (rnd() - 0.5) * 0.2 },
    lifeMin = 1.6,
    lifeMax = 2.6,
    sizeStart = 0.6,
    sizeEnd = 1.6,
    color = { r = 0.82, g = 0.68, b = 0.45, a = 0.16 },
    colorEnd = { r = 0.88, g = 0.76, b = 0.55, a = 0.0 },
    gravity = 0.0,
    drag = 0.25,
    additive = false,
    texture = "assets/sprites/vfx/dust.png"
  })
end

-- Snowfield: steady light snowfall in the volume the showcase camera frames.
local acc = 0

function on_update(ent, dt)
  acc = acc + dt
  if acc < 0.06 then return end
  acc = 0

  local rnd = math.random
  for i = 1, 2 do
    EmitParticles({
      pos = { x = -15 + rnd() * 30, y = 5.0 + rnd() * 4.0, z = -6 + rnd() * 20 },
      count = 5,
      shape = "point",
      vel = { x = (rnd() - 0.5) * 0.5, y = -1.1, z = (rnd() - 0.5) * 0.5 },
      lifeMin = 4.5,
      lifeMax = 7.0,
      sizeStart = 0.18,
      sizeEnd = 0.1,
      color = { r = 0.92, g = 0.96, b = 1.0, a = 0.9 },
      colorEnd = { r = 0.9, g = 0.95, b = 1.0, a = 0.0 },
      gravity = -0.08,
      drag = 0.05,
      additive = true,
      texture = "assets/sprites/vfx/glow_soft.png"
    })
  end
end

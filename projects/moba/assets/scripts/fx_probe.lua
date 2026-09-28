-- fx_probe: deterministic soft-particle / decal probe for screenshot A/B.
-- Emits a soft (alpha) particle burst + an additive glow burst + a ground
-- decal ring/disc every frame near the camera focus point.

local spawned = false

local function emit()
    -- soft alpha smoke (depth-faded billboards): rises from the ground point
    EmitParticles({ pos = { x = 0, y = 0.05, z = 0 }, count = 24,
        vel = { x = 0, y = 0.55, z = 0 }, speedMin = 0.1, speedMax = 0.6,
        lifeMin = 1.2, lifeMax = 2.2, sizeStart = 0.9, sizeEnd = 1.6,
        color = { r = 0.9, g = 0.9, b = 0.95, a = 0.85 },
        colorEnd = { r = 0.7, g = 0.7, b = 0.8, a = 0.0 },
        gravity = -0.05, additive = false })
    -- additive glow sparks at the same spot (camera-distance fading must not
    -- wash these out either)
    EmitParticles({ pos = { x = 1.2, y = 0.25, z = 0.4 }, count = 24,
        vel = { x = 0, y = 1.2, z = 0 }, speedMin = 0.8, speedMax = 2.4,
        lifeMin = 0.5, lifeMax = 1.0, sizeStart = 0.5, sizeEnd = 0.05,
        color = { r = 1.0, g = 0.75, b = 0.3, a = 0.9 },
        colorEnd = { r = 1.0, g = 0.3, b = 0.1, a = 0.0 }, additive = true })
end

function on_update(dt)
    emit()
    if not spawned then
        spawned = true
        SpawnDecal("assets/sprites/decal_ring.png", 0, 0.05, 0, 3.0, 0.9, 0.2, 0.6, 1.0, true)
        SpawnDecal("assets/sprites/decal_disc.png", -2.0, 0.05, 1.0, 2.4, 0.75, 1.0, 0.25, 0.2, true)
    end
end

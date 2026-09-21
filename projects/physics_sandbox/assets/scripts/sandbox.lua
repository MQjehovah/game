-- Physics sandbox: exercises the Jolt-backed physics wrapper and prints a
-- compact per-second regression summary. The visual bodies (tower, thin pad,
-- CCD/discrete balls, bouncers) are authored in sandbox.json; this script owns
-- a few hidden probe bodies plus trigger/cast/overlap/joint coverage.

local frame = 0
local ZONE_OWNER = 9001
local CCD_OWNER = 9004
local zoneId = 0
local jointA, jointB, jointId = 0, 0, 0
local ccdId = 0
local started = false

function on_start(e)
    -- Trigger volume over the tower: reports every body that overlaps it.
    zoneId = PhysicsAddTriggerBox({ x = 0, y = 6, z = 0 }, { x = 4, y = 6, z = 4 },
                                  { owner = ZONE_OWNER })

    -- Hidden pair welded by a fixed joint (joint coverage; floats in space).
    jointA = PhysicsAddSphere({ x = -5, y = 4, z = 0 }, 0.5, true,
                              { owner = 9002, gravityScale = 0 })
    jointB = PhysicsAddSphere({ x = -3, y = 4, z = 0 }, 0.5, true,
                              { owner = 9003, gravityScale = 0 })
    jointId = PhysicsAddFixedJoint(jointA, jointB, { x = -4, y = 4, z = 0 })

    -- Extra hidden ball dropped with CCD onto the thin pad: verifies it lands
    -- on the pad instead of tunnelling through to the ground.
    ccdId = PhysicsAddSphere({ x = 5.5, y = 40, z = 0 }, 0.3, true,
                             { owner = CCD_OWNER, continuous = true })

    started = true
    print(string.format("[sandbox] start: zone=%d joint=%d ccd=%d bodies=%d",
                        zoneId, jointId, ccdId, PhysicsJointCount()))
end

function on_update(e, dt)
    if not started then return end
    frame = frame + 1
    if frame % 60 ~= 0 then return end

    -- Trigger overlap count (owner-first pairs).
    local zoneHits = 0
    for i = 1, #PhysicsTriggers() do
        if PhysicsTriggers()[i].trigger == ZONE_OWNER then zoneHits = zoneHits + 1 end
    end

    -- Overlap query around the tower.
    local overlaps = #PhysicsOverlapSphere({ x = 0, y = 6, z = 0 }, 5.0)

    -- Shape cast straight down over the pad (should hit its thin top).
    local cast = PhysicsSphereCast({ x = 4.5, y = 60, z = 0 }, 0.3, { x = 0, y = -1, z = 0 }, 100.0)
    local castText = cast and string.format("owner=%d d=%.1f", cast.owner, cast.distance) or "miss"

    -- CCD ball: ~2.4 on the pad (CCD worked) vs ~0.3 on the ground (tunnelled).
    local p = PhysicsGetPosition(ccdId)
    local ccdY = p and p.y or -1.0

    print(string.format("[sandbox] f=%d zoneHits=%d overlaps=%d cast[%s] ccdY=%.2f",
                        frame, zoneHits, overlaps, castText, ccdY))
end

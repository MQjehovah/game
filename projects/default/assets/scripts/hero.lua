-- Hero controller (playtest): WASD to move, Space to jump, Left-click melee,
-- 1 fireball, 2 heal. Mana + cooldowns live in GameVars (read by the HUD).
-- Movement is applied directly to the scene entity's transform.

function on_start(e)
  SetVar("hero_mana", 60)
  SetVar("hero_max_mana", 60)
  SetVar("hero_level", 1)
  SetVar("hero_xp", 0)
  SetVar("hero_gold", 0)
  SetVar("hero_yaw", 0)
  SetVar("hero_fire_cd", 0)
  SetVar("hero_heal_cd", 0)
  SetVar("hero_melee_cd", 0)
  SetVar("hero_max_hp", 100)
  SetVar("hero_on_ground", 1)
  SetVar("hero_yvel", 0)
end

function on_update(e, dt)
  -- cooldowns
  local fire_cd = GetVar("hero_fire_cd") or 0
  local heal_cd = GetVar("hero_heal_cd") or 0
  local melee_cd = GetVar("hero_melee_cd") or 0
  fire_cd = math.max(0, fire_cd - dt)
  heal_cd = math.max(0, heal_cd - dt)
  melee_cd = math.max(0, melee_cd - dt)
  SetVar("hero_fire_cd", fire_cd)
  SetVar("hero_heal_cd", heal_cd)
  SetVar("hero_melee_cd", melee_cd)

  -- mana regen
  local mana = GetVar("hero_mana") or 60
  local maxMana = GetVar("hero_max_mana") or 60
  mana = math.min(maxMana, mana + 4.0 * dt)
  SetVar("hero_mana", mana)

  -- --- movement (world axes: W = -Z, D = +X) ---
  local fwd = InputAxis("forward")   -- W-S
  local strafe = InputAxis("strafe") -- D-A
  local speed = 4.5
  local pos = GetPosition(e)
  if pos == nil then return end
  local dx = strafe * speed * dt
  local dz = -fwd * speed * dt
  pos.x = pos.x + dx
  pos.z = pos.z + dz

  -- face the movement direction (keep last facing when standing still)
  local yaw = GetVar("hero_yaw") or 0
  if fwd ~= 0 or strafe ~= 0 then
    yaw = math.atan(dx, -dz)
    SetVar("hero_yaw", yaw)
    SetRotationY(e, yaw)
  end

  -- --- jump + gravity (flat village ground at y=0) ---
  local grounded = (GetVar("hero_on_ground") or 1) > 0
  local yvel = GetVar("hero_yvel") or 0
  if InputKey("space") > 0 and grounded then
    yvel = 7.0
    grounded = false
  end
  if not grounded then
    yvel = yvel - 20.0 * dt
    pos.y = pos.y + yvel * dt
    if pos.y <= 0 then
      pos.y = 0
      yvel = 0
      grounded = true
    end
  end
  SetVar("hero_on_ground", grounded and 1 or 0)
  SetVar("hero_yvel", yvel)
  SetPosition(e, pos)

  -- --- skills ---
  local dir = { x = math.sin(yaw), y = 0, z = -math.cos(yaw) }

  if InputMousePressed("left") and melee_cd <= 0 then
    local hits = Gameplay.MeleeArc({ x = pos.x, y = pos.y + 0.8, z = pos.z }, dir, 2.4, 100, 12, e)
    melee_cd = 0.5
    SetVar("hero_melee_cd", melee_cd)
    if hits > 0 then PlaySfx("melee") end
  end

  if InputKey("1") > 0 and fire_cd <= 0 and mana >= 8 then
    SpawnProjectile({ x = pos.x + dir.x * 0.8, y = pos.y + 1.2, z = pos.z + dir.z * 0.8 },
                    dir, 14, 18, 2.5, e)
    mana = mana - 8
    fire_cd = 0.8
    SetVar("hero_mana", mana)
    SetVar("hero_fire_cd", fire_cd)
    PlaySfx("fireball")
  end

  if InputKey("2") > 0 and heal_cd <= 0 and mana >= 10 then
    local hp = GetHealth(e)
    local maxHp = GetVar("hero_max_hp") or 100
    if hp > 0 and hp < maxHp then
      SetHealth(e, math.min(maxHp, hp + 25))
      mana = mana - 10
      heal_cd = 3.0
      SetVar("hero_mana", mana)
      SetVar("hero_heal_cd", heal_cd)
      PlaySfx("heal")
    end
  end
end

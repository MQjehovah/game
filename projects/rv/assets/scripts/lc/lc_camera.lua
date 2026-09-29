-- lc_camera.lua
-- 相机导演：驻车机位/追逐/驾驶席/主角越肩

function updateCamera(dt)
  if camEnt == nil then return end
  local tx, ty, tz, lookx, looky, lookz
  if onFoot and player.active then
    local fx, fz = math.sin(player.yaw), math.cos(player.yaw)
    tx = player.x - fx * 3.6
    ty = 2.0
    tz = player.z - fz * 3.6
    -- 看向人物前方地平线（不是人物本人）：人物落在画面下沿，视野朝前
    lookx = player.x + fx * 6.0
    looky = 1.2
    lookz = player.z + fz * 6.0
  elseif phase == "driving" and driveView == "cab" then
    local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
    local cx2, cz2 = rotOf(0.45, 1.4)
    tx, ty, tz = cx2, 1.55, cz2
    lookx, looky, lookz = rv.x + fx * 12, 1.1, rv.z + fz * 12
  elseif phase == "driving" then
    local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
    local cp, sp = math.cos(DRIVE.camPitch), math.sin(DRIVE.camPitch)
    tx = rv.x - fx * DRIVE.camDist * cp
    ty = 1.4 + sp * DRIVE.camDist
    tz = rv.z - fz * DRIVE.camDist * cp
    lookx, looky, lookz = rv.x + fx * 2.5, 0.9, rv.z + fz * 2.5
  else
    tx, ty, tz = rv.x, 11.5, rv.z - 9.2
    lookx, looky, lookz = rv.x, 0.0, rv.z
  end
  if camSmooth == nil then camSmooth = { x = tx, y = ty, z = tz } end
  local k = 1 - math.exp(-5.0 * dt)
  camSmooth.x = camSmooth.x + (tx - camSmooth.x) * k
  camSmooth.y = camSmooth.y + (ty - camSmooth.y) * k
  camSmooth.z = camSmooth.z + (tz - camSmooth.z) * k
  local sx, sy, sz = camSmooth.x, camSmooth.y, camSmooth.z
  if phase == "driving" and driveView == "chase" then
    local amp = math.min(0.05, math.abs(rv.speed) * 0.005)
    sx = sx + (math.random() - 0.5) * amp
    sy = sy + (math.random() - 0.5) * amp
  end
  if recoilT > 0 then sy = sy + recoilT * 0.3 end  -- 射击后坐力（轻微上抬）
  SetPosition(camEnt, { x = sx, y = sy, z = sz })
  local dx, dy, dz = lookx - sx, looky - sy, lookz - sz
  local len = math.sqrt(dx * dx + dy * dy + dz * dz)
  if len > 1e-4 then SetLook(camEnt, dx / len, dy / len, dz / len) end
end

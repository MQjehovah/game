-- lc_camera.lua
-- 相机导演：第一人称（驻车/驾驶席）、追逐视角、改装俯视

-- 引擎 FPS 相机约定：cameraFocus = 眼睛 + 视线 * dist，
-- cameraYaw/Pitch 用引擎的 -Z 前向约定，cameraDist = 2。
function setFpsCamera(ex, ey, ez, yaw, pitch)
  -- 撞击抖动：驾驶时叠加短促的位置/角度扰动
  local sh = rv.shakeT or 0
  if sh > 0 and phase == "driving" then
    ex = ex + (math.random() - 0.5) * sh * 0.80
    ey = ey + (math.random() - 0.5) * sh * 0.60
    ez = ez + (math.random() - 0.5) * sh * 0.80
    yaw = yaw + (math.random() - 0.5) * sh * 0.12
    pitch = pitch + (math.random() - 0.5) * sh * 0.10
  end
  local cp = math.cos(pitch)
  local fx, fy, fz = math.sin(yaw) * cp, math.sin(pitch), math.cos(yaw) * cp
  SetVar("cameraMouseLock", 1)
  SetVar("cameraFocus", { x = ex + fx * 2.0, y = ey + fy * 2.0, z = ez + fz * 2.0 })
  SetVar("cameraYaw", yaw + math.pi)
  SetVar("cameraPitch", -pitch)
  SetVar("cameraDist", 2.0)
end

function setTopDownCamera()
  SetVar("cameraMouseLock", 0)
  local gy = rv.groundY or 0
  local tx, ty, tz = rv.x, 11.5 + gy, rv.z - 9.2
  if camSmooth == nil then camSmooth = { x = tx, y = ty, z = tz } end
  SetPosition(camEnt, { x = tx, y = ty, z = tz })
  local dx, dy, dz = rv.x - tx, gy - ty, rv.z - tz
  local len = math.sqrt(dx * dx + dy * dy + dz * dz)
  if len > 1e-4 then SetLook(camEnt, dx / len, dy / len, dz / len) end
end

function updateCamera(dt)
  if camEnt == nil then return end

  -- 没有第一人称主角时退回俯视（例如脚本异常/驾驶转场）
  if phase == "parked" and not (onFoot and player.active) then
    setTopDownCamera()
    return
  end

  -- 驾驶席 / 追逐
  if phase == "driving" then
    if driveView == "chase" then
      SetVar("cameraMouseLock", 0)
      local gy = rv.groundY or 0
      local fx, fz = math.sin(rv.yaw), math.cos(rv.yaw)
      local cp, sp = math.cos(DRIVE.camPitch), math.sin(DRIVE.camPitch)
      local tx = rv.x - fx * DRIVE.camDist * cp
      local ty = 1.4 + sp * DRIVE.camDist + gy
      local tz = rv.z - fz * DRIVE.camDist * cp
      SetPosition(camEnt, { x = tx, y = ty, z = tz })
      local dx, dy, dz = rv.x + fx * 2.5 - tx, 0.9 + gy - ty, rv.z + fz * 2.5 - tz
      local len = math.sqrt(dx * dx + dy * dy + dz * dz)
      if len > 1e-4 then SetLook(camEnt, dx / len, dy / len, dz / len) end
      return
    end
    -- 驾驶席第一人称：鼠标可以左右张望（限制 ±90°）
    driveLookYaw = driveLookYaw or 0
    driveLookPitch = driveLookPitch or 0
    local rawX, rawY = InputMouseX(), InputMouseY()
    local dxm = (math.abs(rawX) > 110) and 0 or math.max(-70, math.min(70, rawX))
    local dym = (math.abs(rawY) > 110) and 0 or math.max(-70, math.min(70, rawY))
    driveLookYaw = math.max(-1.5, math.min(1.5,
      driveLookYaw - dxm * SENSITIVITY))
    driveLookPitch = math.max(-0.9, math.min(0.9,
      driveLookPitch - dym * SENSITIVITY))
    -- 驾驶席在挡风玻璃后方（车头局部 z≈4.0），不要放到生活区中间，
    -- 否则车顶太阳能/行李架的背面会挡住半个屏幕。
    local ex, ez = rotOf(0.0, 3.15)
    local eyeY = 1.58 + (rv.groundY or 0) - math.sin(rv.pitch or 0) * 3.15
    setFpsCamera(ex, eyeY, ez, rv.yaw + driveLookYaw, driveLookPitch)
    return
  end

  -- 驻车第一人称
  local ex, ey, ez = playerEye()
  setFpsCamera(ex, ey, ez, player.yaw, player.pitch + (recoilT or 0) * 0.25)
end

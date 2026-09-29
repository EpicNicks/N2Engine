-- Fly camera: "Camera Move" moves relative to where the camera faces, "Camera Rotate" turns and looks.
-- Port of test_project/src/CameraController.cpp.

local CameraController = {}
CameraController.__index = CameraController

local SPEED = 10.0
local SENSITIVITY = 2.0
local MAX_PITCH = 89.0 * Math.DEG_TO_RAD

function CameraController:OnAttach()
    self.move = Vector2(0, 0)
    self.look = Vector2(0, 0)
    self.pitch = 0.0
    self.yaw = 0.0

    local controls = Input.LoadActionMap("Main Controls")
    if controls == nil then
        Debug.Warn("CameraController: 'Main Controls' action map not found")
        return
    end

    controls:Get("Camera Move"):Subscribe(function(action)
        self.move = action:GetVector2Value()
    end)
    controls:Get("Camera Rotate"):Subscribe(function(action)
        self.look = action:GetVector2Value()
    end)
end

function CameraController:OnUpdate()
    local camera = Camera.Main()
    if camera == nil then
        return
    end

    local dt = Time.deltaTime

    -- Movement
    local moveDir = Vector3(self.move.x, 0.0, -self.move.y)
    camera:SetPosition(camera:GetPosition() + camera:GetRotation() * (moveDir * (SPEED * dt)))

    -- Rotation
    if self.look.x ~= 0 or self.look.y ~= 0 then
        self.yaw = self.yaw - self.look.x * SENSITIVITY * dt
        self.pitch = math.max(-MAX_PITCH, math.min(MAX_PITCH, self.pitch + self.look.y * SENSITIVITY * dt))
    end

    camera:SetRotation(Quaternion.FromEulerAngles(self.pitch, self.yaw, 0.0))
end

return CameraController

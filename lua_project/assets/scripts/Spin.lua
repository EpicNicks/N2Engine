-- Turns its object about the world's Y axis, on top of the rotation it started with (the smoke test's textured cube
-- and robot). M ("Spin Toggle") pauses and resumes it while the camera is at its object's station.

local Spin = {}
Spin.__index = Spin

local RADIANS_PER_SECOND = 0.6
local RANGE = 12.0 -- how far from the object the camera still counts as being at its station (in x)

function Spin:OnAttach()
    self.angle = 0.0
    self.paused = false
    local positionable = self.gameObject:GetPositionable()
    self.startRotation = positionable and positionable:GetRotation() or Quaternion.FromEulerAngles(0.0, 0.0, 0.0)

    local controls = Input.LoadActionMap("Main Controls")
    local action = controls and controls:Get("Spin Toggle")
    if action ~= nil then
        action:Subscribe(function(changed)
            if changed:GetPhase() == ActionPhase.Started and self:AtStation() then
                self.paused = not self.paused
            end
        end)
    end
end

-- Whether the camera is within RANGE of the object in x
function Spin:AtStation()
    local camera = Camera.Main()
    local positionable = self.gameObject:GetPositionable()
    return camera ~= nil and positionable ~= nil
        and math.abs(camera:GetPosition().x - positionable:GetPosition().x) < RANGE
end

function Spin:OnUpdate()
    local positionable = self.gameObject:GetPositionable()
    if positionable == nil or self.paused then
        return
    end
    self.angle = self.angle + RADIANS_PER_SECOND * Time.deltaTime
    positionable:SetRotation(Quaternion.FromAxisAngle(Vector3.Up, self.angle) * self.startRotation)
end

return Spin

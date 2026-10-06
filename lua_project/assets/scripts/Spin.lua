-- Turns its object about the world's Y axis, on top of the rotation it started with (the smoke test's textured cube
-- and robot).

local Spin = {}
Spin.__index = Spin

local RADIANS_PER_SECOND = 0.6

function Spin:OnAttach()
    self.angle = 0.0
    local positionable = self.gameObject:GetPositionable()
    self.startRotation = positionable and positionable:GetRotation() or Quaternion.FromEulerAngles(0.0, 0.0, 0.0)
end

function Spin:OnUpdate()
    local positionable = self.gameObject:GetPositionable()
    if positionable == nil then
        return
    end
    self.angle = self.angle + RADIANS_PER_SECOND * Time.deltaTime
    positionable:SetRotation(Quaternion.FromAxisAngle(Vector3.Up, self.angle) * self.startRotation)
end

return Spin

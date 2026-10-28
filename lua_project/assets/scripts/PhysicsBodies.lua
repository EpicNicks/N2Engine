-- The physics station's bodies (scene.lua builds the first ones, StationKeys.lua makes new ones on keys G and Z). A
-- dynamic Rigidbody ignores changes to its object's position, so putting a body back means making a new one.
-- Each function returns an object in no scene yet; the caller adds it.

local PhysicsBodies = {}

local SPHERE_RADIUS = 1.0
local BALL_RADIUS = 0.4

-- The falling blue cube
function PhysicsBodies.CreateCube()
    local cube = GameObject.Create("TestCube")
    cube:AddComponent("CubeRenderer"):SetColor(Color.Blue)
    cube:AddComponent("BoxCollider"):SetSize(Vector3.One)
    local body = cube:AddComponent("Rigidbody")
    body:SetBodyType(BodyType.Dynamic)
    body:SetGravityEnabled(true)
    return cube
end

-- A dynamic sphere of `radius` at `position`, in `color`
local function CreateSphere(name, radius, color, position)
    local sphere = GameObject.Create(name)
    sphere:CreatePositionable()
    sphere:GetPositionable():SetPosition(position)
    local renderer = sphere:AddComponent("SphereRenderer")
    renderer:SetRadius(radius)
    renderer:SetColor(color)
    sphere:AddComponent("SphereCollider"):SetRadius(radius)
    local body = sphere:AddComponent("Rigidbody")
    body:SetGravityEnabled(true)
    body:SetBodyType(BodyType.Dynamic)
    return sphere
end

-- The falling red sphere, offset so it lands on the cube's edge
function PhysicsBodies.CreateSphere()
    return CreateSphere("TestSphere", SPHERE_RADIUS, Color.Red, Vector3(0.5, 4.0, 0.0))
end

-- A small green ball dropped from above at x
function PhysicsBodies.CreateBall(index, x)
    return CreateSphere("Test Ball " .. index, BALL_RADIUS, Color.Green, Vector3(x, 7.0, 0.0))
end

return PhysicsBodies

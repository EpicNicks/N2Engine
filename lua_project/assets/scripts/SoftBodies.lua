-- The soft-body station's bodies (scene.lua builds them, SoftBodyStation.lua rebuilds them when the backend changes:
-- a SoftBody reads its backend and resolution when it starts, so a new backend means a new body). Each is a
-- MeshRenderer plus a SoftBody on one object, with no Rigidbody. The CPU backend collides only with the plane
-- y = FLOOR_TOP (SetGround); the PhysX backend collides with the scene's colliders, so scene.lua gives the floor a
-- static BoxCollider at the same height.

local SoftBodies = {}

SoftBodies.CENTER_X = 210.0
SoftBodies.FLOOR_TOP = -1.9
-- What J cycles through, as SoftBody:SetBackend names them
SoftBodies.BACKENDS = { "Auto", "CPU", "PhysX" }

local ROBOT = "res://models/smoke_robot.gltf"
local TILT = Quaternion.FromEulerAngles(0.35, 0.0, 0.3) -- the cubes land on a corner, all the same way

-- Three cubes (soft, medium, stiff), two spheres (low and high damping) and the robot's body, side by side. A body
-- is dropped from `y` (its centre), `scale` times the mesh's size. stiffness is Young's modulus in pascals (rubber
-- is about 1e6), damping the velocity decay in 1/s; resolution is the voxels along the longest axis (the Lua API
-- has no tetrahedron count, so the signs show this).
SoftBodies.Specs = {
    {
        key = "Soft Cube", title = "Soft cube", mesh = "Cube", material = "res://materials/softbody_red.mat",
        x = -7.5, y = 3.2, scale = 1.6, tilt = TILT, stiffness = 1.0e4, damping = 0.1, resolution = 8,
    },
    {
        key = "Medium Cube", title = "Medium cube", mesh = "Cube", material = "res://materials/softbody_yellow.mat",
        x = -4.5, y = 3.2, scale = 1.6, tilt = TILT, stiffness = 1.0e5, damping = 0.1, resolution = 8,
    },
    {
        key = "Stiff Cube", title = "Stiff cube", mesh = "Cube", material = "res://materials/softbody_blue.mat",
        x = -1.5, y = 3.2, scale = 1.6, tilt = TILT, stiffness = 1.0e6, damping = 0.1, resolution = 8,
    },
    {
        key = "Low Damping", title = "Low damping", mesh = "Sphere", material = "res://materials/softbody_green.mat",
        x = 1.5, y = 3.2, scale = 1.6, stiffness = 3.0e4, damping = 0.1, resolution = 8,
    },
    {
        key = "High Damping", title = "High damping", mesh = "Sphere",
        material = "res://materials/softbody_cyan.mat",
        x = 4.5, y = 3.2, scale = 1.6, stiffness = 3.0e4, damping = 3.0, resolution = 8,
    },
    {
        -- The robot's body mesh and face material, on their own object (the robot's other nodes are not part of it)
        key = "Robot", title = "Robot body", mesh = ROBOT .. "#mesh/Body", material = ROBOT .. "#material/Face",
        x = 7.5, y = 3.2, scale = 1.0, stiffness = 5.0e4, damping = 0.3, resolution = 8,
    },
}

-- "1e+04" -> "1e4": a short stiffness for a sign
function SoftBodies.Short(number)
    return (string.format("%.0e", number):gsub("e%+?0*(%d)", "e%1"))
end

-- A new body object (in no scene yet: the caller adds it, which starts the body with `backend`) and its SoftBody
function SoftBodies.Create(spec, backend)
    local gameObject = GameObject.Create("Soft Body " .. spec.key)
    gameObject:CreatePositionable()
    local positionable = gameObject:GetPositionable()
    positionable:SetPosition(Vector3(SoftBodies.CENTER_X + spec.x, spec.y, 0.0))
    positionable:SetScale(Vector3(spec.scale, spec.scale, spec.scale))
    if spec.tilt ~= nil then
        positionable:SetRotation(spec.tilt)
    end
    local renderer = gameObject:AddComponent("MeshRenderer")
    renderer:SetMesh(spec.mesh)
    renderer:SetMaterial(1, spec.material)
    local body = gameObject:AddComponent("SoftBody")
    body:SetBackend(backend)
    body:SetResolution(spec.resolution)
    body:SetStiffness(spec.stiffness)
    body:SetDamping(spec.damping)
    body:SetGround(true, SoftBodies.FLOOR_TOP)
    return gameObject, body
end

return SoftBodies

-- Builds the same scene as TestEngine() in test_project/src/Main.cpp:
-- a falling cube and sphere over a static floor, a fly camera and Escape to quit.

local scene = SceneManager.GetCurrentScene()
if scene == nil then
    error("scene.lua needs a loaded scene")
end

Window.SetClearColor(Color.Magenta)
Window.SetMode(WindowMode.Windowed)

-- Input: created before the behaviour scripts attach, since they subscribe to these actions.
-- Binding fields and names follow the action map JSON format (keys like "W", axes like "LeftX").
local controls = Input.CreateActionMap("Main Controls", {
    ["Camera Move"] = {
        { type = "Vector2Composite", up = "W", down = "S", left = "A", right = "D" },
        { type = "GamepadStick", xAxis = "LeftX", yAxis = "LeftY", deadzone = 0.25, invertY = true },
    },
    ["Camera Rotate"] = {
        { type = "Vector2Composite", up = "Up", down = "Down", left = "Left", right = "Right" },
        { type = "GamepadStick", xAxis = "RightX", yAxis = "RightY", deadzone = 0.25 },
    },
    ["Quit"] = {
        { type = "KeyboardButton", key = "Escape" },
    },
})
if controls ~= nil then
    Input.LoadActionMap("Main Controls")
end

-- Falling cube
local cube = GameObject.Create("TestCube")
cube:AddComponent("CubeRenderer"):SetColor(Color.Blue)
cube:AddComponent("BoxCollider"):SetSize(Vector3.One)
local cubeBody = cube:AddComponent("Rigidbody")
cubeBody:SetBodyType(BodyType.Dynamic)
cubeBody:SetGravityEnabled(true)

-- Falling sphere, offset so it lands on the cube's edge
local SPHERE_RADIUS = 1.0
local sphere = GameObject.Create("TestSphere")
sphere:CreatePositionable()
sphere:GetPositionable():SetPosition(Vector3(0.5, 4.0, 0.0))
local sphereRenderer = sphere:AddComponent("SphereRenderer")
sphereRenderer:SetRadius(SPHERE_RADIUS)
sphereRenderer:SetColor(Color.Red)
sphere:AddComponent("SphereCollider"):SetRadius(SPHERE_RADIUS)
local sphereBody = sphere:AddComponent("Rigidbody")
sphereBody:SetGravityEnabled(true)
sphereBody:SetBodyType(BodyType.Dynamic)

-- Static floor (a collider without a Rigidbody doesn't move)
local FLOOR_SIZE = Vector3(30.0, 1.0, 30.0)
local floor = GameObject.Create("TestFloor")
floor:CreatePositionable()
floor:GetPositionable():SetPosition(Vector3(0.0, -5.0, 0.0))
floor:AddComponent("CubeRenderer"):SetSize(FLOOR_SIZE)
floor:AddComponent("BoxCollider"):SetSize(FLOOR_SIZE)

-- Behaviours
local cameraController = GameObject.Create("Camera Controller")
cameraController:AddComponent("LuaComponent"):SetScript("res://scripts/CameraController.lua")

local quitHandler = GameObject.Create("Quit Handler")
quitHandler:AddComponent("LuaComponent"):SetScript("res://scripts/QuitHandler.lua")

for _, gameObject in ipairs({ cube, sphere, floor, cameraController, quitHandler }) do
    scene:AddRootGameObject(gameObject)
end

Debug.Log("scene.lua: scene built")

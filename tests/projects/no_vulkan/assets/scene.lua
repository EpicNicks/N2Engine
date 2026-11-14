-- A game that leaves the Vulkan and WebGPU renderers out and keeps every other module (docs/project-modules.html). The Linux CI
-- job builds its player, checks that --renderer vulkan is refused and runs it on the software renderer, and prints what that saves.

local scene = SceneManager.GetCurrentScene()
if scene == nil then
    error("scene.lua needs a loaded scene")
end

Window.SetClearColor(Color.new(0.12, 0.13, 0.17, 1.0))

local cube = GameObject.Create("Cube")
cube:CreatePositionable()
local renderer = cube:AddComponent("CubeRenderer")
renderer:SetSize(Vector3(3.0, 3.0, 3.0))
renderer:SetColor(Color.Red)
scene:AddRootGameObject(cube)

Debug.Log("scene.lua: project without the Vulkan renderer running")

-- A game that leaves the audio and UI modules out and keeps every other module (docs/project-modules.html). The web CI job
-- builds and runs a package of it, which is what shows a player without them still starts and draws, and what they cost.

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

Debug.Log("scene.lua: project without audio or UI running")

-- The smallest game: it leaves every optional module out, physics, particles and soft bodies (docs/project-modules.html). The web
-- CI job builds and runs a package of it, which is what shows a player without them still starts and draws.

local scene = SceneManager.GetCurrentScene()
if scene == nil then
    error("scene.lua needs a loaded scene")
end

Window.SetClearColor(Color.new(0.12, 0.13, 0.17, 1.0))

local cube = GameObject.Create("Cube")
cube:CreatePositionable()
local renderer = cube:AddComponent("CubeRenderer")
renderer:SetSize(Vector3(3.0, 3.0, 3.0))
renderer:SetColor(Color.Green)
scene:AddRootGameObject(cube)

Debug.Log("scene.lua: no-physics project running")

-- The features of issue #445 used together in one scene (../README.md). Run this in an empty scene: it adds one object,
-- the Director, whose script builds the rest, drives it for two seconds and checks what happened.

local scene = SceneManager.GetCurrentScene()
if scene == nil then
    error("main.lua needs a loaded scene")
end

Window.SetClearColor(Color.new(0.0, 0.0, 0.0, 1.0))

local director = GameObject.Create("Director")
scene:AddRootGameObject(director)
director:AddComponent("LuaComponent"):SetScript("res://scripts/Director.lua")

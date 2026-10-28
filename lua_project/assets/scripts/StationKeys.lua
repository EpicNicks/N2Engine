-- The keys of the smoke test's stations 1 to 6 and demo stations 9 and 10 (the particle station, 8, and the soft-body
-- station, 11, have their own scripts). Each key acts only while the camera is at its station, as in
-- SoftBodyStation.lua; the HUD text of each station (SmokeStations.lua, DemoStations.lua) names them. The spin toggle
-- on M is in Spin.lua.
--   1 Physics       F drops a green ball, Z puts the cube and sphere back and removes the balls
--   2 World text    T changes the large sample text
--   3 Text effects  T cycles the size of the five sample words
--   4 World canvas  Space clicks the orange button (the same Button:Click a mouse click ends in)
--   5 Materials     N cycles the textured cube's material
--   9 World canvas  Space clicks button A. Button B has no key: Click would skip the solid block in front of it
--  10 Text passes   O re-orders the "Before" word's two passes as the "After" word has them, and back

local PhysicsBodies = require("scripts.PhysicsBodies")

local StationKeys = {}
StationKeys.__index = StationKeys

local RANGE = 12.0 -- how far from the station's camera the camera still counts as being at it (in x)
local MAX_BALLS = 12
local BALL_XS = { -2.5, 1.5, -0.5, 3.0, -4.0, 0.5 } -- where F drops the balls, in turn
local LARGE_TEXTS = { "SDF text 1.0", "Quartz jumps 1.0", "WAVE Fi 0123 1.0" }
local EFFECT_WORDS = { "Outline", "Shadow", "Glow", "Softness", "Combined" }
local EFFECT_SIZES = { 0.7, 0.5, 0.8 } -- em, 0.7 as scene.lua builds them
local CUBE_MATERIALS = { "res://materials/checker_lit.mat", "res://materials/lit_white.mat",
    "res://materials/unlit_white.mat" }

-- Calls fn when the named action starts, if the map has it
local function OnStarted(controls, name, fn)
    local action = controls:Get(name)
    if action ~= nil then
        action:Subscribe(function(changed)
            if changed:GetPhase() == ActionPhase.Started then
                fn()
            end
        end)
    end
end

-- The component `type` of the scene's object called `name`, or nil
local function ComponentOf(name, type)
    local scene = SceneManager.GetCurrentScene()
    local object = scene and scene:FindGameObject(name)
    return object and object:GetComponent(type)
end

function StationKeys:OnAttach()
    self.balls = {}
    self.ballCount = 0
    self.largeTextIndex = 1
    self.effectSizeIndex = 1
    self.materialIndex = 1
    self.passesOrdered = false
    self.beforeCaption = nil

    local controls = Input.LoadActionMap("Main Controls")
    if controls == nil then
        Debug.Warn("StationKeys: 'Main Controls' action map not found")
        return
    end
    OnStarted(controls, "Physics Drop Ball", function() self:At(-3.5, function() self:DropBall() end) end)
    OnStarted(controls, "Physics Reset", function() self:At(-3.5, function() self:ResetPhysics() end) end)
    OnStarted(controls, "Text Next", function()
        self:At(-30.8, function() self:NextLargeText() end)
        self:At(-60.6, function() self:NextEffectSize() end)
    end)
    OnStarted(controls, "Press Button", function()
        self:At(30.0, function() self:Click("World Button") end)
        self:At(150.0, function() self:Click("Trigger Demo A Button") end)
    end)
    OnStarted(controls, "Material Next", function() self:At(59.4, function() self:NextCubeMaterial() end) end)
    OnStarted(controls, "Passes Order", function() self:At(180.0, function() self:TogglePassOrder() end) end)
end

-- Runs fn when the camera is within RANGE of `x`, a station's camera x
function StationKeys:At(x, fn)
    local camera = Camera.Main()
    if camera ~= nil and math.abs(camera:GetPosition().x - x) < RANGE then
        fn()
    end
end

-- Adds a new object to the current scene
local function Spawn(gameObject)
    local scene = SceneManager.GetCurrentScene()
    if scene ~= nil then
        scene:AddRootGameObject(gameObject)
    end
end

-- Destroys the scene's object called `name`, if there is one
local function Remove(name)
    local scene = SceneManager.GetCurrentScene()
    local object = scene and scene:FindGameObject(name)
    if object ~= nil then
        scene:DestroyGameObject(object)
    end
end

-- Station 1: a ball from above, the oldest going when there are MAX_BALLS
function StationKeys:DropBall()
    local scene = SceneManager.GetCurrentScene()
    if scene == nil then
        return
    end
    self.ballCount = self.ballCount + 1
    local ball = PhysicsBodies.CreateBall(self.ballCount, BALL_XS[(self.ballCount - 1) % #BALL_XS + 1])
    table.insert(self.balls, ball)
    Spawn(ball)
    if #self.balls > MAX_BALLS then
        scene:DestroyGameObject(table.remove(self.balls, 1))
    end
end

-- Station 1: new cube and sphere above the floor, no balls
function StationKeys:ResetPhysics()
    local scene = SceneManager.GetCurrentScene()
    if scene == nil then
        return
    end
    for _, ball in ipairs(self.balls) do
        scene:DestroyGameObject(ball)
    end
    self.balls = {}
    Remove("TestCube")
    Remove("TestSphere")
    Spawn(PhysicsBodies.CreateCube())
    Spawn(PhysicsBodies.CreateSphere())
end

-- Station 2
function StationKeys:NextLargeText()
    local text = ComponentOf("Text Large", "TextRenderer")
    if text ~= nil then
        self.largeTextIndex = self.largeTextIndex % #LARGE_TEXTS + 1
        text:SetText(LARGE_TEXTS[self.largeTextIndex])
    end
end

-- Station 3: the effects are in ems, so they grow and shrink with the word
function StationKeys:NextEffectSize()
    self.effectSizeIndex = self.effectSizeIndex % #EFFECT_SIZES + 1
    for _, word in ipairs(EFFECT_WORDS) do
        local text = ComponentOf("Effect " .. word, "TextRenderer")
        if text ~= nil then
            text:SetFontSize(EFFECT_SIZES[self.effectSizeIndex])
        end
    end
end

-- Stations 4 and 9: the Button on the object called `name`
function StationKeys:Click(name)
    local button = ComponentOf(name, "Button")
    if button ~= nil then
        button:Click()
    end
end

-- Station 5
function StationKeys:NextCubeMaterial()
    local cube = ComponentOf("Material Textured Cube", "MeshRenderer")
    if cube ~= nil then
        self.materialIndex = self.materialIndex % #CUBE_MATERIALS + 1
        cube:SetMaterial(1, CUBE_MATERIALS[self.materialIndex])
    end
end

-- Station 10: the "Before" word's passes, in added order (red hides yellow) or with orders (yellow in front)
function StationKeys:TogglePassOrder()
    local before = ComponentOf("Pass Before", "TextRenderer")
    local caption = ComponentOf("Pass Before Caption", "TextRenderer")
    if before == nil then
        return
    end
    self.passesOrdered = not self.passesOrdered
    before:ClearEffectPasses()
    if self.passesOrdered then
        before:AddEffectPass(0.0, 0.0, Color.Yellow, 0.05, 0.0, 0)
        before:AddEffectPass(0.0, 0.0, Color.Red, 0.11, 0.0, -10)
    else
        before:AddEffectPass(0.0, 0.0, Color.Yellow, 0.05)
        before:AddEffectPass(0.0, 0.0, Color.Red, 0.11)
    end
    if caption ~= nil then
        if self.beforeCaption == nil then
            self.beforeCaption = caption:GetText()
        end
        caption:SetText(self.passesOrdered and "O pressed: the same two passes with orders red -10, yellow 0, so the "
            .. "yellow is in front, as in After. O again: added order." or self.beforeCaption)
    end
end

return StationKeys

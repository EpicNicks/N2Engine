-- Builds the scene, drives it and checks it (issue #445). The results are on the instance: `done` once the checks have
-- run, `passed` (how many held) and `failures` (the names of those that didn't). They are also printed and shown on screen.

local Director = {}
Director.__index = Director

local CHECK_AT_FRAME = 120
local FLOOR_TOP = -3.5

local function root(gameObject)
    SceneManager.GetCurrentScene():AddRootGameObject(gameObject)
    return gameObject
end

-- A new object in the world at (x, y). GameObject.Create makes an object without a position, so one is added
local function thing(name, x, y)
    local gameObject = root(GameObject.Create(name))
    gameObject:CreatePositionable()
    gameObject:GetPositionable():SetPosition(Vector3.new(x, y, 0))
    return gameObject
end

-- An element centred on the canvas, `x` and `y` from its middle, in canvas units
local function place(parent, element, x, y, width, height)
    parent:AddChild(element)
    local rect = element:GetComponent("RectTransform")
    rect:SetAnchorMin(Vector2.new(0.5, 0.5))
    rect:SetAnchorMax(Vector2.new(0.5, 0.5))
    rect:SetPivot(Vector2.new(0.5, 0.5))
    rect:SetAnchoredPosition(Vector2.new(x, y))
    rect:SetSizeDelta(Vector2.new(width, height))
    return rect
end

function Director:BuildHud()
    -- Laid out for 400 x 300 and scaled with the window
    local canvas = root(UI.CreateCanvas("Hud"))
    self.scaler = canvas:AddComponent("CanvasScaler")
    self.scaler:SetReferenceResolution(Vector2.new(400, 300))

    -- A turned panel that masks a larger child, so the child is cut to the turned outline
    local panel = UI.CreateElement("Turned Panel")
    panel:AddComponent("Image"):SetColor(Color.new(0.15, 0.2, 0.6, 1.0))
    self.panel = place(canvas, panel, -120, 80, 100, 60)
    self.panel:SetRotation(30)
    self.panel:SetScale(Vector2.new(1.2, 1.0))
    panel:AddComponent("RectMask")
    local stripe = UI.CreateElement("Stripe")
    stripe:AddComponent("Image"):SetColor(Color.new(0.9, 0.7, 0.1, 1.0))
    place(panel, stripe, 0, 0, 300, 12)

    -- A menu for the keyboard and gamepad: arrows move, Enter or South presses
    self.presses = { Start = 0, Options = 0, Quit = 0 }
    self.buttons = {}
    for index, name in ipairs({ "Start", "Options", "Quit" }) do
        local button = UI.CreateButton(name, name)
        place(canvas, button, 120, 110 - (index - 1) * 36, 120, 30)
        button:GetComponent("Button"):AddOnClick(function()
            self.presses[name] = self.presses[name] + 1
        end)
        self.buttons[name] = button
    end

    local title = UI.CreateText("Title", "A title far too long to fit the space it was given")
    place(canvas, title, -100, 130, 160, 20)
    self.title = title:GetComponent("UIText")
    self.title:SetFontSize(14)
    self.title:SetWrap(false)
    self.title:SetOverflow("Ellipsis")

    local rich = UI.CreateText("Rich", "<b>Bold</b>, <i>italic</i>, <color=#FF8040>orange</color>, <size=150%>big</size>")
    place(canvas, rich, 0, -110, 360, 30)
    self.rich = rich:GetComponent("UIText")
    self.rich:SetFontSize(14)
    self.rich:SetRichText(true)

    local status = UI.CreateText("Status", "running")
    place(canvas, status, 0, -135, 360, 20)
    self.status = status:GetComponent("UIText")
    self.status:SetFontSize(12)
end

function Director:BuildWorld()
    local camera = Camera.Main()
    if camera ~= nil then
        camera:SetPosition(Vector3.new(0, 0, 10))
        camera:SetOrthographic(-8, 8, -6, 6, 0.1, 100)
    end

    local floor = thing("Floor", 0, FLOOR_TOP - 0.5)
    floor:AddComponent("BoxCollider2D"):SetSize(Vector2.new(16, 1))
    floor:AddComponent("Sprite2D"):SetSize(Vector2.new(16, 1))

    -- A soft ball dropped on the floor
    local ball = thing("Jelly", -3, 0)
    self.jelly = ball:AddComponent("SoftBody2D")
    self.jelly:SetCircle(1.0, 16)
    self.jelly:SetColor(Color.new(0.9, 0.3, 0.5, 1.0))

    -- Sparks that land on the floor. Collision is set before the system starts, so it runs on the CPU backend
    local fountain = thing("Sparks", 3, -1)
    local sparks = fountain:AddComponent("ParticleSystem")
    sparks:SetShape("Circle")
    sparks:SetShapeRadius(0.3)
    sparks:SetEmissionRate(60)
    sparks:SetLifetime(3, 4)
    sparks:SetSpeed(0.5, 1.5)
    sparks:SetSize(0.15, 0.05)
    sparks:SetColliderCollision(true, false)
    sparks:SetCollisionResponse(0.05, 0.3, 0.2, 0.1)
    self.sparks = sparks
    self.hits = 0

    -- A user shader: drawn by its "fallback" where it can't run (the software renderer)
    local lamp = thing("Lamp", 0, 2)
    self.lamp = lamp:AddComponent("Sprite2D")
    self.lamp:SetSize(Vector2.new(1.5, 1.5))
    self.lamp:SetShader("res://shaders/pulse.n2shader")

    -- Full-screen effects on the scene; the HUD is drawn after them
    local effects = root(GameObject.Create("Effects"))
    self.effects = effects:AddComponent("PostProcessStack")
    self.effects:Set({ blur = true, blurStrength = 0.4, distortion = 0.15, waveAmplitude = 0.004, waveSpeed = 0.5 })
end

function Director:OnAttach()
    self.frames = 0
    self.checked = false
    self.done = false
    self.passed = 0
    self.failures = {}
    self:BuildHud()
    self:BuildWorld()
end

function Director:OnFixedUpdate()
    if self.sparks ~= nil then
        self.hits = self.hits + self.sparks:GetCollisionCount()
    end
end

function Director:Check(name, condition)
    if condition then
        self.passed = self.passed + 1
    else
        table.insert(self.failures, name)
        print("features-445: FAILED " .. name)
    end
end

function Director:RunChecks()
    self:Check("the canvas scales with the window", self.scaler:GetAppliedScale() > 0)

    local corners = self.panel:GetCanvasCorners()
    self:Check("the panel is turned", math.abs(corners[1].y - corners[4].y) > 1)
    local there = self.panel:LocalToCanvas(Vector2.new(10, 5))
    local back = self.panel:CanvasToLocal(there)
    self:Check("canvas and local points round-trip",
        back ~= nil and math.abs(back.x - 10) < 0.01 and math.abs(back.y - 5) < 0.01)

    UI.SetSelected(self.buttons.Start)
    self:Check("Down moves the selection", UI.MoveSelection("Down"))
    local selected = UI.GetSelected()
    self:Check("the selection is on Options", selected ~= nil and selected:GetName() == "Options")
    UI.Submit()
    self:Check("Submit presses the selected button",
        self.presses.Options == 1 and self.presses.Start == 0 and self.presses.Quit == 0)

    self:Check("the long title overflows its rect", self.title:IsOverflowing())
    self:Check("rich text hides its tags", self.rich:GetPlainText() == "Bold, italic, orange, big")

    local centre = self.jelly:GetCentre()
    self:Check("the soft body has a body", self.jelly:HasBody())
    self:Check("the soft body fell and rests on the floor", centre.y < -1.5 and centre.y > FLOOR_TOP)

    local lowest = math.huge
    for _, particle in ipairs(self.sparks:GetParticles()) do
        lowest = math.min(lowest, particle.position.y)
    end
    self:Check("particles hit the floor", self.hits > 0)
    self:Check("no particle fell through the floor", lowest > FLOOR_TOP - 0.01)

    self:Check("the sprite uses the user shader", self.lamp:GetShader() == "res://shaders/pulse.n2shader")
    local fields = self.effects:Get()
    self:Check("the post-processing settings are kept",
        fields.blur == true and math.abs(fields.distortion - 0.15) < 1e-5)
end

function Director:OnUpdate()
    self.frames = self.frames + 1
    if self.checked or self.frames < CHECK_AT_FRAME then
        return
    end
    self.checked = true -- once, even if a check raises an error: `done` then stays false
    self:RunChecks()
    self.done = true
    local summary = string.format("%d checks passed, %d failed", self.passed, #self.failures)
    self.status:SetText(summary)
    print("features-445: " .. summary)
end

return Director

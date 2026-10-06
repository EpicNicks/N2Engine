-- The GPU smoke test (docs/testing.html, "GPU smoke test"): one scene that runs every rendering path CI can't see,
-- as six stations the camera jumps between with keys 1-6 (scripts/SmokeStations.lua), plus a screen-space UI panel.
-- Each item has a caption saying what correct output looks like; the HUD says what to look for at each station.
--   1 Physics       the original test scene, as TestEngine() in test_project/src/Main.cpp builds it: a falling cube
--                   and sphere over a static floor
--   2 World text    TextRenderer at three sizes and a wrapped, centred paragraph
--   3 Text effects  outline, shadow, glow, softness and all combined
--   4 World canvas  a world-space Canvas with an Image, UIText and a Button clicked through the camera's ray
--   5 Materials     a lit textured cube from a .mat file, lit and unlit spheres, opaque / blend / mask side by side
--   6 glTF model    models/smoke_robot.gltf (lua_project/tools/make_smoke_assets.py) instantiated twice
--   UI panel        screen-space Canvas: a sprite Image, UIText alignment and wrapping, Buttons with a counter

local Stations = require("scripts.SmokeStations")

local scene = SceneManager.GetCurrentScene()
if scene == nil then
    error("scene.lua needs a loaded scene")
end

Window.SetClearColor(Color.new(0.12, 0.13, 0.17, 1.0))
Window.SetMode(WindowMode.Windowed)

-- Input: created before the behaviour scripts attach, since they subscribe to these actions.
-- Binding fields and names follow the action map JSON format (keys like "W", axes like "LeftX").
local actions = {
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
    ["Toggle UI Panel"] = {
        { type = "KeyboardButton", key = "U" },
    },
}
for index = 1, #Stations do
    actions["Station " .. index] = { { type = "KeyboardButton", key = "Key" .. index } }
end
local controls = Input.CreateActionMap("Main Controls", actions)
if controls ~= nil then
    Input.LoadActionMap("Main Controls")
end

local roots = {}

-- Gives the object a Positionable at `position` (and `scale`, when given)
local function Place(gameObject, position, scale)
    gameObject:CreatePositionable()
    local positionable = gameObject:GetPositionable()
    positionable:SetPosition(position)
    if scale ~= nil then
        positionable:SetScale(scale)
    end
    return gameObject
end

-- A root object with a TextRenderer, its first line's top-left at `position`; returns the TextRenderer
local function WorldText(name, text, position, fontSize, color, maxWidth)
    local gameObject = GameObject.Create(name)
    local renderer = gameObject:AddComponent("TextRenderer")
    renderer:SetText(text)
    renderer:SetFontSize(fontSize)
    renderer:SetColor(color or Color.White)
    if maxWidth ~= nil then
        renderer:SetMaxWidth(maxWidth)
    end
    Place(gameObject, position)
    table.insert(roots, gameObject)
    return renderer
end

local CAPTION = Color.new(1.0, 0.92, 0.55, 1.0)
local DARK_CAPTION = Color.new(0.05, 0.05, 0.08, 1.0)

-- A station's title sign, "N  Name", at the top-left of its view
local function StationTitle(index, position)
    WorldText("Station " .. index .. " Title", index .. "  " .. Stations[index].name, position, 0.45, Color.White)
end

-- A root object drawing a built-in mesh with a .mat material
local function MeshObject(name, mesh, materialPath, position, scale)
    local gameObject = GameObject.Create(name)
    local renderer = gameObject:AddComponent("MeshRenderer")
    renderer:SetMesh(mesh)
    renderer:SetMaterial(1, materialPath)
    Place(gameObject, position, scale)
    table.insert(roots, gameObject)
    return gameObject
end

-- A root object with an unlit, coloured CubeRenderer
local function ColouredCube(name, color, position, size)
    local gameObject = GameObject.Create(name)
    local renderer = gameObject:AddComponent("CubeRenderer")
    renderer:SetColor(color)
    renderer:SetSize(size)
    Place(gameObject, position)
    table.insert(roots, gameObject)
    return gameObject
end

-- ===== UI helpers =====

-- Places a UI element by its top-left corner, measured from its parent's top-left (x right, yTop down)
local function Layout(element, x, yTop, width, height)
    local rect = element:GetComponent("RectTransform")
    rect:SetAnchorMin(Vector2(0, 1))
    rect:SetAnchorMax(Vector2(0, 1))
    rect:SetPivot(Vector2(0, 1))
    rect:SetAnchoredPosition(Vector2(x, -yTop))
    rect:SetSizeDelta(Vector2(width, height))
    return element
end

-- An element under `parent` with an Image of `color` (raycastTarget off unless `raycast`)
local function Panel(parent, name, color, raycast)
    local element = UI.CreateElement(name)
    parent:AddChild(element)
    local image = element:AddComponent("Image")
    image:SetColor(color)
    image:SetRaycastTarget(raycast == true)
    return element, image
end

-- A UIText under `parent`; options: size, color, h, v (alignment), wrap
local function Label(parent, name, text, options)
    local element = UI.CreateText(name, text)
    parent:AddChild(element)
    local uiText = element:GetComponent("UIText")
    uiText:SetFontSize(options.size or 16)
    uiText:SetColor(options.color or Color.White)
    uiText:SetAlignment(options.h or "Left", options.v or "Top")
    uiText:SetWrap(options.wrap ~= false)
    return element, uiText
end

-- A Button under `parent` whose Image is `color` and that gets darker when hovered and darker still when pressed
local function TintedButton(parent, name, label, color, labelSize)
    local element = UI.CreateButton(name, label)
    parent:AddChild(element)
    element:GetComponent("Image"):SetColor(color)
    local button = element:GetComponent("Button")
    button:SetNormalColor(Color.White)
    button:SetHighlightedColor(Color.new(0.75, 0.75, 0.75, 1.0))
    button:SetPressedColor(Color.new(0.45, 0.45, 0.45, 1.0))
    button:SetDisabledColor(Color.new(0.6, 0.6, 0.6, 0.4))
    button:SetFadeDuration(0.1)
    element:FindChild("Label"):GetComponent("UIText"):SetFontSize(labelSize or 20)
    return element, button
end

-- ===== Station 1: physics (the original test scene) =====

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

for _, gameObject in ipairs({ cube, sphere, floor }) do
    table.insert(roots, gameObject)
end

StationTitle(1, Vector3(-9.0, 7.0, 0.0))
WorldText("Physics Caption", "The blue cube and the red sphere fall onto the grey floor and settle; the sphere "
    .. "rolls off the cube's edge.", Vector3(-9.0, 6.2, 0.0), 0.3, CAPTION, 8.0)

-- ===== Station 2: world text (TextRenderer) =====

do
    local x = -35.3
    StationTitle(2, Vector3(x, 3.9, 0.0))
    WorldText("Text Large", "SDF text 1.0", Vector3(x, 3.2, 0.0), 1.0)
    WorldText("Text Medium", "Medium, 0.5 units per em", Vector3(x, 1.7, 0.0), 0.5)
    WorldText("Text Small", "Small, 0.25 units per em: the quick brown fox jumps over the lazy dog 0123456789",
        Vector3(x, 0.9, 0.0), 0.25, Color.White, 8.0)
    local paragraph = WorldText("Text Paragraph", "This paragraph has a 6 unit wrap width and centre alignment, so "
        .. "it breaks onto several lines, each centred under the one above.", Vector3(-31.3, -0.2, 0.0), 0.3,
        Color.new(0.55, 0.75, 1.0, 1.0), 6.0)
    paragraph:SetAlignment("Center", "Top")
    WorldText("Text Caption", "Look for: crisp letters with smooth edges at every size (fly closer with W: they "
        .. "stay sharp), no boxes, blocks or stray marks around them, even spacing; the blue paragraph on several "
        .. "centred lines.", Vector3(x, -2.5, 0.0), 0.2, CAPTION, 9.0)
end

-- ===== Station 3: text effects =====

do
    local board = GameObject.Create("Effects Board")
    local boardRenderer = board:AddComponent("QuadRenderer")
    boardRenderer:SetColor(Color.new(0.5, 0.5, 0.55, 1.0))
    boardRenderer:SetSize(Vector3(10.6, 8.2, 1.0))
    Place(board, Vector3(-60.6, 0.4, -0.05))
    table.insert(roots, board)

    local x = -65.4
    StationTitle(3, Vector3(x, 4.1, 0.0))

    local function Effect(name, y, caption)
        local renderer = WorldText("Effect " .. name, name, Vector3(x, y, 0.0), 0.7)
        WorldText("Effect " .. name .. " Caption", caption, Vector3(-61.6, y - 0.05, 0.0), 0.2, DARK_CAPTION, 5.0)
        return renderer
    end

    Effect("Outline", 3.2, "White letters with an even red rim, about 0.08 em, all round."):SetOutline(0.08, Color.Red)
    Effect("Shadow", 1.9, "A hard black copy of each letter, offset down and to the right.")
        :SetShadow(0.08, -0.08, Color.Black)
    Effect("Glow", 0.6, "A soft cyan halo fading out around the letters (OpenGL; none on software)."):SetShadow(0.0,
        0.0, Color.Cyan, 0.12)
    Effect("Softness", -0.7, "Blurred letter edges, fading over about 0.1 em (OpenGL; hard on software).")
        :SetSoftness(0.12)
    local combined = Effect("Combined", -2.0, "Yellow letters, thin black outline, soft shadow down-right, "
        .. "slightly soft edges.")
    combined:SetColor(Color.Yellow)
    combined:SetOutline(0.05, Color.Black)
    combined:SetShadow(0.06, -0.06, Color.new(0.0, 0.0, 0.0, 0.7), 0.04)
    combined:SetSoftness(0.03)
end

-- ===== Station 4: world-space canvas =====

local worldClicks = 0
do
    local canvas = UI.CreateCanvas("Smoke World Canvas", "WorldSpace") -- scale 0.01: 600 x 400 is 6 x 4 units
    canvas:GetComponent("Canvas"):SetSize(Vector2(600, 400))
    canvas:GetPositionable():SetPosition(Vector3(30.0, 0.3, 0.0))
    table.insert(roots, canvas)

    local background = Panel(canvas, "World Background", Color.new(0.1, 0.25, 0.55, 0.75))
    background:GetComponent("RectTransform"):StretchToParent()

    Layout(Label(canvas, "World Title", "4  World-space canvas", { size = 36, h = "Center" }), 20, 16, 560, 48)

    local icon = Layout(UI.CreateElement("World Icon"), 30, 110, 140, 140)
    canvas:AddChild(icon)
    icon:AddComponent("Image"):SetSprite("res://ui/icon.png")

    local buttonObject, button = TintedButton(canvas, "World Button", "Click me", Color.new(1.0, 0.6, 0.15, 1.0), 30)
    Layout(buttonObject, 230, 120, 300, 80)
    local counter, counterText = Label(canvas, "World Counter", "World clicks: 0",
        { size = 28, h = "Center", v = "Middle", wrap = false })
    Layout(counter, 230, 215, 300, 44)
    button:AddOnClick(function()
        worldClicks = worldClicks + 1
        counterText:SetText("World clicks: " .. worldClicks)
    end)

    Layout(Label(canvas, "World Caption", "Hover: darker. Hold: darker still. Release over it: +1. The red cube "
        .. "behind shows through; the green cube in front hides the lower-left corner.", { size = 20 }),
        110, 290, 470, 100)

    ColouredCube("World Canvas Behind", Color.Red, Vector3(32.6, 1.3, -1.5), Vector3(1.2, 1.2, 1.2))
    ColouredCube("World Canvas In Front", Color.Green, Vector3(27.3, -1.4, 0.8), Vector3(0.8, 0.8, 0.8))
end

-- ===== Station 5: meshes and materials =====

do
    StationTitle(5, Vector3(55.5, 4.0, 0.0))

    local texturedCube = MeshObject("Material Textured Cube", "Cube", "res://materials/checker_lit.mat",
        Vector3(56.8, 2.2, 0.0), Vector3(1.4, 1.4, 1.4))
    texturedCube:GetPositionable():SetRotation(Quaternion.FromEulerAngles(0.45, 0.6, 0.0))
    texturedCube:AddComponent("LuaComponent"):SetScript("res://scripts/Spin.lua")
    MeshObject("Material Lit Sphere", "Sphere", "res://materials/lit_white.mat", Vector3(59.6, 2.2, 0.0),
        Vector3(1.5, 1.5, 1.5))
    MeshObject("Material Unlit Sphere", "Sphere", "res://materials/unlit_white.mat", Vector3(62.2, 2.2, 0.0),
        Vector3(1.5, 1.5, 1.5))
    WorldText("Material Cube Caption", "Lit, textured (.mat): checker upright on each face (red top-left); faces "
        .. "brighten and darken as it turns.", Vector3(55.6, 1.1, 0.0), 0.15, CAPTION, 2.5)
    WorldText("Material Lit Caption", "Lit: bright upper left, dark lower right, a small highlight.",
        Vector3(58.5, 1.1, 0.0), 0.15, CAPTION, 2.3)
    WorldText("Material Unlit Caption", "Unlit: a flat white disc, no shading.", Vector3(61.1, 1.1, 0.0), 0.15,
        CAPTION, 2.3)

    ColouredCube("Material Bar", Color.Yellow, Vector3(59.4, -1.3, -0.6), Vector3(8.2, 0.5, 0.2))
    local quadScale = Vector3(2.0, 2.0, 1.0)
    MeshObject("Material Opaque Quad", "Quad", "res://materials/alpha_opaque.mat", Vector3(56.8, -1.3, 0.0), quadScale)
    MeshObject("Material Blend Quad", "Quad", "res://materials/alpha_blend.mat", Vector3(59.4, -1.3, 0.0), quadScale)
    MeshObject("Material Mask Quad", "Quad", "res://materials/alpha_mask.mat", Vector3(62.0, -1.3, 0.0), quadScale)
    WorldText("Material Opaque Caption", "Opaque: the whole striped square; the yellow bar is hidden behind it.",
        Vector3(55.8, -2.45, 0.0), 0.15, CAPTION, 2.3)
    WorldText("Material Blend Caption", "Blend: invisible at the left, fading in to solid at the right; the bar "
        .. "shows through on the left.", Vector3(58.4, -2.45, 0.0), 0.15, CAPTION, 2.3)
    WorldText("Material Mask Caption", "Mask 0.5: the left half cut away at a hard vertical edge, the right half "
        .. "solid.", Vector3(61.0, -2.45, 0.0), 0.15, CAPTION, 2.3)
end

-- ===== Station 6: a glTF model =====

do
    StationTitle(6, Vector3(86.5, 3.2, 0.0))
    WorldText("Model Caption", "Left: as imported. Right: the same model, spinning as one piece. Grey body with a "
        .. "face (cyan eyes above a red mouth, upright), orange arms (the right one tilted out), a teal pyramid head, "
        .. "an orange antenna.", Vector3(86.5, 2.6, 0.0), 0.16, CAPTION, 6.5)

    local static = Place(GameObject.Create("Robot Static"), Vector3(87.8, -1.9, 0.0), Vector3(1.2, 1.2, 1.2))
    Model.Instantiate("res://models/smoke_robot.gltf", static)
    table.insert(roots, static)

    local spinning = Place(GameObject.Create("Robot Spinning"), Vector3(91.2, -1.9, 0.0), Vector3(1.2, 1.2, 1.2))
    Model.Instantiate("res://models/smoke_robot.gltf", spinning)
    spinning:AddComponent("LuaComponent"):SetScript("res://scripts/Spin.lua")
    table.insert(roots, spinning)
end

-- ===== Screen-space UI panel (U hides it) =====

local uiClicks = 0
do
    local canvas = UI.CreateCanvas("Smoke UI Panel")
    canvas:GetComponent("Canvas"):SetSortOrder(1)
    table.insert(roots, canvas)

    -- A dark glass column on the right of the window
    local panel = Panel(canvas, "UI Panel Background", Color.new(0.05, 0.05, 0.08, 0.7), true)
    local rect = panel:GetComponent("RectTransform")
    rect:SetAnchorMin(Vector2(1, 1))
    rect:SetAnchorMax(Vector2(1, 1))
    rect:SetPivot(Vector2(1, 1))
    rect:SetAnchoredPosition(Vector2(-8, -8))
    rect:SetSizeDelta(Vector2(300, 470))

    Layout(Label(panel, "UI Title", "Screen-space UI (U hides)", { size = 18, wrap = false }), 10, 8, 280, 24)

    local icon = Layout(UI.CreateElement("UI Icon"), 10, 38, 64, 64)
    panel:AddChild(icon)
    icon:AddComponent("Image"):SetSprite("res://ui/icon.png")
    Layout(Label(panel, "UI Icon Caption", "Sprite: red square top-left, white arrow pointing up, square and "
        .. "sharp-edged.", { size = 13, color = CAPTION }), 84, 38, 206, 64)

    local alignments = {
        { "Left / Top", "Left", "Top" },
        { "Center / Middle", "Center", "Middle" },
        { "Right / Bottom", "Right", "Bottom" },
    }
    for index, alignment in ipairs(alignments) do
        local box = Layout(Panel(panel, "UI Align Box " .. index, Color.new(0.25, 0.25, 0.3, 1.0)), 10,
            110 + (index - 1) * 34, 280, 30)
        local text = Label(box, "UI Align " .. alignment[1], alignment[1],
            { size = 14, h = alignment[2], v = alignment[3], wrap = false })
        text:GetComponent("RectTransform"):StretchToParent()
    end
    Layout(Label(panel, "UI Align Caption", "Each label hugs its box's left-top, centre, right-bottom.",
        { size = 12, color = CAPTION }), 10, 212, 280, 18)

    local wrapBox = Layout(Panel(panel, "UI Wrap Box", Color.new(0.25, 0.25, 0.3, 1.0)), 10, 234, 280, 58)
    local wrapText = Label(wrapBox, "UI Wrap Text", "UIText wraps this long sentence at the box's width, so it "
        .. "runs onto three lines without spilling out of the box.", { size = 14 })
    wrapText:GetComponent("RectTransform"):StretchToParent()
    Layout(Label(panel, "UI Wrap Caption", "The wrapped text stays inside its box.", { size = 12, color = CAPTION }),
        10, 296, 280, 18)

    local buttonObject, button = TintedButton(panel, "UI Button", "Click me", Color.new(0.35, 0.65, 1.0, 1.0), 18)
    Layout(buttonObject, 10, 320, 140, 40)
    local counter, counterText = Label(panel, "UI Counter", "Clicks: 0",
        { size = 18, v = "Middle", wrap = false })
    Layout(counter, 160, 320, 130, 40)
    button:AddOnClick(function()
        uiClicks = uiClicks + 1
        counterText:SetText("Clicks: " .. uiClicks)
    end)

    local toggleObject, toggle = TintedButton(panel, "UI Toggle", "Disable it", Color.new(0.85, 0.85, 0.85, 1.0), 16)
    Layout(toggleObject, 10, 366, 140, 32)
    local toggleLabel = toggleObject:FindChild("Label"):GetComponent("UIText")
    toggle:AddOnClick(function()
        local enable = not button:IsInteractable()
        button:SetInteractable(enable)
        toggleLabel:SetText(enable and "Disable it" or "Enable it")
    end)

    Layout(Label(panel, "UI Button Caption", "Hover Click me: darker. Hold: darker still. Release: Clicks +1. "
        .. "Disabled: greyed and see-through, clicks ignored. The panel is dark glass: the world shows through.",
        { size = 12, color = CAPTION }), 10, 404, 280, 60)
end

-- ===== HUD: the current station and what to look for (scripts/SmokeHud.lua) =====

do
    local hud = UI.CreateCanvas("Smoke HUD")
    table.insert(roots, hud)

    local background = Panel(hud, "HUD Background", Color.new(0.0, 0.0, 0.0, 0.6))
    local rect = background:GetComponent("RectTransform")
    rect:SetAnchorMin(Vector2(0, 1))
    rect:SetAnchorMax(Vector2(0, 1))
    rect:SetPivot(Vector2(0, 1))
    rect:SetAnchoredPosition(Vector2(8, -8))
    rect:SetSizeDelta(Vector2(600, 116))

    Layout(Label(background, "HUD Title", "", { size = 20, wrap = false }), 10, 6, 580, 26)
    Layout(Label(background, "HUD Keys", "Keys 1-6: stations   WASD: move   Arrows: look   U: UI panel   "
        .. "Mouse: buttons   Esc: quit", { size = 13, color = Color.new(0.8, 0.8, 0.85, 1.0), wrap = false }),
        10, 34, 580, 18)
    Layout(Label(background, "HUD Caption", "", { size = 14, color = CAPTION }), 10, 54, 580, 60)

    hud:AddComponent("LuaComponent"):SetScript("res://scripts/SmokeHud.lua")
end

-- ===== Behaviours =====

local cameraController = GameObject.Create("Camera Controller")
cameraController:AddComponent("LuaComponent"):SetScript("res://scripts/CameraController.lua")
table.insert(roots, cameraController)

local quitHandler = GameObject.Create("Quit Handler")
quitHandler:AddComponent("LuaComponent"):SetScript("res://scripts/QuitHandler.lua")
table.insert(roots, quitHandler)

for _, gameObject in ipairs(roots) do
    scene:AddRootGameObject(gameObject)
end

Debug.Log("scene.lua: scene built (GPU smoke test: keys 1-" .. #Stations .. " visit the stations)")

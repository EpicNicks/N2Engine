-- The smoke test's heads-up display (on the "Smoke HUD" canvas, bottom-left): which station the camera is at and
-- what to look for there, also written to the log in case text doesn't draw. Keys 1-7 ("Station N") change station,
-- as in CameraController.lua, and show the screen-space UI panel only at the station that tests it; U ("Toggle UI
-- Panel") shows or hides the panel anyway, and H ("Toggle HUD") the HUD.

local Stations = require("scripts.SmokeStations")

local SmokeHud = {}
SmokeHud.__index = SmokeHud

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

function SmokeHud:OnAttach()
    self:ShowStation(1)

    local controls = Input.LoadActionMap("Main Controls")
    if controls == nil then
        Debug.Warn("SmokeHud: 'Main Controls' action map not found")
        return
    end

    for index = 1, #(Stations or {}) do
        OnStarted(controls, "Station " .. index, function() self:ShowStation(index) end)
    end
    OnStarted(controls, "Toggle UI Panel", function() self:TogglePanel() end)
    OnStarted(controls, "Toggle HUD", function() self:ToggleHud() end)
end

-- The UIText on the HUD's child called `name`, or nil
function SmokeHud:Text(name)
    local child = self.gameObject:FindChildRecursive(name)
    return child and child:GetComponent("UIText")
end

function SmokeHud:ShowStation(index)
    local station = Stations and Stations[index]
    if station == nil then
        return
    end
    local heading = string.format("Station %d/%d: %s", index, #Stations, station.name)
    local title = self:Text("HUD Title")
    if title ~= nil then
        title:SetText(heading)
    end
    local caption = self:Text("HUD Caption")
    if caption ~= nil then
        caption:SetText(station.look)
    end
    local panel = self:Panel()
    if panel ~= nil then
        panel:SetActive(station.uiPanel == true)
    end
    Debug.Log("GPU smoke test, " .. heading .. ". Look for: " .. station.look)
end

-- The screen-space UI station's canvas, or nil
function SmokeHud:Panel()
    local scene = SceneManager.GetCurrentScene()
    return scene and scene:FindGameObject("Smoke UI Panel")
end

function SmokeHud:TogglePanel()
    local panel = self:Panel()
    if panel ~= nil then
        panel:SetActive(not panel:IsActive())
    end
end

function SmokeHud:ToggleHud()
    local background = self.gameObject:FindChild("HUD Background")
    if background ~= nil then
        background:SetActive(not background:IsActive())
    end
end

return SmokeHud

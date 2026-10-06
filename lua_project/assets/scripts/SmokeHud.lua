-- The smoke test's heads-up display (on the "Smoke HUD" canvas): which station the camera is at and what to look
-- for there, also written to the log in case text doesn't draw. Keys 1-6 ("Station N") change station, as in
-- CameraController.lua; U ("Toggle UI Panel") shows and hides the screen-space UI station.

local Stations = require("scripts.SmokeStations")

local SmokeHud = {}
SmokeHud.__index = SmokeHud

function SmokeHud:OnAttach()
    self:ShowStation(1)

    local controls = Input.LoadActionMap("Main Controls")
    if controls == nil then
        Debug.Warn("SmokeHud: 'Main Controls' action map not found")
        return
    end

    for index = 1, #(Stations or {}) do
        local station = controls:Get("Station " .. index)
        if station ~= nil then
            station:Subscribe(function(action)
                if action:GetPhase() == ActionPhase.Started then
                    self:ShowStation(index)
                end
            end)
        end
    end

    local toggle = controls:Get("Toggle UI Panel")
    if toggle ~= nil then
        toggle:Subscribe(function(action)
            if action:GetPhase() == ActionPhase.Started then
                self:TogglePanel()
            end
        end)
    end
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
    Debug.Log("GPU smoke test, " .. heading .. ". Look for: " .. station.look)
end

function SmokeHud:TogglePanel()
    local scene = SceneManager.GetCurrentScene()
    local panel = scene and scene:FindGameObject("Smoke UI Panel")
    if panel ~= nil then
        panel:SetActive(not panel:IsActive())
    end
end

return SmokeHud

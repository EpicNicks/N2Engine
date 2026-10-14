-- The particle station's behaviour (demo station 8): keys B (burst), P (play / stop), C (clear) and K (cycle the
-- requested backend Auto -> CPU -> PhysX) act only while the camera is at the station, and the signs above the emitters
-- show live which backend each ParticleSystem asked for, which is RUNNING and how many particles are alive.
-- Changing the backend replaces the emitters, since a system reads its backend when it starts.

local Emitters = require("scripts.ParticleEmitters")

local ParticleStation = {}
ParticleStation.__index = ParticleStation

local REFRESH_SECONDS = 0.2 -- the signs are rewritten five times a second, not every frame
local RANGE = 15.0          -- how far from the station's centre the camera still counts as being at it

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

-- What the status sign says is running, from what was asked for and what the system reports
local function Running(asked, active)
    if active == nil then
        return "starting"
    elseif active == "PhysX" then
        return "PhysX GPU"
    elseif asked == "PhysX" then
        return "CPU (PhysX asked: fell back)"
    end
    return "CPU"
end

function ParticleStation:OnAttach()
    self.backendIndex = 1
    self.playing = true
    self.elapsed = REFRESH_SECONDS
    self.items = nil
    self.info = nil
    self.infoText = nil

    local controls = Input.LoadActionMap("Main Controls")
    if controls == nil then
        Debug.Warn("ParticleStation: 'Main Controls' action map not found")
        return
    end
    OnStarted(controls, "Particles Burst", function()
        self:ForEach(function(item) item.ps:EmitBurst(item.spec.burst) end)
    end)
    OnStarted(controls, "Particles Toggle", function() self:TogglePlaying() end)
    OnStarted(controls, "Particles Clear", function()
        self:ForEach(function(item) item.ps:Clear() end)
    end)
    OnStarted(controls, "Particles Backend", function() self:CycleBackend() end)
end

-- The emitters and signs scene.lua built, found once
function ParticleStation:Items()
    if self.items ~= nil then
        return self.items
    end
    local items = {}
    local scene = SceneManager.GetCurrentScene()
    if scene ~= nil then
        for _, spec in ipairs(Emitters.Specs) do
            local emitter = scene:FindGameObject("Particles " .. spec.key)
            local status = scene:FindGameObject("Particles Status " .. spec.key)
            if emitter ~= nil and status ~= nil then
                table.insert(items, {
                    spec = spec,
                    gameObject = emitter,
                    ps = emitter:GetComponent("ParticleSystem"),
                    status = status:GetComponent("TextRenderer"),
                    statusText = "",
                })
            end
        end
        local info = scene:FindGameObject("Particles Info")
        self.info = info and info:GetComponent("TextRenderer")
    end
    self.items = items
    return items
end

function ParticleStation:AtStation()
    local camera = Camera.Main()
    return camera ~= nil and math.abs(camera:GetPosition().x - Emitters.CENTER_X) < RANGE
end

function ParticleStation:Backend()
    return Emitters.BACKENDS[self.backendIndex]
end

-- Runs fn(item) for every emitter, when the camera is at the station
function ParticleStation:ForEach(fn)
    if not self:AtStation() then
        return
    end
    for _, item in ipairs(self:Items()) do
        fn(item)
    end
end

function ParticleStation:TogglePlaying()
    if not self:AtStation() then
        return
    end
    self.playing = not self.playing
    for _, item in ipairs(self:Items()) do
        if item.spec.continuous then
            if self.playing then
                item.ps:Play()
            else
                item.ps:Stop()
            end
        end
    end
end

-- Replaces every emitter with a new one asking for the next backend (the old ones go at the end of the frame)
function ParticleStation:CycleBackend()
    if not self:AtStation() then
        return
    end
    local scene = SceneManager.GetCurrentScene()
    if scene == nil then
        return
    end
    self.backendIndex = self.backendIndex % #Emitters.BACKENDS + 1
    for _, item in ipairs(self:Items()) do
        scene:DestroyGameObject(item.gameObject)
        item.gameObject, item.ps = Emitters.Create(item.spec, self:Backend(), self.playing)
        scene:AddRootGameObject(item.gameObject)
    end
    self.elapsed = REFRESH_SECONDS
end

function ParticleStation:OnUpdate()
    self.elapsed = self.elapsed + Time.deltaTime
    if self.elapsed < REFRESH_SECONDS then
        return
    end
    self.elapsed = 0.0

    local asked = self:Backend()
    local fellBack = false
    for _, item in ipairs(self:Items()) do
        local active = item.ps:GetActiveBackend()
        if asked == "PhysX" and active ~= nil and active ~= "PhysX" then
            fellBack = true
        end
        local text = string.format("asked: %s\nrunning: %s\nalive: %d", asked, Running(asked, active),
            item.ps:GetAliveCount())
        if text ~= item.statusText then
            item.statusText = text
            item.status:SetText(text)
        end
    end

    if self.info ~= nil then
        local text = string.format("Physics.IsGpuAvailable(): %s   backend asked: %s\n"
            .. "B burst   P play / stop   C clear   K backend: Auto, CPU, PhysX",
            tostring(Physics.IsGpuAvailable()), asked)
        if fellBack then
            text = text .. "\nPhysX asked but CPU is running: no GPU particle backend here (the log has one warning "
                .. "per emitter)."
        end
        if text ~= self.infoText then
            self.infoText = text
            self.info:SetText(text)
        end
    end
end

return ParticleStation

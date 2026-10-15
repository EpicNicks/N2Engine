-- The soft-body station's behaviour (demo station 11, key Minus): keys R (drop every body again), J (cycle the
-- requested backend Auto -> CPU -> PhysX) and V (push every body upwards) act only while the camera is at the station,
-- and the signs under the bodies' titles show live which backend each SoftBody asked for and which is RUNNING.
-- Changing the backend replaces the bodies, since a SoftBody reads its backend only when it starts.

local Bodies = require("scripts.SoftBodies")

local SoftBodyStation = {}
SoftBodyStation.__index = SoftBodyStation

local REFRESH_SECONDS = 0.2 -- the signs are rewritten five times a second, not every frame
local RANGE = 15.0          -- how far from the station's centre the camera still counts as being at it
local START_SECONDS = 1.0   -- a body with no backend yet is "starting" for this long, then "inert"
local PUSH = Vector3(0.0, 6.0, 0.0) -- what V adds to every body's velocity, in world units per second

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

-- What the status sign says is running, from what was asked for and what GetActiveBackendName reports ("" before
-- the body started, or when it is inert: no mesh, a Rigidbody, or a mesh it cannot tetrahedralise)
local function Running(asked, active, age)
    if active == "" then
        return age < START_SECONDS and "starting" or "inert"
    elseif active == "PhysX" then
        return "PhysX GPU"
    elseif asked == "PhysX" then
        return "CPU (PhysX asked: fell back)"
    end
    return "CPU"
end

function SoftBodyStation:OnAttach()
    self.backendIndex = 1
    self.clock = 0.0
    self.elapsed = REFRESH_SECONDS
    self.items = nil
    self.info = nil
    self.infoText = nil

    local controls = Input.LoadActionMap("Main Controls")
    if controls == nil then
        Debug.Warn("SoftBodyStation: 'Main Controls' action map not found")
        return
    end
    OnStarted(controls, "Soft Bodies Reset", function()
        self:ForEach(function(item)
            item.body:Reset()
            item.created = self.clock
        end)
    end)
    OnStarted(controls, "Soft Bodies Backend", function() self:CycleBackend() end)
    OnStarted(controls, "Soft Bodies Push", function()
        self:ForEach(function(item) item.body:AddVelocity(PUSH) end)
    end)
end

-- The bodies and signs scene.lua built, found once
function SoftBodyStation:Items()
    if self.items ~= nil then
        return self.items
    end
    local items = {}
    local scene = SceneManager.GetCurrentScene()
    if scene ~= nil then
        for _, spec in ipairs(Bodies.Specs) do
            local object = scene:FindGameObject("Soft Body " .. spec.key)
            local status = scene:FindGameObject("Soft Body Status " .. spec.key)
            if object ~= nil and status ~= nil then
                table.insert(items, {
                    spec = spec,
                    gameObject = object,
                    body = object:GetComponent("SoftBody"),
                    status = status:GetComponent("TextRenderer"),
                    statusText = "",
                    created = 0.0,
                })
            end
        end
        local info = scene:FindGameObject("Soft Bodies Info")
        self.info = info and info:GetComponent("TextRenderer")
    end
    self.items = items
    return items
end

function SoftBodyStation:AtStation()
    local camera = Camera.Main()
    return camera ~= nil and math.abs(camera:GetPosition().x - Bodies.CENTER_X) < RANGE
end

function SoftBodyStation:Backend()
    return Bodies.BACKENDS[self.backendIndex]
end

-- Runs fn(item) for every body, when the camera is at the station
function SoftBodyStation:ForEach(fn)
    if not self:AtStation() then
        return
    end
    for _, item in ipairs(self:Items()) do
        fn(item)
    end
end

-- Replaces every body with a new one asking for the next backend (the old ones go at the end of the frame), each
-- back at its start pose above the floor
function SoftBodyStation:CycleBackend()
    if not self:AtStation() then
        return
    end
    local scene = SceneManager.GetCurrentScene()
    if scene == nil then
        return
    end
    self.backendIndex = self.backendIndex % #Bodies.BACKENDS + 1
    for _, item in ipairs(self:Items()) do
        scene:DestroyGameObject(item.gameObject)
        item.gameObject, item.body = Bodies.Create(item.spec, self:Backend())
        item.created = self.clock
        scene:AddRootGameObject(item.gameObject)
    end
    self.elapsed = REFRESH_SECONDS
end

function SoftBodyStation:OnUpdate()
    self.clock = self.clock + Time.deltaTime
    self.elapsed = self.elapsed + Time.deltaTime
    if self.elapsed < REFRESH_SECONDS then
        return
    end
    self.elapsed = 0.0

    local asked = self:Backend()
    local fellBack = false
    for _, item in ipairs(self:Items()) do
        local active = item.body:GetActiveBackendName()
        if asked == "PhysX" and active == "CPU" then
            fellBack = true
        end
        local text = string.format("asked: %s\nrunning: %s", asked, Running(asked, active, self.clock - item.created))
        if text ~= item.statusText then
            item.statusText = text
            item.status:SetText(text)
        end
    end

    if self.info ~= nil then
        local text = string.format("Physics.IsGpuAvailable(): %s   backend asked: %s\n"
            .. "R drop again   J backend: Auto, CPU, PhysX   V push up",
            tostring(Physics.IsGpuAvailable()), asked)
        if fellBack then
            text = text .. "\nPhysX asked but CPU is running: there is no PhysX GPU soft-body backend here (the log "
                .. "has one warning per body)."
        end
        if text ~= self.infoText then
            self.infoText = text
            self.info:SetText(text)
        end
    end
end

return SoftBodyStation

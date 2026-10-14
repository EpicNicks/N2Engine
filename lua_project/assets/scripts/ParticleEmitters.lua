-- The particle station's emitters (scene.lua builds them, ParticleStation.lua rebuilds them when the backend changes:
-- the backend is read when a system starts, so a new backend means a new system). Water, sand and smoke are tuned
-- with the ParticleSystem fields (rate, shape, speed, size, colour, gravity scale, drag, the ground plane) for the CPU
-- backend, and with pbdMaterial (Water, Sand, Smoke: the PBD preset the PhysX GPU backend uses; the CPU ignores it).

local Emitters = {}

Emitters.CENTER_X = 120.0
Emitters.GROUND_Y = -1.9
-- What K cycles through, as ParticleSystem:SetBackend names them
Emitters.BACKENDS = { "Auto", "CPU", "PhysX" }

Emitters.Specs = {
    {
        key = "Water",
        detail = "fountain, continuous",
        x = -5.5,
        y = Emitters.GROUND_Y + 0.05,
        continuous = true,
        burst = 120,
        configure = function(ps)
            ps:SetPbdMaterial("Water")
            ps:SetShape("Cone")
            ps:SetShapeRadius(0.15)
            ps:SetConeAngle(10.0)
            ps:SetSpeed(7.0, 9.0)
            ps:SetLifetime(1.8, 2.4)
            ps:SetSize(0.14, 0.1)
            ps:SetStartColor(Color.new(0.35, 0.6, 1.0, 0.9))
            ps:SetEndColor(Color.new(0.2, 0.4, 1.0, 0.2))
            ps:SetEmissionRate(150.0)
            ps:SetMaxParticles(2000)
            ps:SetGravityScale(1.0)
            ps:SetDrag(0.0)
            ps:SetGround(true, Emitters.GROUND_Y, 0.25)
        end,
    },
    {
        key = "Sand",
        detail = "burst, lands and lies",
        x = 0.0,
        y = 1.0,
        continuous = false,
        burst = 400,
        configure = function(ps)
            ps:SetPbdMaterial("Sand")
            ps:SetShape("Cone")
            ps:SetShapeRadius(0.3)
            ps:SetConeAngle(40.0)
            ps:SetSpeed(3.0, 6.0)
            ps:SetLifetime(7.0, 9.0)
            ps:SetSize(0.1, 0.1)
            ps:SetStartColor(Color.new(0.86, 0.7, 0.35, 1.0))
            ps:SetEndColor(Color.new(0.86, 0.7, 0.35, 1.0))
            ps:SetEmissionRate(0.0)
            ps:SetMaxParticles(1500)
            ps:SetGravityScale(1.0)
            ps:SetDrag(0.4)
            ps:SetGround(true, Emitters.GROUND_Y, 0.05)
        end,
    },
    {
        key = "Smoke",
        detail = "column, rises and fades",
        x = 5.5,
        y = Emitters.GROUND_Y + 0.05,
        continuous = true,
        burst = 40,
        configure = function(ps)
            ps:SetPbdMaterial("Smoke")
            ps:SetShape("Cone")
            ps:SetShapeRadius(0.3)
            ps:SetConeAngle(8.0)
            ps:SetSpeed(0.8, 1.4)
            ps:SetLifetime(3.5, 5.0)
            ps:SetSize(0.35, 1.3)
            ps:SetStartColor(Color.new(0.55, 0.55, 0.58, 0.55))
            ps:SetEndColor(Color.new(0.3, 0.3, 0.32, 0.0))
            ps:SetEmissionRate(30.0)
            ps:SetMaxParticles(600)
            ps:SetGravityScale(-0.12)
            ps:SetDrag(0.3)
            ps:SetGround(false, 0.0, 0.0)
        end,
    },
}

-- A new emitter object (in no scene yet: the caller adds it, which starts the system with `backend`) and its
-- ParticleSystem. `playing` false leaves a continuous emitter stopped, as after P.
function Emitters.Create(spec, backend, playing)
    local gameObject = GameObject.Create("Particles " .. spec.key)
    gameObject:CreatePositionable()
    gameObject:GetPositionable():SetPosition(Vector3(Emitters.CENTER_X + spec.x, spec.y, 0.0))
    local ps = gameObject:AddComponent("ParticleSystem")
    ps:SetBackend(backend)
    spec.configure(ps)
    ps:SetPlayOnAwake(spec.continuous and playing)
    -- Emitted when the system starts: the sand's only particles, and a first puff for the others
    ps:EmitBurst(spec.burst)
    return gameObject, ps
end

return Emitters

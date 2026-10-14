#include "engine/particles/PbdParticleBackend.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "engine/Logger.hpp"

namespace N2Engine::Particles
{
    namespace
    {
        constexpr float kMinLifetime = 1e-3f; // a lifetime of 0 would make a particle that is born dead

        float Finite(const float value, const float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        Physics::ParticleParams Sanitized(Physics::ParticleParams params)
        {
            params.gravity.x = Finite(params.gravity.x, 0.0f);
            params.gravity.y = Finite(params.gravity.y, 0.0f);
            params.gravity.z = Finite(params.gravity.z, 0.0f);
            params.lifetimeMin = std::max(Finite(params.lifetimeMin, 1.0f), kMinLifetime);
            params.lifetimeMax = std::max(Finite(params.lifetimeMax, params.lifetimeMin), params.lifetimeMin);
            params.drag = std::max(Finite(params.drag, 0.0f), 0.0f);
            // The ground plane and bounce belong to the CPU backend: PhysX particles collide with the scene's
            // colliders, so these are kept but never used here
            return params;
        }

        float InverseMass(const PbdPreset &preset)
        {
            const float spacing = std::max(preset.particleSpacing, 1e-4f);
            const float mass = std::max(preset.density, 1e-3f) * spacing * spacing * spacing;
            return 1.0f / mass;
        }
    }

    void PbdParticleBackend::System::Remove(const std::uint32_t index)
    {
        const std::uint32_t last = count - 1;
        if (index != last)
        {
            staging.positions[index] = staging.positions[last];
            staging.velocities[index] = staging.velocities[last];
            staging.phases[index] = staging.phases[last];
            age[index] = age[last];
            lifetime[index] = lifetime[last];
        }
        age.pop_back();
        lifetime.pop_back();
        --count;
    }

    PbdParticleBackend::PbdParticleBackend(std::unique_ptr<IPbdDriver> driver) : _driver(std::move(driver)) {}

    PbdParticleBackend::~PbdParticleBackend()
    {
        ReleaseAll();
    }

    PbdParticleBackend::System *PbdParticleBackend::Find(const Physics::ParticleSystemHandle handle)
    {
        // The generation check comes first: a handle of another backend object (a replaced or recreated one, at
        // whatever address) is never looked up
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _systems.find(handle);
        return it == _systems.end() ? nullptr : &it->second;
    }

    const PbdParticleBackend::System *PbdParticleBackend::Find(const Physics::ParticleSystemHandle handle) const
    {
        if (!OwnsHandle(handle))
        {
            return nullptr;
        }
        const auto it = _systems.find(handle);
        return it == _systems.end() ? nullptr : &it->second;
    }

    void PbdParticleBackend::Fail(const std::string &reason)
    {
        if (_failed)
        {
            return;
        }
        _failed = true;
        _failureReason = reason;
        Logger::Warn(std::format("PhysX GPU particles: {}; the GPU backend is switched off", reason));
    }

    void PbdParticleBackend::Upload(System &system, const std::uint32_t dirty)
    {
        if (_failed || system.driverId == kInvalidPbdSystem)
        {
            return;
        }
        std::string error;
        if (!_driver->Upload(system.driverId, system.count, dirty, error))
        {
            Fail(error.empty() ? "the particle upload to the GPU failed" : error);
        }
    }

    Physics::ParticleSystemHandle PbdParticleBackend::CreateSystem(const Physics::ParticleSystemDesc &desc)
    {
        if (_failed)
        {
            return Physics::kInvalidParticleSystem;
        }

        System system;
        system.maxParticles = std::min(desc.maxParticles, kMaxParticlesLimit);
        system.params = Sanitized(desc.params);
        system.preset = GetPbdPreset(desc.material);
        system.invMass = InverseMass(system.preset);
        system.random.Seed(desc.seed);
        system.material =
            ComposePbdMaterial(system.preset, system.params.gravity, _driver->GetSceneGravity(), system.params.drag);

        // A system of 0 particles has nothing to simulate: it exists (and accepts nothing) without a GPU system
        if (system.maxParticles > 0)
        {
            std::string error;
            const PbdSystemId id = _driver->CreateSystem(
                PbdSystemDesc{.maxParticles = system.maxParticles, .material = system.material, .preset = system.preset},
                error);
            if (id == kInvalidPbdSystem)
            {
                Fail(error.empty() ? "the GPU could not create a PBD particle system" : error);
                return Physics::kInvalidParticleSystem;
            }
            system.driverId = id;
            system.staging = _driver->GetStaging(id);
            system.phase = _driver->GetPhase(id);
            if (system.staging.positions.size() < system.maxParticles ||
                system.staging.velocities.size() < system.maxParticles ||
                system.staging.phases.size() < system.maxParticles)
            {
                _driver->DestroySystem(id);
                Fail("the GPU particle buffer is smaller than asked for");
                return Physics::kInvalidParticleSystem;
            }
        }

        const Physics::ParticleSystemHandle handle = Physics::MakeParticleHandle(GetGeneration(), _nextIndex++);
        _systems.emplace(handle, std::move(system));
        return handle;
    }

    void PbdParticleBackend::Destroy(const Physics::ParticleSystemHandle handle)
    {
        if (System *system = Find(handle))
        {
            if (system->driverId != kInvalidPbdSystem)
            {
                _driver->DestroySystem(system->driverId);
            }
            _systems.erase(handle);
        }
    }

    void PbdParticleBackend::ReleaseAll()
    {
        for (auto &[handle, system] : _systems)
        {
            static_cast<void>(handle);
            if (system.driverId != kInvalidPbdSystem)
            {
                _driver->DestroySystem(system.driverId);
            }
        }
        _systems.clear();
    }

    void PbdParticleBackend::SetParams(const Physics::ParticleSystemHandle handle, const Physics::ParticleParams &params)
    {
        System *system = Find(handle);
        if (!system)
        {
            return;
        }
        system->params = Sanitized(params);
        if (_failed || system->driverId == kInvalidPbdSystem)
        {
            return;
        }
        const PbdMaterialParams material = ComposePbdMaterial(system->preset, system->params.gravity,
                                                              _driver->GetSceneGravity(), system->params.drag);
        if (!(material == system->material))
        {
            _driver->SetMaterial(system->driverId, material);
            system->material = material;
        }
    }

    std::size_t PbdParticleBackend::Spawn(const Physics::ParticleSystemHandle handle,
                                          const std::span<const Math::Vector3> positions,
                                          const std::span<const Math::Vector3> velocities)
    {
        System *system = Find(handle);
        if (!system || _failed)
        {
            return 0;
        }
        const std::size_t room = system->maxParticles > system->count ? system->maxParticles - system->count : 0;
        const std::size_t count = std::min({positions.size(), velocities.size(), room});
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t slot = system->count + i;
            system->staging.positions[slot] = {positions[i].x, positions[i].y, positions[i].z, system->invMass};
            system->staging.velocities[slot] = {velocities[i].x, velocities[i].y, velocities[i].z, 0.0f};
            system->staging.phases[slot] = system->phase;
            system->age.push_back(0.0f);
            system->lifetime.push_back(system->random.Range(system->params.lifetimeMin, system->params.lifetimeMax));
        }
        system->count += static_cast<std::uint32_t>(count);
        if (count > 0)
        {
            Upload(*system, kPbdPositions | kPbdVelocities | kPbdPhases);
        }
        return count;
    }

    void PbdParticleBackend::Clear(const Physics::ParticleSystemHandle handle)
    {
        System *system = Find(handle);
        if (!system)
        {
            return;
        }
        const bool hadParticles = system->count > 0;
        system->count = 0;
        system->age.clear();
        system->lifetime.clear();
        if (hadParticles)
        {
            Upload(*system, kPbdNothing); // the active count drops to 0; no data to send
        }
    }

    void PbdParticleBackend::OnSceneStepped(const float dt)
    {
        if (_failed || !std::isfinite(dt) || dt < 0.0f)
        {
            return;
        }
        for (auto &[handle, system] : _systems)
        {
            static_cast<void>(handle);
            if (system.count == 0 || system.driverId == kInvalidPbdSystem)
            {
                continue;
            }

            std::string error;
            if (!_driver->ReadBack(system.driverId, system.count, error))
            {
                Fail(error.empty() ? "the particle read-back from the GPU failed" : error);
                return;
            }

            for (std::uint32_t i = 0; i < system.count; ++i)
            {
                system.age[i] += dt;
            }
            // The dead are replaced by the last living particle, which is then checked in its new slot
            bool removed = false;
            for (std::uint32_t i = 0; i < system.count;)
            {
                if (system.age[i] >= system.lifetime[i])
                {
                    system.Remove(i);
                    removed = true;
                }
                else
                {
                    ++i;
                }
            }
            if (removed)
            {
                Upload(system, kPbdPositions | kPbdVelocities);
                if (_failed)
                {
                    return;
                }
            }
        }
    }

    std::size_t PbdParticleBackend::GetAliveCount(const Physics::ParticleSystemHandle handle) const
    {
        const System *system = Find(handle);
        return system ? system->count : 0;
    }

    void PbdParticleBackend::CopyPositions(const Physics::ParticleSystemHandle handle,
                                           std::vector<Math::Vector4> &out) const
    {
        out.clear();
        const System *system = Find(handle);
        if (!system)
        {
            return;
        }
        out.resize(system->count);
        for (std::uint32_t i = 0; i < system->count; ++i)
        {
            const PbdVec4 &p = system->staging.positions[i];
            Math::Vector4 &v = out[i];
            v.x = p.x;
            v.y = p.y;
            v.z = p.z;
            v.w = std::clamp(system->age[i] / system->lifetime[i], 0.0f, 1.0f);
        }
    }
}

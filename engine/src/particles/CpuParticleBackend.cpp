#include "engine/particles/CpuParticleBackend.hpp"

#include <algorithm>
#include <cmath>

namespace N2Engine::Particles
{
    namespace
    {
        constexpr float kMinLifetime = 1e-3f; // a lifetime of 0 would make a particle that is born dead

        float Finite(const float value, const float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }
    }

    Physics::ParticleParams CpuParticleBackend::Sanitized(Physics::ParticleParams params)
    {
        params.gravity.x = Finite(params.gravity.x, 0.0f);
        params.gravity.y = Finite(params.gravity.y, 0.0f);
        params.gravity.z = Finite(params.gravity.z, 0.0f);
        params.lifetimeMin = std::max(Finite(params.lifetimeMin, 1.0f), kMinLifetime);
        params.lifetimeMax = std::max(Finite(params.lifetimeMax, params.lifetimeMin), params.lifetimeMin);
        params.drag = std::max(Finite(params.drag, 0.0f), 0.0f);
        params.groundHeight = Finite(params.groundHeight, 0.0f);
        params.bounce = std::clamp(Finite(params.bounce, 0.0f), 0.0f, 1.0f);
        return params;
    }

    void CpuParticleBackend::System::Remove(const std::size_t index)
    {
        const std::size_t last = Count() - 1;
        if (index != last)
        {
            px[index] = px[last];
            py[index] = py[last];
            pz[index] = pz[last];
            vx[index] = vx[last];
            vy[index] = vy[last];
            vz[index] = vz[last];
            age[index] = age[last];
            lifetime[index] = lifetime[last];
        }
        px.pop_back();
        py.pop_back();
        pz.pop_back();
        vx.pop_back();
        vy.pop_back();
        vz.pop_back();
        age.pop_back();
        lifetime.pop_back();
    }

    Physics::ParticleSystemHandle CpuParticleBackend::CreateSystem(const Physics::ParticleSystemDesc &desc)
    {
        const Physics::ParticleSystemHandle handle = Physics::MakeParticleHandle(GetGeneration(), _nextIndex++);
        System &system = _systems[handle];
        system.maxParticles = std::min(desc.maxParticles, kMaxParticlesLimit);
        system.params = Sanitized(desc.params);
        system.random.Seed(desc.seed);
        return handle;
    }

    void CpuParticleBackend::Destroy(const Physics::ParticleSystemHandle handle)
    {
        _systems.erase(handle);
    }

    void CpuParticleBackend::SetParams(const Physics::ParticleSystemHandle handle, const Physics::ParticleParams &params)
    {
        if (const auto it = _systems.find(handle); it != _systems.end())
        {
            it->second.params = Sanitized(params);
        }
    }

    std::size_t CpuParticleBackend::Spawn(const Physics::ParticleSystemHandle handle,
                                          const std::span<const Math::Vector3> positions,
                                          const std::span<const Math::Vector3> velocities)
    {
        const auto it = _systems.find(handle);
        if (it == _systems.end())
        {
            return 0;
        }
        System &system = it->second;
        const std::size_t room = system.maxParticles > system.Count() ? system.maxParticles - system.Count() : 0;
        const std::size_t count = std::min({positions.size(), velocities.size(), room});
        for (std::size_t i = 0; i < count; ++i)
        {
            system.px.push_back(positions[i].x);
            system.py.push_back(positions[i].y);
            system.pz.push_back(positions[i].z);
            system.vx.push_back(velocities[i].x);
            system.vy.push_back(velocities[i].y);
            system.vz.push_back(velocities[i].z);
            system.age.push_back(0.0f);
            system.lifetime.push_back(system.random.Range(system.params.lifetimeMin, system.params.lifetimeMax));
        }
        return count;
    }

    void CpuParticleBackend::Clear(const Physics::ParticleSystemHandle handle)
    {
        if (const auto it = _systems.find(handle); it != _systems.end())
        {
            System &system = it->second;
            // clear keeps the capacity
            system.px.clear();
            system.py.clear();
            system.pz.clear();
            system.vx.clear();
            system.vy.clear();
            system.vz.clear();
            system.age.clear();
            system.lifetime.clear();
        }
    }

    void CpuParticleBackend::Step(const float dt)
    {
        if (!(dt > 0.0f) || !std::isfinite(dt))
        {
            return;
        }
        // Systems are independent, so the order they are visited in (the map's) changes nothing
        for (auto &entry : _systems)
        {
            System &s = entry.second;
            const Physics::ParticleParams &p = s.params;
            const float gx = p.gravity.x * dt;
            const float gy = p.gravity.y * dt;
            const float gz = p.gravity.z * dt;
            const float damping = std::max(0.0f, 1.0f - p.drag * dt);

            std::size_t i = 0;
            while (i < s.Count())
            {
                s.age[i] += dt;
                if (s.age[i] >= s.lifetime[i])
                {
                    s.Remove(i); // the last particle moved into slot i: look at it next
                    continue;
                }

                s.vx[i] = (s.vx[i] + gx) * damping;
                s.vy[i] = (s.vy[i] + gy) * damping;
                s.vz[i] = (s.vz[i] + gz) * damping;
                s.px[i] += s.vx[i] * dt;
                s.py[i] += s.vy[i] * dt;
                s.pz[i] += s.vz[i] * dt;

                if (p.groundEnabled && s.py[i] < p.groundHeight)
                {
                    s.py[i] = p.groundHeight;
                    if (s.vy[i] < 0.0f)
                    {
                        s.vy[i] = -s.vy[i] * p.bounce;
                    }
                }
                ++i;
            }
        }
    }

    std::size_t CpuParticleBackend::GetAliveCount(const Physics::ParticleSystemHandle handle) const
    {
        const auto it = _systems.find(handle);
        return it == _systems.end() ? 0 : it->second.Count();
    }

    void CpuParticleBackend::CopyPositions(const Physics::ParticleSystemHandle handle,
                                           std::vector<Math::Vector4> &out) const
    {
        out.clear();
        const auto it = _systems.find(handle);
        if (it == _systems.end())
        {
            return;
        }
        const System &s = it->second;
        out.resize(s.Count());
        for (std::size_t i = 0; i < s.Count(); ++i)
        {
            Math::Vector4 &v = out[i];
            v.x = s.px[i];
            v.y = s.py[i];
            v.z = s.pz[i];
            v.w = std::clamp(s.age[i] / s.lifetime[i], 0.0f, 1.0f);
        }
    }
}

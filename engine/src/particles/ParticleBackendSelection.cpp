#include "engine/particles/ParticleBackendSelection.hpp"

#include <format>

namespace N2Engine::Particles
{
    std::atomic<int> ParticleUsage::_cpu{0};
    std::atomic<int> ParticleUsage::_gpu{0};

    BackendSelection SelectParticleBackend(const ParticleBackend requested, Physics::IPhysicsBackend *physics)
    {
        BackendSelection selection;
        if (requested == ParticleBackend::CPU)
        {
            return selection;
        }

        Physics::IParticleBackend *gpu = physics ? physics->GetParticleBackend() : nullptr;
        if (gpu)
        {
            selection.gpu = gpu;
            return selection;
        }

        // Auto quietly takes the CPU backend; an explicit PhysX request that can't be met says why, once per system
        if (requested == ParticleBackend::PhysX)
        {
            selection.fellBack = true;
            const std::string detail = physics ? physics->GetParticlesGpuStatus().detail : "there is no physics backend";
            selection.reason = std::format(
                "ParticleSystem backend is PhysX, but no PhysX GPU particle backend is available ({}); "
                "using the CPU backend",
                detail.empty() ? "physics.gpu is off or the GPU is unavailable" : detail);
        }
        return selection;
    }

    void ParticleUsage::Add(const bool gpu)
    {
        (gpu ? _gpu : _cpu).fetch_add(1);
    }

    void ParticleUsage::Remove(const bool gpu)
    {
        (gpu ? _gpu : _cpu).fetch_sub(1);
    }

    int ParticleUsage::GetCount(const bool gpu)
    {
        return (gpu ? _gpu : _cpu).load();
    }

    std::string ParticleUsage::Compose(const std::string &base)
    {
        const std::string usage = Describe();
        if (usage.empty())
        {
            return base;
        }
        return base.empty() ? usage : base + "; " + usage;
    }

    std::string ParticleUsage::Describe()
    {
        const int cpu = _cpu.load();
        const int gpu = _gpu.load();
        if (cpu <= 0 && gpu <= 0)
        {
            return {};
        }
        std::string text = "ParticleSystems:";
        if (cpu > 0)
        {
            text += std::format(" {} CPU", cpu);
        }
        if (gpu > 0)
        {
            text += std::format("{} {} PhysX", cpu > 0 ? "," : "", gpu);
        }
        return text;
    }
}

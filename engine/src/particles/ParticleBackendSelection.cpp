#include "engine/particles/ParticleBackendSelection.hpp"

#include <format>

#include <renderer/common/GpuCompute.hpp>

namespace N2Engine::Particles
{
    std::atomic<int> ParticleUsage::_cpu{0};
    std::atomic<int> ParticleUsage::_gpu{0};
    std::atomic<int> ParticleUsage::_webGpu{0};

    BackendSelection SelectParticleBackend(const ParticleBackend requested, Physics::IPhysicsBackend *physics,
                                           Renderer::Common::IGpuCompute *compute, const bool autoUsesCompute)
    {
        BackendSelection selection;
        if (requested == ParticleBackend::CPU)
        {
            return selection;
        }

        const bool computeUsable = compute && compute->IsUsable();
        if (requested == ParticleBackend::WebGPU)
        {
            if (computeUsable)
            {
                selection.compute = compute;
                return selection;
            }
            selection.fellBack = true;
            selection.reason = "ParticleSystem backend is WebGPU, but the renderer has no WebGPU compute (it runs only "
                               "on the WebGPU renderer, as in the browser); using the CPU backend";
            return selection;
        }

        Physics::IParticleBackend *gpu = physics ? physics->GetParticleBackend() : nullptr;
        if (gpu)
        {
            selection.gpu = gpu;
            return selection;
        }

        // Auto: after PhysX, the renderer's compute when the project lets the GPU simulate (physics.gpu auto or on, not
        // deterministic) and the device is a real GPU (a fallback adapter is the CPU emulating one: the CPU backend is
        // faster there), then quietly the CPU backend
        if (requested == ParticleBackend::Auto)
        {
            if (autoUsesCompute && computeUsable && !compute->IsSoftwareDevice())
            {
                selection.compute = compute;
            }
            return selection;
        }

        // An explicit PhysX request that can't be met says why, once per system
        selection.fellBack = true;
        const std::string detail = physics ? physics->GetParticlesGpuStatus().detail : "there is no physics backend";
        selection.reason = std::format(
            "ParticleSystem backend is PhysX, but no PhysX GPU particle backend is available ({}); "
            "using the CPU backend",
            detail.empty() ? "physics.gpu is off or the GPU is unavailable" : detail);
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

    void ParticleUsage::AddWebGpu()
    {
        _webGpu.fetch_add(1);
    }

    void ParticleUsage::RemoveWebGpu()
    {
        _webGpu.fetch_sub(1);
    }

    int ParticleUsage::GetWebGpuCount()
    {
        return _webGpu.load();
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
        const int counts[] = {_cpu.load(), _gpu.load(), _webGpu.load()};
        const char *names[] = {"CPU", "PhysX", "WebGPU"};
        std::string text;
        for (int i = 0; i < 3; ++i)
        {
            if (counts[i] <= 0)
            {
                continue;
            }
            text += std::format("{} {} {}", text.empty() ? "ParticleSystems:" : ",", counts[i], names[i]);
        }
        return text;
    }
}

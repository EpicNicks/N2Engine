#include "engine/softbody/SoftBodyBackendSelection.hpp"

#include <format>

namespace N2Engine::SoftBodies
{
    std::atomic<int> SoftBodyUsage::_cpu{0};
    std::atomic<int> SoftBodyUsage::_gpu{0};

    SoftBodyBackendSelection SelectSoftBodyBackend(const SoftBodyBackend requested, Physics::IPhysicsBackend *physics)
    {
        SoftBodyBackendSelection selection;
        if (requested == SoftBodyBackend::CPU)
        {
            return selection;
        }

        Physics::ISoftBodyBackend *gpu = physics ? physics->GetSoftBodyBackend() : nullptr;
        if (gpu)
        {
            selection.gpu = gpu;
            return selection;
        }

        // Auto quietly takes the CPU backend; an explicit PhysX request that can't be met says why, once per body
        if (requested == SoftBodyBackend::PhysX)
        {
            selection.fellBack = true;
            const std::string detail = physics ? physics->GetSoftBodiesGpuStatus().detail : "there is no physics backend";
            selection.reason = std::format(
                "SoftBody backend is PhysX, but no PhysX GPU soft-body backend is available ({}); "
                "using the CPU backend",
                detail.empty() ? "physics.gpu is off or the GPU is unavailable" : detail);
        }
        return selection;
    }

    void SoftBodyUsage::Add(const bool gpu)
    {
        (gpu ? _gpu : _cpu).fetch_add(1);
    }

    void SoftBodyUsage::Remove(const bool gpu)
    {
        (gpu ? _gpu : _cpu).fetch_sub(1);
    }

    int SoftBodyUsage::GetCount(const bool gpu)
    {
        return (gpu ? _gpu : _cpu).load();
    }

    std::string SoftBodyUsage::Compose(const std::string &base)
    {
        const std::string usage = Describe();
        if (usage.empty())
        {
            return base;
        }
        return base.empty() ? usage : base + "; " + usage;
    }

    std::string SoftBodyUsage::Describe()
    {
        const int cpu = _cpu.load();
        const int gpu = _gpu.load();
        if (cpu <= 0 && gpu <= 0)
        {
            return {};
        }
        std::string text = "SoftBodies:";
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

#include "engine/softbody/SoftBodyBackendSelection.hpp"

#include <format>

#include <renderer/common/GpuCompute.hpp>

namespace N2Engine::SoftBodies
{
    std::atomic<int> SoftBodyUsage::_cpu{0};
    std::atomic<int> SoftBodyUsage::_gpu{0};
    std::atomic<int> SoftBodyUsage::_webGpu{0};

    SoftBodyBackendSelection SelectSoftBodyBackend(const SoftBodyBackend requested, Physics::IPhysicsBackend *physics,
                                                   Renderer::Common::IGpuCompute *compute, const bool autoUsesCompute)
    {
        SoftBodyBackendSelection selection;
        if (requested == SoftBodyBackend::CPU)
        {
            return selection;
        }

        const bool computeUsable = compute && compute->IsUsable();
        if (requested == SoftBodyBackend::WebGPU)
        {
            if (computeUsable)
            {
                selection.compute = compute;
                return selection;
            }
            selection.fellBack = true;
            selection.reason = "SoftBody backend is WebGPU, but the renderer has no WebGPU compute (it runs only on the "
                               "WebGPU renderer, as in the browser); using the CPU backend";
            return selection;
        }

        Physics::ISoftBodyBackend *gpu = physics ? physics->GetSoftBodyBackend() : nullptr;
        if (gpu)
        {
            selection.gpu = gpu;
            return selection;
        }

        // Auto: after PhysX, the renderer's compute when the project lets the GPU simulate (physics.gpu auto or on, not
        // deterministic) and the device is a real GPU (a fallback adapter is the CPU emulating one: the CPU backend is
        // faster there), then quietly the CPU backend
        if (requested == SoftBodyBackend::Auto)
        {
            if (autoUsesCompute && computeUsable && !compute->IsSoftwareDevice())
            {
                selection.compute = compute;
            }
            return selection;
        }

        // An explicit PhysX request that can't be met says why, once per body
        selection.fellBack = true;
        const std::string detail = physics ? physics->GetSoftBodiesGpuStatus().detail : "there is no physics backend";
        selection.reason = std::format(
            "SoftBody backend is PhysX, but no PhysX GPU soft-body backend is available ({}); "
            "using the CPU backend",
            detail.empty() ? "physics.gpu is off or the GPU is unavailable" : detail);
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

    void SoftBodyUsage::AddWebGpu()
    {
        _webGpu.fetch_add(1);
    }

    void SoftBodyUsage::RemoveWebGpu()
    {
        _webGpu.fetch_sub(1);
    }

    int SoftBodyUsage::GetWebGpuCount()
    {
        return _webGpu.load();
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
        const int counts[] = {_cpu.load(), _gpu.load(), _webGpu.load()};
        const char *names[] = {"CPU", "PhysX", "WebGPU"};
        std::string text;
        for (int i = 0; i < 3; ++i)
        {
            if (counts[i] <= 0)
            {
                continue;
            }
            text += std::format("{} {} {}", text.empty() ? "SoftBodies:" : ",", counts[i], names[i]);
        }
        return text;
    }
}

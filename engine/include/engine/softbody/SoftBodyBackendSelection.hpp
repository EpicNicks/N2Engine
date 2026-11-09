#pragma once

#include <atomic>
#include <string>

#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/physics/ISoftBodyBackend.hpp"
#include "engine/serialization/FieldInfo.hpp" // N2_SERIALIZE_ENUM

namespace Renderer::Common
{
    class IGpuCompute;
}

namespace N2Engine::SoftBodies
{
    /// Which simulation a SoftBody asks for (its `backend` field)
    enum class SoftBodyBackend
    {
        /// Resolved when the body starts, the first that is there: the PhysX GPU backend (the physics scene has one:
        /// physics.gpu is auto or on and the GPU probe is Ready), then the WebGPU backend (the renderer is WebGPU, as in
        /// the browser, physics.gpu is auto or on and physics.deterministic is off), then the CPU backend
        Auto,
        /// The PhysX GPU backend; the CPU backend, with one warning, where there is none
        PhysX,
        /// Always the CPU backend: the same body on every machine
        CPU,
        /// The WebGPU compute backend (WebGpuSoftBodyBackend) on the renderer's device whatever physics.gpu says; the CPU
        /// backend, with one warning, where the renderer has no compute (it is not WebGPU)
        WebGPU
    };

    N2_SERIALIZE_ENUM(SoftBodyBackend, {
                      { SoftBodyBackend::Auto, "Auto" },
                      { SoftBodyBackend::PhysX, "PhysX" },
                      { SoftBodyBackend::CPU, "CPU" },
                      { SoftBodyBackend::WebGPU, "WebGPU" }
                      })

    /// The backend a SoftBody ended up with
    struct SoftBodyBackendSelection
    {
        /// The physics backend's GPU soft-body backend, when that is the one to use
        Physics::ISoftBodyBackend *gpu = nullptr;
        /// The renderer's compute, when the WebGPU backend is the one to use (then `gpu` is null). Both null: the CPU
        Renderer::Common::IGpuCompute *compute = nullptr;
        /// The request was PhysX or WebGPU and couldn't be met. `reason` says why (a warning for the log); empty otherwise.
        bool fellBack = false;
        std::string reason;
    };

    /**
     * Picks the backend for `requested`, with the semantics of Particles::SelectParticleBackend. `physics` is the
     * application's physics backend, which may be null (none built, or it failed to start). The PhysX GPU soft-body
     * backend exists only when physics->GetSoftBodyBackend() returns one (the PhysX physics backend returns its
     * DeformableSoftBodyBackend when the GPU scene is up and has not failed), else the selection is the CPU backend, and Auto is silent about it while PhysX falls
     * back with a reason. `compute` and `autoUsesCompute` add the WebGPU backend as for particles
     * (Particles::SelectParticleBackend).
     */
    [[nodiscard]] SoftBodyBackendSelection SelectSoftBodyBackend(SoftBodyBackend requested,
                                                                 Physics::IPhysicsBackend *physics,
                                                                 Renderer::Common::IGpuCompute *compute = nullptr,
                                                                 bool autoUsesCompute = false);

    /**
     * How many live SoftBody components use each backend, for the SoftBodies health row (Application::GetHealth adds
     * it to the row's detail). A process-wide count: the engine has one application, and a body registers when it
     * starts and leaves when it is destroyed.
     */
    class SoftBodyUsage
    {
    public:
        /// The bool forms count the CPU (false) and PhysX (true) backends; the WebGPU backend has its own
        static void Add(bool gpu);
        static void Remove(bool gpu);
        [[nodiscard]] static int GetCount(bool gpu);
        static void AddWebGpu();
        static void RemoveWebGpu();
        [[nodiscard]] static int GetWebGpuCount();
        /// "SoftBodies: 2 CPU, 1 PhysX, 3 WebGPU" (only the backends in use); empty when there are none
        [[nodiscard]] static std::string Describe();
        /// `base` (the SoftBodies row's detail from the GPU probe) followed by Describe() after "; ", or `base`
        /// alone when there are no bodies
        [[nodiscard]] static std::string Compose(const std::string &base);

    private:
        static std::atomic<int> _cpu;
        static std::atomic<int> _gpu;
        static std::atomic<int> _webGpu;
    };
}

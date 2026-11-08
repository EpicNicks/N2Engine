#pragma once

#include <atomic>
#include <string>

#include "engine/physics/IParticleBackend.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/serialization/FieldInfo.hpp" // N2_SERIALIZE_ENUM

namespace Renderer::Common
{
    class IGpuCompute;
}

namespace N2Engine::Particles
{
    /// Which simulation a ParticleSystem asks for (its `backend` field)
    enum class ParticleBackend
    {
        /// Resolved when the system starts, the first that is there: the PhysX GPU backend (the physics scene has one:
        /// physics.gpu is auto or on and the GPU probe is Ready), then the WebGPU backend (the renderer is WebGPU, as in
        /// the browser, physics.gpu is auto or on and physics.deterministic is off), then the CPU backend
        Auto,
        /// The PhysX GPU backend; the CPU backend, with one warning, where there is none
        PhysX,
        /// Always the CPU backend: the same particles on every machine
        CPU,
        /// The WebGPU compute backend (WebGpuParticleBackend) on the renderer's device whatever physics.gpu says; the CPU
        /// backend, with one warning, where the renderer has no compute (it is not WebGPU)
        WebGPU
    };

    N2_SERIALIZE_ENUM(ParticleBackend, {
                      { ParticleBackend::Auto, "Auto" },
                      { ParticleBackend::PhysX, "PhysX" },
                      { ParticleBackend::CPU, "CPU" },
                      { ParticleBackend::WebGPU, "WebGPU" }
                      })

    /// The backend a ParticleSystem ended up with
    struct BackendSelection
    {
        /// The physics backend's GPU particle backend, when that is the one to use
        Physics::IParticleBackend *gpu = nullptr;
        /// The renderer's compute, when the WebGPU backend is the one to use (then `gpu` is null). Both null: the CPU
        Renderer::Common::IGpuCompute *compute = nullptr;
        /// The request was PhysX or WebGPU and couldn't be met. `reason` says why (a warning for the log); empty otherwise.
        bool fellBack = false;
        std::string reason;
    };

    /**
     * Picks the backend for `requested`. `physics` is the application's physics backend, which may be null (none
     * built, or it failed to start). The PhysX GPU particle backend exists only when physics->GetParticleBackend()
     * returns one (the PhysX backend does when its GPU scene is up and the GPU still works). `compute` is the renderer's
     * compute (Application::GetGpuCompute: the WebGPU renderer's), which may be null; Auto takes it only when
     * `autoUsesCompute` (Application::IsGpuSimulationAllowed: physics.gpu auto or on, physics.deterministic off), WebGPU
     * whenever it is usable. When no GPU backend serves the request the selection is the CPU backend: Auto is silent
     * about it, while PhysX and WebGPU fall back with a reason.
     */
    [[nodiscard]] BackendSelection SelectParticleBackend(ParticleBackend requested, Physics::IPhysicsBackend *physics,
                                                         Renderer::Common::IGpuCompute *compute = nullptr,
                                                         bool autoUsesCompute = false);

    /**
     * How many live ParticleSystems use each backend, for the Particles health row (Application::GetHealth adds
     * it to the row's detail). A process-wide count: the engine has one application, and a system registers
     * when it attaches and leaves when it is destroyed.
     */
    class ParticleUsage
    {
    public:
        /// The bool forms count the CPU (false) and PhysX (true) backends; the WebGPU backend has its own
        static void Add(bool gpu);
        static void Remove(bool gpu);
        [[nodiscard]] static int GetCount(bool gpu);
        static void AddWebGpu();
        static void RemoveWebGpu();
        [[nodiscard]] static int GetWebGpuCount();
        /// "ParticleSystems: 2 CPU, 1 PhysX, 3 WebGPU" (only the backends in use); empty when there are none
        [[nodiscard]] static std::string Describe();
        /// `base` (the Particles row's detail from the GPU probe) followed by Describe() after "; ", or `base`
        /// alone when there are no systems
        [[nodiscard]] static std::string Compose(const std::string &base);

    private:
        static std::atomic<int> _cpu;
        static std::atomic<int> _gpu;
        static std::atomic<int> _webGpu;
    };
}

#pragma once

#include <atomic>
#include <string>

#include "engine/physics/IParticleBackend.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/serialization/FieldInfo.hpp" // N2_SERIALIZE_ENUM

namespace N2Engine::Particles
{
    /// Which simulation a ParticleSystem asks for (its `backend` field)
    enum class ParticleBackend
    {
        /// The PhysX GPU backend when the physics scene has one (physics.gpu is auto or on and the GPU probe is
        /// Ready), else the CPU backend
        Auto,
        /// The PhysX GPU backend; the CPU backend, with one warning, where there is none
        PhysX,
        /// Always the CPU backend: the same particles on every machine
        CPU
    };

    N2_SERIALIZE_ENUM(ParticleBackend, {
                      { ParticleBackend::Auto, "Auto" },
                      { ParticleBackend::PhysX, "PhysX" },
                      { ParticleBackend::CPU, "CPU" }
                      })

    /// The backend a ParticleSystem ended up with
    struct BackendSelection
    {
        /// The physics backend's GPU particle backend, when that is the one to use; nullptr means the CPU backend
        Physics::IParticleBackend *gpu = nullptr;
        /// The request was PhysX and couldn't be met. `reason` says why (a warning for the log); empty otherwise.
        bool fellBack = false;
        std::string reason;
    };

    /**
     * Picks the backend for `requested`. `physics` is the application's physics backend, which may be null (none
     * built, or it failed to start). The PhysX GPU particle backend exists only when physics->GetParticleBackend()
     * returns one (the PhysX backend does when its GPU scene is up and the GPU still works), else the selection is
     * the CPU backend, and Auto is silent about it while PhysX falls back with a reason.
     */
    [[nodiscard]] BackendSelection SelectParticleBackend(ParticleBackend requested, Physics::IPhysicsBackend *physics);

    /**
     * How many live ParticleSystems use each backend, for the Particles health row (Application::GetHealth adds
     * it to the row's detail). A process-wide count: the engine has one application, and a system registers
     * when it attaches and leaves when it is destroyed.
     */
    class ParticleUsage
    {
    public:
        static void Add(bool gpu);
        static void Remove(bool gpu);
        [[nodiscard]] static int GetCount(bool gpu);
        /// "ParticleSystems: 2 CPU, 1 PhysX"; empty when there are none
        [[nodiscard]] static std::string Describe();
        /// `base` (the Particles row's detail from the GPU probe) followed by Describe() after "; ", or `base`
        /// alone when there are no systems
        [[nodiscard]] static std::string Compose(const std::string &base);

    private:
        static std::atomic<int> _cpu;
        static std::atomic<int> _gpu;
    };
}

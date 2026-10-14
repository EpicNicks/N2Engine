#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "engine/physics/BackendHandle.hpp"
#include "engine/physics/ParticleMaterial.hpp"

namespace N2Engine::Physics
{
    /**
     * One particle system inside a particle backend. 0 is never a valid handle.
     *
     * A handle carries the GENERATION of the backend that issued it (the upper 32 bits; the lower 32 are the
     * system's index in that backend). Every backend object takes a new generation from a process-wide counter
     * when it is constructed, so a backend that was replaced or recreated, even at the same address as the old
     * one, does not know the handles of its predecessor: they fail its generation check and are ignored, instead
     * of naming one of the new backend's systems. The helper is shared with the other feature backends
     * (BackendHandle.hpp); the names below are this feature's.
     */
    using ParticleSystemHandle = BackendHandle;
    inline constexpr ParticleSystemHandle kInvalidParticleSystem = kInvalidBackendHandle;

    namespace Detail
    {
        struct ParticleGenerationTag;
    }

    [[nodiscard]] constexpr ParticleSystemHandle MakeParticleHandle(const std::uint32_t generation,
                                                                    const std::uint32_t index)
    {
        return MakeBackendHandle(generation, index);
    }

    [[nodiscard]] constexpr std::uint32_t ParticleHandleGeneration(const ParticleSystemHandle handle)
    {
        return BackendHandleGeneration(handle);
    }

    [[nodiscard]] constexpr std::uint32_t ParticleHandleIndex(const ParticleSystemHandle handle)
    {
        return BackendHandleIndex(handle);
    }

    /// The next backend generation: 1, 2, 3, ... for the life of the process (0 is never issued, so a handle of
    /// generation 0 belongs to no backend)
    [[nodiscard]] inline std::uint32_t NextParticleBackendGeneration()
    {
        return NextBackendGeneration<Detail::ParticleGenerationTag>();
    }

    /// The simulation settings a backend can change at any time (SetParams). Emission (when and where particles
    /// start, with what velocity) is the caller's: the backend only simulates what Spawn gives it.
    struct ParticleParams
    {
        /// Acceleration in world units per second squared (the ParticleSystem multiplies the world gravity by its
        /// gravityScale)
        Math::Vector3 gravity{0.0f, -9.81f, 0.0f};
        /// A particle's lifetime is picked from [lifetimeMin, lifetimeMax] seconds when it is spawned
        float lifetimeMin = 1.0f;
        float lifetimeMax = 1.0f;
        /// Velocity lost per second, as a fraction (0 = none; v *= max(0, 1 - drag * dt) each step)
        float drag = 0.0f;
        /// Particles bounce off the plane y = groundHeight (world space), keeping `bounce` of the speed they hit
        /// it with; a backend with no ground plane ignores these
        bool groundEnabled = false;
        float groundHeight = 0.0f;
        float bounce = 0.3f;
    };

    struct ParticleSystemDesc
    {
        /// The most particles alive at once; Spawn accepts no more than there is room for
        std::uint32_t maxParticles = 1000;
        /// Seeds the backend's own random choices (a spawned particle's lifetime). The same seed and the same calls
        /// give the same particles.
        std::uint32_t seed = 1;
        /// The PhysX backend's material preset (fluid, granular or smoke-like behaviour); the CPU backend ignores it
        ParticleMaterial material = ParticleMaterial::Water;
        ParticleParams params;
    };

    /**
     * Simulates particles for ParticleSystem components. The seam between the component and whatever runs the
     * simulation: the engine's CPU backend (Particles::CpuParticleBackend, every machine, headless included) and,
     * later, a PhysX GPU backend that IPhysicsBackend::GetParticleBackend returns when the scene runs on the GPU.
     * No PhysX or CUDA type appears here.
     *
     * A backend holds many systems, each named by a handle. Everything is called on the main thread. A backend
     * gets its generation (see ParticleSystemHandle) when it is constructed, and returns it from GetGeneration;
     * every handle it issues carries it, and every call with a handle of another generation is ignored.
     */
    class IParticleBackend
    {
    public:
        virtual ~IParticleBackend() = default;

        /// What the handles this backend issued carry; never 0, never the same for two backend objects
        [[nodiscard]] std::uint32_t GetGeneration() const { return _generation; }
        /// True when `handle` was issued by this backend object (its generation matches). It says nothing of
        /// whether the system still exists.
        [[nodiscard]] bool OwnsHandle(const ParticleSystemHandle handle) const
        {
            return BackendHandleOwnedBy(_generation, handle);
        }

        /// "CPU" or "PhysX": what health and logs call the backend
        [[nodiscard]] virtual std::string_view GetName() const = 0;

        /// kInvalidParticleSystem if the backend can't make another system
        [[nodiscard]] virtual ParticleSystemHandle CreateSystem(const ParticleSystemDesc &desc) = 0;
        virtual void Destroy(ParticleSystemHandle handle) = 0;
        virtual void SetParams(ParticleSystemHandle handle, const ParticleParams &params) = 0;

        /**
         * Adds particles at `positions` (world space) moving at `velocities` (the same count; extra entries of
         * the longer span are ignored). Returns how many were accepted: fewer than asked when the system is full
         * (maxParticles), none for an unknown handle.
         */
        virtual std::size_t Spawn(ParticleSystemHandle handle, std::span<const Math::Vector3> positions,
                                  std::span<const Math::Vector3> velocities) = 0;
        /// Removes every particle of the system
        virtual void Clear(ParticleSystemHandle handle) = 0;

        /**
         * Advances the simulation by `dt` seconds. A CPU backend does its work here, and its owner calls this once
         * per fixed step. A backend that simulates inside the physics scene (the scene steps it) does nothing.
         */
        virtual void Step(float dt) = 0;

        [[nodiscard]] virtual std::size_t GetAliveCount(ParticleSystemHandle handle) const = 0;
        /**
         * Replaces `out` with one entry per living particle: x, y, z the world position, w the particle's age as a
         * fraction of its lifetime (0 when it was spawned, towards 1 as it dies). `out` keeps its capacity, so a
         * caller that reuses it allocates nothing once it has grown.
         */
        virtual void CopyPositions(ParticleSystemHandle handle, std::vector<Math::Vector4> &out) const = 0;

    protected:
        IParticleBackend() : _generation(NextParticleBackendGeneration()) {}
        /// For a backend that wraps another and must hand out handles the wrapper's own generation owns
        explicit IParticleBackend(const std::uint32_t generation) : _generation(generation) {}

    private:
        std::uint32_t _generation;
    };
}

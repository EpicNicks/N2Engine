#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "engine/particles/ParticleRandom.hpp"
#include "engine/physics/IParticleBackend.hpp"

namespace N2Engine::Particles
{
    /**
     * The particle simulation that needs nothing but the CPU: it runs on every machine, headless hosts and CI
     * included, and is what a ParticleSystem uses when the PhysX GPU backend isn't there.
     *
     * Each particle is semi-implicit Euler integrated (gravity, then drag, then position) in Step, which the
     * ParticleSystem calls once per fixed step, so the result depends only on the fixed timestep and the calls
     * made, never on the frame rate. It is deterministic: the arithmetic is plain float code in a fixed order
     * (no SIMD vector types, no std random distributions), and the only random choice (a lifetime within the
     * range) comes from a ParticleRandom seeded by the system's desc. A particle that reaches its lifetime is
     * removed by moving the last particle into its slot, so the order of particles changes as they die (the same
     * way every run).
     *
     * Collision: only the optional ground plane (ParticleParams::groundEnabled). Particles do not collide with
     * colliders or with each other; that is the PhysX backend's job (the CPU backend never queries the physics
     * world, so it also behaves the same with no physics backend).
     *
     * Memory: a system's arrays grow as particles are spawned (never past maxParticles) and keep their capacity,
     * so a steady stream of particles allocates nothing after the first seconds.
     */
    class CpuParticleBackend final : public Physics::IParticleBackend
    {
    public:
        /// A system asking for more than this many particles gets this many (the desc is clamped, not refused)
        static constexpr std::uint32_t kMaxParticlesLimit = 1000000;

        [[nodiscard]] std::string_view GetName() const override { return "CPU"; }

        [[nodiscard]] Physics::ParticleSystemHandle CreateSystem(const Physics::ParticleSystemDesc &desc) override;
        void Destroy(Physics::ParticleSystemHandle handle) override;
        void SetParams(Physics::ParticleSystemHandle handle, const Physics::ParticleParams &params) override;
        std::size_t Spawn(Physics::ParticleSystemHandle handle, std::span<const Math::Vector3> positions,
                          std::span<const Math::Vector3> velocities) override;
        void Clear(Physics::ParticleSystemHandle handle) override;
        void Step(float dt) override;
        [[nodiscard]] std::size_t GetAliveCount(Physics::ParticleSystemHandle handle) const override;
        void CopyPositions(Physics::ParticleSystemHandle handle, std::vector<Math::Vector4> &out) const override;

        /// How many systems exist (for tests and diagnostics)
        [[nodiscard]] std::size_t GetSystemCount() const { return _systems.size(); }

    private:
        struct System
        {
            std::uint32_t maxParticles = 0;
            Physics::ParticleParams params;
            ParticleRandom random;
            // One entry per living particle, in step order
            std::vector<float> px, py, pz;
            std::vector<float> vx, vy, vz;
            std::vector<float> age, lifetime;

            [[nodiscard]] std::size_t Count() const { return age.size(); }
            void Remove(std::size_t index);
        };

        static Physics::ParticleParams Sanitized(Physics::ParticleParams params);

        std::unordered_map<Physics::ParticleSystemHandle, System> _systems;
        Physics::ParticleSystemHandle _nextHandle = 1;
    };
}

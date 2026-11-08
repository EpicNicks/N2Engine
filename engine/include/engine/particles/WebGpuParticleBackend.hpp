#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <renderer/common/GpuCompute.hpp>

#include "engine/particles/ParticleRandom.hpp"
#include "engine/physics/IParticleBackend.hpp"

namespace N2Engine::Particles
{
    /**
     * Particles simulated on the renderer's GPU through WebGPU compute (Renderer::Common::IGpuCompute; the WGSL is
     * renderer/shaders/webgpu/compute/particles.wgsl). What a ParticleSystem uses for the WebGPU backend, and for Auto in a
     * browser whose renderer is WebGPU (see SelectParticleBackend). Position-based particles on a hashed grid, with the
     * material preset of the system (GetPbdPreset: spacing, friction, damping, fluid or granular):
     * - Water and Smoke solve a one-sided density constraint (position based fluids), so they spread and pool instead of
     *   passing through each other; Water adds a small artificial pressure (surface tension);
     * - Sand solves contact constraints with friction, so it piles up;
     * - every particle falls with the system's own gravity (gravityScale applied, so a negative scale rises), loses
     *   velocity to drag plus the preset's damping, and keeps above the optional ground plane (groundEnabled), off which
     *   it bounces with `bounce` of its speed. No scene colliders: like the CPU backend, the ground plane is the only
     *   collision with the world.
     * Each Step runs kSubSteps sub-steps of kIterations solver iterations.
     *
     * The CPU keeps the bookkeeping: which slots hold a particle, every particle's lifetime (counted down with the same
     * float arithmetic the GPU uses), so GetAliveCount never waits for the GPU. Positions come back asynchronously: each
     * Step asks for a copy of the positions when none is in flight, and CopyPositions gives the latest copy that has
     * arrived (one to a few steps old; in a browser the copy lands only after control returned to its event loop). A
     * particle spawned after that copy was taken is placed where it would be on a ballistic path from its spawn.
     *
     * The backend lives as long as the renderer that made its compute (the `lifetime` token): once that expires, or the
     * device is lost, or a GPU call fails, IsHealthy is false, it simulates nothing more and the owner moves its system to
     * the CPU. Not deterministic across GPUs (the order threads meet in the grid and floating-point details vary).
     */
    class WebGpuParticleBackend final : public Physics::IParticleBackend
    {
    public:
        static constexpr int kSubSteps = 2;
        static constexpr int kIterations = 3;
        /// Particles one grid bucket keeps for the neighbour search (more are left out of it)
        static constexpr std::uint32_t kCellCapacity = 16;
        /// The most particles one system may have (the component's own limit is lower)
        static constexpr std::uint32_t kMaxParticlesLimit = 1000000;

        WebGpuParticleBackend(Renderer::Common::IGpuCompute *compute, std::weak_ptr<const void> lifetime);
        ~WebGpuParticleBackend() override;
        WebGpuParticleBackend(const WebGpuParticleBackend &) = delete;
        WebGpuParticleBackend &operator=(const WebGpuParticleBackend &) = delete;

        [[nodiscard]] std::string_view GetName() const override { return "WebGPU"; }

        /// kInvalidParticleSystem when the backend is not healthy, for no room (maxParticles 0), or when the GPU refuses a
        /// buffer or a kernel
        [[nodiscard]] Physics::ParticleSystemHandle CreateSystem(const Physics::ParticleSystemDesc &desc) override;
        void Destroy(Physics::ParticleSystemHandle handle) override;
        void SetParams(Physics::ParticleSystemHandle handle, const Physics::ParticleParams &params) override;
        std::size_t Spawn(Physics::ParticleSystemHandle handle, std::span<const Math::Vector3> positions,
                          std::span<const Math::Vector3> velocities) override;
        void Clear(Physics::ParticleSystemHandle handle) override;
        void Step(float dt) override;
        [[nodiscard]] std::size_t GetAliveCount(Physics::ParticleSystemHandle handle) const override;
        void CopyPositions(Physics::ParticleSystemHandle handle, std::vector<Math::Vector4> &out) const override;

        /// True while the renderer lives, its device runs and every GPU call so far succeeded
        [[nodiscard]] bool IsHealthy() const;

        // For tests
        /// NATIVE ONLY: waits for the GPU and lands every read in flight, then starts and lands a fresh one per system, so
        /// CopyPositions gives the positions as of the last Step. False in a browser or when not healthy
        bool SyncForTest();
        [[nodiscard]] std::size_t GetSystemCount() const { return _systems.size(); }

    private:
        struct GpuParams; // the WGSL Params block (WebGpuParticleBackend.cpp)

        struct System
        {
            Physics::ParticleParams params;
            std::uint32_t capacity = 0;
            std::uint32_t hashBuckets = 0;
            bool granular = false;
            float spacing = 0.1f;
            float radius = 0.2f;
            float restDensity = 1.0f;
            float restGradientSq = 1.0f;
            float friction = 0.0f;
            float damping = 0.0f;
            float tensile = 0.0f;
            ParticleRandom random;

            Renderer::Common::GpuBufferId uniform = 0;
            Renderer::Common::GpuBufferId positions = 0;
            Renderer::Common::GpuBufferId velocities = 0;
            Renderer::Common::GpuBufferId predicted = 0;
            Renderer::Common::GpuBufferId deltas = 0;
            Renderer::Common::GpuBufferId cellCounts = 0;
            Renderer::Common::GpuBufferId cellItems = 0;

            // Slots: [0, highWater) may hold particles; `remaining` > 0 marks a live one
            std::vector<float> remaining;
            std::vector<float> lifetime;
            std::vector<Math::Vector3> spawnPosition;
            std::vector<Math::Vector3> spawnVelocity;
            std::vector<std::uint64_t> spawnStep; // `steps` when the particle was spawned
            std::vector<std::uint32_t> freeSlots;   // a min-heap of dead slots below highWater (may hold stale entries)
            std::uint32_t highWater = 0;
            std::size_t alive = 0;
            std::uint64_t steps = 0;

            // The asynchronous copy of the positions
            Renderer::Common::GpuReadTicket ticket = 0;
            std::uint64_t ticketStep = 0;
            std::vector<Math::Vector4> mirror; // the last copy that landed: x, y, z and the GPU's remaining lifetime
            std::uint64_t mirrorStep = 0;      // `steps` when it was taken (0: none yet)
            std::vector<std::uint8_t> readBytes;
        };

        [[nodiscard]] bool Usable() const;
        void MarkBroken();
        [[nodiscard]] System *Find(Physics::ParticleSystemHandle handle);
        [[nodiscard]] const System *Find(Physics::ParticleSystemHandle handle) const;
        void ReleaseBuffers(System &system);
        [[nodiscard]] bool MakeKernels();
        [[nodiscard]] std::uint32_t TakeSlot(System &system);
        void FreeSlot(System &system, std::uint32_t slot);
        /// Lands the system's read in flight if it has completed (const callers too: the mirror is a cache)
        void PollRead(System &system) const;
        void StepSystem(System &system, float dt);
        [[nodiscard]] bool WriteParams(System &system, float h) const;

        Renderer::Common::IGpuCompute *_compute;
        std::weak_ptr<const void> _lifetime;
        bool _broken = false;
        Renderer::Common::GpuKernelId _clearCells = 0;
        Renderer::Common::GpuKernelId _predict = 0;
        Renderer::Common::GpuKernelId _lambdas = 0;
        Renderer::Common::GpuKernelId _deltas = 0;
        Renderer::Common::GpuKernelId _apply = 0;
        Renderer::Common::GpuKernelId _finalize = 0;
        mutable std::map<std::uint32_t, System> _systems; // by handle index; mutable: CopyPositions lands reads
        std::uint32_t _nextIndex = 1;
    };
}

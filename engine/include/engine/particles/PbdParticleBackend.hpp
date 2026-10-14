#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/particles/ParticleRandom.hpp"
#include "engine/particles/PbdDriver.hpp"
#include "engine/physics/IParticleBackend.hpp"

namespace N2Engine::Particles
{
    /**
     * The PhysX GPU particle backend's logic, on top of an IPbdDriver (the only thing that touches PhysX and CUDA).
     * With the real driver it is what the physics backend returns from GetParticleBackend when the GPU scene is up;
     * the tests run it against a fake driver on every CI leg.
     *
     * One PBD system per ParticleSystem, each with a buffer of maxParticles slots. The slots 0..count-1 are the
     * living particles; the host mirror the driver exposes holds them, and the backend ages them on the host:
     *
     *  - Spawn writes new particles into the mirror after the last slot (position, inverse mass, velocity, phase),
     *    capped at maxParticles, and uploads: the GPU buffer takes the mirror and the active count grows.
     *  - OnSceneStepped(dt) is called by the physics backend after every scene step (the scene simulates the
     *    particles; Step does nothing). It reads the positions and velocities back into the mirror, ages every
     *    particle by dt, and removes the ones past their lifetime by moving the last live particle into their slot
     *    (the CPU backend's rule), uploading the compacted mirror when anything died.
     *  - CopyPositions and GetAliveCount read the mirror only: no GPU call.
     *
     * Failure is soft: when the driver refuses a system or a transfer, the backend remembers why (HasFailed), logs
     * once and does nothing more with the GPU; its owner then stops offering it, and the ParticleSystems that used
     * it restart on the CPU backend.
     *
     * Handles carry this backend's generation (IParticleBackend), so a handle of a replaced or recreated backend
     * is ignored here, even if the new backend sits at the same address.
     */
    class PbdParticleBackend final : public Physics::IParticleBackend
    {
    public:
        /// A system asking for more than this many particles gets this many
        static constexpr std::uint32_t kMaxParticlesLimit = 1000000;

        explicit PbdParticleBackend(std::unique_ptr<IPbdDriver> driver);
        ~PbdParticleBackend() override;

        [[nodiscard]] std::string_view GetName() const override { return "PhysX"; }

        [[nodiscard]] Physics::ParticleSystemHandle CreateSystem(const Physics::ParticleSystemDesc &desc) override;
        void Destroy(Physics::ParticleSystemHandle handle) override;
        void SetParams(Physics::ParticleSystemHandle handle, const Physics::ParticleParams &params) override;
        std::size_t Spawn(Physics::ParticleSystemHandle handle, std::span<const Math::Vector3> positions,
                          std::span<const Math::Vector3> velocities) override;
        void Clear(Physics::ParticleSystemHandle handle) override;
        /// Nothing: the physics scene steps the particles (see OnSceneStepped)
        void Step(float dt) override { static_cast<void>(dt); }
        [[nodiscard]] std::size_t GetAliveCount(Physics::ParticleSystemHandle handle) const override;
        void CopyPositions(Physics::ParticleSystemHandle handle, std::vector<Math::Vector4> &out) const override;

        /// After the scene stepped by `dt` seconds (and fetchResults returned): refreshes the mirrors from the GPU,
        /// ages the particles and removes the dead
        void OnSceneStepped(float dt);

        /// The driver (or a transfer) failed; the backend does nothing with the GPU from then on
        [[nodiscard]] bool HasFailed() const { return _failed; }
        [[nodiscard]] const std::string &GetFailureReason() const { return _failureReason; }
        /// Destroys every system (the driver's, and the backend's records of them)
        void ReleaseAll();

        [[nodiscard]] std::string GetDeviceName() const { return _driver->GetDeviceName(); }
        [[nodiscard]] std::size_t GetSystemCount() const { return _systems.size(); }

    private:
        struct System
        {
            PbdSystemId driverId = kInvalidPbdSystem; // none for a system of 0 particles
            std::uint32_t maxParticles = 0;
            std::uint32_t count = 0;
            Physics::ParticleParams params;
            PbdPreset preset;
            PbdMaterialParams material; // what the driver has
            ParticleRandom random;
            PbdStaging staging;
            std::uint32_t phase = 0;
            float invMass = 1.0f;
            std::vector<float> age;
            std::vector<float> lifetime;

            void Remove(std::uint32_t index);
        };

        [[nodiscard]] System *Find(Physics::ParticleSystemHandle handle);
        [[nodiscard]] const System *Find(Physics::ParticleSystemHandle handle) const;
        void Fail(const std::string &reason);
        /// Sends the mirror (the first system.count entries) to the GPU
        void Upload(System &system, std::uint32_t dirty);

        std::unique_ptr<IPbdDriver> _driver;
        std::unordered_map<Physics::ParticleSystemHandle, System> _systems;
        std::uint32_t _nextIndex = 1;
        bool _failed = false;
        std::string _failureReason;
    };
}

#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <math/Vector3.hpp>

#include "engine/physics/ParticleMaterial.hpp"

namespace N2Engine::Particles
{
    /// A PBD material, in the parameters of PxPhysics::createPBDMaterial (see GetPbdPreset for the values of each
    /// preset and what the backend changes at run time)
    struct PbdMaterialParams
    {
        float friction = 0.0f;
        float damping = 0.0f;
        float adhesion = 0.0f;
        float viscosity = 0.0f;
        float vorticityConfinement = 0.0f;
        float surfaceTension = 0.0f;
        float cohesion = 0.0f;
        float lift = 0.0f;
        float drag = 0.0f;
        float cflCoefficient = 1.0f;
        float gravityScale = 1.0f;

        [[nodiscard]] bool operator==(const PbdMaterialParams &) const = default;
    };

    /// A material preset: the PBD material and how the particles are laid out and treated by the solver
    struct PbdPreset
    {
        PbdMaterialParams material;
        /// The distance between neighbouring particles at rest, in world units (sizes the solver's offsets)
        float particleSpacing = 0.1f;
        /// Kilograms per cubic metre; a particle's mass is density * spacing^3
        float density = 1000.0f;
        /// Particles generate fluid density constraints with their neighbours (water), else they only collide
        bool fluid = false;
    };

    /// The PBD parameters of a preset. These are starting values chosen from PhysX's particle snippets and the
    /// behaviour each material should have; they have not been tuned on a GPU (see docs/particles.html).
    [[nodiscard]] PbdPreset GetPbdPreset(Physics::ParticleMaterial material);

    /// The material for `preset` under the system's current parameters: gravityScale is the part of `gravity` along
    /// the scene's gravity (times the preset's), and the system's drag is added to the preset's damping
    [[nodiscard]] PbdMaterialParams ComposePbdMaterial(const PbdPreset &preset, const Math::Vector3 &gravity,
                                                       const Math::Vector3 &sceneGravity, float drag);

    /// A driver's id for one PBD particle system
    using PbdSystemId = std::uint32_t;
    inline constexpr PbdSystemId kInvalidPbdSystem = 0;

    /// 16 bytes, laid out like PxVec4 (x, y, z, w)
    struct PbdVec4
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;
    };
    static_assert(sizeof(PbdVec4) == 16);

    /// Which host arrays changed and must be sent to the GPU (bit flags)
    enum PbdDirty : std::uint32_t
    {
        kPbdNothing = 0,
        kPbdPositions = 1u << 0,
        kPbdVelocities = 1u << 1,
        kPbdPhases = 1u << 2,
    };

    struct PbdSystemDesc
    {
        std::uint32_t maxParticles = 0;
        PbdMaterialParams material;
        PbdPreset preset;
    };

    /// The host mirror of one system's particle buffer: maxParticles entries each
    struct PbdStaging
    {
        /// x, y, z the position, w the inverse mass
        std::span<PbdVec4> positions;
        /// x, y, z the velocity, w 0
        std::span<PbdVec4> velocities;
        std::span<std::uint32_t> phases;
    };

    /**
     * The thin layer between the PBD backend's logic (slots, emission, ageing, compaction, presets, handles,
     * fallbacks) and the PhysX/CUDA calls. PbdParticleBackend only ever talks to this; the real implementation
     * (Physics::PhysXPbdDriver) is the one place a CUDA context, a PBD system or a device pointer appears, and the
     * tests run the backend against a fake. Everything is called on the main thread, outside a scene step.
     *
     * Each system has a host mirror (PbdStaging). The backend writes it and calls Upload to send it to the GPU,
     * and calls ReadBack after the scene step to refresh it from the GPU.
     */
    class IPbdDriver
    {
    public:
        virtual ~IPbdDriver() = default;

        /// The GPU's name, for the health row
        [[nodiscard]] virtual std::string GetDeviceName() const = 0;
        /// The scene's gravity (PBD particles fall with it, scaled by their material)
        [[nodiscard]] virtual Math::Vector3 GetSceneGravity() const = 0;

        /// Makes a PBD system with a material, a phase and a buffer for maxParticles particles (not empty: at least
        /// 1). kInvalidPbdSystem with `error` set when it can't (no GPU library, out of GPU memory, ...)
        [[nodiscard]] virtual PbdSystemId CreateSystem(const PbdSystemDesc &desc, std::string &error) = 0;
        /// Releases the system and everything it owns. An unknown id is ignored.
        virtual void DestroySystem(PbdSystemId id) = 0;

        /// The host mirror of the system (empty spans for an unknown id)
        [[nodiscard]] virtual PbdStaging GetStaging(PbdSystemId id) = 0;
        /// The phase value every particle of the system is written with
        [[nodiscard]] virtual std::uint32_t GetPhase(PbdSystemId id) const = 0;
        /// Changes the material of a live system
        virtual void SetMaterial(PbdSystemId id, const PbdMaterialParams &material) = 0;

        /// Sends the `dirty` host arrays (the first `activeCount` entries) to the GPU, sets the number of active
        /// particles to activeCount and tells the solver what changed. False on a failure (`error` says why).
        [[nodiscard]] virtual bool Upload(PbdSystemId id, std::uint32_t activeCount, std::uint32_t dirty,
                                          std::string &error) = 0;
        /// Copies the first `count` positions and velocities from the GPU into the host mirror, after the scene
        /// step finished. False on a failure (`error` says why).
        [[nodiscard]] virtual bool ReadBack(PbdSystemId id, std::uint32_t count, std::string &error) = 0;
    };
}

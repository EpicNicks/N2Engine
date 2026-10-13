#pragma once

#include <cstdint>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include "engine/particles/ParticleRandom.hpp"
#include "engine/serialization/FieldInfo.hpp" // N2_SERIALIZE_ENUM

namespace N2Engine::Particles
{
    /// Where an emitter starts particles, and in which directions
    enum class EmitterShape
    {
        /// From a disc of shapeRadius on the emitter's local XZ plane, along the emitter's local +Y axis, spread
        /// within coneAngle degrees of it (0 = a straight stream, 90 = a flat fan, 180 = every direction)
        Cone,
        /// From a random point inside a sphere of shapeRadius, moving straight away from its centre
        Sphere
    };

    N2_SERIALIZE_ENUM(EmitterShape, {
                      { EmitterShape::Cone, "Cone" },
                      { EmitterShape::Sphere, "Sphere" }
                      })

    /// What ParticleEmitter::Generate needs of the component's settings
    struct EmitterShapeSettings
    {
        EmitterShape shape = EmitterShape::Cone;
        float shapeRadius = 0.0f;
        /// Half angle of the cone, in degrees
        float coneAngle = 25.0f;
        float speedMin = 2.0f;
        float speedMax = 4.0f;
    };

    /**
     * The emitting half of a particle system, with no engine dependencies beyond the maths types: it decides how
     * many particles are due (a rate that accumulates across steps, so 7 particles per second at a 0.02 s step
     * gives one every 7th step, never a rounding drift) and where each one starts and how fast it moves.
     * The simulation of what it emits is an IParticleBackend's.
     *
     * Deterministic: the same seed and the same calls give the same particles (see ParticleRandom).
     */
    class ParticleEmitter
    {
    public:
        explicit ParticleEmitter(const std::uint32_t seed = 1) : _random(seed) {}

        /// Starts the random sequence again and forgets the rate's fraction
        void Reset(std::uint32_t seed);
        /// Forgets the rate's fraction only (a Stop followed by a Play starts a stream afresh)
        void ResetRate() { _fraction = 0.0f; }

        /**
         * The whole particles due after `dt` seconds at `rate` per second, remembering the fraction left over. 0
         * for a rate or step that is not positive and finite. A huge step is limited to 4096 particles at once
         * (the cap of any system is far lower in practice), the rest of the fraction is dropped.
         */
        [[nodiscard]] std::uint32_t Advance(float rate, float dt);

        /**
         * Replaces `positions` and `velocities` with `count` new particles for an emitter at `origin` rotated by
         * `rotation`. The vectors keep their capacity between calls.
         */
        void Generate(const EmitterShapeSettings &settings, const Math::Vector3 &origin,
                      const Math::Quaternion &rotation, std::uint32_t count, std::vector<Math::Vector3> &positions,
                      std::vector<Math::Vector3> &velocities);

    private:
        ParticleRandom _random;
        float _fraction = 0.0f;
    };
}

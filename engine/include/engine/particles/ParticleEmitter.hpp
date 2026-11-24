#pragma once

#include <cstdint>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/particles/ParticleRandom.hpp"
#include "engine/serialization/FieldInfo.hpp" // N2_SERIALIZE_ENUM

namespace N2Engine::Particles
{
    /// Where an emitter starts particles, and in which directions. With emitFromEdge a shape starts them on its outline
    /// (the cone's base rim, the sphere's surface, the circle's rim, the box's faces) instead of anywhere inside it.
    enum class EmitterShape
    {
        /// From a disc of shapeRadius on the emitter's local XZ plane, along the emitter's local +Y axis, spread
        /// within coneAngle degrees of it (0 = a straight stream, 90 = a flat fan, 180 = every direction)
        Cone,
        /// From a random point inside a sphere of shapeRadius, moving straight away from its centre
        Sphere,
        /// 2D: from a disc of shapeRadius on the emitter's local XY plane, moving straight away from its centre within
        /// that plane (issue #445)
        Circle,
        /// 2D: from a line of length 2 * shapeRadius along the emitter's local X axis, along its local +Y axis, spread
        /// within coneAngle degrees of it in the XY plane (issue #445)
        Edge,
        /// From a box of boxSize centred on the emitter (its local axes), along the local +Y axis spread within
        /// coneAngle degrees, as the cone (issue #445)
        Box
    };

    N2_SERIALIZE_ENUM(EmitterShape, {
                      { EmitterShape::Cone, "Cone" },
                      { EmitterShape::Sphere, "Sphere" },
                      { EmitterShape::Circle, "Circle" },
                      { EmitterShape::Edge, "Edge" },
                      { EmitterShape::Box, "Box" }
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
        /// Start on the shape's outline instead of anywhere inside it (Edge is a line either way)
        bool emitFromEdge = false;
        /// The Box shape's full size along the emitter's local X, Y and Z axes
        Math::Vector3 boxSize{1.0f, 1.0f, 1.0f};
        /// Each particle's start rotation and angular velocity are picked from these ranges, in degrees and degrees per
        /// second (they only turn its quad)
        float startRotationMin = 0.0f;
        float startRotationMax = 0.0f;
        float angularVelocityMin = 0.0f;
        float angularVelocityMax = 0.0f;
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
        explicit ParticleEmitter(const std::uint32_t seed = 1) : _random(seed), _spinRandom(SpinSeed(seed)) {}

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
         * `rotation`, and `spins` (when given) with each one's start rotation (x) and angular velocity (y) in radians
         * (per second). The vectors keep their capacity between calls. The rotations come from a random stream of their
         * own, so they never change the positions and velocities a seed gives.
         */
        void Generate(const EmitterShapeSettings &settings, const Math::Vector3 &origin,
                      const Math::Quaternion &rotation, std::uint32_t count, std::vector<Math::Vector3> &positions,
                      std::vector<Math::Vector3> &velocities, std::vector<Math::Vector2> *spins = nullptr);

    private:
        [[nodiscard]] static constexpr std::uint32_t SpinSeed(const std::uint32_t seed) { return seed ^ 0x2545F491u; }

        ParticleRandom _random;
        ParticleRandom _spinRandom;
        float _fraction = 0.0f;
    };
}

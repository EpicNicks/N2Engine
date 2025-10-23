#pragma once

#include <math/Vector3.hpp>

namespace N2Engine
{
    namespace Physics
    {
        class Rigidbody
        {
        public:
            bool useGravity{true};
            float mass{1.0f};
            bool isKinematic{false};

            void ApplyForce(N2Engine::Math::Vector3 force, ForceMode forceMode);
        };

        enum class ForceMode
        {
            Force,         // Add a continuous force to the rigidbody, using its mass.
            Acceleration,  // Add a continuous acceleration to the rigidbody, ignoring its mass.
            Impulse,       // Add an instant force impulse to the rigidbody, using its mass.
            VelocityChange // Add an instant velocity change to the rigidbody, ignoring its mass.
        };
    }
}
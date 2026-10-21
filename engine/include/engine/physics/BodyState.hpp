#pragma once

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

namespace N2Engine::Physics
{
    /// The full dynamic state of one body, as IPhysicsBackend::GetBodyState reads it and SetBodyState writes it back
    /// (what client reconciliation and rollback need per body)
    struct BodyState
    {
        Math::Vector3 position = Math::Vector3::Zero;
        Math::Quaternion rotation = Math::Quaternion::Identity;
        Math::Vector3 linearVelocity = Math::Vector3::Zero;
        Math::Vector3 angularVelocity = Math::Vector3::Zero;
        /// The body is asleep (not simulated until something wakes it). A sleeping body has zero velocities.
        bool sleeping = false;
    };
}

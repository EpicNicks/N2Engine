#pragma once

#include <cstdint>

#include <math/Vector3.hpp>

#include "engine/physics/PhysicsHandle.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Physics
{
    /// The kinds of joint a backend can make (IPhysicsBackend::CreateJoint)
    enum class JointType : std::uint8_t
    {
        Fixed,    // the two bodies are welded: no relative movement or rotation
        Hinge,    // rotation about one axis (a door, a wheel), optionally limited to an angle range
        Ball,     // a shared point, free rotation (a shoulder)
        Slider,   // movement along one axis, no rotation, optionally limited to a distance range (a piston)
        Distance  // keeps the two anchors a distance apart: a rod, or a rope when limits are given
    };

    // literals as values because the mapping shouldn't change if one of the JointType constants has a name change
    N2_SERIALIZE_ENUM(JointType, {
                      { JointType::Fixed, "Fixed" },
                      { JointType::Hinge, "Hinge" },
                      { JointType::Ball, "Ball" },
                      { JointType::Slider, "Slider" },
                      { JointType::Distance, "Distance" }
                      })

    /// A joint a backend made: the slot in its joint table and the generation of that slot, like PhysicsBodyHandle
    struct PhysicsJointHandle
    {
        std::uint32_t index = UINT32_MAX;
        std::uint32_t generation = 0;

        [[nodiscard]] bool IsValid() const { return index != UINT32_MAX; }
        bool operator==(const PhysicsJointHandle &other) const = default;
    };

    inline constexpr auto INVALID_PHYSICS_JOINT_HANDLE = PhysicsJointHandle{};

    /// Everything a backend needs to build a joint. The anchors are what Unity calls anchor and connectedAnchor: the
    /// joint holds the point `anchorA` of body A (in A's local space) at the point `anchorB` of body B. It is made
    /// from the bodies' poses at that moment, so what is rotated or apart then is the joint's zero: a hinge's angle
    /// 0, a slider's distance 0, a fixed joint's relative pose.
    struct JointDesc
    {
        JointType type = JointType::Fixed;
        PhysicsBodyHandle bodyA;
        /// Invalid: the joint holds body A to the world, at the world position `anchorB`
        PhysicsBodyHandle bodyB;
        Math::Vector3 anchorA = Math::Vector3::Zero;
        Math::Vector3 anchorB = Math::Vector3::Zero;
        /// Hinge axis or slider direction, in body A's local space (ignored by the other types)
        Math::Vector3 axis = Math::Vector3(1.0f, 0.0f, 0.0f);
        /// Whether the two bodies still collide with each other (off by default, as in Unity)
        bool collideConnected = false;
        /// Hinge: the angle range in radians, within [-pi, pi]. Slider: the distance range along the axis. Distance:
        /// the allowed distance range (without limits it is held at its distance when the joint is made). Ignored by
        /// Fixed and Ball.
        bool useLimits = false;
        float limitMin = 0.0f;
        float limitMax = 0.0f;
        /// A component owns the joint (the Joint component sets it). After a rollback restore re-makes such a joint from
        /// the saved state, it waits for its owner to claim it (IPhysicsBackend::ClaimJoint); one nobody claims by the
        /// next Update is ended. A joint made straight through the backend (false) is never ended that way.
        bool owned = false;
    };
}

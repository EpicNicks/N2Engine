#pragma once

#include <math/Vector3.hpp>
#include <vector>
#include <limits>
#include "engine/physics/PhysicsHandle.hpp"

namespace N2Engine
{
    class GameObject;
    class Rigidbody;
}

namespace N2Engine::Physics
{
    class ICollider;

    struct RaycastHit
    {
        bool hit = false;
        Math::Vector3 point = Math::Vector3::Zero;
        Math::Vector3 normal = Math::Vector3::Zero;
        float distance = 0.0f;

        GameObject* gameObject = nullptr;
        Rigidbody* rigidbody = nullptr;
        ICollider* collider = nullptr;
        PhysicsBodyHandle bodyHandle;

        RaycastHit() = default;
    };

    class Raycast
    {
    public:
        static bool Single(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = 0xFFFFFFFF);

        static int All(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            std::vector<RaycastHit>& hits,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = 0xFFFFFFFF);

        static bool SphereCast(
            const Math::Vector3& origin,
            float radius,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = 0xFFFFFFFF);

        static bool Any(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = 0xFFFFFFFF);
    };
}
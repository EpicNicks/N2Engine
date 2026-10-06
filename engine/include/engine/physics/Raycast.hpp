#pragma once

#include <cstdint>
#include <vector>

#include <math/Vector3.hpp>
#include <limits>
#include "engine/Layers.hpp"
#include "engine/physics/PhysicsHandle.hpp"

namespace N2Engine
{
    class GameObject;
}

namespace N2Engine::Physics
{
    class Rigidbody;
    class ICollider;

    /// Whether a ray query hits trigger colliders (Unity's QueryTriggerInteraction, without UseGlobal)
    enum class QueryTriggers : std::uint8_t
    {
        /// Triggers are hit like solid colliders (the default, and what every query did before)
        Collide,
        /// Trigger colliders are skipped: the ray goes through them
        Ignore
    };

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

    /// layerMask picks the layers a query can hit (Layers::MaskOf, Layers::GetMask); 0 hits nothing. The
    /// default is every layer except Ignore Raycast, as in Unity. `triggers` says whether Single and All hit
    /// trigger colliders (they do by default).
    class Raycast
    {
    public:
        static bool Single(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = Layers::DefaultRaycastMask,
            QueryTriggers triggers = QueryTriggers::Collide);

        static int All(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            std::vector<RaycastHit>& hits,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = Layers::DefaultRaycastMask,
            QueryTriggers triggers = QueryTriggers::Collide);

        static bool SphereCast(
            const Math::Vector3& origin,
            float radius,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = Layers::DefaultRaycastMask);

        static bool Any(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = Layers::DefaultRaycastMask);
    };
}
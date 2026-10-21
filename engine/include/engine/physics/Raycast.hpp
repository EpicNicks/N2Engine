#pragma once

#include <cstdint>
#include <vector>

#include <math/Quaternion.hpp>
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

    /// The rules every backend's queries follow, so a scene gives the same answers on PhysX and on Jolt:
    ///  - A query that cannot hit finds nothing: a zero or non-finite direction, a non-finite origin, a maxDistance
    ///    that is not above zero (or NaN), a radius or half extent that is not above zero.
    ///  - A longer maxDistance (infinity included) is clamped to kMaxQueryDistance; a sweep (SphereCast, CapsuleCast)
    ///    to kMaxSweepDistance, which stays within what PhysX takes for a sweep and keeps a long shape cast precise.
    ///  - A ray or cast that STARTS inside a collider hits it at distance 0, with the normal against the direction
    ///    (-direction) and the point at the start (the ray origin, or the centre of the swept shape).
    ///  - The single-hit queries (Raycast, SphereCast, CapsuleCast) take the nearest hit; hits within
    ///    kQueryTieTolerance of it count as ties, and the tie with the lowest body handle (index, then generation),
    ///    then the collider registered first on that body, wins.
    ///  - RaycastAll is sorted by distance, then body handle, then collider order; Overlap reports each collider once,
    ///    sorted by body handle, then collider order. A collider whose shape only touches the query shape is not
    ///    promised to be reported.
    struct QueryRules
    {
        static constexpr float kMaxQueryDistance = 1.0e9f;
        static constexpr float kMaxSweepDistance = 1.0e7f;
        static constexpr float kQueryTieTolerance = 1.0e-3f;
    };

    /// The shape an overlap query tests; Physics::Overlap builds one. A capsule is the segment point0-point1 (the
    /// centres of its two end spheres) with a radius, as in Unity's Physics.OverlapCapsule.
    struct OverlapShape
    {
        enum class Kind : std::uint8_t
        {
            Sphere,
            Box,
            Capsule
        };

        Kind kind = Kind::Sphere;
        Math::Vector3 center = Math::Vector3::Zero; // sphere and box
        float radius = 0.5f;                        // sphere and capsule
        Math::Vector3 halfExtents{0.5f, 0.5f, 0.5f}; // box
        Math::Quaternion rotation = Math::Quaternion::Identity; // box
        Math::Vector3 point0 = Math::Vector3::Zero; // capsule
        Math::Vector3 point1 = Math::Vector3::Zero; // capsule
    };

    /// One collider an overlap query found
    struct OverlapHit
    {
        GameObject* gameObject = nullptr;
        Rigidbody* rigidbody = nullptr;
        ICollider* collider = nullptr;
        PhysicsBodyHandle bodyHandle;
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

        /// Sweeps a capsule (the segment point0-point1 with `radius`, Unity's Physics.CapsuleCast) along `direction`
        static bool CapsuleCast(
            const Math::Vector3& point0,
            const Math::Vector3& point1,
            float radius,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = Layers::DefaultRaycastMask,
            QueryTriggers triggers = QueryTriggers::Collide);

        static bool Any(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            float maxDistance = std::numeric_limits<float>::infinity(),
            uint32_t layerMask = Layers::DefaultRaycastMask);
    };

    /// Overlap queries: every collider that overlaps (or touches) a shape placed in the scene, each once, sorted by
    /// body handle and collider order (QueryRules). They return the number found and replace `hits`.
    class Overlap
    {
    public:
        static int Sphere(
            const Math::Vector3& center,
            float radius,
            std::vector<OverlapHit>& hits,
            uint32_t layerMask = Layers::DefaultRaycastMask,
            QueryTriggers triggers = QueryTriggers::Collide);

        static int Box(
            const Math::Vector3& center,
            const Math::Vector3& halfExtents,
            const Math::Quaternion& rotation,
            std::vector<OverlapHit>& hits,
            uint32_t layerMask = Layers::DefaultRaycastMask,
            QueryTriggers triggers = QueryTriggers::Collide);

        static int Capsule(
            const Math::Vector3& point0,
            const Math::Vector3& point1,
            float radius,
            std::vector<OverlapHit>& hits,
            uint32_t layerMask = Layers::DefaultRaycastMask,
            QueryTriggers triggers = QueryTriggers::Collide);
    };
}

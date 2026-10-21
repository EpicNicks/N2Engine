#include "engine/physics/Raycast.hpp"
#include "engine/Application.hpp"
#include "engine/physics/IPhysicsBackend.hpp"

namespace N2Engine::Physics
{
    bool Raycast::Single(
        const Math::Vector3& origin,
        const Math::Vector3& direction,
        RaycastHit& hit,
        const float maxDistance,
        const uint32_t layerMask,
        const QueryTriggers triggers)
    {
        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
        {
            hit = RaycastHit{}; // nothing of a previous hit survives a miss
            return false;
        }

        return backend->Raycast(origin, direction, hit, maxDistance, layerMask, triggers);
    }

    int Raycast::All(
        const Math::Vector3& origin,
        const Math::Vector3& direction,
        std::vector<RaycastHit>& hits,
        const float maxDistance,
        const uint32_t layerMask,
        const QueryTriggers triggers)
    {
        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
        {
            hits.clear();
            return 0;
        }

        return backend->RaycastAll(origin, direction, hits, maxDistance, layerMask, triggers);
    }

    bool Raycast::SphereCast(
        const Math::Vector3& origin,
        float radius,
        const Math::Vector3& direction,
        RaycastHit& hit,
        float maxDistance,
        uint32_t layerMask)
    {
        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
        {
            hit = RaycastHit{};
            return false;
        }

        return backend->SphereCast(origin, radius, direction, hit, maxDistance, layerMask);
    }

    bool Raycast::CapsuleCast(
        const Math::Vector3& point0,
        const Math::Vector3& point1,
        const float radius,
        const Math::Vector3& direction,
        RaycastHit& hit,
        const float maxDistance,
        const uint32_t layerMask,
        const QueryTriggers triggers)
    {
        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
        {
            hit = RaycastHit{};
            return false;
        }

        return backend->CapsuleCast(point0, point1, radius, direction, hit, maxDistance, layerMask, triggers);
    }

    bool Raycast::Any(
        const Math::Vector3& origin,
        const Math::Vector3& direction,
        const float maxDistance,
        const uint32_t layerMask)
    {
        RaycastHit hit;
        return Single(origin, direction, hit, maxDistance, layerMask);
    }

    namespace
    {
        int OverlapWith(const OverlapShape& shape, std::vector<OverlapHit>& hits, const uint32_t layerMask,
                        const QueryTriggers triggers)
        {
            auto* backend = Application::GetInstance().Get3DPhysicsBackend();
            if (!backend)
            {
                hits.clear();
                return 0;
            }
            return backend->Overlap(shape, hits, layerMask, triggers);
        }
    }

    int Overlap::Sphere(
        const Math::Vector3& center,
        const float radius,
        std::vector<OverlapHit>& hits,
        const uint32_t layerMask,
        const QueryTriggers triggers)
    {
        OverlapShape shape;
        shape.kind = OverlapShape::Kind::Sphere;
        shape.center = center;
        shape.radius = radius;
        return OverlapWith(shape, hits, layerMask, triggers);
    }

    int Overlap::Box(
        const Math::Vector3& center,
        const Math::Vector3& halfExtents,
        const Math::Quaternion& rotation,
        std::vector<OverlapHit>& hits,
        const uint32_t layerMask,
        const QueryTriggers triggers)
    {
        OverlapShape shape;
        shape.kind = OverlapShape::Kind::Box;
        shape.center = center;
        shape.halfExtents = halfExtents;
        shape.rotation = rotation;
        return OverlapWith(shape, hits, layerMask, triggers);
    }

    int Overlap::Capsule(
        const Math::Vector3& point0,
        const Math::Vector3& point1,
        const float radius,
        std::vector<OverlapHit>& hits,
        const uint32_t layerMask,
        const QueryTriggers triggers)
    {
        OverlapShape shape;
        shape.kind = OverlapShape::Kind::Capsule;
        shape.point0 = point0;
        shape.point1 = point1;
        shape.radius = radius;
        return OverlapWith(shape, hits, layerMask, triggers);
    }

    // The defaults of the query members a backend may lack (declared in IPhysicsBackend.hpp, which only forward
    // declares the hit types): nothing found, and nothing of a previous result survives
    bool IPhysicsBackend::CapsuleCast(const Math::Vector3&, const Math::Vector3&, float, const Math::Vector3&,
                                      RaycastHit& hit, float, uint32_t, QueryTriggers)
    {
        hit = RaycastHit{};
        return false;
    }

    int IPhysicsBackend::Overlap(const OverlapShape&, std::vector<OverlapHit>& hits, uint32_t, QueryTriggers)
    {
        hits.clear();
        return 0;
    }
}

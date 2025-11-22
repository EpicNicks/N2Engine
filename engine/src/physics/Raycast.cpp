#include "engine/physics/Raycast.hpp"
#include "engine/Application.hpp"

namespace N2Engine::Physics
{
    bool Raycast::Single(
        const Math::Vector3& origin,
        const Math::Vector3& direction,
        RaycastHit& hit,
        const float maxDistance,
        const uint32_t layerMask)
    {
        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
        {
            hit.hit = false;
            return false;
        }

        return backend->Raycast(origin, direction, hit, maxDistance, layerMask);
    }

    int Raycast::All(
        const Math::Vector3& origin,
        const Math::Vector3& direction,
        std::vector<RaycastHit>& hits,
        const float maxDistance,
        const uint32_t layerMask)
    {
        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
        {
            hits.clear();
            return 0;
        }

        return backend->RaycastAll(origin, direction, hits, maxDistance, layerMask);
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
            hit.hit = false;
            return false;
        }

        return backend->SphereCast(origin, radius, direction, hit, maxDistance, layerMask);
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
}
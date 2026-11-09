#include "engine/physics2d/Physics2DQueries.hpp"

#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

namespace N2Engine::Physics2D
{
    namespace
    {
        Physics2DWorld *CurrentWorld()
        {
            const Scene *scene = SceneManager::GetCurScene();
            return scene ? scene->GetPhysics2DWorld() : nullptr;
        }
    }

    bool Raycast(const Math::Vector2 &origin, const Math::Vector2 &direction, RaycastHit2D &hit, const float maxDistance,
                 const std::uint32_t layerMask, const Physics::QueryTriggers triggers)
    {
        hit = RaycastHit2D{};
        Physics2DWorld *world = CurrentWorld();
        return world && world->Raycast(origin, direction, hit, maxDistance, layerMask, triggers);
    }

    int RaycastAll(const Math::Vector2 &origin, const Math::Vector2 &direction, std::vector<RaycastHit2D> &hits, const float maxDistance,
                   const std::uint32_t layerMask, const Physics::QueryTriggers triggers)
    {
        hits.clear();
        Physics2DWorld *world = CurrentWorld();
        return world ? world->RaycastAll(origin, direction, hits, maxDistance, layerMask, triggers) : 0;
    }

    bool CircleCast(const Math::Vector2 &origin, const float radius, const Math::Vector2 &direction, RaycastHit2D &hit, const float maxDistance,
                    const std::uint32_t layerMask, const Physics::QueryTriggers triggers)
    {
        hit = RaycastHit2D{};
        Physics2DWorld *world = CurrentWorld();
        return world && world->CircleCast(origin, radius, direction, hit, maxDistance, layerMask, triggers);
    }

    bool BoxCast(const Math::Vector2 &origin, const Math::Vector2 &halfExtents, const float angleRadians, const Math::Vector2 &direction,
                 RaycastHit2D &hit, const float maxDistance, const std::uint32_t layerMask, const Physics::QueryTriggers triggers)
    {
        hit = RaycastHit2D{};
        Physics2DWorld *world = CurrentWorld();
        return world && world->BoxCast(origin, halfExtents, angleRadians, direction, hit, maxDistance, layerMask, triggers);
    }

    int OverlapCircle(const Math::Vector2 &center, const float radius, std::vector<OverlapHit2D> &hits, const std::uint32_t layerMask,
                      const Physics::QueryTriggers triggers)
    {
        hits.clear();
        Physics2DWorld *world = CurrentWorld();
        return world ? world->OverlapCircle(center, radius, hits, layerMask, triggers) : 0;
    }

    int OverlapBox(const Math::Vector2 &center, const Math::Vector2 &halfExtents, const float angleRadians, std::vector<OverlapHit2D> &hits,
                   const std::uint32_t layerMask, const Physics::QueryTriggers triggers)
    {
        hits.clear();
        Physics2DWorld *world = CurrentWorld();
        return world ? world->OverlapBox(center, halfExtents, angleRadians, hits, layerMask, triggers) : 0;
    }
}

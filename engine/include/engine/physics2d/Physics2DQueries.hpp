#pragma once

// Scene queries on the loaded scene's 2D world. They follow Physics::QueryRules (Raycast.hpp), so a 2D query behaves like a 3D
// one: degenerate input finds nothing, a start inside a collider hits at distance 0 with the normal against the direction, and
// ties and lists are ordered by body handle. With no 2D world (no 2D component yet, or a build without Box2D) they find nothing.

#include <cstdint>
#include <limits>
#include <vector>

#include "engine/Layers.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/physics2d/Physics2DTypes.hpp"

namespace N2Engine::Physics2D
{
    inline constexpr float kNoLimit = std::numeric_limits<float>::infinity();

    bool Raycast(const Math::Vector2 &origin, const Math::Vector2 &direction, RaycastHit2D &hit, float maxDistance = kNoLimit,
                 std::uint32_t layerMask = Layers::DefaultRaycastMask,
                 Physics::QueryTriggers triggers = Physics::QueryTriggers::Collide);

    int RaycastAll(const Math::Vector2 &origin, const Math::Vector2 &direction, std::vector<RaycastHit2D> &hits,
                   float maxDistance = kNoLimit, std::uint32_t layerMask = Layers::DefaultRaycastMask,
                   Physics::QueryTriggers triggers = Physics::QueryTriggers::Collide);

    bool CircleCast(const Math::Vector2 &origin, float radius, const Math::Vector2 &direction, RaycastHit2D &hit,
                    float maxDistance = kNoLimit, std::uint32_t layerMask = Layers::DefaultRaycastMask,
                    Physics::QueryTriggers triggers = Physics::QueryTriggers::Collide);

    bool BoxCast(const Math::Vector2 &origin, const Math::Vector2 &halfExtents, float angleRadians,
                 const Math::Vector2 &direction, RaycastHit2D &hit, float maxDistance = kNoLimit,
                 std::uint32_t layerMask = Layers::DefaultRaycastMask,
                 Physics::QueryTriggers triggers = Physics::QueryTriggers::Collide);

    int OverlapCircle(const Math::Vector2 &center, float radius, std::vector<OverlapHit2D> &hits,
                      std::uint32_t layerMask = Layers::DefaultRaycastMask,
                      Physics::QueryTriggers triggers = Physics::QueryTriggers::Collide);

    int OverlapBox(const Math::Vector2 &center, const Math::Vector2 &halfExtents, float angleRadians,
                   std::vector<OverlapHit2D> &hits, std::uint32_t layerMask = Layers::DefaultRaycastMask,
                   Physics::QueryTriggers triggers = Physics::QueryTriggers::Collide);
}

#pragma once

// What both 3D backends share of their scene queries: the input checks and the ordering of QueryRules (Raycast.hpp),
// so PhysX and Jolt find the same hits in the same order for the same scene. Internal to engine/src/physics.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/physics/Raycast.hpp"

namespace N2Engine::Physics::QueryUtil
{
    inline bool IsFinite(const Math::Vector3 &v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    /// The unit direction of a ray or cast and its length (clamped to the limit: QueryRules::kMaxQueryDistance, or
    /// kMaxSweepDistance for a sweep)
    struct Sweep
    {
        Math::Vector3 direction;
        float distance = 0.0f;
    };

    /// Nothing for a ray or cast that cannot hit: a non-finite origin, a zero or non-finite direction, a maxDistance
    /// that is not above zero (NaN included)
    inline std::optional<Sweep> ReadSweep(const Math::Vector3 &origin, const Math::Vector3 &direction,
                                          const float maxDistance,
                                          const float limit = QueryRules::kMaxQueryDistance)
    {
        if (!IsFinite(origin) || !IsFinite(direction) || !(maxDistance > 0.0f))
        {
            return std::nullopt;
        }
        const float lengthSquared = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
        if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared))
        {
            return std::nullopt;
        }
        const float length = std::sqrt(lengthSquared);
        Sweep sweep;
        sweep.direction = Math::Vector3(direction.x / length, direction.y / length, direction.z / length);
        sweep.distance = std::min(maxDistance, limit);
        return sweep;
    }

    inline bool IsValidRadius(const float radius)
    {
        return radius > 0.0f && std::isfinite(radius);
    }

    /// A shape an overlap can test: finite positions, radius and half extents above zero, a rotation of some length
    inline bool IsValidOverlapShape(const OverlapShape &shape)
    {
        switch (shape.kind)
        {
        case OverlapShape::Kind::Sphere:
            return IsFinite(shape.center) && IsValidRadius(shape.radius);
        case OverlapShape::Kind::Box:
        {
            const Math::Quaternion &q = shape.rotation;
            const float lengthSquared =
                q.GetX() * q.GetX() + q.GetY() * q.GetY() + q.GetZ() * q.GetZ() + q.GetW() * q.GetW();
            return IsFinite(shape.center) && IsValidRadius(shape.halfExtents.x) && IsValidRadius(shape.halfExtents.y) &&
                   IsValidRadius(shape.halfExtents.z) && std::isfinite(lengthSquared) && lengthSquared > 1e-12f;
        }
        case OverlapShape::Kind::Capsule:
            return IsFinite(shape.point0) && IsFinite(shape.point1) && IsValidRadius(shape.radius);
        }
        return false;
    }

    /// Where a hit sits in the order of QueryRules: its distance, its body's handle and the position of its collider
    /// among the colliders registered on that body
    struct HitKey
    {
        float distance = 0.0f;
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
        std::size_t colliderOrder = 0;
    };

    inline HitKey KeyOf(const float distance, const PhysicsBodyHandle handle, const std::size_t colliderOrder)
    {
        return HitKey{distance, handle.index, handle.generation, colliderOrder};
    }

    /// Overlap's order (and the tie-break of the single-hit queries): handle, then collider, then distance
    inline bool HandleThenDistanceLess(const HitKey &a, const HitKey &b)
    {
        return std::tie(a.index, a.generation, a.colliderOrder, a.distance) <
               std::tie(b.index, b.generation, b.colliderOrder, b.distance);
    }

    /// The single-hit queries' pick: the hits within kQueryTieTolerance of the nearest are ties, and the one with the
    /// lowest handle (then collider) wins. SIZE_MAX for no keys.
    inline std::size_t PickClosest(const std::vector<HitKey> &keys)
    {
        float nearest = std::numeric_limits<float>::infinity();
        for (const HitKey &key : keys)
        {
            nearest = std::min(nearest, key.distance);
        }
        std::size_t best = SIZE_MAX;
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (keys[i].distance <= nearest + QueryRules::kQueryTieTolerance &&
                (best == SIZE_MAX || HandleThenDistanceLess(keys[i], keys[best])))
            {
                best = i;
            }
        }
        return best;
    }

    /// Sorts `items` (hits of any type) by their keys with `less`, keeping items and keys together
    template <typename T, typename Less>
    void SortByKeys(std::vector<T> &items, std::vector<HitKey> &keys, Less less)
    {
        std::vector<std::size_t> order(items.size());
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            order[i] = i;
        }
        std::ranges::sort(order, [&keys, &less](const std::size_t a, const std::size_t b)
                          { return less(keys[a], keys[b]); });
        std::vector<T> sortedItems;
        std::vector<HitKey> sortedKeys;
        sortedItems.reserve(items.size());
        sortedKeys.reserve(keys.size());
        for (const std::size_t i : order)
        {
            sortedItems.push_back(items[i]);
            sortedKeys.push_back(keys[i]);
        }
        items = std::move(sortedItems);
        keys = std::move(sortedKeys);
    }

    /// RaycastAll's order: the single-hit pick (PickClosest) taken again and again from what is left, so the first hit
    /// is the one Raycast returns and hits within kQueryTieTolerance of the nearest come in handle (then collider)
    /// order, not in the order of float noise. (A plain sort by distance with a tolerance is not a strict ordering.)
    template <typename T>
    void SortByPick(std::vector<T> &items, std::vector<HitKey> &keys)
    {
        std::vector<T> sortedItems;
        std::vector<HitKey> sortedKeys;
        sortedItems.reserve(items.size());
        sortedKeys.reserve(keys.size());
        std::vector<HitKey> left = keys;
        std::vector<std::size_t> leftIndex(items.size());
        for (std::size_t i = 0; i < leftIndex.size(); ++i)
        {
            leftIndex[i] = i;
        }
        while (!left.empty())
        {
            const std::size_t pick = PickClosest(left);
            if (pick == SIZE_MAX)
            {
                break; // only NaN distances get here
            }
            sortedItems.push_back(items[leftIndex[pick]]);
            sortedKeys.push_back(left[pick]);
            left.erase(left.begin() + static_cast<std::ptrdiff_t>(pick));
            leftIndex.erase(leftIndex.begin() + static_cast<std::ptrdiff_t>(pick));
        }
        // Whatever PickClosest could not place (only NaN distances) keeps its order, at the end
        for (const std::size_t i : leftIndex)
        {
            sortedItems.push_back(items[i]);
            sortedKeys.push_back(keys[i]);
        }
        items = std::move(sortedItems);
        keys = std::move(sortedKeys);
    }

    /// The position of `collider` in `colliders` (the registration order), or the list's size for one not there
    template <typename Collider>
    std::size_t ColliderOrder(const std::vector<Collider *> &colliders, const Collider *collider)
    {
        const auto it = std::ranges::find(colliders, collider);
        return static_cast<std::size_t>(it - colliders.begin());
    }

    /// The rotation that turns unit vector `from` onto unit vector `to` (the shortest one), as (x, y, z, w). A
    /// half turn about an axis perpendicular to `from` when they are opposite.
    struct ShortestRotation
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    };

    inline ShortestRotation RotationBetween(const Math::Vector3 &from, const Math::Vector3 &to)
    {
        const float dot = from.x * to.x + from.y * to.y + from.z * to.z;
        ShortestRotation r;
        if (dot < -0.999999f)
        {
            // Opposite: any axis perpendicular to `from`
            Math::Vector3 axis(0.0f, -from.z, from.y); // from x (1, 0, 0)
            if (axis.x * axis.x + axis.y * axis.y + axis.z * axis.z < 1e-6f)
            {
                axis = Math::Vector3(-from.z, 0.0f, from.x); // from x (0, 1, 0)
            }
            const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            r.x = axis.x / length;
            r.y = axis.y / length;
            r.z = axis.z / length;
            r.w = 0.0f;
            return r;
        }
        // q = (from x to, 1 + from . to), normalised
        const float cx = from.y * to.z - from.z * to.y;
        const float cy = from.z * to.x - from.x * to.z;
        const float cz = from.x * to.y - from.y * to.x;
        const float cw = 1.0f + dot;
        const float length = std::sqrt(cx * cx + cy * cy + cz * cz + cw * cw);
        r.x = cx / length;
        r.y = cy / length;
        r.z = cz / length;
        r.w = cw / length;
        return r;
    }

    /// A capsule given by the centres of its end spheres, as a centre, a unit axis and the half length of the segment
    /// (0 and axis +Y for coincident points: the capsule is a sphere)
    struct CapsuleFrame
    {
        Math::Vector3 center;
        Math::Vector3 axis{0.0f, 1.0f, 0.0f};
        float halfLength = 0.0f;
    };

    inline CapsuleFrame CapsuleFrameOf(const Math::Vector3 &point0, const Math::Vector3 &point1)
    {
        CapsuleFrame frame;
        frame.center = Math::Vector3((point0.x + point1.x) * 0.5f, (point0.y + point1.y) * 0.5f,
                                     (point0.z + point1.z) * 0.5f);
        const float dx = point1.x - point0.x;
        const float dy = point1.y - point0.y;
        const float dz = point1.z - point0.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length > 1e-6f && std::isfinite(length))
        {
            frame.axis = Math::Vector3(dx / length, dy / length, dz / length);
            frame.halfLength = 0.5f * length;
        }
        return frame;
    }

    /// QueryRules' answer for a ray or cast that starts inside what it hits: distance 0, the normal against the
    /// direction, the point at the start
    inline void MarkStartedInside(RaycastHit &hit, const Math::Vector3 &start, const Math::Vector3 &unitDirection)
    {
        hit.distance = 0.0f;
        hit.point = start;
        hit.normal = Math::Vector3(-unitDirection.x, -unitDirection.y, -unitDirection.z);
    }
}

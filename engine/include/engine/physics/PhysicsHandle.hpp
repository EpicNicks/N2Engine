#pragma once

#include <cstdint>
#include <functional>

namespace N2Engine
{
    namespace Physics
    {
        struct PhysicsBodyHandle
        {
            uint32_t index = UINT32_MAX;
            uint32_t generation = 0;

            bool IsValid() const { return index != UINT32_MAX; }

            bool operator==(const PhysicsBodyHandle &other) const
            {
                return index == other.index && generation == other.generation;
            }
        };
    }
}

// Hash function for use in unordered_map
namespace std
{
    template <>
    struct hash<N2Engine::Physics::PhysicsBodyHandle>
    {
        size_t operator()(const N2Engine::Physics::PhysicsBodyHandle &h) const
        {
            return hash<uint32_t>()(h.index) ^ (hash<uint32_t>()(h.generation) << 1);
        }
    };
}
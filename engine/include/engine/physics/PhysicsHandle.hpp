#pragma once

#include <cstdint>
#include <functional>

namespace N2Engine::Physics
{
    struct PhysicsBodyHandle
    {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;

        [[nodiscard]] bool IsValid() const { return index != UINT32_MAX; }

        bool operator==(const PhysicsBodyHandle &other) const
        {
            return index == other.index && generation == other.generation;
        }
        bool operator!=(const PhysicsBodyHandle &other) const
        {
            return !(*this == other);
        }
    };

    inline constexpr auto INVALID_PHYSICS_HANDLE = PhysicsBodyHandle{};
}


// Hash function for use in unordered_map
template <>
struct std::hash<N2Engine::Physics::PhysicsBodyHandle>
{
    size_t operator()(const N2Engine::Physics::PhysicsBodyHandle &h) const noexcept
    {
        return hash<uint32_t>()(h.index) ^ (hash<uint32_t>()(h.generation) << 1);
    }
};

#pragma once
#include <cstdint>
#include <optional>
#include <random>

#include "math/DeterministicRandom.hpp"

namespace N2Engine::Math
{
    struct Vector2;

    // The engine's global random functions. Until Seed is called they draw from one std::mt19937 seeded from
    // std::random_device (not reproducible, and std::uniform_*_distribution is implementation-defined). After
    // Seed(n) every function below draws from one global DeterministicRandom stream instead: the same seed gives the
    // same sequence on every platform. Main thread only, like the generator it replaces.
    namespace Random
    {
        inline std::mt19937& GetRandomEngine()
        {
            static std::random_device rd;
            static std::mt19937 gen(rd());
            return gen;
        }

        namespace Detail
        {
            // Empty until Seed is called; the one place the seeded state lives
            inline std::optional<DeterministicRandom>& SeededStream()
            {
                static std::optional<DeterministicRandom> stream;
                return stream;
            }
        }

        /// Switches Random.* to the deterministic generator and restarts its sequence from `seed`. Calling it again
        /// restarts the sequence. Without a call nothing changes.
        inline void Seed(const std::uint32_t seed) { Detail::SeededStream().emplace(seed); }

        /// Back to the std::random_device generator (not reproducible), as before any Seed call. For tests and tools:
        /// a game that must be reproducible has no reason to call it.
        inline void Unseed() { Detail::SeededStream().reset(); }

        [[nodiscard]] inline bool IsSeeded() { return Detail::SeededStream().has_value(); }

        // Random float [0, 1] (seeded: [0, 1))
        inline float RandomFloat()
        {
            if (auto& seeded = Detail::SeededStream())
            {
                return seeded->NextFloat();
            }
            static std::uniform_real_distribution<float> dis(0.0f, 1.0f);
            return dis(GetRandomEngine());
        }

        // Random float in range
        inline float RandomRange(const float min, const float max)
        {
            return min + RandomFloat() * (max - min);
        }

        // Random int in range [min, max] inclusive
        inline int RandomInt(const int min, const int max)
        {
            if (auto& seeded = Detail::SeededStream())
            {
                return seeded->NextInt(min, max);
            }
            std::uniform_int_distribution<int> dis(min, max);
            return dis(GetRandomEngine());
        }

        Vector2 RandomInUnitCircle();
        Vector2 RandomOnUnitCircle();
    }
}

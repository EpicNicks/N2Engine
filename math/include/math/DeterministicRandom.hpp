#pragma once

#include <cmath>
#include <cstdint>

#include "math/Vector2.hpp"

namespace N2Engine::Math
{
    /**
     * A seedable random number generator whose sequence is a function of the seed alone: the same on every platform,
     * compiler and standard library. Its integer paths (NextU32, NextFloat, Range, NextInt) use integer arithmetic
     * only and no libm, and make a float from an integer by one exact multiplication; std::uniform_*_distribution
     * promises none of that. The algorithm is the one of Particles::ParticleRandom (SplitMix32-style mixing of a
     * counter), so the same seed gives the same u32 sequence there (a test keeps the two equal).
     *
     * One object is one stream: give each system that must be reproducible on its own its own object and seed.
     * Not thread safe.
     *
     * The unit-circle samples use rejection sampling on an integer grid, so they need no sin/cos; OnUnitCircle
     * ends in one std::sqrt and one division, both correctly rounded by IEEE 754 (so they agree wherever doubles
     * do), not in a libm approximation.
     */
    class DeterministicRandom
    {
    public:
        explicit DeterministicRandom(const std::uint32_t seed = 1) : _state(seed) {}

        void Seed(const std::uint32_t seed) { _state = seed; }

        /// The whole stream state: a counter. Seed(State()) puts a stream back where it was (what a rollback snapshot
        /// keeps of the session RNG).
        [[nodiscard]] std::uint32_t State() const noexcept { return _state; }

        [[nodiscard]] std::uint32_t NextU32()
        {
            _state += 0x9E3779B9u;
            std::uint32_t z = _state;
            z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
            z = (z ^ (z >> 13)) * 0xC2B2AE35u;
            return z ^ (z >> 16);
        }

        /// In [0, 1): 24 random bits, so every value is exactly representable
        [[nodiscard]] float NextFloat() { return static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f); }

        /// In [lo, hi) (lo when hi <= lo)
        [[nodiscard]] float Range(const float lo, const float hi)
        {
            const float t = NextFloat();
            return hi > lo ? lo + (hi - lo) * t : lo;
        }

        /// Uniform in [min, max], both ends included, without modulo bias (min when max <= min)
        [[nodiscard]] int NextInt(const int min, const int max)
        {
            if (max <= min)
            {
                return min;
            }
            const std::uint64_t span = static_cast<std::uint64_t>(static_cast<std::int64_t>(max) - min) + 1;
            constexpr std::uint64_t range32 = std::uint64_t{1} << 32;
            std::uint32_t value = NextU32();
            if (span < range32)
            {
                // Rejection: the largest multiple of span that fits in 32 bits is the accepted region
                const std::uint64_t limit = range32 - range32 % span;
                while (value >= limit)
                {
                    value = NextU32();
                }
            }
            // span == 2^32 (the whole int range) takes every value as it is
            return static_cast<int>(static_cast<std::int64_t>(min) + static_cast<std::int64_t>(value % span));
        }

        /// Uniform over the unit disc (length <= 1)
        [[nodiscard]] Vector2 InUnitCircle()
        {
            std::int64_t x = 0;
            std::int64_t y = 0;
            std::int64_t lengthSquared = 0;
            DrawDiscPoint(x, y, lengthSquared);
            return Vector2(static_cast<float>(x) * (1.0f / 65536.0f), static_cast<float>(y) * (1.0f / 65536.0f));
        }

        /// Uniform on the unit circle (length 1 to float precision)
        [[nodiscard]] Vector2 OnUnitCircle()
        {
            std::int64_t x = 0;
            std::int64_t y = 0;
            std::int64_t lengthSquared = 0;
            DrawDiscPoint(x, y, lengthSquared);
            // sqrt and division of doubles are each one correctly rounded operation, and every integer here is exact in
            // a double, so the result is the same wherever IEEE 754 doubles are
            const double length = std::sqrt(static_cast<double>(lengthSquared));
            return Vector2(static_cast<float>(static_cast<double>(x) / length),
                           static_cast<float>(static_cast<double>(y) / length));
        }

    private:
        /// A point of the grid of odd integers in [-65535, 65535]^2 that lies inside the disc of radius 65536 (x and y
        /// are never 0, so the point is never the centre). One NextU32 per try: its low and high 16 bits.
        void DrawDiscPoint(std::int64_t &x, std::int64_t &y, std::int64_t &lengthSquared)
        {
            constexpr std::int64_t radiusSquared = std::int64_t{65536} * 65536;
            do
            {
                const std::uint32_t bits = NextU32();
                x = static_cast<std::int64_t>(bits & 0xFFFFu) * 2 + 1 - 65536;
                y = static_cast<std::int64_t>(bits >> 16) * 2 + 1 - 65536;
                lengthSquared = x * x + y * y;
            }
            while (lengthSquared > radiusSquared);
        }

        std::uint32_t _state;
    };
}

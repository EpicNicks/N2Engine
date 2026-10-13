#pragma once

#include <cstdint>

namespace N2Engine::Particles
{
    /**
     * A small random number generator for particles: the same seed gives the same sequence on every platform and
     * compiler (integer arithmetic only, floats made from the integer by one exact multiplication), which
     * std::uniform_real_distribution does not promise. SplitMix32-style mixing of a counter.
     */
    class ParticleRandom
    {
    public:
        explicit ParticleRandom(const std::uint32_t seed = 1) : _state(seed) {}

        void Seed(const std::uint32_t seed) { _state = seed; }

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

        /// In [lo, hi] (lo when hi <= lo)
        [[nodiscard]] float Range(const float lo, const float hi)
        {
            const float t = NextFloat();
            return hi > lo ? lo + (hi - lo) * t : lo;
        }

    private:
        std::uint32_t _state;
    };
}

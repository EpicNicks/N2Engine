#pragma once

#include <cstdint>

namespace N2Engine::N2Net
{
    /// The simulator's pseudo-random numbers: xoshiro256** seeded through SplitMix64, written out here so the stream
    /// is the same on every compiler, standard library and architecture (std::random_device and the std
    /// distributions are not). Integer arithmetic only.
    class SimRandom
    {
    public:
        explicit SimRandom(std::uint64_t seed) noexcept
        {
            std::uint64_t mix = seed;
            for (std::uint64_t &word : state_)
            {
                word = SplitMix64(mix);
            }
        }

        /// An independent stream for (seed, a, b): one per link direction, so adding a link does not change what
        /// another link's packets do
        [[nodiscard]] static SimRandom ForStream(std::uint64_t seed, std::uint32_t a, std::uint32_t b) noexcept
        {
            std::uint64_t mix = seed ^ 0x9E3779B97F4A7C15ull;
            mix += (static_cast<std::uint64_t>(a) << 32) | b;
            return SimRandom(SplitMix64(mix));
        }

        [[nodiscard]] std::uint64_t Next() noexcept
        {
            const std::uint64_t result = Rotl(state_[1] * 5u, 7) * 9u;
            const std::uint64_t t = state_[1] << 17;
            state_[2] ^= state_[0];
            state_[3] ^= state_[1];
            state_[1] ^= state_[2];
            state_[0] ^= state_[3];
            state_[2] ^= t;
            state_[3] = Rotl(state_[3], 45);
            return result;
        }

        /// A value in [0, bound) (0 when bound is 0): the high 32 bits scaled by a multiply-shift, so it is the same
        /// everywhere. The tiny bias of that method is irrelevant to a network simulator.
        [[nodiscard]] std::uint32_t NextBelow(std::uint32_t bound) noexcept
        {
            const std::uint64_t high = Next() >> 32;
            return static_cast<std::uint32_t>((high * bound) >> 32);
        }

    private:
        static std::uint64_t Rotl(std::uint64_t value, int shift) noexcept
        {
            return (value << shift) | (value >> (64 - shift));
        }

        static std::uint64_t SplitMix64(std::uint64_t &state) noexcept
        {
            state += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        std::uint64_t state_[4]{};
    };
} // namespace N2Engine::N2Net

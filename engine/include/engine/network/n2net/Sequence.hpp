#pragma once

#include <cstdint>

namespace N2Engine::N2Net
{
    /// Wraparound-safe ordering of 16-bit sequence numbers: `a` is more recent than `b` when it is 1 to 32767 steps
    /// ahead of it modulo 65536. A difference of exactly 32768 is ambiguous and counts as neither more recent.
    [[nodiscard]] constexpr bool SequenceMoreRecent(std::uint16_t a, std::uint16_t b) noexcept
    {
        const std::uint16_t difference = static_cast<std::uint16_t>(a - b);
        return difference != 0 && difference < 32768;
    }

    /// Steps from `from` forward to `to` modulo 65536 (0 when equal)
    [[nodiscard]] constexpr std::uint16_t SequenceDistance(std::uint16_t from, std::uint16_t to) noexcept
    {
        return static_cast<std::uint16_t>(to - from);
    }
} // namespace N2Engine::N2Net

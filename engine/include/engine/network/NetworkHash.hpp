#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace N2Engine::Network
{
    /// 64-bit FNV-1a, fed field by field. It is not a security feature (a fingerprint only has to tell two builds or two
    /// scenes apart; the connection is authenticated by the secure layer), so it is small, has no dependency and builds
    /// on every platform including the web. Integers are fed little-endian and a string is fed with its length first,
    /// so two different field sequences never feed the same bytes.
    class Fnv1a64
    {
    public:
        void U8(const std::uint8_t value) noexcept
        {
            _state ^= value;
            _state *= kPrime;
        }

        void U16(const std::uint16_t value) noexcept
        {
            U8(static_cast<std::uint8_t>(value & 0xFFu));
            U8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
        }

        void U32(const std::uint32_t value) noexcept
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                U8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        }

        void U64(const std::uint64_t value) noexcept
        {
            for (int shift = 0; shift < 64; shift += 8)
            {
                U8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        }

        void Bytes(const std::span<const std::uint8_t> bytes) noexcept
        {
            for (const std::uint8_t byte : bytes)
            {
                U8(byte);
            }
        }

        void String(const std::string_view text) noexcept
        {
            U32(static_cast<std::uint32_t>(text.size()));
            for (const char character : text)
            {
                U8(static_cast<std::uint8_t>(character));
            }
        }

        [[nodiscard]] std::uint64_t Value() const noexcept { return _state; }

    private:
        static constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
        static constexpr std::uint64_t kPrime = 1099511628211ull;

        std::uint64_t _state = kOffsetBasis;
    };
}

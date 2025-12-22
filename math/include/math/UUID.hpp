#pragma once

#include <array>
#include <string>
#include <cstdint>
#include <optional>

namespace N2Engine::Math
{
    class UUID
    {
    public:
        UUID() = default;

        static std::optional<UUID> FromString(const std::string &string);
        static UUID Random();
        [[nodiscard]] std::string ToString() const;

        bool operator==(const UUID &other) const noexcept
        {
            return bytes == other.bytes;
        }

        bool operator!=(const UUID &other) const noexcept
        {
            return !(*this == other);
        }

        // Expose bytes for hashing (const access only)
        [[nodiscard]] const std::array<uint8_t, 16> &GetBytes() const noexcept { return bytes; }

        static const UUID ZERO;

    private:
        std::array<uint8_t, 16> bytes{};
        explicit UUID(std::array<uint8_t, 16> bytes) : bytes(bytes) {}
    };
}

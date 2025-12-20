#pragma once

#include <array>
#include <string>
#include <cstdint>

namespace N2Engine::Math
{
    class UUID
    {
    public:
        UUID();
        explicit UUID(const std::string &str);
        [[nodiscard]] std::string ToString() const;

        bool operator==(const UUID &other) const
        {
            return bytes == other.bytes;
        }

        bool operator!=(const UUID &other) const
        {
            return !(*this == other);
        }

        // Expose bytes for hashing (const access only)
        [[nodiscard]] const std::array<uint8_t, 16> &GetBytes() const { return bytes; }

    private:
        std::array<uint8_t, 16> bytes{};
    };
}

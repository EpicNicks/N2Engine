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

        static UUID Random();

        static std::optional<UUID> FromString(const std::string &string);
        [[nodiscard]] std::string ToString() const;

        static UUID GenerateNameBased(const UUID& namespaceUUID, const std::string& name);

        bool operator==(const UUID& other) const = default;

        // Expose bytes for hashing (const access only)
        [[nodiscard]] const std::array<uint8_t, 16> &GetBytes() const noexcept { return _data; }

        static const UUID ZERO;

    private:
        std::array<uint8_t, 16> _data{};
        explicit UUID(std::array<uint8_t, 16> bytes) : _data(bytes) {}
    };
}

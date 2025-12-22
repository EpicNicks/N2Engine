#include <random>
#include <sstream>
#include <iomanip>

#include "math/UUID.hpp"

using namespace N2Engine::Math;

const UUID UUID::ZERO{};

std::optional<UUID> UUID::FromString(const std::string &str)
{
    if (str.length() != 36)
    {
        return std::nullopt;
    }

    if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-')
    {
        return std::nullopt;
    }

    UUID uuid = ZERO;

    constexpr auto isValidHex = [](const char c) -> bool {
        return (c >= '0' && c <= '9') ||
               (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    };

    constexpr auto hex = [](const char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };

    size_t byteIndex = 0;
    bool highNibble = true;
    uint8_t current = 0;

    for (const char c : str)
    {
        if (c == '-')
        {
            continue;
        }

        if (!isValidHex(c))
        {
            return std::nullopt;
        }

        const uint8_t value = hex(c);

        if (highNibble)
        {
            current = value << 4;
            highNibble = false;
        }
        else
        {
            uuid.bytes[byteIndex++] = current | value;
            highNibble = true;

            if (byteIndex >= uuid.bytes.size())
            {
                break;
            }
        }
    }

    if (byteIndex != 16)
    {
        return std::nullopt;
    }

    return uuid;
}

UUID UUID::Random()
{
    UUID uuid = ZERO;

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    const uint64_t high = dis(gen);
    const uint64_t low = dis(gen);

    // Fill the bytes
    for (int i = 0; i < 8; ++i)
    {
        uuid.bytes[i] = static_cast<uint8_t>((high >> (i * 8)) & 0xFF);
        uuid.bytes[i + 8] = static_cast<uint8_t>((low >> (i * 8)) & 0xFF);
    }

    // Set version to 4 (UUID v4)
    uuid.bytes[6] = (uuid.bytes[6] & 0x0F) | 0x40;
    // Set variant to 10x
    uuid.bytes[8] = (uuid.bytes[8] & 0x3F) | 0x80;

    return uuid;
}

std::string UUID::ToString() const
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i)
    {
        oss << std::setw(2) << static_cast<int>(bytes[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9)
            oss << "-";
    }
    return oss.str();
}
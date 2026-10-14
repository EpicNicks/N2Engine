#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <thread>
#include <sstream>
#include <iomanip>
#include <TinySHA1/TinySHA1.hpp>

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
            uuid._data[byteIndex++] = current | value;
            highNibble = true;

            if (byteIndex >= uuid._data.size())
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

namespace
{
    // One generator per thread, seeded once on the thread's first call. The seed mixes several random_device words with
    // a high-resolution time stamp, the thread id and an address (ASLR), so a random_device that returns a constant
    // (a broken RDSEED) still gives distinct streams in different threads and runs. Not cryptographically secure.
    std::mt19937_64 MakeThreadGenerator()
    {
        std::random_device rd;
        std::array<std::uint32_t, 12> words{};
        for (std::size_t i = 0; i < 8; ++i)
        {
            words[i] = rd();
        }

        const auto ticks = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        const auto threadHash = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        static int anchor = 0;
        const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&anchor)) ^
                             static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&words));

        words[8] = static_cast<std::uint32_t>(ticks);
        words[9] = static_cast<std::uint32_t>(ticks >> 32) ^ static_cast<std::uint32_t>(address >> 32);
        words[10] = static_cast<std::uint32_t>(threadHash) ^ static_cast<std::uint32_t>(address);
        words[11] = static_cast<std::uint32_t>(threadHash >> 32);

        std::seed_seq seq(words.begin(), words.end());
        return std::mt19937_64(seq);
    }
}

UUID UUID::Random()
{
    UUID uuid = ZERO;

    thread_local std::mt19937_64 gen = MakeThreadGenerator();
    std::uniform_int_distribution<uint64_t> dis;

    const uint64_t high = dis(gen);
    const uint64_t low = dis(gen);

    // Fill the bytes
    for (int i = 0; i < 8; ++i)
    {
        uuid._data[i] = static_cast<uint8_t>((high >> (i * 8)) & 0xFF);
        uuid._data[i + 8] = static_cast<uint8_t>((low >> (i * 8)) & 0xFF);
    }

    // Set version to 4 (UUID v4)
    uuid._data[6] = (uuid._data[6] & 0x0F) | 0x40;
    // Set variant to 10x
    uuid._data[8] = (uuid._data[8] & 0x3F) | 0x80;

    return uuid;
}

std::string UUID::ToString() const
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i)
    {
        oss << std::setw(2) << static_cast<int>(_data[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9)
            oss << "-";
    }
    return oss.str();
}

// UUID v5 implementation using TinySHA1
UUID UUID::GenerateNameBased(const UUID& namespaceUUID, const std::string& name)
{
    std::vector<uint8_t> data;

    const auto& nsBytes = namespaceUUID.GetBytes();
    data.insert(data.end(), nsBytes.begin(), nsBytes.end());
    data.insert(data.end(), name.begin(), name.end());

    sha1::SHA1 sha;
    sha.processBytes(data.data(), data.size());

    uint32_t digest[5];          // SHA-1 = 160 bits = 5 × 32-bit words
    sha.getDigest(digest);

    // Convert digest words to bytes (big-endian)
    uint8_t hash[20];
    for (int i = 0; i < 5; ++i)
    {
        hash[i * 4 + 0] = static_cast<uint8_t>((digest[i] >> 24) & 0xFF);
        hash[i * 4 + 1] = static_cast<uint8_t>((digest[i] >> 16) & 0xFF);
        hash[i * 4 + 2] = static_cast<uint8_t>((digest[i] >> 8) & 0xFF);
        hash[i * 4 + 3] = static_cast<uint8_t>((digest[i]) & 0xFF);
    }

    // Create UUID from first 16 bytes of hash
    UUID result;
    std::memcpy(result._data.data(), hash, 16);

    // Set version (5) and variant bits (RFC 4122)
    result._data[6] = (result._data[6] & 0x0F) | 0x50; // Version 5
    result._data[8] = (result._data[8] & 0x3F) | 0x80; // Variant

    return result;
}

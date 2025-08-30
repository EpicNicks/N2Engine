#include <random>
#include <sstream>
#include <iomanip>

#include "engine/math/UUID.hpp"

using namespace N2Engine::Math;

UUID::UUID()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t high = dis(gen);
    uint64_t low = dis(gen);

    // Fill the bytes
    for (int i = 0; i < 8; ++i)
    {
        bytes[i] = static_cast<uint8_t>((high >> (i * 8)) & 0xFF);
        bytes[i + 8] = static_cast<uint8_t>((low >> (i * 8)) & 0xFF);
    }

    // Set version to 4 -----
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    // Set variant to 10x -----
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
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
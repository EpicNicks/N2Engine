#pragma once

#include <cstddef>
#include <cstring>

#include "math/UUID.hpp"

namespace N2Engine
{
    struct UUIDHash
    {
        size_t operator()(const Math::UUID &uuid) const noexcept
        {
            const auto &bytes = uuid.GetBytes();
            size_t hash = 0;
            for (size_t i = 0; i < bytes.size(); i += sizeof(size_t))
            {
                size_t chunk = 0;
                std::memcpy(&chunk, &bytes[i], std::min(sizeof(size_t), bytes.size() - i));
                hash ^= std::hash<size_t>{}(chunk) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };
}
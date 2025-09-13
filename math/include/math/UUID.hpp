#pragma once

#include <array>
#include <string>
#include <cstdint>

namespace N2Engine
{
    namespace Math
    {
        class UUID
        {
        public:
            UUID();
            std::string ToString() const;

        private:
            std::array<uint8_t, 16> bytes;
        };

    }
}
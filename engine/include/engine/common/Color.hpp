#pragma once

#include <math/Vector4.hpp>
#include <cstdint>

namespace N2Engine
{
    namespace Common
    {
        class Color : public Math::Vector4
        {
            using Vector4::Vector4;

        public:
            float &r = w;
            float &g = x;
            float &b = y;
            float &a = z;

            Color(const Color &other) : Vector4(other) {}
            Color(const Vector4& other) : Vector4(other) {}
            Color &operator=(const Color &other);

            static const Color White;
            static const Color Black;
            static const Color Red;
            static const Color Green;
            static const Color Blue;
            static const Color Cyan;
            static const Color Yellow;
            static const Color Magenta;
            static const Color Transparent;

            static Color FromHex(std::uint32_t hexValue);
            std::uint32_t ToHex() const;
        };
    }
}
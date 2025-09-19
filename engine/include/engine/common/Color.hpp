#pragma once

#include <math/VectorN.hpp>
#include <cstdint>

namespace N2Engine
{
    namespace Common
    {
        class Color : public N2Engine::Math::VectorN<float, 4>
        {
            using N2Engine::Math::VectorN<float, 4>::VectorN;

        public:
            float &r = vector[0];
            float &g = vector[1];
            float &b = vector[2];
            float &a = vector[3];

            static Color White();
            static Color Black();
            static Color Red();
            static Color Green();
            static Color Blue();
            static Color Cyan();
            static Color Yellow();
            static Color Magenta();
            static Color Transparent();
            static Color FromHex(std::uint32_t hexValue);
            std::uint32_t ToHex() const;

            Color(const Color &other) : VectorN(other) {}

            Color &operator=(const Color &other);
        };
    }
}
#pragma once

#include "VectorN.hpp"

namespace N2Engine
{
    namespace Math
    {
        struct Vector2 : public VectorN<float, 2>
        {
        public:
            float &x = vector[0];
            float &y = vector[1];

            Vector2(const VectorN<float, 2> &base) : VectorN<float, 2>(base) {}
        };
    }
}
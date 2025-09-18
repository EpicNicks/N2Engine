#pragma once

#include "math/VectorN.hpp"

namespace N2Engine
{
    namespace Math
    {
        struct Vector4 : public VectorN<float, 4>
        {
            using VectorN<float, 4>::VectorN;

        public:
            float &w = vector[0];
            float &x = vector[1];
            float &y = vector[2];
            float &z = vector[3];

            Vector4 &operator=(const Vector4 &other);
            Vector4(const Vector4 &other);
            Vector4(const VectorN<float, 4> &base);
        };

        using Vector4f = Vector4;
    }
}
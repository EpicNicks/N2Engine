#pragma once

#include "VectorN.hpp"

namespace N2Engine
{
    namespace Math
    {

        struct Vector3 : public VectorN<float, 3>
        {
            using VectorN<float, 3>::VectorN;

        public:
            float &x = vector[0];
            float &y = vector[1];
            float &z = vector[2];
        };
    }
}
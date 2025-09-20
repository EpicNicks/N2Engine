#pragma once

#include <cstddef>

namespace N2Engine
{
    class Vector3;
    class Quaternion;
    template <typename T, std::size_t M, std::size_t N>
    class Matrix;

    namespace Math
    {
        /**
         * Responsible for Initializing SIMD across all types which support SIMD operations
         */
        void InitializeSIMD();
    }
}
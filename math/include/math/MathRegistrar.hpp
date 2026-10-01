#pragma once

#include <cstddef>

#include "math/CpuInfo.hpp"

namespace N2Engine
{
    class Vector3;
    class Quaternion;
    template <typename T, std::size_t M, std::size_t N>
    class Matrix;

    namespace Math
    {
        /**
         * Responsible for Initializing SIMD across all types which support SIMD operations: selects the highest
         * tier the CPU supports
         */
        void InitializeSIMD();

        /**
         * Switches every SIMD-dispatching type to the given tier, lowered to what the CPU supports. Until the first
         * call (or InitializeSIMD) they use the scalar implementations. Tests use this to run the same math under
         * each tier, restoring Scalar after.
         *
         * The math target builds with PUBLIC /arch:AVX (-mavx elsewhere), so the whole binary already requires an
         * AVX CPU: the lower tiers choose between algorithms, they are not a fallback for older hardware
         */
        void SetSIMDLevel(SIMDLevel level);
    }
}
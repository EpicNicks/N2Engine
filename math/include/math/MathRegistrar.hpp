#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

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
         * Which tier the project asks for (the project setting math.simdTier). Auto is the default and means the
         * highest tier the CPU supports, exactly what InitializeSIMD() does; the others name one tier.
         */
        enum class SIMDTierRequest
        {
            Auto,
            Scalar,
            SSE2,
            SSE41,
            AVX,
            NEON
        };

        /**
         * "auto", "scalar", "sse2", "sse4.1" (or "sse41"), "avx", "neon", case-insensitive; nullopt for anything else
         */
        [[nodiscard]] std::optional<SIMDTierRequest> ParseSIMDTierRequest(std::string_view text);

        /// The tier a request names; nullopt for Auto
        [[nodiscard]] std::optional<SIMDLevel> ToSIMDLevel(SIMDTierRequest request);

        /// "Scalar", "SSE2", "SSE4.1", "AVX" or "NEON"
        [[nodiscard]] const char *SIMDLevelName(SIMDLevel level);

        /**
         * Selects the tier a request names and returns the tier now in use. Auto selects DetectSIMDLevel(). A tier
         * this target cannot run (above the CPU's, or of the other architecture family) is NOT refused: it is
         * lowered by ClampSIMDLevel, so the caller compares the result with ToSIMDLevel(request) to warn
         */
        SIMDLevel InitializeSIMD(SIMDTierRequest request);

        /**
         * Whether this library was built with the strict floating-point profile (CMake option N2ENGINE_STRICT_FP,
         * which defines N2_STRICT_FP for every target): no FMA contraction, MSVC /fp:precise, no -ffast-math or
         * -march=native. Answers for the compiled math library, so it is the same in every target of one build.
         */
        [[nodiscard]] bool IsStrictFp();

        /**
         * Switches every SIMD-dispatching type to the given tier, lowered to what the target supports (Scalar for a
         * tier of the other architecture family: an x86 tier on aarch64, NEON on x86). Until the first
         * call (or InitializeSIMD) they use the scalar implementations. Tests use this to run the same math under
         * each tier, restoring Scalar after.
         *
         * The math target builds with PUBLIC /arch:AVX (-mavx elsewhere), so the whole binary already requires an
         * AVX CPU: the lower tiers choose between algorithms, they are not a fallback for older hardware. On aarch64
         * the only non-scalar tier is NEON, which is baseline there; Vector2 (batches), Vector3 and Vector4 have NEON
         * versions, Quaternion and the matrices stay scalar
         */
        void SetSIMDLevel(SIMDLevel level);
    }
}
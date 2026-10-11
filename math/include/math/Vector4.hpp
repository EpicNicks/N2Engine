#pragma once

// Define compiler-conditional macros for SIMD target attributes
#if defined(__GNUC__) || defined(__clang__)
#define TARGET_AVX __attribute__((target("avx")))
#define TARGET_AVX2 __attribute__((target("avx2")))
#define TARGET_FMA __attribute__((target("fma")))
#define TARGET_SSE4_1 __attribute__((target("sse4.1")))
#else
// MSVC doesn't need/support target attributes
#define TARGET_AVX
#define TARGET_AVX2
#define TARGET_FMA
#define TARGET_SSE4_1
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
#include <span>
#include "math/CpuInfo.hpp"
#include "math/VectorN.hpp"
#include "math/Constants.hpp"
#include "math/CpuInfo.hpp"

namespace N2Engine::Math
{
    class Vector4
    {
    public:
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201)
#endif
        union alignas(16)
        {
            struct
            {
                float w;
                float x;
                float y;
                float z;
            };

            N2Engine::Math::SimdStorage simd_data;
        };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    private:
        // Function pointer types for SIMD dispatch
        using BinaryFunc = Vector4 (*)(const Vector4 &, const Vector4 &);
        using ScalarFunc = Vector4 (*)(const Vector4 &, float);
        using UnaryFunc = Vector4 (*)(const Vector4 &);
        using DotFunc = float (*)(const Vector4 &, const Vector4 &);
        using LengthFunc = float (*)(const Vector4 &);

    public:
        // Constructors
        Vector4() : w(0.0f), x(0.0f), y(0.0f), z(0.0f) {}
        Vector4(float w, float x, float y, float z) : w(w), x(x), y(y), z(z) {}
        explicit Vector4(float scalar) : w(scalar), x(scalar), y(scalar), z(scalar) {}

        // Copy constructor and assignment
        Vector4(const Vector4 &other) : simd_data(other.simd_data) {}

        Vector4& operator=(const Vector4 &other)
        {
            if (this != &other)
            {
                simd_data = other.simd_data;
            }
            return *this;
        }

        Vector4(const VectorN<float, 4> &vectorN) : w(vectorN[0]), x(vectorN[1]), y(vectorN[2]), z(vectorN[3]) {}

        Vector4& operator=(const VectorN<float, 4> &vectorN)
        {
            w = vectorN[0];
            x = vectorN[1];
            y = vectorN[2];
            z = vectorN[3];
            return *this;
        }

        // NOLINTNEXTLINE(google-explicit-constructor)
        operator VectorN<float, 4>() const
        {
            return VectorN<float, 4>{w, x, y, z};
        }

        [[nodiscard]] VectorN<float, 4> ToVectorN() const
        {
            return VectorN<float, 4>{w, x, y, z};
        }

        static const Vector4 Zero;
        static const Vector4 One;
        static const Vector4 UnitW;
        static const Vector4 UnitX;
        static const Vector4 UnitY;
        static const Vector4 UnitZ;

        Vector4 operator+(const Vector4 &other) const
        {
            return add_func(*this, other);
        }

        Vector4 operator-(const Vector4 &other) const
        {
            return sub_func(*this, other);
        }

        Vector4 operator-() const
        {
            // Multiplying by -1 (unlike 0 - v) flips the sign of zero too
            return scalar_mul_func(*this, -1.0f);
        }

        Vector4 operator*(float scalar) const
        {
            return scalar_mul_func(*this, scalar);
        }

        Vector4 operator/(float scalar) const
        {
            return scalar_div_func(*this, scalar);
        }

        Vector4& operator+=(const Vector4 &other)
        {
            *this = add_func(*this, other);
            return *this;
        }

        Vector4& operator-=(const Vector4 &other)
        {
            *this = sub_func(*this, other);
            return *this;
        }

        Vector4& operator*=(float scalar)
        {
            *this = scalar_mul_func(*this, scalar);
            return *this;
        }

        Vector4& operator/=(float scalar)
        {
            *this = scalar_div_func(*this, scalar);
            return *this;
        }

        bool operator==(const Vector4 &other) const
        {
            return (std::abs(w - other.w) < Constants::EPSILON &&
                std::abs(x - other.x) < Constants::EPSILON &&
                std::abs(y - other.y) < Constants::EPSILON &&
                std::abs(z - other.z) < Constants::EPSILON);
        }

        bool operator!=(const Vector4 &other) const
        {
            return !(*this == other);
        }

        // Array access
        float& operator[](size_t index)
        {
            return (&w)[index];
        }

        const float& operator[](size_t index) const
        {
            return (&w)[index];
        }

        // SIMD-dispatched vector operations
        [[nodiscard]] float Dot(const Vector4 &other) const
        {
            return dot_func(*this, other);
        }

        [[nodiscard]] float Length() const
        {
            return length_func(*this);
        }

        [[nodiscard]] float LengthSquared() const
        {
            return dot_func(*this, *this);
        }

        [[nodiscard]] Vector4 Normalized() const
        {
            return normalize_func(*this);
        }

        Vector4& Normalize()
        {
            *this = normalize_func(*this);
            return *this;
        }

        [[nodiscard]] float Distance(const Vector4 &other) const
        {
            return length_func(sub_func(*this, other));
        }

        [[nodiscard]] float DistanceSquared(const Vector4 &other) const
        {
            const Vector4 diff = sub_func(*this, other);
            return dot_func(diff, diff);
        }

        // Component-wise operations
        static Vector4 Min(const Vector4 &a, const Vector4 &b)
        {
            return min_func(a, b);
        }

        static Vector4 Max(const Vector4 &a, const Vector4 &b)
        {
            return max_func(a, b);
        }

        [[nodiscard]] Vector4 Min(const Vector4 &other) const
        {
            return min_func(*this, other);
        }

        [[nodiscard]] Vector4 Max(const Vector4 &other) const
        {
            return max_func(*this, other);
        }

        [[nodiscard]] Vector4 Clamp(const Vector4 &min, const Vector4 &max) const
        {
            return min_func(max_func(*this, min), max);
        }

        static Vector4 Clamp(const Vector4 &value, const Vector4 &min, const Vector4 &max)
        {
            return value.Clamp(min, max);
        }

        // Floor/Ceil/Round (SIMD-dispatched; SSE4.1 and up)
        [[nodiscard]] Vector4 Floor() const
        {
            return floor_func(*this);
        }

        [[nodiscard]] Vector4 Ceil() const
        {
            return ceil_func(*this);
        }

        [[nodiscard]] Vector4 Round() const
        {
            return round_func(*this);
        }

        static Vector4 Floor(const Vector4 &v) { return floor_func(v); }
        static Vector4 Ceil(const Vector4 &v) { return ceil_func(v); }
        static Vector4 Round(const Vector4 &v) { return round_func(v); }

        // Absolute value per component
        [[nodiscard]] Vector4 Abs() const
        {
            return abs_func(*this);
        }

        static Vector4 Abs(const Vector4 &v)
        {
            return abs_func(v);
        }

        // Sign per component (branchless)
        [[nodiscard]] Vector4 Sign() const
        {
            return {
                static_cast<float>((0.0f < w) - (w < 0.0f)),
                static_cast<float>((0.0f < x) - (x < 0.0f)),
                static_cast<float>((0.0f < y) - (y < 0.0f)),
                static_cast<float>((0.0f < z) - (z < 0.0f))
            };
        }

        // Min/Max component values
        [[nodiscard]] float MaxComponent() const
        {
            return std::max({w, x, y, z});
        }

        [[nodiscard]] float MinComponent() const
        {
            return std::min({w, x, y, z});
        }

        [[nodiscard]] float Sum() const
        {
            return w + x + y + z;
        }

        // Vector4-specific operations
        [[nodiscard]] Vector4 Project(const Vector4 &onto) const
        {
            const float dot = Dot(onto);
            const float ontoMagSq = onto.Dot(onto);
            if (ontoMagSq == 0.0f)
                return Zero;
            return onto * (dot / ontoMagSq);
        }

        [[nodiscard]] Vector4 Reject(const Vector4 &onto) const
        {
            return *this - Project(onto);
        }

        [[nodiscard]] Vector4 Reflect(const Vector4 &normal) const
        {
            const float dot = Dot(normal);
            return *this - normal * (2.0f * dot);
        }

        [[nodiscard]] Vector4 Scale(const Vector4 &other) const
        {
            return scale_func(*this, other);
        }

        static Vector4 Lerp(const Vector4 &a, const Vector4 &b, const float t)
        {
            return a + (b - a) * t;
        }

        [[nodiscard]] Vector4 Lerp(const Vector4 &other, const float t) const
        {
            return Lerp(*this, other, t);
        }

        [[nodiscard]] Vector4 MoveTowards(const Vector4 &target, const float maxDelta) const
        {
            const Vector4 diff = target - *this;
            const float distance = diff.Length();

            if (distance <= maxDelta || distance == 0.0f)
            {
                return target;
            }

            return *this + diff * (maxDelta / distance);
        }

        static Vector4 MoveTowards(const Vector4 &current, const Vector4 &target, float maxDelta)
        {
            return current.MoveTowards(target, maxDelta);
        }

        static Vector4 Slerp(const Vector4 &a, const Vector4 &b, float t)
        {
            float dot = a.Dot(b);
            dot = std::clamp(dot, -1.0f, 1.0f);

            const float theta = std::acos(dot) * t;
            const Vector4 relativeVec = (b - a * dot).Normalized();

            return a * std::cos(theta) + relativeVec * std::sin(theta);
        }

        [[nodiscard]] Vector4 Slerp(const Vector4 &other, float t) const
        {
            return Slerp(*this, other, t);
        }

        [[nodiscard]] Vector4 ClampMagnitude(float maxLength) const
        {
            const float lengthSq = LengthSquared();
            if (lengthSq > maxLength * maxLength)
            {
                const float length = std::sqrt(lengthSq);
                return *this * (maxLength / length);
            }
            return *this;
        }

        static Vector4 ClampMagnitude(const Vector4 &v, const float maxLength)
        {
            return v.ClampMagnitude(maxLength);
        }

        // Utility functions
        [[nodiscard]] bool IsZero(const float tolerance = Constants::EPSILON) const
        {
            return LengthSquared() < tolerance * tolerance;
        }

        [[nodiscard]] bool IsNormalized(const float tolerance = 1e-6f) const
        {
            const float lengthSq = LengthSquared();
            return std::abs(lengthSq - 1.0f) < tolerance;
        }

        // Selects the implementation tier the operations dispatch to; see Math::SetSIMDLevel
        static void SetSIMDLevel(SIMDLevel level);

        // ===== BATCH OPERATIONS =====

        // Raw pointer interface (zero overhead, maximum flexibility). The pointers must address contiguous Vector4s:
        // a derived type such as Common::Color (48 bytes, with its reference members) converts implicitly but would
        // be walked with the wrong stride
        static void AddBatch(const Vector4 *a, const Vector4 *b, Vector4 *result, size_t count);
        static void SubBatch(const Vector4 *a, const Vector4 *b, Vector4 *result, size_t count);
        static void ScalarMulBatch(const Vector4 *input, Vector4 *output, float scalar, size_t count);
        static void DotBatch(const Vector4 *a, const Vector4 *b, float *result, size_t count);
        static void NormalizeBatch(Vector4 *vectors, size_t count);
        static void LengthBatch(const Vector4 *vectors, float *result, size_t count);

        // std::vector interface (convenient, safe)
        static std::vector<Vector4> AddBatch(const std::vector<Vector4> &a, const std::vector<Vector4> &b);
        static std::vector<Vector4> SubBatch(const std::vector<Vector4> &a, const std::vector<Vector4> &b);
        static std::vector<Vector4> ScalarMulBatch(const std::vector<Vector4> &input, float scalar);
        static std::vector<float> DotBatch(const std::vector<Vector4> &a, const std::vector<Vector4> &b);
        static void NormalizeBatch(std::vector<Vector4> &vectors);
        static std::vector<float> LengthBatch(const std::vector<Vector4> &vectors);

        // std::span interface (modern C++, zero overhead)
        static void AddBatch(std::span<const Vector4> a, std::span<const Vector4> b, std::span<Vector4> result);
        static void SubBatch(std::span<const Vector4> a, std::span<const Vector4> b, std::span<Vector4> result);
        static void ScalarMulBatch(std::span<const Vector4> input, std::span<Vector4> output, float scalar);
        static void DotBatch(std::span<const Vector4> a, std::span<const Vector4> b, std::span<float> result);
        static void NormalizeBatch(std::span<Vector4> vectors);
        static void LengthBatch(std::span<const Vector4> vectors, std::span<float> result);

        // Transform arrays
        static void TransformBatch(const Vector4 *input, Vector4 *output, size_t count,
                                   const std::function<Vector4(const Vector4 &)> &transform);
        static std::vector<Vector4> TransformBatch(const std::vector<Vector4> &input,
                                                   const std::function<Vector4(const Vector4 &)> &transform);

    private:
        // ===== SCALAR IMPLEMENTATIONS =====
        static Vector4 AddScalar(const Vector4 &a, const Vector4 &b)
        {
            return {a.w + b.w, a.x + b.x, a.y + b.y, a.z + b.z};
        }

        static Vector4 SubScalar(const Vector4 &a, const Vector4 &b)
        {
            return {a.w - b.w, a.x - b.x, a.y - b.y, a.z - b.z};
        }

        static Vector4 ScalarMulScalar(const Vector4 &v, float scalar)
        {
            return {v.w * scalar, v.x * scalar, v.y * scalar, v.z * scalar};
        }

        static Vector4 ScalarDivScalar(const Vector4 &v, float scalar)
        {
            if (std::abs(scalar) < Constants::EPSILON)
            {
                return Zero;
            }
            return {v.w / scalar, v.x / scalar, v.y / scalar, v.z / scalar};
        }

        static Vector4 ScaleScalar(const Vector4 &a, const Vector4 &b)
        {
            return {a.w * b.w, a.x * b.x, a.y * b.y, a.z * b.z};
        }

        static float DotScalar(const Vector4 &a, const Vector4 &b)
        {
            // Summed in pairs, the same order as the SIMD versions (dpps sums (w + x) + (y + z) too)
            return (a.w * b.w + a.x * b.x) + (a.y * b.y + a.z * b.z);
        }

        static float LengthScalar(const Vector4 &v)
        {
            return std::sqrt(DotScalar(v, v));
        }

        static Vector4 NormalizeScalar(const Vector4 &v)
        {
            const float length = LengthScalar(v);
            if (length < Constants::EPSILON)
            {
                return Zero;
            }
            return {v.w / length, v.x / length, v.y / length, v.z / length};
        }

        static Vector4 MinScalar(const Vector4 &a, const Vector4 &b)
        {
            return {std::min(a.w, b.w), std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
        }

        static Vector4 MaxScalar(const Vector4 &a, const Vector4 &b)
        {
            return {std::max(a.w, b.w), std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
        }

        static Vector4 FloorScalar(const Vector4 &v)
        {
            return {std::floor(v.w), std::floor(v.x), std::floor(v.y), std::floor(v.z)};
        }

        static Vector4 CeilScalar(const Vector4 &v)
        {
            return {std::ceil(v.w), std::ceil(v.x), std::ceil(v.y), std::ceil(v.z)};
        }

        static Vector4 RoundScalar(const Vector4 &v)
        {
            return {std::round(v.w), std::round(v.x), std::round(v.y), std::round(v.z)};
        }

        static Vector4 AbsScalar(const Vector4 &v)
        {
            return {std::abs(v.w), std::abs(v.x), std::abs(v.y), std::abs(v.z)};
        }

        // Static function pointers - initialized to safe defaults
        inline static BinaryFunc add_func = &AddScalar;
        inline static BinaryFunc sub_func = &SubScalar;
        inline static ScalarFunc scalar_mul_func = &ScalarMulScalar;
        inline static ScalarFunc scalar_div_func = &ScalarDivScalar;
        inline static BinaryFunc scale_func = &ScaleScalar;
        inline static DotFunc dot_func = &DotScalar;
        inline static LengthFunc length_func = &LengthScalar;
        inline static UnaryFunc normalize_func = &NormalizeScalar;
        inline static BinaryFunc min_func = &MinScalar;
        inline static BinaryFunc max_func = &MaxScalar;
        inline static UnaryFunc floor_func = &FloorScalar;
        inline static UnaryFunc ceil_func = &CeilScalar;
        inline static UnaryFunc round_func = &RoundScalar;
        inline static UnaryFunc abs_func = &AbsScalar;
        // The batch operations use their AVX versions only at the AVX tier
        inline static SIMDLevel simd_level = SIMDLevel::Scalar;

#ifdef N2_MATH_X86
        // ===== SSE2 IMPLEMENTATIONS =====
        static Vector4 AddSSE2(const Vector4 &a, const Vector4 &b)
        {
            Vector4 result;
            result.simd_data = _mm_add_ps(a.simd_data, b.simd_data);
            return result;
        }

        static Vector4 SubSSE2(const Vector4 &a, const Vector4 &b)
        {
            Vector4 result;
            result.simd_data = _mm_sub_ps(a.simd_data, b.simd_data);
            return result;
        }

        static Vector4 ScalarMulSSE2(const Vector4 &v, float scalar)
        {
            Vector4 result;
            result.simd_data = _mm_mul_ps(v.simd_data, _mm_set1_ps(scalar));
            return result;
        }

        static Vector4 ScalarDivSSE2(const Vector4 &v, float scalar)
        {
            if (std::abs(scalar) < Constants::EPSILON)
            {
                return Zero;
            }
            Vector4 result;
            result.simd_data = _mm_div_ps(v.simd_data, _mm_set1_ps(scalar));
            return result;
        }

        static Vector4 ScaleSSE2(const Vector4 &a, const Vector4 &b)
        {
            Vector4 result;
            result.simd_data = _mm_mul_ps(a.simd_data, b.simd_data);
            return result;
        }

        static float DotSSE2(const Vector4 &a, const Vector4 &b)
        {
            // (w*w' + x*x') + (y*y' + z*z'), like DotScalar
            const __m128 mul = _mm_mul_ps(a.simd_data, b.simd_data);
            const __m128 pairs = _mm_add_ps(mul, _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(2, 3, 0, 1)));
            const __m128 sum = _mm_add_ss(pairs, _mm_movehl_ps(pairs, pairs));
            return _mm_cvtss_f32(sum);
        }

        static float LengthSSE2(const Vector4 &v)
        {
            return std::sqrt(DotSSE2(v, v));
        }

        static Vector4 NormalizeSSE2(const Vector4 &v)
        {
            // A real sqrt and divide: _mm_rsqrt_ps is only accurate to about 12 bits
            const float length = LengthSSE2(v);
            if (length < Constants::EPSILON)
            {
                return Zero;
            }

            Vector4 result;
            result.simd_data = _mm_div_ps(v.simd_data, _mm_set1_ps(length));
            return result;
        }

        static Vector4 MinSSE2(const Vector4 &a, const Vector4 &b)
        {
            // minps returns its second operand for NaN or equal (+-0) inputs, so (b, a) matches std::min(a, b),
            // which is (b < a) ? b : a
            Vector4 result;
            result.simd_data = _mm_min_ps(b.simd_data, a.simd_data);
            return result;
        }

        static Vector4 MaxSSE2(const Vector4 &a, const Vector4 &b)
        {
            // Operands swapped like MinSSE2, to match std::max(a, b), which is (a < b) ? b : a
            Vector4 result;
            result.simd_data = _mm_max_ps(b.simd_data, a.simd_data);
            return result;
        }

        static Vector4 AbsSSE2(const Vector4 &v)
        {
            Vector4 result;
            // Clear the sign bit (branchless)
            result.simd_data = _mm_andnot_ps(_mm_set1_ps(-0.0f), v.simd_data);
            return result;
        }
#else
        // No SSE on this architecture: the scalar versions (the dispatch never picks the SSE tiers here)
        static Vector4 AddSSE2(const Vector4 &a, const Vector4 &b) { return AddScalar(a, b); }
        static Vector4 SubSSE2(const Vector4 &a, const Vector4 &b) { return SubScalar(a, b); }
        static Vector4 ScalarMulSSE2(const Vector4 &v, float scalar) { return ScalarMulScalar(v, scalar); }
        static Vector4 ScalarDivSSE2(const Vector4 &v, float scalar) { return ScalarDivScalar(v, scalar); }
        static Vector4 ScaleSSE2(const Vector4 &a, const Vector4 &b) { return ScaleScalar(a, b); }
        static float DotSSE2(const Vector4 &a, const Vector4 &b) { return DotScalar(a, b); }
        static float LengthSSE2(const Vector4 &v) { return LengthScalar(v); }
        static Vector4 NormalizeSSE2(const Vector4 &v) { return NormalizeScalar(v); }
        static Vector4 MinSSE2(const Vector4 &a, const Vector4 &b) { return MinScalar(a, b); }
        static Vector4 MaxSSE2(const Vector4 &a, const Vector4 &b) { return MaxScalar(a, b); }
        static Vector4 AbsSSE2(const Vector4 &v) { return AbsScalar(v); }
#endif

        // ===== SSE4.1 IMPLEMENTATIONS =====
#ifdef N2_MATH_SSE41
        TARGET_SSE4_1 static float DotSSE41(const Vector4 &a, const Vector4 &b)
        {
            // Mask 0xF1: multiply all four lanes, sum into lane 0
            return _mm_cvtss_f32(_mm_dp_ps(a.simd_data, b.simd_data, 0xF1));
        }

        TARGET_SSE4_1 static float LengthSSE41(const Vector4 &v)
        {
            return _mm_cvtss_f32(_mm_sqrt_ss(_mm_dp_ps(v.simd_data, v.simd_data, 0xF1)));
        }

        TARGET_SSE4_1 static Vector4 NormalizeSSE41(const Vector4 &v)
        {
            // Mask 0xFF: dot of all four lanes broadcast to every lane. A real sqrt and divide, since
            // _mm_rsqrt_ps is only accurate to about 12 bits
            const __m128 length = _mm_sqrt_ps(_mm_dp_ps(v.simd_data, v.simd_data, 0xFF));
            if (_mm_cvtss_f32(length) < Constants::EPSILON)
            {
                return Zero;
            }

            Vector4 result;
            result.simd_data = _mm_div_ps(v.simd_data, length);
            return result;
        }

        TARGET_SSE4_1 static Vector4 FloorSSE41(const Vector4 &v)
        {
            Vector4 result;
            result.simd_data = _mm_floor_ps(v.simd_data);
            return result;
        }

        TARGET_SSE4_1 static Vector4 CeilSSE41(const Vector4 &v)
        {
            Vector4 result;
            result.simd_data = _mm_ceil_ps(v.simd_data);
            return result;
        }

        TARGET_SSE4_1 static Vector4 RoundSSE41(const Vector4 &v)
        {
            // std::round rounds halfway cases away from zero, but _MM_FROUND_TO_NEAREST_INT rounds them to even
            // (2.5 -> 2). So truncate, then step away from zero when the dropped fraction is >= 0.5. Infinities
            // give a NaN fraction, which compares false, so they pass through unchanged
            const __m128 sign_mask = _mm_set1_ps(-0.0f);
            const __m128 sign = _mm_and_ps(v.simd_data, sign_mask);
            const __m128 truncated = _mm_round_ps(v.simd_data, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
            const __m128 fraction = _mm_andnot_ps(sign_mask, _mm_sub_ps(v.simd_data, truncated));
            const __m128 away = _mm_or_ps(sign, _mm_set1_ps(1.0f));
            const __m128 step = _mm_and_ps(_mm_cmpge_ps(fraction, _mm_set1_ps(0.5f)), away);

            // Put the input's sign back: -0 + +0 is +0, but std::round(-0.3) is -0
            Vector4 result;
            result.simd_data = _mm_or_ps(_mm_add_ps(truncated, step), sign);
            return result;
        }
#else
        static float DotSSE41(const Vector4 &a, const Vector4 &b) { return DotSSE2(a, b); }
        static float LengthSSE41(const Vector4 &v) { return LengthSSE2(v); }
        static Vector4 NormalizeSSE41(const Vector4 &v) { return NormalizeSSE2(v); }
        static Vector4 FloorSSE41(const Vector4 &v) { return FloorScalar(v); }
        static Vector4 CeilSSE41(const Vector4 &v) { return CeilScalar(v); }
        static Vector4 RoundSSE41(const Vector4 &v) { return RoundScalar(v); }
#endif
    };

    // Non-member operators
    inline Vector4 operator*(float scalar, const Vector4 &v)
    {
        return v * scalar;
    }

    // Type alias for compatibility
    using Vector4f = Vector4;
}
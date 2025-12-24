#pragma once

#include <cmath>
#include <functional>
#include <vector>
#include <span>
#include <immintrin.h>
#include "math/VectorN.hpp"
#include "math/Constants.hpp"

namespace N2Engine::Math
{
    class Vector4
    {
    public:
        union alignas(16)
        {
            struct
            {
                float w;
                float x;
                float y;
                float z;
            };

            __m128 simd_data;
        };

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
            Vector4 result;
            result.simd_data = _mm_add_ps(simd_data, other.simd_data);
            return result;
        }

        Vector4 operator-(const Vector4 &other) const
        {
            Vector4 result;
            result.simd_data = _mm_sub_ps(simd_data, other.simd_data);
            return result;
        }

        Vector4 operator-() const
        {
            Vector4 result;
            __m128 zero = _mm_setzero_ps();
            result.simd_data = _mm_sub_ps(zero, simd_data);
            return result;
        }

        Vector4 operator*(float scalar) const
        {
            Vector4 result;
            __m128 scalar_vec = _mm_set1_ps(scalar);
            result.simd_data = _mm_mul_ps(simd_data, scalar_vec);
            return result;
        }

        Vector4 operator/(float scalar) const
        {
            if (std::abs(scalar) < Constants::EPSILON)
            {
                return Zero;
            }
            Vector4 result;
            __m128 scalar_vec = _mm_set1_ps(scalar);
            result.simd_data = _mm_div_ps(simd_data, scalar_vec);
            return result;
        }

        Vector4& operator+=(const Vector4 &other)
        {
            simd_data = _mm_add_ps(simd_data, other.simd_data);
            return *this;
        }

        Vector4& operator-=(const Vector4 &other)
        {
            simd_data = _mm_sub_ps(simd_data, other.simd_data);
            return *this;
        }

        Vector4& operator*=(float scalar)
        {
            __m128 scalar_vec = _mm_set1_ps(scalar);
            simd_data = _mm_mul_ps(simd_data, scalar_vec);
            return *this;
        }

        Vector4& operator/=(float scalar)
        {
            if (std::abs(scalar) < Constants::EPSILON)
            {
                *this = Zero;
                return *this;
            }
            __m128 scalar_vec = _mm_set1_ps(scalar);
            simd_data = _mm_div_ps(simd_data, scalar_vec);
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

        // SIMD-optimized vector operations
        [[nodiscard]] float Dot(const Vector4 &other) const
        {
#ifdef __SSE4_1__
            __m128 result = _mm_dp_ps(simd_data, other.simd_data, 0xF1);
            return _mm_cvtss_f32(result);
#else
            __m128 mul = _mm_mul_ps(simd_data, other.simd_data);
            __m128 shuf = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 sums = _mm_add_ps(mul, shuf);
            shuf = _mm_movehl_ps(shuf, sums);
            sums = _mm_add_ss(sums, shuf);
            return _mm_cvtss_f32(sums);
#endif
        }

        [[nodiscard]] float Length() const
        {
#ifdef __SSE4_1__
            __m128 dot = _mm_dp_ps(simd_data, simd_data, 0xF1);
            __m128 length = _mm_sqrt_ss(dot);
            return _mm_cvtss_f32(length);
#else
            return std::sqrt(Dot(*this));
#endif
        }

        [[nodiscard]] float LengthSquared() const
        {
            return Dot(*this);
        }

        [[nodiscard]] Vector4 Normalized() const
        {
            const float lengthSq = LengthSquared();
            if (lengthSq < Constants::EPSILON * Constants::EPSILON)
            {
                return Zero;
            }

            Vector4 result;
#ifdef __SSE4_1__
            __m128 length_sq_vec = _mm_dp_ps(simd_data, simd_data, 0xFF);
            __m128 inv_length = _mm_rsqrt_ps(length_sq_vec);
            result.simd_data = _mm_mul_ps(simd_data, inv_length);
#else
            __m128 length_sq_vec = _mm_set1_ps(lengthSq);
            __m128 inv_length = _mm_rsqrt_ps(length_sq_vec);
            result.simd_data = _mm_mul_ps(simd_data, inv_length);
#endif
            return result;
        }

        Vector4& Normalize()
        {
            *this = Normalized();
            return *this;
        }

        [[nodiscard]] float Distance(const Vector4 &other) const
        {
            return (*this - other).Length();
        }

        [[nodiscard]] float DistanceSquared(const Vector4 &other) const
        {
            Vector4 diff = *this - other;
            return diff.Dot(diff);
        }

        // Component-wise operations (SIMD-optimized)
        static Vector4 Min(const Vector4 &a, const Vector4 &b)
        {
            Vector4 result;
            result.simd_data = _mm_min_ps(a.simd_data, b.simd_data);
            return result;
        }

        static Vector4 Max(const Vector4 &a, const Vector4 &b)
        {
            Vector4 result;
            result.simd_data = _mm_max_ps(a.simd_data, b.simd_data);
            return result;
        }

        [[nodiscard]] Vector4 Min(const Vector4 &other) const
        {
            return Min(*this, other);
        }

        [[nodiscard]] Vector4 Max(const Vector4 &other) const
        {
            return Max(*this, other);
        }

        [[nodiscard]] Vector4 Clamp(const Vector4 &min, const Vector4 &max) const
        {
            return Min(Max(*this, min), max);
        }

        static Vector4 Clamp(const Vector4 &value, const Vector4 &min, const Vector4 &max)
        {
            return value.Clamp(min, max);
        }

        // Floor/Ceil/Round (SIMD-optimized with SSE4.1)
        [[nodiscard]] Vector4 Floor() const
        {
            Vector4 result;
#ifdef __SSE4_1__
            result.simd_data = _mm_floor_ps(simd_data);
#else
            result.w = std::floor(w);
            result.x = std::floor(x);
            result.y = std::floor(y);
            result.z = std::floor(z);
#endif
            return result;
        }

        [[nodiscard]] Vector4 Ceil() const
        {
            Vector4 result;
#ifdef __SSE4_1__
            result.simd_data = _mm_ceil_ps(simd_data);
#else
            result.w = std::ceil(w);
            result.x = std::ceil(x);
            result.y = std::ceil(y);
            result.z = std::ceil(z);
#endif
            return result;
        }

        [[nodiscard]] Vector4 Round() const
        {
            Vector4 result;
#ifdef __SSE4_1__
            result.simd_data = _mm_round_ps(simd_data, _MM_FROUND_TO_NEAREST_INT);
#else
            result.w = std::round(w);
            result.x = std::round(x);
            result.y = std::round(y);
            result.z = std::round(z);
#endif
            return result;
        }

        static Vector4 Floor(const Vector4 &v) { return v.Floor(); }
        static Vector4 Ceil(const Vector4 &v) { return v.Ceil(); }
        static Vector4 Round(const Vector4 &v) { return v.Round(); }

        // Absolute value per component
        [[nodiscard]] Vector4 Abs() const
        {
            Vector4 result;
            __m128 sign_mask = _mm_set1_ps(-0.0f);
            result.simd_data = _mm_andnot_ps(sign_mask, simd_data);
            return result;
        }

        static Vector4 Abs(const Vector4 &v)
        {
            return v.Abs();
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
            Vector4 result;
            result.simd_data = _mm_mul_ps(simd_data, other.simd_data);
            return result;
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

        // ===== BATCH OPERATIONS =====

        // Raw pointer interface (zero overhead, maximum flexibility)
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
    };

    // Non-member operators
    inline Vector4 operator*(float scalar, const Vector4 &v)
    {
        return v * scalar;
    }

    // Type alias for compatibility
    using Vector4f = Vector4;
}
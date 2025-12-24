#pragma once

#include <cmath>
#include <functional>
#include "math/VectorN.hpp" // For VectorN compatibility
#include "math/Constants.hpp"

#ifdef __AVX__
#include <immintrin.h>
#endif

namespace N2Engine::Math
{
    class Vector2
    {
    public:
        float x;
        float y;

        // Constructors
        Vector2() : x(0.0f), y(0.0f) {}
        Vector2(float x, float y) : x(x), y(y) {}
        explicit Vector2(float scalar) : x(scalar), y(scalar) {}

        // Copy constructor and assignment
        Vector2(const Vector2 &other) = default;
        Vector2& operator=(const Vector2 &other) = default;

        // Conversion constructors and operators for VectorN compatibility
        Vector2(const VectorN<float, 2> &vectorN) : x(vectorN[0]), y(vectorN[1]) {}

        Vector2& operator=(const VectorN<float, 2> &vectorN)
        {
            x = vectorN[0];
            y = vectorN[1];
            return *this;
        }

        // Implicit conversion to VectorN<float, 2>
        // NOLINTNEXTLINE(google-explicit-constructor)
        operator VectorN<float, 2>() const
        {
            return VectorN<float, 2>{x, y};
        }

        // Explicit conversion method (alternative to implicit)
        [[nodiscard]] VectorN<float, 2> ToVectorN() const
        {
            return VectorN<float, 2>{x, y};
        }

        // Static constant helpers
        static const Vector2 Zero;
        static const Vector2 One;
        static const Vector2 Up;
        static const Vector2 Down;
        static const Vector2 Left;
        static const Vector2 Right;

        // Basic operations (scalar - no SIMD overhead)
        Vector2 operator+(const Vector2 &other) const
        {
            return {x + other.x, y + other.y};
        }

        Vector2 operator-(const Vector2 &other) const
        {
            return {x - other.x, y - other.y};
        }

        Vector2 operator-() const
        {
            return {-x, -y};
        }

        Vector2 operator*(float scalar) const
        {
            return {x * scalar, y * scalar};
        }

        Vector2 operator/(float scalar) const
        {
            if (std::abs(scalar) < Constants::EPSILON)
            {
                return Zero;
            }
            const float inv_scalar = 1.0f / scalar;
            return {x * inv_scalar, y * inv_scalar};
        }

        Vector2& operator+=(const Vector2 &other)
        {
            x += other.x;
            y += other.y;
            return *this;
        }

        Vector2& operator-=(const Vector2 &other)
        {
            x -= other.x;
            y -= other.y;
            return *this;
        }

        Vector2& operator*=(float scalar)
        {
            x *= scalar;
            y *= scalar;
            return *this;
        }

        Vector2& operator/=(float scalar)
        {
            if (std::abs(scalar) < Constants::EPSILON)
            {
                *this = Zero;
                return *this;
            }
            const float inv_scalar = 1.0f / scalar;
            x *= inv_scalar;
            y *= inv_scalar;
            return *this;
        }

        bool operator==(const Vector2 &other) const
        {
            return (std::abs(x - other.x) < Constants::EPSILON &&
                std::abs(y - other.y) < Constants::EPSILON);
        }

        bool operator!=(const Vector2 &other) const
        {
            return !(*this == other);
        }

        // Array access
        float& operator[](size_t index)
        {
            return (&x)[index];
        }

        const float& operator[](size_t index) const
        {
            return (&x)[index];
        }

        // Vector operations
        [[nodiscard]] float Dot(const Vector2 &other) const
        {
            return x * other.x + y * other.y;
        }

        [[nodiscard]] float Length() const
        {
            return std::sqrt(x * x + y * y);
        }

        [[nodiscard]] float LengthSquared() const
        {
            return x * x + y * y;
        }

        [[nodiscard]] Vector2 Normalized() const
        {
            const float length = Length();
            if (length < Constants::EPSILON)
            {
                return Zero;
            }
            const float inv_length = 1.0f / length;
            return {x * inv_length, y * inv_length};
        }

        Vector2& Normalize()
        {
            const float length = Length();
            if (length >= Constants::EPSILON)
            {
                const float inv_length = 1.0f / length;
                x *= inv_length;
                y *= inv_length;
            }
            else
            {
                *this = Zero;
            }
            return *this;
        }

        [[nodiscard]] float Distance(const Vector2 &other) const
        {
            const float dx = x - other.x;
            const float dy = y - other.y;
            return std::sqrt(dx * dx + dy * dy);
        }

        [[nodiscard]] float DistanceSquared(const Vector2 &other) const
        {
            const float dx = x - other.x;
            const float dy = y - other.y;
            return dx * dx + dy * dy;
        }

        // Component-wise operations
        static Vector2 Min(const Vector2 &a, const Vector2 &b)
        {
            return {std::min(a.x, b.x), std::min(a.y, b.y)};
        }

        static Vector2 Max(const Vector2 &a, const Vector2 &b)
        {
            return {std::max(a.x, b.x), std::max(a.y, b.y)};
        }

        [[nodiscard]] Vector2 Min(const Vector2 &other) const
        {
            return Min(*this, other);
        }

        [[nodiscard]] Vector2 Max(const Vector2 &other) const
        {
            return Max(*this, other);
        }

        // Clamp each component
        [[nodiscard]] Vector2 Clamp(const Vector2 &min, const Vector2 &max) const
        {
            return Min(Max(*this, min), max);
        }

        static Vector2 Clamp(const Vector2 &value, const Vector2 &min, const Vector2 &max)
        {
            return value.Clamp(min, max);
        }

        // Floor/Ceil/Round
        [[nodiscard]] Vector2 Floor() const
        {
            return {std::floor(x), std::floor(y)};
        }

        [[nodiscard]] Vector2 Ceil() const
        {
            return {std::ceil(x), std::ceil(y)};
        }

        [[nodiscard]] Vector2 Round() const
        {
            return {std::round(x), std::round(y)};
        }

        static Vector2 Floor(const Vector2 &v)
        {
            return v.Floor();
        }

        static Vector2 Ceil(const Vector2 &v)
        {
            return v.Ceil();
        }

        static Vector2 Round(const Vector2 &v)
        {
            return v.Round();
        }

        // Absolute value per component
        [[nodiscard]] Vector2 Abs() const
        {
            return {std::abs(x), std::abs(y)};
        }

        static Vector2 Abs(const Vector2 &v)
        {
            return v.Abs();
        }

        // Sign per component (branchless)
        [[nodiscard]] Vector2 Sign() const
        {
            return {
                static_cast<float>((0.0f < x) - (x < 0.0f)),
                static_cast<float>((0.0f < y) - (y < 0.0f))
            };
        }

        // Min/Max component values
        [[nodiscard]] float MaxComponent() const
        {
            return std::max(x, y);
        }

        [[nodiscard]] float MinComponent() const
        {
            return std::min(x, y);
        }

        [[nodiscard]] float Sum() const
        {
            return x + y;
        }

        // Vector2-specific operations
        [[nodiscard]] float Cross(const Vector2 &other) const
        {
            return x * other.y - y * other.x;
        }

        static float Cross(const Vector2 &a, const Vector2 &b)
        {
            return a.Cross(b);
        }

        [[nodiscard]] Vector2 Perpendicular() const
        {
            return {-y, x};
        }

        [[nodiscard]] Vector2 PerpendicularCW() const
        {
            return {y, -x};
        }

        [[nodiscard]] float Angle() const
        {
            return std::atan2(y, x);
        }

        [[nodiscard]] float AngleTo(const Vector2 &other) const
        {
            const float cross = Cross(other);
            const float dot = Dot(other);
            return std::atan2(cross, dot);
        }

        static Vector2 FromAngle(float radians)
        {
            return {std::cos(radians), std::sin(radians)};
        }

        static Vector2 FromAngle(float radians, float magnitude)
        {
            return FromAngle(radians) * magnitude;
        }

        [[nodiscard]] Vector2 Rotated(float radians) const
        {
            const float cos_r = std::cos(radians);
            const float sin_r = std::sin(radians);
            return {x * cos_r - y * sin_r, x * sin_r + y * cos_r};
        }

        Vector2& Rotate(float radians)
        {
            const float cos_r = std::cos(radians);
            const float sin_r = std::sin(radians);
            const float new_x = x * cos_r - y * sin_r;
            const float new_y = x * sin_r + y * cos_r;
            x = new_x;
            y = new_y;
            return *this;
        }

        [[nodiscard]] Vector2 Project(const Vector2 &onto) const
        {
            const float dot = Dot(onto);
            const float ontoMagSq = onto.Dot(onto);
            if (ontoMagSq == 0.0f)
                return Zero;
            return onto * (dot / ontoMagSq);
        }

        [[nodiscard]] Vector2 Reject(const Vector2 &onto) const
        {
            return *this - Project(onto);
        }

        [[nodiscard]] Vector2 Reflect(const Vector2 &normal) const
        {
            const float dot = Dot(normal);
            return *this - normal * (2.0f * dot);
        }

        [[nodiscard]] Vector2 Scale(const Vector2 &other) const
        {
            return {x * other.x, y * other.y};
        }

        static Vector2 Lerp(const Vector2 &a, const Vector2 &b, const float t)
        {
            return a + (b - a) * t;
        }

        [[nodiscard]] Vector2 Lerp(const Vector2 &other, const float t) const
        {
            return Lerp(*this, other, t);
        }

        [[nodiscard]] Vector2 MoveTowards(const Vector2 &target, const float maxDelta) const
        {
            const Vector2 diff = target - *this;
            const float distance = diff.Length();

            if (distance <= maxDelta || distance == 0.0f)
            {
                return target;
            }

            return *this + diff * (maxDelta / distance);
        }

        static Vector2 MoveTowards(const Vector2 &current, const Vector2 &target, float maxDelta)
        {
            return current.MoveTowards(target, maxDelta);
        }

        static Vector2 Slerp(const Vector2 &a, const Vector2 &b, float t)
        {
            float dot = a.Dot(b);
            dot = std::clamp(dot, -1.0f, 1.0f);

            const float theta = std::acos(dot) * t;
            const Vector2 relativeVec = (b - a * dot).Normalized();

            return a * std::cos(theta) + relativeVec * std::sin(theta);
        }

        [[nodiscard]] Vector2 Slerp(const Vector2 &other, float t) const
        {
            return Slerp(*this, other, t);
        }

        [[nodiscard]] Vector2 ClampMagnitude(float maxLength) const
        {
            const float lengthSq = LengthSquared();
            if (lengthSq > maxLength * maxLength)
            {
                const float length = std::sqrt(lengthSq);
                return *this * (maxLength / length);
            }
            return *this;
        }

        static Vector2 ClampMagnitude(const Vector2 &v, const float maxLength)
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

        [[nodiscard]] bool IsParallel(const Vector2 &other, const float tolerance = 1e-6f) const
        {
            return std::abs(Cross(other)) < tolerance;
        }

        [[nodiscard]] bool IsPerpendicular(const Vector2 &other, const float tolerance = 1e-6f) const
        {
            return std::abs(Dot(other)) < tolerance;
        }

        // SIMD batch operations for arrays of Vector2s
        // These use AVX when available to process multiple vectors efficiently
        static void AddBatch(const Vector2 *a, const Vector2 *b, Vector2 *result, size_t count);
        static void SubBatch(const Vector2 *a, const Vector2 *b, Vector2 *result, size_t count);
        static void ScalarMulBatch(const Vector2 *input, Vector2 *output, float scalar, size_t count);
        static void DotBatch(const Vector2 *a, const Vector2 *b, float *result, size_t count);
        static void NormalizeBatch(Vector2 *vectors, size_t count);
        static void LengthBatch(const Vector2 *vectors, float *result, size_t count);

        // Transform arrays of Vector2s
        static void TransformBatch(const Vector2 *input, Vector2 *output, size_t count,
                                   const std::function<Vector2(const Vector2 &)> &transform);
    };

    // Non-member operators
    inline Vector2 operator*(float scalar, const Vector2 &v)
    {
        return v * scalar;
    }

    // Type alias for compatibility
    using Vector2f = Vector2;
}
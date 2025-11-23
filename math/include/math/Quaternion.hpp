#pragma once

#include <cmath>
#include <immintrin.h>
#include "math/Matrix.hpp"

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

#ifdef _WIN32
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif

namespace N2Engine::Math
{
    class Vector3;

    class Quaternion
    {
    private:
        // SIMD-optimized storage
        union alignas(16)
        {
            struct
            {
                float w, x, y, z;
            };
            __m128 simd_data;
        };

        // Function pointer types for SIMD dispatch
        using AddFunc = Quaternion (*)(const Quaternion &, const Quaternion &);
        using SubFunc = Quaternion (*)(const Quaternion &, const Quaternion &);
        using MulFunc = Quaternion (*)(const Quaternion &, const Quaternion &);
        using ScalarMulFunc = Quaternion (*)(const Quaternion &, float);
        using DotFunc = float (*)(const Quaternion &, const Quaternion &);
        using LengthFunc = float (*)(const Quaternion &);
        using NormalizeFunc = Quaternion (*)(const Quaternion &);

    public:
        static const float EPSILON;

        // Constructors
        Quaternion() : w(1.0f), x(0.0f), y(0.0f), z(0.0f) {}

        Quaternion(float w, float x, float y, float z) : w(w), x(x), y(y), z(z) {}

        explicit Quaternion(const Vector3 &axis, float angle);
        explicit Quaternion(float pitch, float yaw, float roll);

        float GetW() const { return w; }
        float GetX() const { return x; }
        float GetY() const { return y; }
        float GetZ() const { return z; }

        // Static factory methods
        static Quaternion Identity()
        {
            return Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
        }

        static Quaternion FromAxisAngle(const Vector3 &axis, float angle)
        {
            return Quaternion(axis, angle);
        }

        static Quaternion FromEulerAngles(float pitch, float yaw, float roll)
        {
            return Quaternion(pitch, yaw, roll);
        }

        static Quaternion LookRotation(const Vector3 &forward, const Vector3 &up);
        static Quaternion Slerp(const Quaternion &a, const Quaternion &b, float t);
        static Quaternion Lerp(const Quaternion &a, const Quaternion &b, float t);

        // SIMD-optimized basic operations
        Quaternion operator+(const Quaternion &other) const
        {
            return add_func(*this, other);
        }

        Quaternion operator-(const Quaternion &other) const
        {
            return sub_func(*this, other);
        }

        Quaternion operator*(const Quaternion &other) const
        {
            return mul_func(*this, other);
        }

        Quaternion operator*(float scalar) const
        {
            return scalar_mul_func(*this, scalar);
        }

        Vector3 operator*(const Vector3 &other) const;
        Quaternion operator/(float scalar) const;

        Quaternion &operator+=(const Quaternion &other)
        {
            *this = add_func(*this, other);
            return *this;
        }

        Quaternion &operator-=(const Quaternion &other)
        {
            *this = sub_func(*this, other);
            return *this;
        }

        Quaternion &operator*=(const Quaternion &other)
        {
            *this = mul_func(*this, other);
            return *this;
        }

        Quaternion &operator*=(float scalar)
        {
            *this = scalar_mul_func(*this, scalar);
            return *this;
        }

        Quaternion &operator/=(float scalar);

        bool operator==(const Quaternion &other) const;
        bool operator!=(const Quaternion &other) const;

        // SIMD-optimized quaternion specific operations
        float Length() const
        {
            return length_func(*this);
        }

        float LengthSquared() const
        {
            return dot_func(*this, *this);
        }

        Quaternion Normalized() const
        {
            return normalize_func(*this);
        }

        Quaternion &Normalize()
        {
            *this = normalize_func(*this);
            return *this;
        }

        Quaternion Conjugate() const
        {
            Quaternion result;
            // Conjugate: negate x, y, z components
            result.simd_data = _mm_xor_ps(simd_data, _mm_set_ps(-0.0f, -0.0f, -0.0f, 0.0f));
            return result;
        }

        Quaternion Inverse() const;

        float Dot(const Quaternion &other) const
        {
            return dot_func(*this, other);
        }

        float Angle(const Quaternion &other) const;

        // Rotation operations
        Vector3 Rotate(const Vector3 &vector) const;
        Vector3 ToEulerAngles() const;
        Matrix<float, 4, 4> ToMatrix() const;

        // Utility
        bool IsNormalized(float tolerance = 1e-6f) const;
        bool IsIdentity(float tolerance = 1e-6f) const;

        // SIMD initialization - call once at startup
        static void InitializeSIMD();

    private:
        // ===== SCALAR IMPLEMENTATIONS =====
        static Quaternion AddScalar(const Quaternion &a, const Quaternion &b)
        {
            return Quaternion(a.w + b.w, a.x + b.x, a.y + b.y, a.z + b.z);
        }

        static Quaternion SubScalar(const Quaternion &a, const Quaternion &b)
        {
            return Quaternion(a.w - b.w, a.x - b.x, a.y - b.y, a.z - b.z);
        }

        static Quaternion MulScalar(const Quaternion &a, const Quaternion &b)
        {
            return Quaternion(
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w);
        }

        static Quaternion ScalarMulScalar(const Quaternion &q, float scalar)
        {
            return Quaternion(q.w * scalar, q.x * scalar, q.y * scalar, q.z * scalar);
        }

        static float DotScalar(const Quaternion &a, const Quaternion &b)
        {
            return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        }

        static float LengthScalar(const Quaternion &q)
        {
            return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        }

        static Quaternion NormalizeScalar(const Quaternion &q)
        {
            float length = LengthScalar(q);
            if (length < EPSILON)
            {
                return Identity();
            }
            float inv_length = 1.0f / length;
            return Quaternion(q.w * inv_length, q.x * inv_length, q.y * inv_length, q.z * inv_length);
        }

        // Static function pointers - initialized to safe defaults
        inline static AddFunc add_func = &AddScalar;
        inline static SubFunc sub_func = &SubScalar;
        inline static MulFunc mul_func = &MulScalar;
        inline static ScalarMulFunc scalar_mul_func = &ScalarMulScalar;
        inline static DotFunc dot_func = &DotScalar;
        inline static LengthFunc length_func = &LengthScalar;
        inline static NormalizeFunc normalize_func = &NormalizeScalar;
        inline static bool initialized = false;

        // ===== SSE2 IMPLEMENTATIONS =====
        static Quaternion AddSSE2(const Quaternion &a, const Quaternion &b)
        {
            Quaternion result;
            result.simd_data = _mm_add_ps(a.simd_data, b.simd_data);
            return result;
        }

        static Quaternion SubSSE2(const Quaternion &a, const Quaternion &b)
        {
            Quaternion result;
            result.simd_data = _mm_sub_ps(a.simd_data, b.simd_data);
            return result;
        }

        static Quaternion ScalarMulSSE2(const Quaternion &q, float scalar)
        {
            Quaternion result;
            __m128 scalar_vec = _mm_set1_ps(scalar);
            result.simd_data = _mm_mul_ps(q.simd_data, scalar_vec);
            return result;
        }

        static float DotSSE2(const Quaternion &a, const Quaternion &b)
        {
            __m128 mul = _mm_mul_ps(a.simd_data, b.simd_data);
            // Horizontal add: sum all 4 components
            __m128 shuf = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 sums = _mm_add_ps(mul, shuf);
            shuf = _mm_movehl_ps(shuf, sums);
            sums = _mm_add_ss(sums, shuf);
            return _mm_cvtss_f32(sums);
        }

        static float LengthSSE2(const Quaternion &q)
        {
            float dot = DotSSE2(q, q);
            return std::sqrt(dot);
        }

        static Quaternion NormalizeSSE2(const Quaternion &q)
        {
            float length_sq = DotSSE2(q, q);
            if (length_sq < EPSILON * EPSILON)
            {
                return Identity();
            }

            Quaternion result;
            __m128 length_sq_vec = _mm_set1_ps(length_sq);
            __m128 inv_length = _mm_rsqrt_ps(length_sq_vec); // Fast reciprocal sqrt
            result.simd_data = _mm_mul_ps(q.simd_data, inv_length);
            return result;
        }

        static Quaternion MulSSE2(const Quaternion &a, const Quaternion &b)
        {
            // SIMD quaternion multiplication - complex but faster for repeated operations
            __m128 a_vec = a.simd_data; // [w, x, y, z]
            __m128 b_vec = b.simd_data; // [w, x, y, z]

            // Create shuffled versions for quaternion multiplication
            __m128 a_wwww = _mm_shuffle_ps(a_vec, a_vec, _MM_SHUFFLE(0, 0, 0, 0)); // [w, w, w, w]
            __m128 a_xyzx = _mm_shuffle_ps(a_vec, a_vec, _MM_SHUFFLE(1, 2, 3, 1)); // [x, y, z, x]
            __m128 a_yzxy = _mm_shuffle_ps(a_vec, a_vec, _MM_SHUFFLE(2, 3, 1, 2)); // [y, z, x, y]
            __m128 a_zxyz = _mm_shuffle_ps(a_vec, a_vec, _MM_SHUFFLE(3, 1, 2, 3)); // [z, x, y, z]

            __m128 b_wzyx = _mm_shuffle_ps(b_vec, b_vec, _MM_SHUFFLE(0, 3, 2, 1)); // [w, z, y, x]
            __m128 b_zwxy = _mm_shuffle_ps(b_vec, b_vec, _MM_SHUFFLE(3, 0, 1, 2)); // [z, w, x, y]
            __m128 b_yxwz = _mm_shuffle_ps(b_vec, b_vec, _MM_SHUFFLE(2, 1, 0, 3)); // [y, x, w, z]

            // Compute quaternion multiplication components
            __m128 result1 = _mm_mul_ps(a_wwww, b_vec);  // w*[w,x,y,z]
            __m128 result2 = _mm_mul_ps(a_xyzx, b_wzyx); // [x,y,z,x]*[w,z,y,x]
            __m128 result3 = _mm_mul_ps(a_yzxy, b_zwxy); // [y,z,x,y]*[z,w,x,y]
            __m128 result4 = _mm_mul_ps(a_zxyz, b_yxwz); // [z,x,y,z]*[y,x,w,z]

            // Apply signs: + - + -
            __m128 signs = _mm_set_ps(-1.0f, 1.0f, -1.0f, 1.0f); // [-, +, -, +]
            result2 = _mm_mul_ps(result2, signs);
            signs = _mm_set_ps(1.0f, -1.0f, 1.0f, -1.0f); // [+, -, +, -]
            result3 = _mm_mul_ps(result3, signs);
            signs = _mm_set_ps(-1.0f, 1.0f, -1.0f, 1.0f); // [-, +, -, +]
            result4 = _mm_mul_ps(result4, signs);

            Quaternion result;
            result.simd_data = _mm_add_ps(_mm_add_ps(result1, result2), _mm_add_ps(result3, result4));
            return result;
        }

        // ===== SSE4.1 IMPLEMENTATIONS =====
#ifdef __SSE4_1__
            TARGET_SSE4_1 static float DotSSE41(const Quaternion &a, const Quaternion &b)
        {
            __m128 result = _mm_dp_ps(a.simd_data, b.simd_data, 0xF1);
            return _mm_cvtss_f32(result);
        }

            TARGET_SSE4_1 static float LengthSSE41(const Quaternion &q)
        {
            __m128 dot = _mm_dp_ps(q.simd_data, q.simd_data, 0xF1);
            __m128 length = _mm_sqrt_ss(dot);
            return _mm_cvtss_f32(length);
        }

            TARGET_SSE4_1 static Quaternion NormalizeSSE41(const Quaternion &q)
        {
            __m128 length_sq = _mm_dp_ps(q.simd_data, q.simd_data, 0xFF);

            // Check for near-zero length
            __m128 epsilon = _mm_set1_ps(EPSILON * EPSILON);
            __m128 mask = _mm_cmplt_ps(length_sq, epsilon);

            if (_mm_movemask_ps(mask) != 0)
            {
                return Identity();
            }

            Quaternion result;
            __m128 inv_length = _mm_rsqrt_ps(length_sq);
            result.simd_data = _mm_mul_ps(q.simd_data, inv_length);
            return result;
        }
#else
        // Fallback to SSE2 versions when SSE4.1 not available
        static float DotSSE41(const Quaternion &a, const Quaternion &b)
        {
            return DotSSE2(a, b);
        }
        static float LengthSSE41(const Quaternion &q)
        {
            return LengthSSE2(q);
        }
        static Quaternion NormalizeSSE41(const Quaternion &q)
        {
            return NormalizeSSE2(q);
        }
#endif
    };

    // Non-member operators
    inline Quaternion operator*(float scalar, const Quaternion &q)
    {
        return q * scalar;
    }

    // Static member definition
    inline const float Quaternion::EPSILON = 1e-6f;
}

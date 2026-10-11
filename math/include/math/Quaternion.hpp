#pragma once

#include <cmath>
#include "math/CpuInfo.hpp"
#include "math/CpuInfo.hpp"
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

namespace N2Engine::Math
{
    class Vector3;

    class Quaternion
    {
    private:
        // SIMD-optimized storage
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4201)
#endif
        union alignas(16)
        {
            struct
            {
                float w, x, y, z;
            };
            N2Engine::Math::SimdStorage simd_data;
        };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

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
        static const Quaternion Identity;

        static Quaternion FromAxisAngle(const Vector3 &axis, float angle)
        {
            return Quaternion(axis, angle);
        }

        static Quaternion FromEulerAngles(float pitch, float yaw, float roll)
        {
            return Quaternion(pitch, yaw, roll);
        }

        static Quaternion FromEulerAngles(const Vector3 &eulerAngles)
        {
            return FromEulerAngles(eulerAngles.x, eulerAngles.y, eulerAngles.z);
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
#ifdef N2_MATH_X86
            result.simd_data = _mm_xor_ps(simd_data, _mm_set_ps(-0.0f, -0.0f, -0.0f, 0.0f));
#else
            result.w = w;
            result.x = -x;
            result.y = -y;
            result.z = -z;
#endif
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

        // Selects the implementation tier the operations dispatch to; see Math::SetSIMDLevel
        static void SetSIMDLevel(SIMDLevel level);

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
                return Identity;
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

#ifdef N2_MATH_X86
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
            // A real sqrt and divide: _mm_rsqrt_ps is only accurate to about 12 bits
            const float length = LengthSSE2(q);
            if (length < EPSILON)
            {
                return Identity;
            }

            Quaternion result;
            result.simd_data = _mm_div_ps(q.simd_data, _mm_set1_ps(length));
            return result;
        }

        static Quaternion MulSSE2(const Quaternion &a, const Quaternion &b)
        {
            // Lanes are [w, x, y, z]. Each column of MulScalar's formula is one component of a (broadcast)
            // times a permutation of b with per-lane signs:
            //   a.w * [ bw,  bx,  by,  bz]
            //   a.x * [-bx,  bw, -bz,  by]
            //   a.y * [-by,  bz,  bw, -bx]
            //   a.z * [-bz, -by,  bx,  bw]
            // _MM_SHUFFLE and _mm_set_ps list lanes from 3 down to 0
            const __m128 a_vec = a.simd_data;
            const __m128 b_vec = b.simd_data;

            const __m128 a_w = _mm_shuffle_ps(a_vec, a_vec, _MM_SHUFFLE(0, 0, 0, 0));
            const __m128 a_x = _mm_shuffle_ps(a_vec, a_vec, _MM_SHUFFLE(1, 1, 1, 1));
            const __m128 a_y = _mm_shuffle_ps(a_vec, a_vec, _MM_SHUFFLE(2, 2, 2, 2));
            const __m128 a_z = _mm_shuffle_ps(a_vec, a_vec, _MM_SHUFFLE(3, 3, 3, 3));

            const __m128 b_xwzy = _mm_shuffle_ps(b_vec, b_vec, _MM_SHUFFLE(2, 3, 0, 1)); // [x, w, z, y]
            const __m128 b_yzwx = _mm_shuffle_ps(b_vec, b_vec, _MM_SHUFFLE(1, 0, 3, 2)); // [y, z, w, x]
            const __m128 b_zyxw = _mm_shuffle_ps(b_vec, b_vec, _MM_SHUFFLE(0, 1, 2, 3)); // [z, y, x, w]

            const __m128 signs_x = _mm_set_ps(1.0f, -1.0f, 1.0f, -1.0f);  // [-, +, -, +]
            const __m128 signs_y = _mm_set_ps(-1.0f, 1.0f, 1.0f, -1.0f);  // [-, +, +, -]
            const __m128 signs_z = _mm_set_ps(1.0f, 1.0f, -1.0f, -1.0f);  // [-, -, +, +]

            const __m128 term_w = _mm_mul_ps(a_w, b_vec);
            const __m128 term_x = _mm_mul_ps(_mm_mul_ps(a_x, b_xwzy), signs_x);
            const __m128 term_y = _mm_mul_ps(_mm_mul_ps(a_y, b_yzwx), signs_y);
            const __m128 term_z = _mm_mul_ps(_mm_mul_ps(a_z, b_zyxw), signs_z);

            // Summed left to right, like MulScalar
            Quaternion result;
            result.simd_data = _mm_add_ps(_mm_add_ps(_mm_add_ps(term_w, term_x), term_y), term_z);
            return result;
        }
#else
        // No SSE on this architecture: the scalar versions (the dispatch never picks the SSE tiers here)
        static Quaternion AddSSE2(const Quaternion &a, const Quaternion &b) { return AddScalar(a, b); }
        static Quaternion SubSSE2(const Quaternion &a, const Quaternion &b) { return SubScalar(a, b); }
        static Quaternion ScalarMulSSE2(const Quaternion &q, float scalar) { return ScalarMulScalar(q, scalar); }
        static float DotSSE2(const Quaternion &a, const Quaternion &b) { return DotScalar(a, b); }
        static float LengthSSE2(const Quaternion &q) { return LengthScalar(q); }
        static Quaternion NormalizeSSE2(const Quaternion &q) { return NormalizeScalar(q); }
        static Quaternion MulSSE2(const Quaternion &a, const Quaternion &b) { return MulScalar(a, b); }
#endif

        // ===== SSE4.1 IMPLEMENTATIONS =====
#ifdef N2_MATH_SSE41
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
            // Mask 0xFF: dot of all four lanes broadcast to all lanes. A real sqrt and divide, since
            // _mm_rsqrt_ps is only accurate to about 12 bits
            const __m128 length = _mm_sqrt_ps(_mm_dp_ps(q.simd_data, q.simd_data, 0xFF));
            if (_mm_cvtss_f32(length) < EPSILON)
            {
                return Identity;
            }

            Quaternion result;
            result.simd_data = _mm_div_ps(q.simd_data, length);
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

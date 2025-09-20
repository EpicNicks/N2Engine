#pragma once

#include <cmath>
#include <immintrin.h>
#include <numbers>
#include <functional>
#include "math/VectorN.hpp" // For VectorN compatibility

#ifdef _WIN32
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif

namespace N2Engine
{
    namespace Math
    {
        class Vector3
        {
        public:
            // SIMD-optimized storage - padded to 4 floats for alignment
            union alignas(16)
            {
                struct
                {
                    float x, y, z, w; // w is padding for SIMD
                };
                __m128 simd_data;
            };

        private:
            // Function pointer types for SIMD dispatch
            using AddFunc = Vector3 (*)(const Vector3 &, const Vector3 &);
            using SubFunc = Vector3 (*)(const Vector3 &, const Vector3 &);
            using ScalarMulFunc = Vector3 (*)(const Vector3 &, float);
            using ScalarDivFunc = Vector3 (*)(const Vector3 &, float);
            using DotFunc = float (*)(const Vector3 &, const Vector3 &);
            using CrossFunc = Vector3 (*)(const Vector3 &, const Vector3 &);
            using LengthFunc = float (*)(const Vector3 &);
            using NormalizeFunc = Vector3 (*)(const Vector3 &);
            using DistanceFunc = float (*)(const Vector3 &, const Vector3 &);

        public:
            static const float EPSILON;

            // Constructors
            Vector3() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
            Vector3(float x, float y, float z) : x(x), y(y), z(z), w(0.0f) {}
            Vector3(float scalar) : x(scalar), y(scalar), z(scalar), w(0.0f) {}

            // Copy constructor and assignment
            Vector3(const Vector3 &other) : simd_data(other.simd_data) {}
            Vector3 &operator=(const Vector3 &other)
            {
                if (this != &other)
                {
                    simd_data = other.simd_data;
                }
                return *this;
            }

            // Conversion constructors and operators for VectorN compatibility
            Vector3(const VectorN<float, 3> &vectorN) : x(vectorN[0]), y(vectorN[1]), z(vectorN[2]), w(0.0f) {}

            Vector3 &operator=(const VectorN<float, 3> &vectorN)
            {
                x = vectorN[0];
                y = vectorN[1];
                z = vectorN[2];
                w = 0.0f;
                return *this;
            }

            // Implicit conversion to VectorN<float, 3>
            operator VectorN<float, 3>() const
            {
                return VectorN<float, 3>{x, y, z};
            }

            // Explicit conversion method (alternative to implicit)
            VectorN<float, 3> ToVectorN() const
            {
                return VectorN<float, 3>{x, y, z};
            }

            // Static factory methods
            static Vector3 Zero() { return Vector3(0.0f, 0.0f, 0.0f); }
            static Vector3 One() { return Vector3(1.0f, 1.0f, 1.0f); }
            static Vector3 Up() { return Vector3(0.0f, 1.0f, 0.0f); }
            static Vector3 Down() { return Vector3(0.0f, -1.0f, 0.0f); }
            static Vector3 Left() { return Vector3(-1.0f, 0.0f, 0.0f); }
            static Vector3 Right() { return Vector3(1.0f, 0.0f, 0.0f); }
            static Vector3 Forward() { return Vector3(0.0f, 0.0f, 1.0f); }
            static Vector3 Back() { return Vector3(0.0f, 0.0f, -1.0f); }

            // SIMD-optimized basic operations
            Vector3 operator+(const Vector3 &other) const
            {
                return add_func(*this, other);
            }

            Vector3 operator-(const Vector3 &other) const
            {
                return sub_func(*this, other);
            }

            Vector3 operator-() const
            {
                return scalar_mul_func(*this, -1.0f);
            }

            Vector3 operator*(float scalar) const
            {
                return scalar_mul_func(*this, scalar);
            }

            Vector3 operator/(float scalar) const
            {
                return scalar_div_func(*this, scalar);
            }

            Vector3 &operator+=(const Vector3 &other)
            {
                *this = add_func(*this, other);
                return *this;
            }

            Vector3 &operator-=(const Vector3 &other)
            {
                *this = sub_func(*this, other);
                return *this;
            }

            Vector3 &operator*=(float scalar)
            {
                *this = scalar_mul_func(*this, scalar);
                return *this;
            }

            Vector3 &operator/=(float scalar)
            {
                *this = scalar_div_func(*this, scalar);
                return *this;
            }

            bool operator==(const Vector3 &other) const
            {
                return (std::abs(x - other.x) < EPSILON &&
                        std::abs(y - other.y) < EPSILON &&
                        std::abs(z - other.z) < EPSILON);
            }

            bool operator!=(const Vector3 &other) const
            {
                return !(*this == other);
            }

            // Array access
            float &operator[](size_t index)
            {
                return (&x)[index];
            }

            const float &operator[](size_t index) const
            {
                return (&x)[index];
            }

            // SIMD-optimized vector operations
            float Dot(const Vector3 &other) const
            {
                return dot_func(*this, other);
            }

            Vector3 Cross(const Vector3 &other) const
            {
                return cross_func(*this, other);
            }

            float Length() const
            {
                return length_func(*this);
            }

            float LengthSquared() const
            {
                return dot_func(*this, *this);
            }

            Vector3 Normalized() const
            {
                return normalize_func(*this);
            }

            Vector3 &Normalize()
            {
                *this = normalize_func(*this);
                return *this;
            }

            float Distance(const Vector3 &other) const
            {
                return distance_func(*this, other);
            }

            float DistanceSquared(const Vector3 &other) const
            {
                Vector3 diff = sub_func(*this, other);
                return dot_func(diff, diff);
            }

            // Vector3-specific operations (using SIMD where beneficial)
            float AngleTo(const Vector3 &other) const
            {
                float dot = Dot(other);
                float magProduct = Length() * other.Length();
                if (magProduct == 0.0f)
                    return 0.0f;
                return std::acos(std::clamp(dot / magProduct, -1.0f, 1.0f));
            }

            Vector3 Project(const Vector3 &onto) const
            {
                float dot = Dot(onto);
                float ontoMagSq = onto.Dot(onto);
                if (ontoMagSq == 0.0f)
                    return Zero();
                return scalar_mul_func(onto, dot / ontoMagSq);
            }

            Vector3 Reject(const Vector3 &onto) const
            {
                return sub_func(*this, Project(onto));
            }

            Vector3 Reflect(const Vector3 &normal) const
            {
                float dot = Dot(normal);
                Vector3 reflection = scalar_mul_func(normal, 2.0f * dot);
                return sub_func(*this, reflection);
            }

            Vector3 Scale(const Vector3 &other) const
            {
                Vector3 result;
                result.simd_data = _mm_mul_ps(simd_data, other.simd_data);
                result.w = 0.0f; // Ensure padding remains 0
                return result;
            }

            static Vector3 Lerp(const Vector3 &a, const Vector3 &b, float t)
            {
                Vector3 diff = sub_func(b, a);
                Vector3 scaled = scalar_mul_func(diff, t);
                return add_func(a, scaled);
            }

            Vector3 Lerp(const Vector3 &other, float t) const
            {
                return Lerp(*this, other, t);
            }

            // Utility functions
            bool IsZero(float tolerance = EPSILON) const
            {
                return Length() < tolerance;
            }

            bool IsNormalized(float tolerance = 1e-6f) const
            {
                float lengthSq = LengthSquared();
                return std::abs(lengthSq - 1.0f) < tolerance;
            }

            bool IsParallel(const Vector3 &other, float tolerance = 1e-6f) const
            {
                Vector3 cross = Cross(other);
                return cross.Length() < tolerance;
            }

            bool IsPerpendicular(const Vector3 &other, float tolerance = 1e-6f) const
            {
                return std::abs(Dot(other)) < tolerance;
            }

            Vector3 GetOrthogonal() const
            {
                if (std::abs(x) < 0.9f)
                {
                    return Vector3(1.0f, 0.0f, 0.0f).Cross(*this).Normalized();
                }
                else
                {
                    return Vector3(0.0f, 1.0f, 0.0f).Cross(*this).Normalized();
                }
            }

            // SIMD initialization - call once at startup
            static void InitializeSIMD();

            // AVX-optimized batch operations for arrays of Vector3s
            static void AddBatch(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count);
            static void SubBatch(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count);
            static void ScalarMulBatch(const Vector3 *input, Vector3 *output, float scalar, size_t count);
            static void DotBatch(const Vector3 *a, const Vector3 *b, float *result, size_t count);
            static void NormalizeBatch(Vector3 *vectors, size_t count);
            static void CrossBatch(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count);

            // Transform arrays of Vector3s (useful for graphics pipelines)
            static void TransformBatch(const Vector3 *input, Vector3 *output, size_t count,
                                       const std::function<Vector3(const Vector3 &)> &transform);

        private:
            // ===== SCALAR IMPLEMENTATIONS =====
            static Vector3 AddScalar(const Vector3 &a, const Vector3 &b)
            {
                return Vector3(a.x + b.x, a.y + b.y, a.z + b.z);
            }

            static Vector3 SubScalar(const Vector3 &a, const Vector3 &b)
            {
                return Vector3(a.x - b.x, a.y - b.y, a.z - b.z);
            }

            static Vector3 ScalarMulScalar(const Vector3 &v, float scalar)
            {
                return Vector3(v.x * scalar, v.y * scalar, v.z * scalar);
            }

            static Vector3 ScalarDivScalar(const Vector3 &v, float scalar)
            {
                if (std::abs(scalar) < EPSILON)
                {
                    return Zero();
                }
                float inv_scalar = 1.0f / scalar;
                return Vector3(v.x * inv_scalar, v.y * inv_scalar, v.z * inv_scalar);
            }

            static float DotScalar(const Vector3 &a, const Vector3 &b)
            {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            }

            static Vector3 CrossScalar(const Vector3 &a, const Vector3 &b)
            {
                return Vector3(
                    a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x);
            }

            static float LengthScalar(const Vector3 &v)
            {
                return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            }

            static Vector3 NormalizeScalar(const Vector3 &v)
            {
                float length = LengthScalar(v);
                if (length < EPSILON)
                {
                    return Zero();
                }
                float inv_length = 1.0f / length;
                return Vector3(v.x * inv_length, v.y * inv_length, v.z * inv_length);
            }

            static float DistanceScalar(const Vector3 &a, const Vector3 &b)
            {
                Vector3 diff = SubScalar(a, b);
                return LengthScalar(diff);
            }

            // Static function pointers - initialized to safe defaults
            inline static AddFunc add_func = &AddScalar;
            inline static SubFunc sub_func = &SubScalar;
            inline static ScalarMulFunc scalar_mul_func = &ScalarMulScalar;
            inline static ScalarDivFunc scalar_div_func = &ScalarDivScalar;
            inline static DotFunc dot_func = &DotScalar;
            inline static CrossFunc cross_func = &CrossScalar;
            inline static LengthFunc length_func = &LengthScalar;
            inline static NormalizeFunc normalize_func = &NormalizeScalar;
            inline static DistanceFunc distance_func = &DistanceScalar;
            inline static bool initialized = false;

            // ===== SSE2 IMPLEMENTATIONS =====
            static Vector3 AddSSE2(const Vector3 &a, const Vector3 &b)
            {
                Vector3 result;
                result.simd_data = _mm_add_ps(a.simd_data, b.simd_data);
                result.w = 0.0f; // Ensure padding remains 0
                return result;
            }

            static Vector3 SubSSE2(const Vector3 &a, const Vector3 &b)
            {
                Vector3 result;
                result.simd_data = _mm_sub_ps(a.simd_data, b.simd_data);
                result.w = 0.0f; // Ensure padding remains 0
                return result;
            }

            static Vector3 ScalarMulSSE2(const Vector3 &v, float scalar)
            {
                Vector3 result;
                __m128 scalar_vec = _mm_set1_ps(scalar);
                result.simd_data = _mm_mul_ps(v.simd_data, scalar_vec);
                result.w = 0.0f; // Ensure padding remains 0
                return result;
            }

            static Vector3 ScalarDivSSE2(const Vector3 &v, float scalar)
            {
                if (std::abs(scalar) < EPSILON)
                {
                    return Zero();
                }
                Vector3 result;
                __m128 scalar_vec = _mm_set1_ps(scalar);
                result.simd_data = _mm_div_ps(v.simd_data, scalar_vec);
                result.w = 0.0f; // Ensure padding remains 0
                return result;
            }

            static float DotSSE2(const Vector3 &a, const Vector3 &b)
            {
                __m128 mul = _mm_mul_ps(a.simd_data, b.simd_data);
                // Only sum first 3 components (x, y, z)
                __m128 shuf = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(1, 0, 3, 2));
                __m128 sums = _mm_add_ps(mul, shuf);
                shuf = _mm_movehl_ps(shuf, sums);
                sums = _mm_add_ss(sums, shuf);
                return _mm_cvtss_f32(sums);
            }

            static Vector3 CrossSSE2(const Vector3 &a, const Vector3 &b)
            {
                Vector3 result;
                // Cross product: a × b = (a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x)

                // Create shuffled vectors for cross product
                __m128 a_yzx = _mm_shuffle_ps(a.simd_data, a.simd_data, _MM_SHUFFLE(3, 0, 2, 1)); // [y, z, x, w]
                __m128 b_zxy = _mm_shuffle_ps(b.simd_data, b.simd_data, _MM_SHUFFLE(3, 1, 0, 2)); // [z, x, y, w]
                __m128 a_zxy = _mm_shuffle_ps(a.simd_data, a.simd_data, _MM_SHUFFLE(3, 1, 0, 2)); // [z, x, y, w]
                __m128 b_yzx = _mm_shuffle_ps(b.simd_data, b.simd_data, _MM_SHUFFLE(3, 0, 2, 1)); // [y, z, x, w]

                __m128 mul1 = _mm_mul_ps(a_yzx, b_zxy); // [y*z, z*x, x*y, w*w]
                __m128 mul2 = _mm_mul_ps(a_zxy, b_yzx); // [z*y, x*z, y*x, w*w]

                result.simd_data = _mm_sub_ps(mul1, mul2);
                result.w = 0.0f; // Ensure padding remains 0
                return result;
            }

            static float LengthSSE2(const Vector3 &v)
            {
                float dot = DotSSE2(v, v);
                return std::sqrt(dot);
            }

            static Vector3 NormalizeSSE2(const Vector3 &v)
            {
                float length_sq = DotSSE2(v, v);
                if (length_sq < EPSILON * EPSILON)
                {
                    return Zero();
                }

                Vector3 result;
                __m128 length_sq_vec = _mm_set1_ps(length_sq);
                __m128 inv_length = _mm_rsqrt_ps(length_sq_vec); // Fast reciprocal sqrt
                result.simd_data = _mm_mul_ps(v.simd_data, inv_length);
                result.w = 0.0f; // Ensure padding remains 0
                return result;
            }

            static float DistanceSSE2(const Vector3 &a, const Vector3 &b)
            {
                Vector3 diff = SubSSE2(a, b);
                return LengthSSE2(diff);
            }

// ===== SSE4.1 IMPLEMENTATIONS =====
#ifdef __SSE4_1__
            __attribute__((target("sse4.1"))) static float DotSSE41(const Vector3 &a, const Vector3 &b)
            {
                // Use dot product instruction, mask to only use first 3 components
                __m128 result = _mm_dp_ps(a.simd_data, b.simd_data, 0x71); // 0111 0001
                return _mm_cvtss_f32(result);
            }

            __attribute__((target("sse4.1"))) static float LengthSSE41(const Vector3 &v)
            {
                __m128 dot = _mm_dp_ps(v.simd_data, v.simd_data, 0x71);
                __m128 length = _mm_sqrt_ss(dot);
                return _mm_cvtss_f32(length);
            }

            __attribute__((target("sse4.1"))) static Vector3 NormalizeSSE41(const Vector3 &v)
            {
                __m128 length_sq = _mm_dp_ps(v.simd_data, v.simd_data, 0x7F);

                // Check for near-zero length
                __m128 epsilon = _mm_set1_ps(EPSILON * EPSILON);
                __m128 mask = _mm_cmplt_ps(length_sq, epsilon);

                if (_mm_movemask_ps(mask) != 0)
                {
                    return Zero();
                }

                Vector3 result;
                __m128 inv_length = _mm_rsqrt_ps(length_sq);
                result.simd_data = _mm_mul_ps(v.simd_data, inv_length);
                result.w = 0.0f; // Ensure padding remains 0
                return result;
            }

            __attribute__((target("sse4.1"))) static float DistanceSSE41(const Vector3 &a, const Vector3 &b)
            {
                Vector3 diff = SubSSE2(a, b); // Can reuse SSE2 sub
                return LengthSSE41(diff);
            }
#else
            // Fallback to SSE2 versions when SSE4.1 not available
            static float DotSSE41(const Vector3 &a, const Vector3 &b)
            {
                return DotSSE2(a, b);
            }
            static float LengthSSE41(const Vector3 &v)
            {
                return LengthSSE2(v);
            }
            static Vector3 NormalizeSSE41(const Vector3 &v)
            {
                return NormalizeSSE2(v);
            }
            static float DistanceSSE41(const Vector3 &a, const Vector3 &b)
            {
                return DistanceSSE2(a, b);
            }
#endif

// ===== AVX IMPLEMENTATIONS =====
#ifdef __AVX__
            // Single Vector3 AVX operations (may not always be faster due to overhead)
            __attribute__((target("avx"))) static Vector3 AddAVX(const Vector3 &a, const Vector3 &b)
            {
                Vector3 result;
                __m256 a_extended = _mm256_castps128_ps256(a.simd_data);
                __m256 b_extended = _mm256_castps128_ps256(b.simd_data);
                __m256 sum = _mm256_add_ps(a_extended, b_extended);
                result.simd_data = _mm256_castps256_ps128(sum);
                result.w = 0.0f;
                return result;
            }

            __attribute__((target("avx"))) static Vector3 SubAVX(const Vector3 &a, const Vector3 &b)
            {
                Vector3 result;
                __m256 a_extended = _mm256_castps128_ps256(a.simd_data);
                __m256 b_extended = _mm256_castps128_ps256(b.simd_data);
                __m256 diff = _mm256_sub_ps(a_extended, b_extended);
                result.simd_data = _mm256_castps256_ps128(diff);
                result.w = 0.0f;
                return result;
            }

            __attribute__((target("avx"))) static Vector3 ScalarMulAVX(const Vector3 &v, float scalar)
            {
                Vector3 result;
                __m256 v_extended = _mm256_castps128_ps256(v.simd_data);
                __m256 scalar_vec = _mm256_set1_ps(scalar);
                __m256 product = _mm256_mul_ps(v_extended, scalar_vec);
                result.simd_data = _mm256_castps256_ps128(product);
                result.w = 0.0f;
                return result;
            }

            // AVX shines for batch operations - process 2 Vector3s at once
            __attribute__((target("avx"))) static void AddBatchAVX(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count)
            {
                size_t i = 0;

                // Process pairs of Vector3s (2 at a time with AVX)
                for (; i + 1 < count; i += 2)
                {
                    // Load 2 Vector3s into one AVX register
                    // [a[i].x, a[i].y, a[i].z, a[i].w, a[i+1].x, a[i+1].y, a[i+1].z, a[i+1].w]
                    __m256 a_batch = _mm256_set_ps(
                        a[i + 1].w, a[i + 1].z, a[i + 1].y, a[i + 1].x,
                        a[i].w, a[i].z, a[i].y, a[i].x);

                    __m256 b_batch = _mm256_set_ps(
                        b[i + 1].w, b[i + 1].z, b[i + 1].y, b[i + 1].x,
                        b[i].w, b[i].z, b[i].y, b[i].x);

                    __m256 sum_batch = _mm256_add_ps(a_batch, b_batch);

                    // Extract results
                    alignas(32) float temp[8];
                    _mm256_store_ps(temp, sum_batch);

                    result[i] = Vector3(temp[0], temp[1], temp[2]);
                    result[i + 1] = Vector3(temp[4], temp[5], temp[6]);
                }

                // Handle remaining Vector3s with SSE
                for (; i < count; ++i)
                {
                    result[i] = AddSSE2(a[i], b[i]);
                }
            }

            __attribute__((target("avx"))) static void DotBatchAVX(const Vector3 *a, const Vector3 *b, float *result, size_t count)
            {
                size_t i = 0;

                // Process pairs of Vector3s
                for (; i + 1 < count; i += 2)
                {
                    __m256 a_batch = _mm256_set_ps(
                        0.0f, a[i + 1].z, a[i + 1].y, a[i + 1].x,
                        0.0f, a[i].z, a[i].y, a[i].x);

                    __m256 b_batch = _mm256_set_ps(
                        0.0f, b[i + 1].z, b[i + 1].y, b[i + 1].x,
                        0.0f, b[i].z, b[i].y, b[i].x);

                    __m256 mul_batch = _mm256_mul_ps(a_batch, b_batch);

                    // Horizontal add within each 128-bit lane
                    __m256 shuf = _mm256_shuffle_ps(mul_batch, mul_batch, _MM_SHUFFLE(2, 3, 0, 1));
                    __m256 sums = _mm256_add_ps(mul_batch, shuf);
                    shuf = _mm256_shuffle_ps(sums, sums, _MM_SHUFFLE(1, 0, 3, 2));
                    sums = _mm256_add_ps(sums, shuf);

                    // Extract the dot products
                    alignas(32) float temp[8];
                    _mm256_store_ps(temp, sums);

                    result[i] = temp[0];
                    result[i + 1] = temp[4];
                }

                // Handle remaining
                for (; i < count; ++i)
                {
                    result[i] = DotSSE2(a[i], b[i]);
                }
            }

            __attribute__((target("avx"))) static void NormalizeBatchAVX(Vector3 *vectors, size_t count)
            {
                size_t i = 0;

                for (; i + 1 < count; i += 2)
                {
                    __m256 v_batch = _mm256_set_ps(
                        0.0f, vectors[i + 1].z, vectors[i + 1].y, vectors[i + 1].x,
                        0.0f, vectors[i].z, vectors[i].y, vectors[i].x);

                    // Compute squared lengths
                    __m256 squared = _mm256_mul_ps(v_batch, v_batch);
                    __m256 shuf = _mm256_shuffle_ps(squared, squared, _MM_SHUFFLE(2, 3, 0, 1));
                    __m256 sums = _mm256_add_ps(squared, shuf);
                    shuf = _mm256_shuffle_ps(sums, sums, _MM_SHUFFLE(1, 0, 3, 2));
                    sums = _mm256_add_ps(sums, shuf);

                    // Reciprocal square root
                    __m256 rsqrt = _mm256_rsqrt_ps(sums);

                    // Broadcast rsqrt to all components
                    __m256 rsqrt_broadcast = _mm256_shuffle_ps(rsqrt, rsqrt, _MM_SHUFFLE(0, 0, 0, 0));
                    rsqrt_broadcast = _mm256_permute2f128_ps(rsqrt_broadcast, rsqrt_broadcast, 0x00);

                    __m256 normalized = _mm256_mul_ps(v_batch, rsqrt_broadcast);

                    alignas(32) float temp[8];
                    _mm256_store_ps(temp, normalized);

                    vectors[i] = Vector3(temp[0], temp[1], temp[2]);
                    vectors[i + 1] = Vector3(temp[4], temp[5], temp[6]);
                }

                for (; i < count; ++i)
                {
                    vectors[i] = NormalizeSSE2(vectors[i]);
                }
            }

            // More sophisticated AVX: Process 8 Vector3s using 3 AVX registers
            __attribute__((target("avx"))) static void ProcessVector3ArrayAVX(const Vector3 *input, Vector3 *output, size_t count, float scalar)
            {
                size_t i = 0;
                const size_t avx_batch_size = 8;

                for (; i + avx_batch_size <= count; i += avx_batch_size)
                {
                    // Load 8 Vector3s into 3 AVX registers (Structure of Arrays approach)
                    __m256 x_vals = _mm256_set_ps(
                        input[i + 7].x, input[i + 6].x, input[i + 5].x, input[i + 4].x,
                        input[i + 3].x, input[i + 2].x, input[i + 1].x, input[i].x);

                    __m256 y_vals = _mm256_set_ps(
                        input[i + 7].y, input[i + 6].y, input[i + 5].y, input[i + 4].y,
                        input[i + 3].y, input[i + 2].y, input[i + 1].y, input[i].y);

                    __m256 z_vals = _mm256_set_ps(
                        input[i + 7].z, input[i + 6].z, input[i + 5].z, input[i + 4].z,
                        input[i + 3].z, input[i + 2].z, input[i + 1].z, input[i].z);

                    // Perform operations on all 8 vectors simultaneously
                    __m256 scalar_vec = _mm256_set1_ps(scalar);
                    x_vals = _mm256_mul_ps(x_vals, scalar_vec);
                    y_vals = _mm256_mul_ps(y_vals, scalar_vec);
                    z_vals = _mm256_mul_ps(z_vals, scalar_vec);

                    // Store results back
                    alignas(32) float x_temp[8], y_temp[8], z_temp[8];
                    _mm256_store_ps(x_temp, x_vals);
                    _mm256_store_ps(y_temp, y_vals);
                    _mm256_store_ps(z_temp, z_vals);

                    for (size_t j = 0; j < avx_batch_size; ++j)
                    {
                        output[i + j] = Vector3(x_temp[j], y_temp[j], z_temp[j]);
                    }
                }

                // Handle remaining vectors
                for (; i < count; ++i)
                {
                    output[i] = input[i] * scalar;
                }
            }
#else
            // Fallback implementations when AVX not available
            static void AddBatchAVX(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count)
            {
                for (size_t i = 0; i < count; ++i)
                    result[i] = AddSSE2(a[i], b[i]);
            }

            static void DotBatchAVX(const Vector3 *a, const Vector3 *b, float *result, size_t count)
            {
                for (size_t i = 0; i < count; ++i)
                    result[i] = DotSSE2(a[i], b[i]);
            }

            static void NormalizeBatchAVX(Vector3 *vectors, size_t count)
            {
                for (size_t i = 0; i < count; ++i)
                    vectors[i] = NormalizeSSE2(vectors[i]);
            }

            static void ProcessVector3ArrayAVX(const Vector3 *input, Vector3 *output, size_t count, float scalar)
            {
                for (size_t i = 0; i < count; ++i)
                    output[i] = ScalarMulSSE2(input[i], scalar);
            }
#endif
        };

        // Non-member operators
        inline Vector3 operator*(float scalar, const Vector3 &v)
        {
            return v * scalar;
        }

        // Static member definition
        inline const float Vector3::EPSILON = 1e-6f;

        // Type alias for compatibility
        using Vector3f = Vector3;
    }
}
#include "math/Vector4.hpp"

#ifdef __AVX__
#include <immintrin.h>
#endif

namespace N2Engine::Math
{
    const Vector4 Vector4::Zero{0.f, 0.f, 0.f, 0.f};
    const Vector4 Vector4::One{1.f, 1.f, 1.f, 1.f};
    const Vector4 Vector4::UnitW{1.f, 0.f, 0.f, 0.f};
    const Vector4 Vector4::UnitX{0.f, 1.f, 0.f, 0.f};
    const Vector4 Vector4::UnitY{0.f, 0.f, 1.f, 0.f};
    const Vector4 Vector4::UnitZ{0.f, 0.f, 0.f, 1.f};

    void Vector4::SetSIMDLevel(const SIMDLevel requested)
    {
        const SIMDLevel level = ClampSIMDLevel(requested);
        simd_level = level;

        // Start from scalar so each tier below only overrides what it accelerates, and switching back down works
        add_func = &AddScalar;
        sub_func = &SubScalar;
        scalar_mul_func = &ScalarMulScalar;
        scalar_div_func = &ScalarDivScalar;
        scale_func = &ScaleScalar;
        dot_func = &DotScalar;
        length_func = &LengthScalar;
        normalize_func = &NormalizeScalar;
        min_func = &MinScalar;
        max_func = &MaxScalar;
        floor_func = &FloorScalar;
        ceil_func = &CeilScalar;
        round_func = &RoundScalar;
        abs_func = &AbsScalar;

        if (level == SIMDLevel::Scalar)
            return;

        // Floor/Ceil/Round have no SSE2 versions (they need SSE4.1's round instruction)
        add_func = &AddSSE2;
        sub_func = &SubSSE2;
        scalar_mul_func = &ScalarMulSSE2;
        scalar_div_func = &ScalarDivSSE2;
        scale_func = &ScaleSSE2;
        dot_func = &DotSSE2;
        length_func = &LengthSSE2;
        normalize_func = &NormalizeSSE2;
        min_func = &MinSSE2;
        max_func = &MaxSSE2;
        abs_func = &AbsSSE2;

        if (level == SIMDLevel::SSE2)
            return;

        // SSE4.1 and AVX: single operations stay 128-bit (AVX only pays off in the batch operations)
        dot_func = &DotSSE41;
        length_func = &LengthSSE41;
        normalize_func = &NormalizeSSE41;
        floor_func = &FloorSSE41;
        ceil_func = &CeilSSE41;
        round_func = &RoundSSE41;
    }

    // ===== RAW POINTER BATCH OPERATIONS (CORE IMPLEMENTATIONS) =====
    // Each processes two Vector4s per 256-bit register only at the AVX tier, and otherwise loops over the
    // dispatched single-vector operations. The AVX versions load two adjacent Vector4s as eight floats
    static_assert(sizeof(Vector4) == 4 * sizeof(float));

    void Vector4::AddBatch(const Vector4 *a, const Vector4 *b, Vector4 *result, size_t count)
    {
        size_t i = 0;
#ifdef __AVX__
        if (simd_level == SIMDLevel::AVX)
        {
            for (; i + 1 < count; i += 2)
            {
                __m256 a_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&a[i]));
                __m256 b_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&b[i]));

                __m256 sum = _mm256_add_ps(a_vec, b_vec);

                _mm256_storeu_ps(reinterpret_cast<float*>(&result[i]), sum);
            }
        }
#endif

        for (; i < count; ++i)
        {
            result[i] = a[i] + b[i];
        }
    }

    void Vector4::SubBatch(const Vector4 *a, const Vector4 *b, Vector4 *result, size_t count)
    {
        size_t i = 0;
#ifdef __AVX__
        if (simd_level == SIMDLevel::AVX)
        {
            for (; i + 1 < count; i += 2)
            {
                __m256 a_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&a[i]));
                __m256 b_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&b[i]));

                __m256 diff = _mm256_sub_ps(a_vec, b_vec);

                _mm256_storeu_ps(reinterpret_cast<float*>(&result[i]), diff);
            }
        }
#endif

        for (; i < count; ++i)
        {
            result[i] = a[i] - b[i];
        }
    }

    void Vector4::ScalarMulBatch(const Vector4 *input, Vector4 *output, float scalar, size_t count)
    {
        size_t i = 0;
#ifdef __AVX__
        if (simd_level == SIMDLevel::AVX)
        {
            __m256 scalar_vec = _mm256_set1_ps(scalar);

            for (; i + 1 < count; i += 2)
            {
                __m256 v = _mm256_loadu_ps(reinterpret_cast<const float*>(&input[i]));
                __m256 product = _mm256_mul_ps(v, scalar_vec);
                _mm256_storeu_ps(reinterpret_cast<float*>(&output[i]), product);
            }
        }
#endif

        for (; i < count; ++i)
        {
            output[i] = input[i] * scalar;
        }
    }

    void Vector4::DotBatch(const Vector4 *a, const Vector4 *b, float *result, size_t count)
    {
        size_t i = 0;
#ifdef __AVX__
        if (simd_level == SIMDLevel::AVX)
        {
            for (; i + 1 < count; i += 2)
            {
                __m256 a_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&a[i]));
                __m256 b_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&b[i]));

                // Two hadds leave each vector's dot product in every lane of its own 128-bit half
                __m256 mul = _mm256_mul_ps(a_vec, b_vec);
                __m256 hadd1 = _mm256_hadd_ps(mul, mul);
                __m256 hadd2 = _mm256_hadd_ps(hadd1, hadd1);

                alignas(32) float temp[8];
                _mm256_store_ps(temp, hadd2);

                result[i] = temp[0];
                result[i + 1] = temp[4];
            }
        }
#endif

        for (; i < count; ++i)
        {
            result[i] = a[i].Dot(b[i]);
        }
    }

    void Vector4::LengthBatch(const Vector4 *vectors, float *result, size_t count)
    {
        size_t i = 0;
#ifdef __AVX__
        if (simd_level == SIMDLevel::AVX)
        {
            for (; i + 1 < count; i += 2)
            {
                __m256 v = _mm256_loadu_ps(reinterpret_cast<const float*>(&vectors[i]));

                __m256 squared = _mm256_mul_ps(v, v);
                __m256 hadd1 = _mm256_hadd_ps(squared, squared);
                __m256 hadd2 = _mm256_hadd_ps(hadd1, hadd1);
                __m256 lengths = _mm256_sqrt_ps(hadd2);

                alignas(32) float temp[8];
                _mm256_store_ps(temp, lengths);

                result[i] = temp[0];
                result[i + 1] = temp[4];
            }
        }
#endif

        for (; i < count; ++i)
        {
            result[i] = vectors[i].Length();
        }
    }

    void Vector4::NormalizeBatch(Vector4 *vectors, size_t count)
    {
        size_t i = 0;
#ifdef __AVX__
        if (simd_level == SIMDLevel::AVX)
        {
            const __m256 epsilon = _mm256_set1_ps(Constants::EPSILON);

            for (; i + 1 < count; i += 2)
            {
                __m256 v = _mm256_loadu_ps(reinterpret_cast<float*>(&vectors[i]));

                // Two hadds leave each vector's squared length in every lane of its own 128-bit half
                __m256 squared = _mm256_mul_ps(v, v);
                __m256 hadd1 = _mm256_hadd_ps(squared, squared);
                __m256 length_sq = _mm256_hadd_ps(hadd1, hadd1);

                // A real sqrt and divide, since _mm256_rsqrt_ps is only accurate to about 12 bits
                __m256 lengths = _mm256_sqrt_ps(length_sq);
                __m256 normalized = _mm256_div_ps(v, lengths);

                // Like NormalizeScalar, near-zero vectors become Zero (a NaN length compares false and stays NaN)
                __m256 is_zero = _mm256_cmp_ps(lengths, epsilon, _CMP_LT_OQ);
                __m256 result_vec = _mm256_andnot_ps(is_zero, normalized);

                _mm256_storeu_ps(reinterpret_cast<float*>(&vectors[i]), result_vec);
            }
        }
#endif

        for (; i < count; ++i)
        {
            vectors[i].Normalize();
        }
    }

    void Vector4::TransformBatch(const Vector4 *input, Vector4 *output, size_t count,
                                 const std::function<Vector4(const Vector4 &)> &transform)
    {
        for (size_t i = 0; i < count; ++i)
        {
            output[i] = transform(input[i]);
        }
    }

    // ===== STD::VECTOR BATCH OPERATIONS (CONVENIENCE WRAPPERS) =====

    std::vector<Vector4> Vector4::AddBatch(const std::vector<Vector4> &a, const std::vector<Vector4> &b)
    {
        const size_t count = std::min(a.size(), b.size());
        std::vector<Vector4> result(count);
        AddBatch(a.data(), b.data(), result.data(), count);
        return result;
    }

    std::vector<Vector4> Vector4::SubBatch(const std::vector<Vector4> &a, const std::vector<Vector4> &b)
    {
        const size_t count = std::min(a.size(), b.size());
        std::vector<Vector4> result(count);
        SubBatch(a.data(), b.data(), result.data(), count);
        return result;
    }

    std::vector<Vector4> Vector4::ScalarMulBatch(const std::vector<Vector4> &input, float scalar)
    {
        std::vector<Vector4> result(input.size());
        ScalarMulBatch(input.data(), result.data(), scalar, input.size());
        return result;
    }

    std::vector<float> Vector4::DotBatch(const std::vector<Vector4> &a, const std::vector<Vector4> &b)
    {
        const size_t count = std::min(a.size(), b.size());
        std::vector<float> result(count);
        DotBatch(a.data(), b.data(), result.data(), count);
        return result;
    }

    void Vector4::NormalizeBatch(std::vector<Vector4> &vectors)
    {
        NormalizeBatch(vectors.data(), vectors.size());
    }

    std::vector<float> Vector4::LengthBatch(const std::vector<Vector4> &vectors)
    {
        std::vector<float> result(vectors.size());
        LengthBatch(vectors.data(), result.data(), vectors.size());
        return result;
    }

    std::vector<Vector4> Vector4::TransformBatch(const std::vector<Vector4> &input,
                                                 const std::function<Vector4(const Vector4 &)> &transform)
    {
        std::vector<Vector4> result(input.size());
        TransformBatch(input.data(), result.data(), input.size(), transform);
        return result;
    }

    // ===== STD::SPAN BATCH OPERATIONS (MODERN C++, ZERO OVERHEAD) =====

    void Vector4::AddBatch(std::span<const Vector4> a, std::span<const Vector4> b, std::span<Vector4> result)
    {
        const size_t count = std::min({a.size(), b.size(), result.size()});
        AddBatch(a.data(), b.data(), result.data(), count);
    }

    void Vector4::SubBatch(std::span<const Vector4> a, std::span<const Vector4> b, std::span<Vector4> result)
    {
        const size_t count = std::min({a.size(), b.size(), result.size()});
        SubBatch(a.data(), b.data(), result.data(), count);
    }

    void Vector4::ScalarMulBatch(std::span<const Vector4> input, std::span<Vector4> output, float scalar)
    {
        const size_t count = std::min(input.size(), output.size());
        ScalarMulBatch(input.data(), output.data(), scalar, count);
    }

    void Vector4::DotBatch(std::span<const Vector4> a, std::span<const Vector4> b, std::span<float> result)
    {
        const size_t count = std::min({a.size(), b.size(), result.size()});
        DotBatch(a.data(), b.data(), result.data(), count);
    }

    void Vector4::NormalizeBatch(std::span<Vector4> vectors)
    {
        NormalizeBatch(vectors.data(), vectors.size());
    }

    void Vector4::LengthBatch(std::span<const Vector4> vectors, std::span<float> result)
    {
        const size_t count = std::min(vectors.size(), result.size());
        LengthBatch(vectors.data(), result.data(), count);
    }
}
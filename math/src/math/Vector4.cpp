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

    // ===== RAW POINTER BATCH OPERATIONS (CORE IMPLEMENTATIONS) =====

    void Vector4::AddBatch(const Vector4 *a, const Vector4 *b, Vector4 *result, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;

        // Process 2 Vector4s at a time using AVX (8 floats)
        for (; i + 1 < count; i += 2)
        {
            __m256 a_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&a[i]));
            __m256 b_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&b[i]));

            __m256 sum = _mm256_add_ps(a_vec, b_vec);

            _mm256_storeu_ps(reinterpret_cast<float*>(&result[i]), sum);
        }

        for (; i < count; ++i)
        {
            result[i] = a[i] + b[i];
        }
#else
        for (size_t i = 0; i < count; ++i)
        {
            result[i] = a[i] + b[i];
        }
#endif
    }

    void Vector4::SubBatch(const Vector4 *a, const Vector4 *b, Vector4 *result, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;

        for (; i + 1 < count; i += 2)
        {
            __m256 a_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&a[i]));
            __m256 b_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&b[i]));

            __m256 diff = _mm256_sub_ps(a_vec, b_vec);

            _mm256_storeu_ps(reinterpret_cast<float*>(&result[i]), diff);
        }

        for (; i < count; ++i)
        {
            result[i] = a[i] - b[i];
        }
#else
        for (size_t i = 0; i < count; ++i)
        {
            result[i] = a[i] - b[i];
        }
#endif
    }

    void Vector4::ScalarMulBatch(const Vector4 *input, Vector4 *output, float scalar, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;
        __m256 scalar_vec = _mm256_set1_ps(scalar);

        for (; i + 1 < count; i += 2)
        {
            __m256 v = _mm256_loadu_ps(reinterpret_cast<const float*>(&input[i]));
            __m256 product = _mm256_mul_ps(v, scalar_vec);
            _mm256_storeu_ps(reinterpret_cast<float*>(&output[i]), product);
        }

        for (; i < count; ++i)
        {
            output[i] = input[i] * scalar;
        }
#else
        for (size_t i = 0; i < count; ++i)
        {
            output[i] = input[i] * scalar;
        }
#endif
    }

    void Vector4::DotBatch(const Vector4 *a, const Vector4 *b, float *result, size_t count)
    {
#if defined(__AVX__) && defined(__SSE4_1__)
        size_t i = 0;

        for (; i + 1 < count; i += 2)
        {
            __m256 a_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&a[i]));
            __m256 b_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&b[i]));

            __m256 mul = _mm256_mul_ps(a_vec, b_vec);
            __m256 hadd1 = _mm256_hadd_ps(mul, mul);
            __m256 hadd2 = _mm256_hadd_ps(hadd1, hadd1);

            alignas(32) float temp[8];
            _mm256_store_ps(temp, hadd2);

            result[i] = temp[0];
            result[i + 1] = temp[4];
        }

        for (; i < count; ++i)
        {
            result[i] = a[i].Dot(b[i]);
        }
#else
        for (size_t i = 0; i < count; ++i)
        {
            result[i] = a[i].Dot(b[i]);
        }
#endif
    }

    void Vector4::LengthBatch(const Vector4 *vectors, float *result, size_t count)
    {
#if defined(__AVX__) && defined(__SSE4_1__)
        size_t i = 0;

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

        for (; i < count; ++i)
        {
            result[i] = vectors[i].Length();
        }
#else
        for (size_t i = 0; i < count; ++i)
        {
            result[i] = vectors[i].Length();
        }
#endif
    }

    void Vector4::NormalizeBatch(Vector4 *vectors, size_t count)
    {
#if defined(__AVX__) && defined(__SSE4_1__)
        size_t i = 0;
        const __m256 epsilon_sq = _mm256_set1_ps(Constants::EPSILON * Constants::EPSILON);

        for (; i + 1 < count; i += 2)
        {
            __m256 v = _mm256_loadu_ps(reinterpret_cast<float*>(&vectors[i]));

            __m256 squared = _mm256_mul_ps(v, v);
            __m256 hadd1 = _mm256_hadd_ps(squared, squared);
            __m256 length_sq = _mm256_hadd_ps(hadd1, hadd1);

            __m256 is_valid = _mm256_cmp_ps(length_sq, epsilon_sq, _CMP_GT_OQ);
            __m256 inv_length = _mm256_rsqrt_ps(length_sq);
            __m256 inv_broadcast = _mm256_permute_ps(inv_length, 0);

            __m256 normalized = _mm256_mul_ps(v, inv_broadcast);
            __m256 result_vec = _mm256_and_ps(normalized, is_valid);

            _mm256_storeu_ps(reinterpret_cast<float*>(&vectors[i]), result_vec);
        }

        for (; i < count; ++i)
        {
            vectors[i].Normalize();
        }
#else
        for (size_t i = 0; i < count; ++i)
        {
            vectors[i].Normalize();
        }
#endif
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
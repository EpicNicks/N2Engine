#include "math/Vector2.hpp"

#ifdef __AVX__
#include <immintrin.h>
#endif

namespace N2Engine::Math
{
    const Vector2 Vector2::Zero{0.f, 0.f};
    const Vector2 Vector2::One{1.f, 1.f};
    const Vector2 Vector2::Up{0.f, 1.f};
    const Vector2 Vector2::Down{0.f, -1.f};
    const Vector2 Vector2::Left{-1.f, 0.f};
    const Vector2 Vector2::Right{1.f, 0.f};

    // ===== BATCH OPERATIONS =====

    void Vector2::AddBatch(const Vector2 *a, const Vector2 *b, Vector2 *result, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;

        // Process 4 Vector2s at a time using AVX (8 floats)
        for (; i + 3 < count; i += 4)
        {
            // Load 4 Vector2s (8 floats: x0,y0,x1,y1,x2,y2,x3,y3)
            __m256 a_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&a[i]));
            __m256 b_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&b[i]));

            __m256 sum = _mm256_add_ps(a_vec, b_vec);

            _mm256_storeu_ps(reinterpret_cast<float*>(&result[i]), sum);
        }

        // Handle remaining vectors with scalar operations
        for (; i < count; ++i)
        {
            result[i] = a[i] + b[i];
        }
#else
        // Fallback to scalar operations
        for (size_t i = 0; i < count; ++i)
        {
            result[i] = a[i] + b[i];
        }
#endif
    }

    void Vector2::SubBatch(const Vector2 *a, const Vector2 *b, Vector2 *result, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;

        for (; i + 3 < count; i += 4)
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

    void Vector2::ScalarMulBatch(const Vector2 *input, Vector2 *output, float scalar, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;
        __m256 scalar_vec = _mm256_set1_ps(scalar);

        for (; i + 3 < count; i += 4)
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

    void Vector2::DotBatch(const Vector2 *a, const Vector2 *b, float *result, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;

        // Process 4 dot products at once
        for (; i + 3 < count; i += 4)
        {
            // Load: a[i].x, a[i].y, a[i+1].x, a[i+1].y, a[i+2].x, a[i+2].y, a[i+3].x, a[i+3].y
            __m256 a_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&a[i]));
            __m256 b_vec = _mm256_loadu_ps(reinterpret_cast<const float*>(&b[i]));

            // Multiply: ax*bx, ay*by, ...
            __m256 mul = _mm256_mul_ps(a_vec, b_vec);

            // Horizontal add pairs: (ax*bx + ay*by), (a1x*b1x + a1y*b1y), ...
            __m256 hadd1 = _mm256_hadd_ps(mul, mul);
            // hadd1 now contains: [dot0, dot0, dot1, dot1, dot2, dot2, dot3, dot3]

            // Extract results
            // We need to extract lane 0, 2, 4, 6 from the result
            alignas(32) float temp[8];
            _mm256_store_ps(temp, hadd1);

            result[i] = temp[0];
            result[i + 1] = temp[2];
            result[i + 2] = temp[4];
            result[i + 3] = temp[6];
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

    void Vector2::LengthBatch(const Vector2 *vectors, float *result, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;

        // Process 4 lengths at once
        for (; i + 3 < count; i += 4)
        {
            __m256 v = _mm256_loadu_ps(reinterpret_cast<const float*>(&vectors[i]));

            // Square: x*x, y*y, ...
            __m256 squared = _mm256_mul_ps(v, v);

            // Horizontal add pairs: (x*x + y*y), ...
            __m256 hadd1 = _mm256_hadd_ps(squared, squared);

            // Square root
            __m256 lengths = _mm256_sqrt_ps(hadd1);

            alignas(32) float temp[8];
            _mm256_store_ps(temp, lengths);

            result[i] = temp[0];
            result[i + 1] = temp[2];
            result[i + 2] = temp[4];
            result[i + 3] = temp[6];
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

    void Vector2::NormalizeBatch(Vector2 *vectors, size_t count)
    {
#ifdef __AVX__
        size_t i = 0;
        const __m256 epsilon_sq = _mm256_set1_ps(Constants::EPSILON * Constants::EPSILON);

        // Process 4 normalizations at once
        for (; i + 3 < count; i += 4)
        {
            __m256 v = _mm256_loadu_ps(reinterpret_cast<const float*>(&vectors[i]));

            // Compute length squared
            __m256 squared = _mm256_mul_ps(v, v);
            __m256 length_sq = _mm256_hadd_ps(squared, squared);

            // Check for zero-length vectors (compare with epsilon)
            __m256 is_valid = _mm256_cmp_ps(length_sq, epsilon_sq, _CMP_GT_OQ);

            // Compute reciprocal square root (inverse length)
            __m256 inv_length = _mm256_rsqrt_ps(length_sq);

            // Broadcast inv_length to match vector layout
            // We need to duplicate each pair: [l0, l0, l1, l1, l2, l2, l3, l3]
            __m256 inv_length_broadcast = _mm256_permute_ps(inv_length, 0b10100000);
            inv_length_broadcast = _mm256_permute2f128_ps(inv_length_broadcast, inv_length_broadcast, 0b00000000);

            // Multiply vector by inverse length
            __m256 normalized = _mm256_mul_ps(v, inv_length_broadcast);

            // Blend with zero for invalid vectors
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

    void Vector2::TransformBatch(const Vector2 *input, Vector2 *output, size_t count,
                                 const std::function<Vector2(const Vector2 &)> &transform)
    {
        // Generic transform - can't easily SIMD optimize without knowing the transform
        for (size_t i = 0; i < count; ++i)
        {
            output[i] = transform(input[i]);
        }
    }
}
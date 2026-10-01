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
        // Matches Normalized() (to rounding): near-zero vectors become Zero, NaN stays NaN
#ifdef __AVX__
        size_t i = 0;
        const __m256 epsilon = _mm256_set1_ps(Constants::EPSILON);

        // Process 4 normalizations at once: x0 y0 x1 y1 | x2 y2 x3 y3
        for (; i + 3 < count; i += 4)
        {
            const __m256 v = _mm256_loadu_ps(reinterpret_cast<const float*>(&vectors[i]));

            // Each vector's squared length in both of its lanes: add every lane to its pair neighbour
            // (x0² y0² x1² y1² + y0² x0² y1² x1²), within each 128-bit half, so no cross-half broadcast
            const __m256 squared = _mm256_mul_ps(v, v);
            const __m256 swapped = _mm256_permute_ps(squared, _MM_SHUFFLE(2, 3, 0, 1));
            const __m256 length_sq = _mm256_add_ps(squared, swapped);

            // A real sqrt and divide: _mm256_rsqrt_ps is only accurate to about 12 bits
            const __m256 length = _mm256_sqrt_ps(length_sq);
            const __m256 normalized = _mm256_div_ps(v, length);

            // Ordered compare: false for NaN, so a NaN vector stays NaN (as in Normalized) instead of
            // becoming Zero; a near-zero one becomes Zero rather than NaN or a huge value
            const __m256 too_short = _mm256_cmp_ps(length, epsilon, _CMP_LT_OQ);
            const __m256 result_vec = _mm256_andnot_ps(too_short, normalized);

            _mm256_storeu_ps(reinterpret_cast<float*>(&vectors[i]), result_vec);
        }

        for (; i < count; ++i)
        {
            vectors[i] = vectors[i].Normalized();
        }
#else
        for (size_t i = 0; i < count; ++i)
        {
            vectors[i] = vectors[i].Normalized();
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
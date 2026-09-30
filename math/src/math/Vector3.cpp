#include "math/Vector3.hpp"
#include "math/CpuInfo.hpp"
#include <iostream>

using namespace N2Engine::Math;

const Vector3 Vector3::Zero{0.f, 0.f, 0.f};
const Vector3 Vector3::One{1.f, 1.f, 1.f};
const Vector3 Vector3::Up{0.f, 1.f, 0.f};
const Vector3 Vector3::Down{0.f, -1.f, 0.f};
const Vector3 Vector3::Left{-1.f, 0.f, 0.f};
const Vector3 Vector3::Right{1.f, 0.f, 0.f};
const Vector3 Vector3::Forward{0.f, 0.f, 1.f};
const Vector3 Vector3::Back{0.f, 0.f, -1.f};

void Vector3::SetSIMDLevel(const SIMDLevel level)
{
    simd_level = level;

    // Start from scalar so each tier below only overrides what it accelerates, and switching back down works
    add_func = &AddScalar;
    sub_func = &SubScalar;
    scalar_mul_func = &ScalarMulScalar;
    scalar_div_func = &ScalarDivScalar;
    dot_func = &DotScalar;
    cross_func = &CrossScalar;
    length_func = &LengthScalar;
    normalize_func = &NormalizeScalar;
    distance_func = &DistanceScalar;
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
    dot_func = &DotSSE2;
    cross_func = &CrossSSE2;
    length_func = &LengthSSE2;
    normalize_func = &NormalizeSSE2;
    distance_func = &DistanceSSE2;
    min_func = &MinSSE2;
    max_func = &MaxSSE2;
    abs_func = &AbsSSE2;

    if (level == SIMDLevel::SSE2)
        return;

    // SSE4.1 and AVX: single operations stay 128-bit (AVX only pays off in the batch operations)
    dot_func = &DotSSE41;
    length_func = &LengthSSE41;
    normalize_func = &NormalizeSSE41;
    distance_func = &DistanceSSE41;
    floor_func = &FloorSSE41;
    ceil_func = &CeilSSE41;
    round_func = &RoundSSE41;
}

// Force template instantiation for common usage patterns
template class std::vector<Vector3>;

// ===== PUBLIC BATCH OPERATIONS =====

void Vector3::AddBatch(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count)
{
#ifdef __AVX__
    if (simd_level == SIMDLevel::AVX)
    {
        AddBatchAVX(a, b, result, count);
        return;
    }
#endif
    // Fallback to sequential SSE operations
    for (size_t i = 0; i < count; ++i)
    {
        result[i] = add_func(a[i], b[i]);
    }
}

void Vector3::SubBatch(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count)
{
    // Similar to AddBatch but with subtraction
    for (size_t i = 0; i < count; ++i)
    {
        result[i] = sub_func(a[i], b[i]);
    }
}

void Vector3::ScalarMulBatch(const Vector3 *input, Vector3 *output, float scalar, size_t count)
{
#ifdef __AVX__
    if (simd_level == SIMDLevel::AVX)
    {
        ProcessVector3ArrayAVX(input, output, count, scalar);
        return;
    }
#endif
    for (size_t i = 0; i < count; ++i)
    {
        output[i] = scalar_mul_func(input[i], scalar);
    }
}

void Vector3::DotBatch(const Vector3 *a, const Vector3 *b, float *result, size_t count)
{
#ifdef __AVX__
    if (simd_level == SIMDLevel::AVX)
    {
        DotBatchAVX(a, b, result, count);
        return;
    }
#endif
    for (size_t i = 0; i < count; ++i)
    {
        result[i] = dot_func(a[i], b[i]);
    }
}

void Vector3::NormalizeBatch(Vector3 *vectors, size_t count)
{
#ifdef __AVX__
    if (simd_level == SIMDLevel::AVX)
    {
        NormalizeBatchAVX(vectors, count);
        return;
    }
#endif
    for (size_t i = 0; i < count; ++i)
    {
        vectors[i] = normalize_func(vectors[i]);
    }
}

void Vector3::CrossBatch(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count)
{
    // Cross product is complex for AVX, so use SSE for now
    for (size_t i = 0; i < count; ++i)
    {
        result[i] = cross_func(a[i], b[i]);
    }
}

void Vector3::TransformBatch(const Vector3 *input, Vector3 *output, size_t count,
                             const std::function<Vector3(const Vector3 &)> &transform)
{
    // Generic transform function - could be optimized further for specific transforms
    for (size_t i = 0; i < count; ++i)
    {
        output[i] = transform(input[i]);
    }
}

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

void Vector3::InitializeSIMD()
{
    if (initialized)
        return;

    std::cout << "Vector3 SIMD Initialization:\n";

    const CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();

    std::cout << "SSE2: " << (features.sse2 ? "Yes" : "No") << "\n";
    std::cout << "SSE4.1: " << (features.sse41 ? "Yes" : "No") << "\n";
    std::cout << "AVX: " << (features.avx ? "Yes" : "No") << "\n";
    std::cout << "AVX2: " << (features.avx2 ? "Yes" : "No") << "\n";

    // Set function pointers based on available features
    // Prefer highest performance implementations available

    if (features.avx)
    {
        std::cout << "Using AVX/SSE4.1 implementations for Vector3\n";

        // Single operations: AVX might not always be faster due to overhead
        // Use SSE4.1/SSE2 for single operations, AVX for batch operations
        add_func = &AddSSE2;
        sub_func = &SubSSE2;
        scalar_mul_func = &ScalarMulSSE2;
        scalar_div_func = &ScalarDivSSE2;

        if (features.sse41)
        {
            dot_func = &DotSSE41;
            length_func = &LengthSSE41;
            normalize_func = &NormalizeSSE41;
            distance_func = &DistanceSSE41;
        }
        else
        {
            dot_func = &DotSSE2;
            length_func = &LengthSSE2;
            normalize_func = &NormalizeSSE2;
            distance_func = &DistanceSSE2;
        }

        cross_func = &CrossSSE2;

        // AVX batch operations will be used automatically in batch functions
    }
    else if (features.sse41)
    {
        std::cout << "Using SSE4.1 implementations for Vector3\n";

        // Use SSE4.1 where available, SSE2 for others
        add_func = &AddSSE2;
        sub_func = &SubSSE2;
        scalar_mul_func = &ScalarMulSSE2;
        scalar_div_func = &ScalarDivSSE2;
        dot_func = &DotSSE41; // SSE4.1 dot product
        cross_func = &CrossSSE2; // SSE2 is fine for cross product
        length_func = &LengthSSE41; // SSE4.1 length
        normalize_func = &NormalizeSSE41; // SSE4.1 normalize
        distance_func = &DistanceSSE41; // SSE4.1 distance
    }
    else if (features.sse2)
    {
        std::cout << "Using SSE2 implementations for Vector3\n";

        add_func = &AddSSE2;
        sub_func = &SubSSE2;
        scalar_mul_func = &ScalarMulSSE2;
        scalar_div_func = &ScalarDivSSE2;
        dot_func = &DotSSE2;
        cross_func = &CrossSSE2;
        length_func = &LengthSSE2;
        normalize_func = &NormalizeSSE2;
        distance_func = &DistanceSSE2;
    }
    else
    {
        std::cout << "Using scalar implementations for Vector3 (no SIMD support)\n";

        // Function pointers already default to scalar implementations
        // No need to change them
    }

    initialized = true;
}

// Force template instantiation for common usage patterns
template class std::vector<Vector3>;

// ===== PUBLIC BATCH OPERATIONS =====

void Vector3::AddBatch(const Vector3 *a, const Vector3 *b, Vector3 *result, size_t count)
{
#ifdef __AVX__
    CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();
    if (features.avx)
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
    CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();
    if (features.avx)
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
    CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();
    if (features.avx)
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
    CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();
    if (features.avx)
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

#include "math/Vector3.hpp"
#include <iostream>

using namespace N2Engine::Math;

namespace CPUInfo
{
    struct CPUFeatures
    {
        bool sse2 = false;
        bool sse3 = false;
        bool sse41 = false;
        bool sse42 = false;
        bool avx = false;
        bool avx2 = false;
    };

    CPUFeatures DetectCPUFeatures()
    {
        CPUFeatures features;

#ifdef _WIN32
        int cpuInfo[4];
        __cpuid(cpuInfo, 0);
        int numIds = cpuInfo[0];

        if (numIds >= 1)
        {
            __cpuid(cpuInfo, 1);
            features.sse2 = (cpuInfo[3] & (1 << 26)) != 0;
            features.sse3 = (cpuInfo[2] & (1 << 0)) != 0;
            features.sse41 = (cpuInfo[2] & (1 << 19)) != 0;
            features.sse42 = (cpuInfo[2] & (1 << 20)) != 0;
            features.avx = (cpuInfo[2] & (1 << 28)) != 0;
        }

        if (numIds >= 7)
        {
            __cpuid(cpuInfo, 7);
            features.avx2 = (cpuInfo[1] & (1 << 5)) != 0;
        }
#elif defined(__GNUC__) || defined(__clang__)
        unsigned int eax, ebx, ecx, edx;

        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        {
            features.sse2 = (edx & bit_SSE2) != 0;
            features.sse3 = (ecx & bit_SSE3) != 0;
            features.sse41 = (ecx & bit_SSE4_1) != 0;
            features.sse42 = (ecx & bit_SSE4_2) != 0;
            features.avx = (ecx & bit_AVX) != 0;
        }

        if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        {
            features.avx2 = (ebx & bit_AVX2) != 0;
        }
#endif

        return features;
    }
}

void Vector3::InitializeSIMD()
{
    if (initialized)
        return;

    CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();

    std::cout << "Vector3 SIMD Initialization:\n";
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
        dot_func = &DotSSE41;             // SSE4.1 dot product
        cross_func = &CrossSSE2;          // SSE2 is fine for cross product
        length_func = &LengthSSE41;       // SSE4.1 length
        normalize_func = &NormalizeSSE41; // SSE4.1 normalize
        distance_func = &DistanceSSE41;   // SSE4.1 distance
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
    std::cout << "Vector3 SIMD initialization complete.\n\n";
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
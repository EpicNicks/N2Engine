#pragma once

#ifdef _WIN32
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif

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

    inline CPUFeatures DetectCPUFeatures()
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
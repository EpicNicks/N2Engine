#pragma once

// The SSE4.1 implementations are compiled when the build targets SSE4.1. MSVC never defines __SSE4_1__, but
// /arch:AVX (which the math target builds with) implies it
#if defined(__SSE4_1__) || defined(__AVX__)
#define N2_MATH_SSE41
#endif

#ifdef _WIN32
#include <intrin.h>
#include <immintrin.h>
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

    // Whether the OS saves the YMM registers across context switches; AVX instructions fault without it.
    // Only valid to call when CPUID reports OSXSAVE (xgetbv is undefined otherwise)
    inline bool OSSavesAVXState()
    {
#ifdef _WIN32
        return (_xgetbv(0) & 0x6) == 0x6;
#elif defined(__GNUC__) || defined(__clang__)
        unsigned int xcr0Low, xcr0High;
        __asm__ volatile("xgetbv" : "=a"(xcr0Low), "=d"(xcr0High) : "c"(0));
        (void)xcr0High;
        return (xcr0Low & 0x6) == 0x6;
#else
        return false;
#endif
    }

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
            const bool osxsave = (cpuInfo[2] & (1 << 27)) != 0;
            features.avx = (cpuInfo[2] & (1 << 28)) != 0 && osxsave && OSSavesAVXState();
        }

        if (numIds >= 7)
        {
            __cpuid(cpuInfo, 7);
            features.avx2 = features.avx && (cpuInfo[1] & (1 << 5)) != 0;
        }
#elif defined(__GNUC__) || defined(__clang__)
        unsigned int eax, ebx, ecx, edx;

        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        {
            features.sse2 = (edx & bit_SSE2) != 0;
            features.sse3 = (ecx & bit_SSE3) != 0;
            features.sse41 = (ecx & bit_SSE4_1) != 0;
            features.sse42 = (ecx & bit_SSE4_2) != 0;
            const bool osxsave = (ecx & bit_OSXSAVE) != 0;
            features.avx = (ecx & bit_AVX) != 0 && osxsave && OSSavesAVXState();
        }

        if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        {
            features.avx2 = features.avx && (ebx & bit_AVX2) != 0;
        }
#endif

        return features;
    }
}

namespace N2Engine::Math
{
    // Instruction set tiers the math types can dispatch to, lowest to highest. Each tier also uses
    // everything below it
    enum class SIMDLevel
    {
        Scalar,
        SSE2,
        SSE41,
        AVX
    };

    // The highest tier this CPU (and OS, for AVX) supports. Detected once, since SetSIMDLevel checks it on every call
    inline SIMDLevel DetectSIMDLevel()
    {
        static const SIMDLevel level = []
        {
            const CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();
            if (features.avx && features.sse41)
                return SIMDLevel::AVX;
            if (features.sse41)
                return SIMDLevel::SSE41;
            if (features.sse2)
                return SIMDLevel::SSE2;
            return SIMDLevel::Scalar;
        }();
        return level;
    }

    // A requested tier, lowered to the highest one this CPU supports
    inline SIMDLevel ClampSIMDLevel(const SIMDLevel requested)
    {
        const SIMDLevel supported = DetectSIMDLevel();
        return requested < supported ? requested : supported;
    }
}
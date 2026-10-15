#include "math/MathRegistrar.hpp"

#include "math/Vector3.hpp"
#include "math/Vector4.hpp"
#include "math/Quaternion.hpp"
#include "math/Matrix.hpp"
#include "math/CpuInfo.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

#ifdef __AVX__
#define HAS_AVX true
#else
#define HAS_AVX false
#endif

#ifdef __SSE4_1__
#define HAS_SSE41 true
#else
#define HAS_SSE41 false
#endif

#ifdef __SSE2__
#define HAS_SSE2 true
#else
#define HAS_SSE2 false
#endif

// General helper
#define HAS_SUPPORT(name, macro) \
    std::cout << name " compiler support: " << (macro ? "Yes" : "No") << std::endl;

void N2Engine::Math::InitializeSIMD()
{
    (void)InitializeSIMD(SIMDTierRequest::Auto);
}

std::optional<N2Engine::Math::SIMDTierRequest> N2Engine::Math::ParseSIMDTierRequest(const std::string_view text)
{
    std::string lower;
    lower.reserve(text.size());
    for (const char c : text)
    {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "auto")
        return SIMDTierRequest::Auto;
    if (lower == "scalar")
        return SIMDTierRequest::Scalar;
    if (lower == "sse2")
        return SIMDTierRequest::SSE2;
    if (lower == "sse4.1" || lower == "sse41")
        return SIMDTierRequest::SSE41;
    if (lower == "avx")
        return SIMDTierRequest::AVX;
    if (lower == "neon")
        return SIMDTierRequest::NEON;
    return std::nullopt;
}

std::optional<N2Engine::Math::SIMDLevel> N2Engine::Math::ToSIMDLevel(const SIMDTierRequest request)
{
    switch (request)
    {
    case SIMDTierRequest::Auto:
        return std::nullopt;
    case SIMDTierRequest::Scalar:
        return SIMDLevel::Scalar;
    case SIMDTierRequest::SSE2:
        return SIMDLevel::SSE2;
    case SIMDTierRequest::SSE41:
        return SIMDLevel::SSE41;
    case SIMDTierRequest::AVX:
        return SIMDLevel::AVX;
    case SIMDTierRequest::NEON:
        return SIMDLevel::NEON;
    }
    return std::nullopt;
}

const char *N2Engine::Math::SIMDLevelName(const SIMDLevel level)
{
    switch (level)
    {
    case SIMDLevel::Scalar:
        return "Scalar";
    case SIMDLevel::SSE2:
        return "SSE2";
    case SIMDLevel::SSE41:
        return "SSE4.1";
    case SIMDLevel::AVX:
        return "AVX";
    case SIMDLevel::NEON:
        return "NEON";
    }
    return "Unknown";
}

bool N2Engine::Math::IsStrictFp()
{
#ifdef N2_STRICT_FP
    return true;
#else
    return false;
#endif
}

N2Engine::Math::SIMDLevel N2Engine::Math::InitializeSIMD(const SIMDTierRequest request)
{
    std::cout << "----------INITIALIZE SIMD BEGIN----------" << std::endl;

    HAS_SUPPORT("AVX", HAS_AVX)
    HAS_SUPPORT("SSE4.1", HAS_SSE41)
    HAS_SUPPORT("SSE2", HAS_SSE2)
#ifdef N2_MATH_NEON
    std::cout << "NEON: Yes (aarch64 baseline)\n";
#endif

    CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();

    std::cout << "SSE2: " << (features.sse2 ? "Yes" : "No") << "\n";
    std::cout << "SSE4.1: " << (features.sse41 ? "Yes" : "No") << "\n";
    std::cout << "AVX: " << (features.avx ? "Yes" : "No") << "\n";
    std::cout << "AVX2: " << (features.avx2 ? "Yes" : "No") << "\n";

    // Auto: the detected tier. A named tier is lowered to what this target supports (never refused)
    const std::optional<SIMDLevel> requested = ToSIMDLevel(request);
    const SIMDLevel level = requested ? ClampSIMDLevel(*requested) : DetectSIMDLevel();
    std::cout << "Math SIMD level: " << SIMDLevelName(level);
    if (requested)
    {
        std::cout << " (requested " << SIMDLevelName(*requested) << (level == *requested ? "" : ", lowered") << ")";
    }
    std::cout << "\n";

    SetSIMDLevel(level);

    std::cout << "----------INITIALIZE SIMD END------------" << std::endl;
    return level;
}

void N2Engine::Math::SetSIMDLevel(const SIMDLevel level)
{
    Vector3::SetSIMDLevel(level);
    Vector4::SetSIMDLevel(level);
    Quaternion::SetSIMDLevel(level);
    Matrix<float, 4, 4>::SetSIMDLevel(level);
    Matrix<float, 3, 3>::SetSIMDLevel(level);
}
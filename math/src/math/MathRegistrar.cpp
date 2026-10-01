#include "math/MathRegistrar.hpp"

#include "math/Vector3.hpp"
#include "math/Vector4.hpp"
#include "math/Quaternion.hpp"
#include "math/Matrix.hpp"
#include "math/CpuInfo.hpp"

#include <iostream>

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
    std::cout << "----------INITIALIZE SIMD BEGIN----------" << std::endl;

    HAS_SUPPORT("AVX", HAS_AVX)
    HAS_SUPPORT("SSE4.1", HAS_SSE41)
    HAS_SUPPORT("SSE2", HAS_SSE2)

    CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();

    std::cout << "SSE2: " << (features.sse2 ? "Yes" : "No") << "\n";
    std::cout << "SSE4.1: " << (features.sse41 ? "Yes" : "No") << "\n";
    std::cout << "AVX: " << (features.avx ? "Yes" : "No") << "\n";
    std::cout << "AVX2: " << (features.avx2 ? "Yes" : "No") << "\n";

    const SIMDLevel level = DetectSIMDLevel();
    constexpr const char *levelNames[] = {"Scalar", "SSE2", "SSE4.1", "AVX"};
    std::cout << "Math SIMD level: " << levelNames[static_cast<int>(level)] << "\n";

    SetSIMDLevel(level);

    std::cout << "----------INITIALIZE SIMD END------------" << std::endl;
}

void N2Engine::Math::SetSIMDLevel(const SIMDLevel level)
{
    Vector3::SetSIMDLevel(level);
    Vector4::SetSIMDLevel(level);
    Quaternion::SetSIMDLevel(level);
    Matrix<float, 4, 4>::SetSIMDLevel(level);
    Matrix<float, 3, 3>::SetSIMDLevel(level);
}
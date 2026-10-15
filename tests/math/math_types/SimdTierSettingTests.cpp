#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <math/CpuInfo.hpp>
#include <math/MathRegistrar.hpp>
#include <math/Matrix.hpp>
#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

// The project setting math.simdTier (Application::Init -> InitializeSIMD(request)) and the strict floating-point
// query. The references below spell out the scalar implementations' operation order (the Scalar tier is the
// definition of the reference result), so forcing Scalar must match them to the last bit.

using namespace N2Engine::Math;

namespace
{
    using Matrix4 = Matrix<float, 4, 4>;

    // Restores the default (Scalar) after each test, like the other dispatch tests
    struct ScalarDispatchGuard
    {
        ~ScalarDispatchGuard()
        {
            SetSIMDLevel(SIMDLevel::Scalar);
        }
    };

    std::uint32_t Bits(const float value)
    {
        return std::bit_cast<std::uint32_t>(value);
    }

    // Bit equality, not ==: it tells -0 from +0 and compares NaN payloads
    void ExpectBitEqual(const float actual, const float expected, const std::string &what)
    {
        EXPECT_EQ(Bits(actual), Bits(expected)) << what << ": " << actual << " vs " << expected;
    }

    void ExpectBitEqual(const Vector3 &actual, const Vector3 &expected, const std::string &what)
    {
        ExpectBitEqual(actual.x, expected.x, what + ".x");
        ExpectBitEqual(actual.y, expected.y, what + ".y");
        ExpectBitEqual(actual.z, expected.z, what + ".z");
    }

    void ExpectBitEqual(const Quaternion &actual, const Quaternion &expected, const std::string &what)
    {
        ExpectBitEqual(actual.GetW(), expected.GetW(), what + ".w");
        ExpectBitEqual(actual.GetX(), expected.GetX(), what + ".x");
        ExpectBitEqual(actual.GetY(), expected.GetY(), what + ".y");
        ExpectBitEqual(actual.GetZ(), expected.GetZ(), what + ".z");
    }

    void ExpectNear(const float actual, const float expected, const std::string &what)
    {
        EXPECT_NEAR(actual, expected, 1e-5f * std::max(1.0f, std::abs(expected))) << what;
    }

    // Fixed inputs with awkward magnitudes, so a different summation order or a divide instead of a multiply by the
    // reciprocal shows up in the last bits
    const std::vector<Vector3> &Vectors()
    {
        static const std::vector<Vector3> values = {
            {0.1f, 0.2f, 0.3f},
            {1.0f, 2.0f, 3.0f},
            {-4.7f, 0.013f, 9.9f},
            {1234.5f, -6789.25f, 0.5f},
            {1e-3f, 2e-3f, 3e-3f},
            {3.0f, 4.0f, 0.0f},
            {-0.7f, 0.1f, -0.3f},
            {1.0f / 3.0f, 2.0f / 7.0f, 5.0f / 11.0f},
        };
        return values;
    }

    const std::vector<Quaternion> &Quaternions()
    {
        static const std::vector<Quaternion> values = {
            {1.0f, 0.0f, 0.0f, 0.0f},
            {0.5f, 0.5f, 0.5f, 0.5f},
            {0.1f, 0.2f, 0.3f, 0.4f},
            {-0.7f, 0.013f, 9.9f, 0.25f},
            {1.0f / 3.0f, 2.0f / 7.0f, 5.0f / 11.0f, 0.77f},
            {2.0f, -3.0f, 5.0f, 0.1f},
        };
        return values;
    }

    // The scalar reference, in the scalar implementations' exact operation order
    float RefDot(const Vector3 &a, const Vector3 &b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Vector3 RefCross(const Vector3 &a, const Vector3 &b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    float RefLength(const Vector3 &v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    Vector3 RefNormalize(const Vector3 &v)
    {
        const float inv = 1.0f / RefLength(v);
        return {v.x * inv, v.y * inv, v.z * inv};
    }

    // ScalarDivScalar multiplies by the reciprocal (the SSE and NEON tiers divide)
    Vector3 RefScalarDiv(const Vector3 &v, const float scalar)
    {
        const float inv = 1.0f / scalar;
        return {v.x * inv, v.y * inv, v.z * inv};
    }

    float RefDot(const Quaternion &a, const Quaternion &b)
    {
        return a.GetW() * b.GetW() + a.GetX() * b.GetX() + a.GetY() * b.GetY() + a.GetZ() * b.GetZ();
    }

    Quaternion RefMul(const Quaternion &a, const Quaternion &b)
    {
        const float aw = a.GetW(), ax = a.GetX(), ay = a.GetY(), az = a.GetZ();
        const float bw = b.GetW(), bx = b.GetX(), by = b.GetY(), bz = b.GetZ();
        return Quaternion(aw * bw - ax * bx - ay * by - az * bz,
                          aw * bx + ax * bw + ay * bz - az * by,
                          aw * by - ax * bz + ay * bw + az * bx,
                          aw * bz + ax * by - ay * bx + az * bw);
    }

    float RefLength(const Quaternion &q)
    {
        return std::sqrt(q.GetW() * q.GetW() + q.GetX() * q.GetX() + q.GetY() * q.GetY() + q.GetZ() * q.GetZ());
    }

    Quaternion RefNormalize(const Quaternion &q)
    {
        const float inv = 1.0f / RefLength(q);
        return Quaternion(q.GetW() * inv, q.GetX() * inv, q.GetY() * inv, q.GetZ() * inv);
    }

    Matrix4 SampleMatrix(const float seed)
    {
        Matrix4 m;
        for (std::size_t row = 0; row < 4; ++row)
        {
            for (std::size_t col = 0; col < 4; ++col)
            {
                m(row, col) = seed * 0.37f + static_cast<float>(row) * 1.3f - static_cast<float>(col) * 0.71f +
                              1.0f / static_cast<float>(row + col + 2);
            }
        }
        return m;
    }

    // Row-major product with the sum accumulated left to right, as MultiplyScalar does
    Matrix4 RefMultiply(const Matrix4 &a, const Matrix4 &b)
    {
        Matrix4 result;
        for (std::size_t i = 0; i < 4; ++i)
        {
            for (std::size_t j = 0; j < 4; ++j)
            {
                float sum = 0.0f;
                for (std::size_t k = 0; k < 4; ++k)
                {
                    sum += a(i, k) * b(k, j);
                }
                result(i, j) = sum;
            }
        }
        return result;
    }
}

TEST(SimdTierSettingTest, ParseAcceptsEveryTierNameInAnyCase)
{
    EXPECT_EQ(ParseSIMDTierRequest("auto"), SIMDTierRequest::Auto);
    EXPECT_EQ(ParseSIMDTierRequest("Scalar"), SIMDTierRequest::Scalar);
    EXPECT_EQ(ParseSIMDTierRequest("SSE2"), SIMDTierRequest::SSE2);
    EXPECT_EQ(ParseSIMDTierRequest("sse4.1"), SIMDTierRequest::SSE41);
    EXPECT_EQ(ParseSIMDTierRequest("SSE41"), SIMDTierRequest::SSE41);
    EXPECT_EQ(ParseSIMDTierRequest("Avx"), SIMDTierRequest::AVX);
    EXPECT_EQ(ParseSIMDTierRequest("NEON"), SIMDTierRequest::NEON);
}

TEST(SimdTierSettingTest, ParseRejectsUnknownNames)
{
    EXPECT_FALSE(ParseSIMDTierRequest("").has_value());
    EXPECT_FALSE(ParseSIMDTierRequest("avx2").has_value());
    EXPECT_FALSE(ParseSIMDTierRequest("fastest").has_value());
    EXPECT_FALSE(ParseSIMDTierRequest(" scalar").has_value());
}

TEST(SimdTierSettingTest, ToSIMDLevelMapsEveryRequestExceptAuto)
{
    EXPECT_FALSE(ToSIMDLevel(SIMDTierRequest::Auto).has_value());
    EXPECT_EQ(ToSIMDLevel(SIMDTierRequest::Scalar), SIMDLevel::Scalar);
    EXPECT_EQ(ToSIMDLevel(SIMDTierRequest::SSE2), SIMDLevel::SSE2);
    EXPECT_EQ(ToSIMDLevel(SIMDTierRequest::SSE41), SIMDLevel::SSE41);
    EXPECT_EQ(ToSIMDLevel(SIMDTierRequest::AVX), SIMDLevel::AVX);
    EXPECT_EQ(ToSIMDLevel(SIMDTierRequest::NEON), SIMDLevel::NEON);
}

TEST(SimdTierSettingTest, AutoSelectsTheDetectedTier)
{
    const ScalarDispatchGuard guard;
    EXPECT_EQ(InitializeSIMD(SIMDTierRequest::Auto), DetectSIMDLevel());
    EXPECT_EQ(Vector3::GetSIMDLevel(), DetectSIMDLevel());
    EXPECT_EQ(Vector4::GetSIMDLevel(), DetectSIMDLevel());
}

TEST(SimdTierSettingTest, ScalarIsAlwaysHonoured)
{
    const ScalarDispatchGuard guard;
    InitializeSIMD(SIMDTierRequest::Auto);
    EXPECT_EQ(InitializeSIMD(SIMDTierRequest::Scalar), SIMDLevel::Scalar);
    EXPECT_EQ(Vector3::GetSIMDLevel(), SIMDLevel::Scalar);
    EXPECT_EQ(Vector4::GetSIMDLevel(), SIMDLevel::Scalar);
}

TEST(SimdTierSettingTest, ATierTheTargetLacksIsLoweredNotRefused)
{
    const ScalarDispatchGuard guard;
    for (const SIMDTierRequest request : {SIMDTierRequest::SSE2, SIMDTierRequest::SSE41, SIMDTierRequest::AVX,
                                          SIMDTierRequest::NEON})
    {
        const SIMDLevel wanted = *ToSIMDLevel(request);
        const SIMDLevel used = InitializeSIMD(request);
        EXPECT_EQ(used, ClampSIMDLevel(wanted)) << SIMDLevelName(wanted);
        EXPECT_TRUE(IsSIMDLevelSupported(used)) << SIMDLevelName(wanted);
        EXPECT_EQ(Vector3::GetSIMDLevel(), used) << SIMDLevelName(wanted);
        if (IsSIMDLevelSupported(wanted))
        {
            EXPECT_EQ(used, wanted) << SIMDLevelName(wanted);
        }
    }
}

TEST(SimdTierSettingTest, ForcedScalarVector3MatchesTheReferenceBitForBit)
{
    const ScalarDispatchGuard guard;
    InitializeSIMD(SIMDTierRequest::Auto);
    ASSERT_EQ(InitializeSIMD(SIMDTierRequest::Scalar), SIMDLevel::Scalar);

    for (const Vector3 &a : Vectors())
    {
        const std::string at = "a=(" + std::to_string(a.x) + "," + std::to_string(a.y) + "," + std::to_string(a.z) + ")";
        ExpectBitEqual(a.Length(), RefLength(a), at + " Length");
        ExpectBitEqual(a.Normalized(), RefNormalize(a), at + " Normalized");
        for (const Vector3 &b : Vectors())
        {
            ExpectBitEqual(a + b, Vector3(a.x + b.x, a.y + b.y, a.z + b.z), at + " Add");
            ExpectBitEqual(a - b, Vector3(a.x - b.x, a.y - b.y, a.z - b.z), at + " Sub");
            ExpectBitEqual(a.Dot(b), RefDot(a, b), at + " Dot");
            ExpectBitEqual(a.Cross(b), RefCross(a, b), at + " Cross");
            ExpectBitEqual(a.Distance(b), RefLength(Vector3(a.x - b.x, a.y - b.y, a.z - b.z)),
                           at + " Distance");
        }
        ExpectBitEqual(a * 0.3f, Vector3(a.x * 0.3f, a.y * 0.3f, a.z * 0.3f), at + " ScalarMul");
        ExpectBitEqual(a / 0.7f, RefScalarDiv(a, 0.7f), at + " ScalarDiv");
    }
}

TEST(SimdTierSettingTest, ForcedScalarQuaternionMatchesTheReferenceBitForBit)
{
    const ScalarDispatchGuard guard;
    InitializeSIMD(SIMDTierRequest::Auto);
    ASSERT_EQ(InitializeSIMD(SIMDTierRequest::Scalar), SIMDLevel::Scalar);

    for (const Quaternion &a : Quaternions())
    {
        ExpectBitEqual(a.Length(), RefLength(a), "Length");
        ExpectBitEqual(a.Normalized(), RefNormalize(a), "Normalized");
        for (const Quaternion &b : Quaternions())
        {
            ExpectBitEqual(a.Dot(b), RefDot(a, b), "Dot");
            ExpectBitEqual(a * b, RefMul(a, b), "Mul");
        }
    }
}

TEST(SimdTierSettingTest, ForcedScalarMatrixProductMatchesTheReferenceBitForBit)
{
    const ScalarDispatchGuard guard;
    InitializeSIMD(SIMDTierRequest::Auto);
    ASSERT_EQ(InitializeSIMD(SIMDTierRequest::Scalar), SIMDLevel::Scalar);

    for (const float seedA : {0.5f, 1.7f, -3.1f})
    {
        for (const float seedB : {0.25f, 2.9f, -0.6f})
        {
            const Matrix4 a = SampleMatrix(seedA);
            const Matrix4 b = SampleMatrix(seedB);
            const Matrix4 product = a * b;
            const Matrix4 expected = RefMultiply(a, b);
            for (std::size_t row = 0; row < 4; ++row)
            {
                for (std::size_t col = 0; col < 4; ++col)
                {
                    ExpectBitEqual(product(row, col), expected(row, col),
                                   "Multiply(" + std::to_string(row) + "," + std::to_string(col) + ")");
                }
            }
        }
    }
}

// Auto keeps the contract the existing dispatch tests assert: within a relative 1e-5 of scalar
TEST(SimdTierSettingTest, AutoStaysWithinToleranceOfTheReference)
{
    const ScalarDispatchGuard guard;
    ASSERT_EQ(InitializeSIMD(SIMDTierRequest::Auto), DetectSIMDLevel());

    for (const Vector3 &a : Vectors())
    {
        ExpectNear(a.Length(), RefLength(a), "Length");
        const Vector3 normalized = a.Normalized();
        const Vector3 expected = RefNormalize(a);
        ExpectNear(normalized.x, expected.x, "Normalized.x");
        ExpectNear(normalized.y, expected.y, "Normalized.y");
        ExpectNear(normalized.z, expected.z, "Normalized.z");
        for (const Vector3 &b : Vectors())
        {
            ExpectNear(a.Dot(b), RefDot(a, b), "Dot");
        }
    }
    for (const Quaternion &a : Quaternions())
    {
        for (const Quaternion &b : Quaternions())
        {
            ExpectNear(a.Dot(b), RefDot(a, b), "Quaternion Dot");
            const Quaternion product = a * b;
            const Quaternion expected = RefMul(a, b);
            const float scale = std::max({std::abs(expected.GetW()), std::abs(expected.GetX()),
                                          std::abs(expected.GetY()), std::abs(expected.GetZ()), 1.0f});
            EXPECT_NEAR(product.GetW(), expected.GetW(), 1e-5f * scale);
            EXPECT_NEAR(product.GetX(), expected.GetX(), 1e-5f * scale);
            EXPECT_NEAR(product.GetY(), expected.GetY(), 1e-5f * scale);
            EXPECT_NEAR(product.GetZ(), expected.GetZ(), 1e-5f * scale);
        }
    }
}

TEST(SimdTierSettingTest, IsStrictFpReportsTheCompiledProfile)
{
    // N2_STRICT_FP is defined for every target by N2ENGINE_STRICT_FP, so the test and the library agree
#ifdef N2_STRICT_FP
    EXPECT_TRUE(IsStrictFp());
#else
    EXPECT_FALSE(IsStrictFp());
#endif
}

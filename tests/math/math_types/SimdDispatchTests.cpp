#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <math/CpuInfo.hpp>
#include <math/MathRegistrar.hpp>
#include <math/Matrix.hpp>
#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

using namespace N2Engine::Math;

namespace
{
    using Matrix4 = Matrix<float, 4, 4>;
    using Matrix3 = Matrix<float, 3, 3>;

    // Relative tolerance: some SIMD paths sum in a different order (or divide instead of multiplying by a
    // reciprocal), so results may differ from scalar in the last bits. Wrong lanes or shuffles are off by far more
    constexpr float TOLERANCE = 1e-5f;

    std::string LevelName(const SIMDLevel level)
    {
        switch (level)
        {
        case SIMDLevel::Scalar:
            return "Scalar";
        case SIMDLevel::SSE2:
            return "SSE2";
        case SIMDLevel::SSE41:
            return "SSE41";
        case SIMDLevel::AVX:
            return "AVX";
        }
        return "Unknown";
    }

    // scale: the magnitude of the other components, since a component that cancels to near zero carries the
    // rounding error of its larger terms
    void ExpectClose(const float actual, const float expected, const std::string &what, const float scale = 1.0f)
    {
        EXPECT_NEAR(actual, expected, TOLERANCE * std::max({1.0f, std::abs(expected), scale})) << what;
    }

    void ExpectClose(const Vector3 &actual, const Vector3 &expected, const std::string &what)
    {
        const float scale = std::max({std::abs(expected.x), std::abs(expected.y), std::abs(expected.z)});
        for (size_t i = 0; i < 3; ++i)
            ExpectClose(actual[i], expected[i], what + "[" + std::to_string(i) + "]", scale);
    }

    void ExpectClose(const Quaternion &actual, const Quaternion &expected, const std::string &what)
    {
        const float scale = std::max({std::abs(expected.GetW()), std::abs(expected.GetX()),
                                      std::abs(expected.GetY()), std::abs(expected.GetZ())});
        ExpectClose(actual.GetW(), expected.GetW(), what + ".w", scale);
        ExpectClose(actual.GetX(), expected.GetX(), what + ".x", scale);
        ExpectClose(actual.GetY(), expected.GetY(), what + ".y", scale);
        ExpectClose(actual.GetZ(), expected.GetZ(), what + ".z", scale);
    }

    template <size_t N>
    void ExpectClose(const Matrix<float, N, N> &actual, const Matrix<float, N, N> &expected, const std::string &what)
    {
        float scale = 0.0f;
        for (size_t row = 0; row < N; ++row)
            for (size_t col = 0; col < N; ++col)
                scale = std::max(scale, std::abs(expected(row, col)));

        for (size_t row = 0; row < N; ++row)
            for (size_t col = 0; col < N; ++col)
                ExpectClose(actual(row, col), expected(row, col),
                            what + "(" + std::to_string(row) + "," + std::to_string(col) + ")", scale);
    }

    // Bit-exact, for operations with exactly one right answer, including the sign of zero. Any NaN matches any NaN
    void ExpectIdentical(const Vector3 &actual, const Vector3 &expected, const std::string &what)
    {
        for (size_t i = 0; i < 3; ++i)
        {
            if (std::isnan(actual[i]) && std::isnan(expected[i]))
                continue;
            EXPECT_EQ(std::bit_cast<uint32_t>(actual[i]), std::bit_cast<uint32_t>(expected[i]))
                << what << "[" << i << "]: " << actual[i] << " vs " << expected[i];
        }
    }

    template <typename T>
    void ExpectClose(const std::vector<T> &actual, const std::vector<T> &expected, const std::string &what)
    {
        ASSERT_EQ(actual.size(), expected.size()) << what;
        for (size_t i = 0; i < actual.size(); ++i)
            ExpectClose(actual[i], expected[i], what + "[" + std::to_string(i) + "]");
    }

    // Non-zero values in every lane, halfway cases for Round, a tiny and a large vector, and the zero vector
    std::vector<Vector3> SampleVectors()
    {
        std::vector<Vector3> vectors = {
            {1.0f, 2.0f, 3.0f},
            {4.0f, 5.0f, 6.0f},
            {3.0f, 4.0f, 0.0f},
            {-2.5f, 0.5f, 1.5f},
            {2.5f, -0.5f, -1.5f},
            {0.1f, -7.3f, 2.2f},
            {1e-3f, 2e-3f, -3e-3f},
            {-1250.0f, 42.0f, 0.75f},
            {0.0f, 0.0f, 0.0f},
            {-0.3f, 0.49f, 7.51f},
            {10.0f, 0.0f, 0.0f},
        };
        // Garbage in the padding lane must not leak into any result
        for (size_t i = 0; i < vectors.size(); ++i)
            vectors[i].w = 7.0f + static_cast<float>(i);
        return vectors;
    }

    // Signed zeros, the float just below 0.5, integers past 2^23, huge values, infinities and NaNs
    std::vector<Vector3> EdgeCaseVectors()
    {
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        return {
            {-0.0f, 0.0f, -0.3f},
            {0.0f, -0.0f, 0.3f},
            {0.49999997f, -0.49999997f, -0.5f},
            {8388609.0f, -8388609.0f, 1e10f},
            {-inf, inf, -1e10f},
            {nan, 1.0f, -2.5f},
            {2.5f, nan, -0.0f},
            {-7.5f, 3.5f, nan},
        };
    }

    // Deliberately not unit length (except the ones built from rotations)
    std::vector<Quaternion> SampleQuaternions()
    {
        return {
            Quaternion(1.0f, 2.0f, 3.0f, 4.0f),
            Quaternion(0.5f, -1.5f, 2.0f, 0.25f),
            Quaternion(-3.0f, 0.1f, 0.2f, -0.7f),
            Quaternion::FromAxisAngle(Vector3(1.0f, 2.0f, 3.0f).Normalized(), 1.1f),
            Quaternion::FromEulerAngles(0.3f, -1.2f, 2.0f),
            Quaternion(0.0f, 0.0f, 0.0f, 0.0f),
        };
    }

    // An affine TRS, a general matrix, a near-singular (det 1e-4) shear and a projective matrix
    std::vector<Matrix4> SampleMatrices4()
    {
        return {
            Matrix4::Translation(Vector3(1.0f, -2.0f, 3.0f)) * Matrix4::RotationY(0.7f) * Matrix4::Scale(2.0f, 0.5f, 3.0f),
            Matrix4{1.0f, 2.0f, 3.0f, 4.0f, 0.5f, -1.0f, 2.0f, 0.25f, 3.0f, 0.0f, -2.0f, 1.0f, 0.1f, 0.2f, 0.3f, 1.5f},
            Matrix4{1.0f, 1.0f, 0.0f, 0.5f, 1.0f, 1.0001f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, -2.0f, 0.0f, 0.0f, 0.0f, 1.0f},
            Matrix4{1.2f, 0.0f, 0.3f, 0.0f, 0.0f, 0.9f, -0.4f, 0.0f, 0.0f, 0.0f, -1.1f, -0.2f, 0.1f, 0.2f, -1.0f, 0.5f},
        };
    }

    std::vector<Matrix3> SampleMatrices3()
    {
        return {
            Matrix3::RotationX(0.4f) * Matrix3::RotationZ(-1.3f),
            Matrix3{1.0f, 2.0f, 3.0f, -4.0f, 0.5f, 6.0f, 7.0f, -8.0f, 0.25f},
            Matrix3{0.1f, -0.2f, 0.3f, 1e3f, 2.5f, -3.5f, 0.0f, 9.0f, -1.0f},
        };
    }

    // Transform inputs chosen so no matrix's projective w is near zero
    std::vector<Vector3> SamplePoints()
    {
        return {{1.0f, 2.0f, 3.0f}, {-0.5f, 4.0f, -2.0f}, {0.25f, -1.5f, 0.75f}};
    }
}

// Runs the same math under the scalar implementations and under one SIMD tier, and compares the results.
// Every test leaves the process on scalar dispatch (the default), so no other test sees SIMD
class SimdDispatchTest : public ::testing::TestWithParam<SIMDLevel>
{
protected:
    void SetUp() override
    {
        SetSIMDLevel(SIMDLevel::Scalar);
        // SetSIMDLevel would clamp an unsupported tier, so the test would silently cover a lower one
        if (DetectSIMDLevel() < GetParam())
            GTEST_SKIP() << "CPU does not support " << LevelName(GetParam());
    }

    void TearDown() override
    {
        SetSIMDLevel(SIMDLevel::Scalar);
    }

    template <typename F>
    static auto Under(const SIMDLevel level, F &compute)
    {
        SetSIMDLevel(level);
        auto result = compute();
        SetSIMDLevel(SIMDLevel::Scalar);
        return result;
    }

    template <typename F>
    void ExpectMatchesScalar(F compute, const std::string &what)
    {
        const auto expected = Under(SIMDLevel::Scalar, compute);
        const auto actual = Under(GetParam(), compute);
        ExpectClose(actual, expected, what);
    }

    template <typename F>
    void ExpectIdenticalToScalar(F compute, const std::string &what)
    {
        const Vector3 expected = Under(SIMDLevel::Scalar, compute);
        const Vector3 actual = Under(GetParam(), compute);
        ExpectIdentical(actual, expected, what);
    }
};

TEST_P(SimdDispatchTest, KnownValues)
{
    SetSIMDLevel(GetParam());

    EXPECT_FLOAT_EQ(Vector3(3.0f, 4.0f, 0.0f).Length(), 5.0f);
    EXPECT_FLOAT_EQ(Vector3(1.0f, 2.0f, 3.0f).Dot(Vector3(4.0f, 5.0f, 6.0f)), 32.0f);
    EXPECT_NEAR(Vector3(10.0f, 0.0f, 0.0f).Normalized().Length(), 1.0f, 1e-6f);
    EXPECT_TRUE(Vector3(2.5f, -2.5f, 0.5f).Round() == Vector3(3.0f, -3.0f, 1.0f));

    const Quaternion product = Quaternion(1.0f, 2.0f, 3.0f, 4.0f) * Quaternion(5.0f, 6.0f, 7.0f, 8.0f);
    EXPECT_FLOAT_EQ(product.GetW(), -60.0f);
    EXPECT_FLOAT_EQ(product.GetX(), 12.0f);
    EXPECT_FLOAT_EQ(product.GetY(), 30.0f);
    EXPECT_FLOAT_EQ(product.GetZ(), 24.0f);

    const Quaternion rotation = Quaternion::FromAxisAngle(Vector3(1.0f, 2.0f, 3.0f).Normalized(), 0.7f);
    EXPECT_NEAR((rotation * Vector3(3.0f, -4.0f, 12.0f)).Length(), 13.0f, 1e-4f);
    EXPECT_NEAR(Quaternion(1.0f, 2.0f, 3.0f, 4.0f).Normalized().Length(), 1.0f, 1e-6f);
}

TEST_P(SimdDispatchTest, Vector3Operations)
{
    const std::vector<Vector3> samples = SampleVectors();
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Vector3 &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectMatchesScalar([&] { return -a; }, "Negate" + at);
        ExpectMatchesScalar([&] { return a * 2.5f; }, "ScalarMul" + at);
        ExpectMatchesScalar([&] { return a / -0.4f; }, "ScalarDiv" + at);
        ExpectMatchesScalar([&] { return a.Length(); }, "Length" + at);
        ExpectMatchesScalar([&] { return a.LengthSquared(); }, "LengthSquared" + at);
        ExpectMatchesScalar([&] { return a.Normalized(); }, "Normalized" + at);

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Vector3 &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectMatchesScalar([&] { return a + b; }, "Add" + ab);
            ExpectMatchesScalar([&] { return a - b; }, "Sub" + ab);
            ExpectMatchesScalar([&] { return a.Dot(b); }, "Dot" + ab);
            ExpectMatchesScalar([&] { return a.Cross(b); }, "Cross" + ab);
            ExpectMatchesScalar([&] { return a.Distance(b); }, "Distance" + ab);
        }
    }
}

TEST_P(SimdDispatchTest, Vector3ExactOperations)
{
    std::vector<Vector3> samples = EdgeCaseVectors();
    for (const Vector3 &v : SampleVectors())
        samples.push_back(v);

    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Vector3 &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectIdenticalToScalar([&] { return a.Floor(); }, "Floor" + at);
        ExpectIdenticalToScalar([&] { return a.Ceil(); }, "Ceil" + at);
        ExpectIdenticalToScalar([&] { return a.Round(); }, "Round" + at);
        ExpectIdenticalToScalar([&] { return a.Abs(); }, "Abs" + at);

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Vector3 &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectIdenticalToScalar([&] { return Vector3::Min(a, b); }, "Min" + ab);
            ExpectIdenticalToScalar([&] { return Vector3::Max(a, b); }, "Max" + ab);
        }
    }
}

TEST_P(SimdDispatchTest, Vector3BatchOperations)
{
    // An odd count larger than 8 exercises both the AVX batches and their scalar tails
    const std::vector<Vector3> a = SampleVectors();
    std::vector<Vector3> b = a;
    std::rotate(b.begin(), b.begin() + 3, b.end());

    ExpectMatchesScalar([&] {
        std::vector<Vector3> result(a.size());
        Vector3::AddBatch(a.data(), b.data(), result.data(), a.size());
        return result;
    }, "AddBatch");
    ExpectMatchesScalar([&] {
        std::vector<Vector3> result(a.size());
        Vector3::SubBatch(a.data(), b.data(), result.data(), a.size());
        return result;
    }, "SubBatch");
    ExpectMatchesScalar([&] {
        std::vector<Vector3> result(a.size());
        Vector3::ScalarMulBatch(a.data(), result.data(), -1.75f, a.size());
        return result;
    }, "ScalarMulBatch");
    ExpectMatchesScalar([&] {
        std::vector<float> result(a.size());
        Vector3::DotBatch(a.data(), b.data(), result.data(), a.size());
        return result;
    }, "DotBatch");
    ExpectMatchesScalar([&] {
        std::vector<Vector3> result(a.size());
        Vector3::CrossBatch(a.data(), b.data(), result.data(), a.size());
        return result;
    }, "CrossBatch");
    ExpectMatchesScalar([&] {
        std::vector<Vector3> result = a;
        Vector3::NormalizeBatch(result.data(), result.size());
        return result;
    }, "NormalizeBatch");
}

TEST_P(SimdDispatchTest, QuaternionOperations)
{
    const std::vector<Quaternion> samples = SampleQuaternions();
    const std::vector<Vector3> vectors = SampleVectors();
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Quaternion &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectMatchesScalar([&] { return a * -1.25f; }, "ScalarMul" + at);
        ExpectMatchesScalar([&] { return a.Dot(a); }, "Dot" + at);
        ExpectMatchesScalar([&] { return a.Length(); }, "Length" + at);
        ExpectMatchesScalar([&] { return a.LengthSquared(); }, "LengthSquared" + at);
        ExpectMatchesScalar([&] { return a.Normalized(); }, "Normalized" + at);
        ExpectMatchesScalar([&] { return a.Inverse(); }, "Inverse" + at);

        for (size_t v = 0; v < vectors.size(); ++v)
            ExpectMatchesScalar([&] { return a.Normalized() * vectors[v]; }, "Rotate" + at + " v=" + std::to_string(v));

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Quaternion &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectMatchesScalar([&] { return a + b; }, "Add" + ab);
            ExpectMatchesScalar([&] { return a - b; }, "Sub" + ab);
            ExpectMatchesScalar([&] { return a * b; }, "Mul" + ab);
            ExpectMatchesScalar([&] { return a.Dot(b); }, "Dot" + ab);
            ExpectMatchesScalar([&] { return Quaternion::Slerp(a.Normalized(), b.Normalized(), 0.3f); }, "Slerp" + ab);
        }
    }
}

TEST_P(SimdDispatchTest, Matrix4Operations)
{
    const std::vector<Matrix4> samples = SampleMatrices4();
    const std::vector<Vector3> points = SamplePoints();
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Matrix4 &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectMatchesScalar([&] { return a * 0.75f; }, "ScalarMul" + at);
        ExpectMatchesScalar([&] { return a.transpose(); }, "Transpose" + at);
        ExpectMatchesScalar([&] { return a.inverse(); }, "Inverse" + at);
        ExpectMatchesScalar([&] { return a.determinant(); }, "Determinant" + at);

        for (size_t p = 0; p < points.size(); ++p)
            ExpectMatchesScalar([&] { return a.TransformPoint(points[p]); }, "TransformPoint" + at + " p=" + std::to_string(p));

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Matrix4 &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectMatchesScalar([&] { return a * b; }, "Multiply" + ab);
            ExpectMatchesScalar([&] { return a + b; }, "Add" + ab);
            ExpectMatchesScalar([&] { return a - b; }, "Sub" + ab);
        }
    }
}

TEST_P(SimdDispatchTest, Matrix3Operations)
{
    const std::vector<Matrix3> samples = SampleMatrices3();
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Matrix3 &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectMatchesScalar([&] { return a * -2.5f; }, "ScalarMul" + at);
        ExpectMatchesScalar([&] { return a.transpose(); }, "Transpose" + at);

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Matrix3 &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectMatchesScalar([&] { return a * b; }, "Multiply" + ab);
            ExpectMatchesScalar([&] { return a + b; }, "Add" + ab);
            ExpectMatchesScalar([&] { return a - b; }, "Sub" + ab);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(SimdLevels, SimdDispatchTest,
                         ::testing::Values(SIMDLevel::SSE2, SIMDLevel::SSE41, SIMDLevel::AVX),
                         [](const ::testing::TestParamInfo<SIMDLevel> &info) { return LevelName(info.param); });

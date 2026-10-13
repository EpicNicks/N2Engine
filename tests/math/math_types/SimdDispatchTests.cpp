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
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

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
        case SIMDLevel::NEON:
            return "NEON";
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

    void ExpectClose(const Vector4 &actual, const Vector4 &expected, const std::string &what)
    {
        const float scale = std::max({std::abs(expected.w), std::abs(expected.x), std::abs(expected.y),
                                      std::abs(expected.z)});
        for (size_t i = 0; i < 4; ++i)
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
    void ExpectIdentical(const float actual, const float expected, const std::string &what)
    {
        if (std::isnan(actual) && std::isnan(expected))
            return;
        EXPECT_EQ(std::bit_cast<uint32_t>(actual), std::bit_cast<uint32_t>(expected))
            << what << ": " << actual << " vs " << expected;
    }

    void ExpectIdentical(const Vector3 &actual, const Vector3 &expected, const std::string &what)
    {
        for (size_t i = 0; i < 3; ++i)
            ExpectIdentical(actual[i], expected[i], what + "[" + std::to_string(i) + "]");
    }

    void ExpectIdentical(const Vector4 &actual, const Vector4 &expected, const std::string &what)
    {
        for (size_t i = 0; i < 4; ++i)
            ExpectIdentical(actual[i], expected[i], what + "[" + std::to_string(i) + "]");
    }

    // Like ExpectClose, but NaN must meet NaN and an infinity must match exactly: EXPECT_NEAR fails on both. The
    // scale only counts the finite components, since an infinite one would make every tolerance infinite
    void ExpectCloseOrSpecial(const float actual, const float expected, const std::string &what, const float scale = 1.0f)
    {
        if (std::isnan(expected) || std::isinf(expected))
            ExpectIdentical(actual, expected, what);
        else
            ExpectClose(actual, expected, what, scale);
    }

    float FiniteScale(const std::initializer_list<float> values)
    {
        float scale = 0.0f;
        for (const float value : values)
        {
            if (std::isfinite(value))
                scale = std::max(scale, std::abs(value));
        }
        return scale;
    }

    void ExpectCloseOrSpecial(const Vector3 &actual, const Vector3 &expected, const std::string &what)
    {
        const float scale = FiniteScale({expected.x, expected.y, expected.z});
        for (size_t i = 0; i < 3; ++i)
            ExpectCloseOrSpecial(actual[i], expected[i], what + "[" + std::to_string(i) + "]", scale);
    }

    void ExpectCloseOrSpecial(const Vector4 &actual, const Vector4 &expected, const std::string &what)
    {
        const float scale = FiniteScale({expected.w, expected.x, expected.y, expected.z});
        for (size_t i = 0; i < 4; ++i)
            ExpectCloseOrSpecial(actual[i], expected[i], what + "[" + std::to_string(i) + "]", scale);
    }

    // Restores the default (scalar) dispatch even when an assertion fails and returns early
    struct ScalarDispatchGuard
    {
        ~ScalarDispatchGuard()
        {
            SetSIMDLevel(SIMDLevel::Scalar);
        }
    };

    // Deterministic, varied values with garbage in Vector3's padding lane
    std::vector<Vector3> VectorPool3(const size_t size)
    {
        std::vector<Vector3> pool;
        for (size_t i = 0; i < size; ++i)
        {
            const float f = static_cast<float>(i);
            Vector3 v(std::sin(f * 1.3f) * 10.0f, std::cos(f * 0.7f) * 5.0f, f * 0.25f - 3.0f);
            v.w = 100.0f + f;
            pool.push_back(v);
        }
        pool[3] = Vector3(0.0f, 0.0f, 0.0f);
        pool[9] = Vector3(1e-6f, -1e-6f, 1e-6f);
        pool[11] = Vector3(-0.0f, -0.0f, -0.0f);
        return pool;
    }

    std::vector<Vector4> VectorPool4(const size_t size)
    {
        std::vector<Vector4> pool;
        for (size_t i = 0; i < size; ++i)
        {
            const float f = static_cast<float>(i);
            pool.emplace_back(std::sin(f * 1.3f) * 10.0f, std::cos(f * 0.7f) * 5.0f, f * 0.25f - 3.0f, std::sin(f) - 0.5f);
        }
        pool[3] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        pool[9] = Vector4(1e-6f, -1e-6f, 1e-6f, -1e-6f);
        pool[11] = Vector4(-0.0f, -0.0f, -0.0f, -0.0f);
        return pool;
    }

    constexpr size_t BATCH_POOL_SIZE = 64;

    // Counts around the NEON four-vector and the AVX two- and eight-vector batch widths, including none
    constexpr size_t BATCH_COUNTS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 15, 16, 17, 31};

    // Input arrays start at these element offsets into the pool, so they do not start at the pool's first element
    constexpr size_t BATCH_OFFSETS[] = {0, 1, 3};

    template <typename T>
    void ExpectIdentical(const std::vector<T> &actual, const std::vector<T> &expected, const std::string &what)
    {
        ASSERT_EQ(actual.size(), expected.size()) << what;
        for (size_t i = 0; i < actual.size(); ++i)
            ExpectIdentical(actual[i], expected[i], what + "[" + std::to_string(i) + "]");
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

    // Like SampleVectors, but every lane is a real component (w is lane 0)
    std::vector<Vector4> SampleVectors4()
    {
        return {
            {1.0f, 2.0f, 3.0f, 4.0f},
            {5.0f, 6.0f, 7.0f, 8.0f},
            {0.0f, 3.0f, 0.0f, 4.0f},
            {-2.5f, 0.5f, 1.5f, -0.5f},
            {2.5f, -0.5f, -1.5f, 3.5f},
            {0.1f, -7.3f, 2.2f, 9.9f},
            {1e-3f, 2e-3f, -3e-3f, 4e-3f},
            {-1250.0f, 42.0f, 0.75f, -3.0f},
            {0.0f, 0.0f, 0.0f, 0.0f},
            {-0.3f, 0.49f, 7.51f, -7.51f},
            {0.0f, 0.0f, 0.0f, 10.0f},
        };
    }

    // The Vector3 edge values, spread over all four lanes
    std::vector<Vector4> EdgeCaseVectors4()
    {
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        return {
            {-0.0f, 0.0f, -0.3f, 0.3f},
            {0.0f, -0.0f, 0.3f, -0.3f},
            {0.49999997f, -0.49999997f, -0.5f, 0.5f},
            {8388609.0f, -8388609.0f, 1e10f, -1e10f},
            {-inf, inf, -1e10f, 1.5f},
            {nan, 1.0f, -2.5f, -0.0f},
            {2.5f, nan, -0.0f, 0.0f},
            {-7.5f, 3.5f, nan, -inf},
            {0.0f, -0.0f, 1.0f, nan},
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
        if (!IsSIMDLevelSupported(GetParam()))
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

    // For inputs with NaN and infinities, where ExpectClose cannot compare
    template <typename F>
    void ExpectSimilarToScalar(F compute, const std::string &what)
    {
        const auto expected = Under(SIMDLevel::Scalar, compute);
        const auto actual = Under(GetParam(), compute);
        ExpectCloseOrSpecial(actual, expected, what);
    }

    template <typename F>
    void ExpectIdenticalToScalar(F compute, const std::string &what)
    {
        const auto expected = Under(SIMDLevel::Scalar, compute);
        const auto actual = Under(GetParam(), compute);
        ExpectIdentical(actual, expected, what);
    }
};

TEST_P(SimdDispatchTest, SelectsRequestedLevel)
{
    // The tests below only cover the tier they name if the dispatch really switched to it
    SetSIMDLevel(GetParam());
    EXPECT_EQ(Vector3::GetSIMDLevel(), GetParam());
    EXPECT_EQ(Vector4::GetSIMDLevel(), GetParam());
    EXPECT_EQ(ClampSIMDLevel(GetParam()), GetParam());
}

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

TEST_P(SimdDispatchTest, Vector4KnownValues)
{
    SetSIMDLevel(GetParam());
    const float nan = std::numeric_limits<float>::quiet_NaN();

    EXPECT_FLOAT_EQ(Vector4(1.0f, 2.0f, 3.0f, 4.0f).Dot(Vector4(5.0f, 6.0f, 7.0f, 8.0f)), 70.0f);
    EXPECT_FLOAT_EQ(Vector4(1.0f, 2.0f, 2.0f, 4.0f).Length(), 5.0f);
    EXPECT_FLOAT_EQ(Vector4(1.0f, 2.0f, 2.0f, 4.0f).Distance(Vector4(2.0f, 4.0f, 4.0f, 8.0f)), 5.0f);

    // A real sqrt: rsqrt was only about 12 bits accurate
    const Vector4 normalized = Vector4(0.0f, 3.0f, 0.0f, 4.0f).Normalized();
    EXPECT_NEAR(normalized.x, 0.6f, 1e-7f);
    EXPECT_NEAR(normalized.z, 0.8f, 1e-7f);
    EXPECT_NEAR(Vector4(1.0f, -2.0f, 3.0f, 0.5f).Normalized().Length(), 1.0f, 1e-6f);
    ExpectIdentical(Vector4(1e-7f, 0.0f, 0.0f, 0.0f).Normalized(), Vector4::Zero, "Normalized near zero");

    std::vector<Vector4> batch = {{0.0f, 3.0f, 0.0f, 4.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f, 0.0f}};
    Vector4::NormalizeBatch(batch);
    ExpectClose(batch[0], Vector4(0.0f, 0.6f, 0.0f, 0.8f), "NormalizeBatch[0]");
    ExpectIdentical(batch[1], Vector4::Zero, "NormalizeBatch[1]");
    ExpectClose(batch[2], Vector4::UnitW, "NormalizeBatch[2]");

    // Halves round away from zero like std::round (not to even), and -0.3 keeps its sign
    ExpectIdentical(Vector4(2.5f, -2.5f, 0.5f, -0.3f).Round(), Vector4(3.0f, -3.0f, 1.0f, -0.0f), "Round");

    // std::min(a, b) is (b < a) ? b : a and std::max(a, b) is (a < b) ? b : a: ties and NaN give a
    const Vector4 a(-0.0f, 1.0f, nan, 3.0f);
    const Vector4 b(0.0f, 2.0f, 1.0f, nan);
    ExpectIdentical(Vector4::Min(a, b), Vector4(-0.0f, 1.0f, nan, 3.0f), "Min");
    ExpectIdentical(Vector4::Max(a, b), Vector4(-0.0f, 2.0f, nan, 3.0f), "Max");

    // Negation flips the sign of zero
    ExpectIdentical(-Vector4(0.0f, 1.0f, -2.0f, -0.0f), Vector4(-0.0f, -1.0f, 2.0f, 0.0f), "Negate");
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

TEST_P(SimdDispatchTest, Vector3EdgeCaseOperations)
{
    // Zero, signed zeros, a vector below the normalize threshold, infinities and NaN in each lane. Whatever the scalar
    // code gives (NaN included) the tier must give too
    std::vector<Vector3> samples = EdgeCaseVectors();
    samples.emplace_back(0.0f, 0.0f, 0.0f);
    samples.emplace_back(-0.0f, -0.0f, -0.0f);
    samples.emplace_back(1e-6f, -1e-6f, 1e-6f);
    samples.emplace_back(1e-30f, 0.0f, -1e-30f);
    samples.emplace_back(3.0f, 4.0f, 12.0f);

    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Vector3 &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectSimilarToScalar([&] { return -a; }, "Negate" + at);
        ExpectSimilarToScalar([&] { return a * 2.5f; }, "ScalarMul" + at);
        ExpectSimilarToScalar([&] { return a.Length(); }, "Length" + at);
        ExpectSimilarToScalar([&] { return a.Normalized(); }, "Normalized" + at);

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Vector3 &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectSimilarToScalar([&] { return a + b; }, "Add" + ab);
            ExpectSimilarToScalar([&] { return a - b; }, "Sub" + ab);
            ExpectSimilarToScalar([&] { return a.Dot(b); }, "Dot" + ab);
            ExpectSimilarToScalar([&] { return a.Cross(b); }, "Cross" + ab);
            ExpectSimilarToScalar([&] { return a.Distance(b); }, "Distance" + ab);
        }
    }

    // Short, zero and negative-zero vectors normalize to exactly Zero
    SetSIMDLevel(GetParam());
    ExpectIdentical(Vector3(0.0f, 0.0f, 0.0f).Normalized(), Vector3::Zero, "Normalized zero");
    ExpectIdentical(Vector3(-0.0f, -0.0f, -0.0f).Normalized(), Vector3::Zero, "Normalized negative zero");
    ExpectIdentical(Vector3(1e-6f, -1e-6f, 1e-6f).Normalized(), Vector3::Zero, "Normalized tiny");
    ExpectIdentical(Vector3(1.0f, 2.0f, 3.0f).Cross(Vector3(1.0f, 2.0f, 3.0f)), Vector3::Zero, "Cross self");
    ExpectIdentical(Vector3(1.0f, 0.0f, 0.0f).Cross(Vector3(0.0f, 1.0f, 0.0f)), Vector3(0.0f, 0.0f, 1.0f), "Cross x y");

    // The padding lane is zero in every result, whatever the inputs hold there
    Vector3 padded(1.0f, 2.0f, 3.0f);
    padded.w = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ((padded + padded).w, 0.0f);
    EXPECT_EQ((padded * 2.0f).w, 0.0f);
    EXPECT_EQ(padded.Cross(padded).w, 0.0f);
    EXPECT_EQ(padded.Normalized().w, 0.0f);
    EXPECT_EQ(Vector3::Min(padded, padded).w, 0.0f);
    EXPECT_EQ(padded.Round().w, 0.0f);
    EXPECT_FLOAT_EQ(padded.Dot(padded), 14.0f);
}

TEST_P(SimdDispatchTest, Vector4EdgeCaseOperations)
{
    std::vector<Vector4> samples = EdgeCaseVectors4();
    samples.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
    samples.emplace_back(-0.0f, -0.0f, -0.0f, -0.0f);
    samples.emplace_back(1e-6f, -1e-6f, 1e-6f, 1e-6f);
    samples.emplace_back(1e-30f, 0.0f, -1e-30f, 0.0f);

    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Vector4 &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectSimilarToScalar([&] { return a * 2.5f; }, "ScalarMul" + at);
        ExpectSimilarToScalar([&] { return a.Length(); }, "Length" + at);
        ExpectSimilarToScalar([&] { return a.Normalized(); }, "Normalized" + at);

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Vector4 &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectSimilarToScalar([&] { return a + b; }, "Add" + ab);
            ExpectSimilarToScalar([&] { return a - b; }, "Sub" + ab);
            ExpectSimilarToScalar([&] { return a.Scale(b); }, "Scale" + ab);
            ExpectSimilarToScalar([&] { return a.Dot(b); }, "Dot" + ab);
        }
    }

    SetSIMDLevel(GetParam());
    ExpectIdentical(Vector4(0.0f, 0.0f, 0.0f, 0.0f).Normalized(), Vector4::Zero, "Normalized zero");
    ExpectIdentical(Vector4(-0.0f, -0.0f, -0.0f, -0.0f).Normalized(), Vector4::Zero, "Normalized negative zero");
    ExpectIdentical(Vector4(1e-6f, -1e-6f, 1e-6f, 1e-6f).Normalized(), Vector4::Zero, "Normalized tiny");
}

TEST_P(SimdDispatchTest, Vector3BatchCountsAndOffsets)
{
    // Every count around the batch widths, on arrays that start part-way into the pool, with two sentinel elements
    // after the last result that must stay untouched (the scalar run leaves them, so any write shows as a mismatch)
    const std::vector<Vector3> pool = VectorPool3(BATCH_POOL_SIZE);
    const Vector3 sentinel(99.0f, 98.0f, 97.0f);

    for (const size_t count : BATCH_COUNTS)
    {
        for (const size_t offset : BATCH_OFFSETS)
        {
            const Vector3 *a = pool.data() + offset;
            const Vector3 *b = pool.data() + 20 + offset;
            const std::string at = " count=" + std::to_string(count) + " offset=" + std::to_string(offset);

            const auto vectorResult = [&](auto batch) {
                std::vector<Vector3> result(count + 2, sentinel);
                batch(result.data());
                return result;
            };

            ExpectMatchesScalar([&] { return vectorResult([&](Vector3 *out) { Vector3::AddBatch(a, b, out, count); }); }, "AddBatch" + at);
            ExpectMatchesScalar([&] { return vectorResult([&](Vector3 *out) { Vector3::SubBatch(a, b, out, count); }); }, "SubBatch" + at);
            ExpectMatchesScalar([&] { return vectorResult([&](Vector3 *out) { Vector3::CrossBatch(a, b, out, count); }); }, "CrossBatch" + at);
            ExpectMatchesScalar([&] { return vectorResult([&](Vector3 *out) { Vector3::ScalarMulBatch(a, out, 0.75f, count); }); }, "ScalarMulBatch" + at);
            ExpectMatchesScalar([&] {
                std::vector<float> result(count + 2, -5.0f);
                Vector3::DotBatch(a, b, result.data(), count);
                return result;
            }, "DotBatch" + at);

            // In place: the output is the input
            ExpectMatchesScalar([&] {
                std::vector<Vector3> result(a, a + count);
                result.insert(result.end(), 2, sentinel);
                Vector3::NormalizeBatch(result.data(), count);
                return result;
            }, "NormalizeBatch" + at);
            ExpectMatchesScalar([&] {
                std::vector<Vector3> result(a, a + count);
                result.insert(result.end(), 2, sentinel);
                Vector3::AddBatch(result.data(), b, result.data(), count);
                return result;
            }, "AddBatch in place" + at);
            ExpectMatchesScalar([&] {
                std::vector<Vector3> result(a, a + count);
                result.insert(result.end(), 2, sentinel);
                Vector3::ScalarMulBatch(result.data(), result.data(), -2.5f, count);
                return result;
            }, "ScalarMulBatch in place" + at);
        }
    }
}

TEST_P(SimdDispatchTest, Vector4BatchCountsAndOffsets)
{
    const std::vector<Vector4> pool = VectorPool4(BATCH_POOL_SIZE);
    const Vector4 sentinel(99.0f, 98.0f, 97.0f, 96.0f);

    for (const size_t count : BATCH_COUNTS)
    {
        for (const size_t offset : BATCH_OFFSETS)
        {
            const Vector4 *a = pool.data() + offset;
            const Vector4 *b = pool.data() + 20 + offset;
            const std::string at = " count=" + std::to_string(count) + " offset=" + std::to_string(offset);

            const auto vectorResult = [&](auto batch) {
                std::vector<Vector4> result(count + 2, sentinel);
                batch(result.data());
                return result;
            };
            const auto floatResult = [&](auto batch) {
                std::vector<float> result(count + 2, -5.0f);
                batch(result.data());
                return result;
            };

            ExpectMatchesScalar([&] { return vectorResult([&](Vector4 *out) { Vector4::AddBatch(a, b, out, count); }); }, "AddBatch" + at);
            ExpectMatchesScalar([&] { return vectorResult([&](Vector4 *out) { Vector4::SubBatch(a, b, out, count); }); }, "SubBatch" + at);
            ExpectMatchesScalar([&] { return vectorResult([&](Vector4 *out) { Vector4::ScalarMulBatch(a, out, 0.75f, count); }); }, "ScalarMulBatch" + at);
            ExpectMatchesScalar([&] { return floatResult([&](float *out) { Vector4::DotBatch(a, b, out, count); }); }, "DotBatch" + at);
            ExpectMatchesScalar([&] { return floatResult([&](float *out) { Vector4::LengthBatch(a, out, count); }); }, "LengthBatch" + at);

            // In place: the output is the input
            ExpectMatchesScalar([&] {
                std::vector<Vector4> result(a, a + count);
                result.insert(result.end(), 2, sentinel);
                Vector4::NormalizeBatch(result.data(), count);
                return result;
            }, "NormalizeBatch" + at);
            ExpectMatchesScalar([&] {
                std::vector<Vector4> result(a, a + count);
                result.insert(result.end(), 2, sentinel);
                Vector4::AddBatch(result.data(), b, result.data(), count);
                return result;
            }, "AddBatch in place" + at);
        }
    }
}

TEST_P(SimdDispatchTest, Vector4Operations)
{
    const std::vector<Vector4> samples = SampleVectors4();
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Vector4 &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectMatchesScalar([&] { return -a; }, "Negate" + at);
        ExpectMatchesScalar([&] { return a * 2.5f; }, "ScalarMul" + at);
        ExpectMatchesScalar([&] { return a / -0.4f; }, "ScalarDiv" + at);
        ExpectMatchesScalar([&] { return a.Length(); }, "Length" + at);
        ExpectMatchesScalar([&] { return a.LengthSquared(); }, "LengthSquared" + at);
        ExpectMatchesScalar([&] { return a.Normalized(); }, "Normalized" + at);
        ExpectMatchesScalar([&] { return a.ClampMagnitude(2.0f); }, "ClampMagnitude" + at);

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Vector4 &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectMatchesScalar([&] { return a + b; }, "Add" + ab);
            ExpectMatchesScalar([&] { return a - b; }, "Sub" + ab);
            ExpectMatchesScalar([&] { return a.Scale(b); }, "Scale" + ab);
            ExpectMatchesScalar([&] { return a.Dot(b); }, "Dot" + ab);
            ExpectMatchesScalar([&] { return a.Distance(b); }, "Distance" + ab);
            ExpectMatchesScalar([&] { return a.DistanceSquared(b); }, "DistanceSquared" + ab);
            ExpectMatchesScalar([&] { return Vector4::Lerp(a, b, 0.3f); }, "Lerp" + ab);
        }
    }
}

TEST_P(SimdDispatchTest, Vector4ExactOperations)
{
    std::vector<Vector4> samples = EdgeCaseVectors4();
    for (const Vector4 &v : SampleVectors4())
        samples.push_back(v);

    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Vector4 &a = samples[i];
        const std::string at = " a=" + std::to_string(i);

        ExpectIdenticalToScalar([&] { return a.Floor(); }, "Floor" + at);
        ExpectIdenticalToScalar([&] { return a.Ceil(); }, "Ceil" + at);
        ExpectIdenticalToScalar([&] { return a.Round(); }, "Round" + at);
        ExpectIdenticalToScalar([&] { return a.Abs(); }, "Abs" + at);
        ExpectIdenticalToScalar([&] { return -a; }, "Negate" + at);

        for (size_t j = 0; j < samples.size(); ++j)
        {
            const Vector4 &b = samples[j];
            const std::string ab = at + " b=" + std::to_string(j);

            ExpectIdenticalToScalar([&] { return Vector4::Min(a, b); }, "Min" + ab);
            ExpectIdenticalToScalar([&] { return Vector4::Max(a, b); }, "Max" + ab);
            ExpectIdenticalToScalar([&] { return a.Clamp(b, samples[0]); }, "Clamp" + ab);
        }
    }
}

TEST_P(SimdDispatchTest, Vector4BatchOperations)
{
    // An odd count exercises both the AVX pairs and the single-vector tail
    const std::vector<Vector4> a = SampleVectors4();
    std::vector<Vector4> b = a;
    std::rotate(b.begin(), b.begin() + 3, b.end());

    ExpectMatchesScalar([&] { return Vector4::AddBatch(a, b); }, "AddBatch");
    ExpectMatchesScalar([&] { return Vector4::SubBatch(a, b); }, "SubBatch");
    ExpectMatchesScalar([&] { return Vector4::ScalarMulBatch(a, -1.75f); }, "ScalarMulBatch");
    ExpectMatchesScalar([&] { return Vector4::DotBatch(a, b); }, "DotBatch");
    ExpectMatchesScalar([&] { return Vector4::LengthBatch(a); }, "LengthBatch");
    ExpectMatchesScalar([&] {
        std::vector<Vector4> result = a;
        Vector4::NormalizeBatch(result);
        return result;
    }, "NormalizeBatch");

    // Edge values (an odd count: AVX pairs plus a tail). Every tier sums the dot in the same order, so the results
    // match scalar bit for bit, and NaN or inf inputs must give NaN where scalar does (not zero)
    const std::vector<Vector4> edges = EdgeCaseVectors4();
    std::vector<Vector4> rotated = edges;
    std::rotate(rotated.begin(), rotated.begin() + 2, rotated.end());

    ExpectIdenticalToScalar([&] { return Vector4::DotBatch(edges, rotated); }, "DotBatch edges");
    ExpectIdenticalToScalar([&] { return Vector4::LengthBatch(edges); }, "LengthBatch edges");
    ExpectIdenticalToScalar([&] {
        std::vector<Vector4> result = edges;
        Vector4::NormalizeBatch(result);
        return result;
    }, "NormalizeBatch edges");
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

// The tiers this target can have: the x86 tiers on x86, NEON on aarch64 (the other family's tiers are clamped to
// scalar there, and a test for them would silently cover scalar)
#ifdef N2_MATH_NEON
INSTANTIATE_TEST_SUITE_P(SimdLevels, SimdDispatchTest,
                         ::testing::Values(SIMDLevel::NEON),
                         [](const ::testing::TestParamInfo<SIMDLevel> &info) { return LevelName(info.param); });
#else
INSTANTIATE_TEST_SUITE_P(SimdLevels, SimdDispatchTest,
                         ::testing::Values(SIMDLevel::SSE2, SIMDLevel::SSE41, SIMDLevel::AVX),
                         [](const ::testing::TestParamInfo<SIMDLevel> &info) { return LevelName(info.param); });
#endif

// ============================================================================
// Tier selection and clamping
// ============================================================================

TEST(SimdLevelTest, DetectedLevelMatchesTheTarget)
{
#ifdef N2_MATH_NEON
    // NEON is baseline on aarch64, so the arm64 CI job runs every SimdLevels test below at the NEON tier
    EXPECT_EQ(DetectSIMDLevel(), SIMDLevel::NEON);
    EXPECT_TRUE(IsSIMDLevelSupported(SIMDLevel::NEON));
    EXPECT_FALSE(IsSIMDLevelSupported(SIMDLevel::SSE2));
    EXPECT_FALSE(IsSIMDLevelSupported(SIMDLevel::SSE41));
    EXPECT_FALSE(IsSIMDLevelSupported(SIMDLevel::AVX));
#else
    EXPECT_NE(DetectSIMDLevel(), SIMDLevel::NEON);
    EXPECT_FALSE(IsSIMDLevelSupported(SIMDLevel::NEON));
#endif
    EXPECT_TRUE(IsSIMDLevelSupported(SIMDLevel::Scalar));
    EXPECT_TRUE(IsSIMDLevelSupported(DetectSIMDLevel()));
}

TEST(SimdLevelTest, ClampLowersToWhatTheTargetSupports)
{
    EXPECT_EQ(ClampSIMDLevel(SIMDLevel::Scalar), SIMDLevel::Scalar);
#ifdef N2_MATH_NEON
    EXPECT_EQ(ClampSIMDLevel(SIMDLevel::NEON), SIMDLevel::NEON);
    // The x86 tiers do not exist on aarch64
    EXPECT_EQ(ClampSIMDLevel(SIMDLevel::SSE2), SIMDLevel::Scalar);
    EXPECT_EQ(ClampSIMDLevel(SIMDLevel::SSE41), SIMDLevel::Scalar);
    EXPECT_EQ(ClampSIMDLevel(SIMDLevel::AVX), SIMDLevel::Scalar);
#else
    // NEON does not exist on x86; a request above the CPU's tier lowers to the CPU's
    EXPECT_EQ(ClampSIMDLevel(SIMDLevel::NEON), SIMDLevel::Scalar);
    EXPECT_EQ(ClampSIMDLevel(SIMDLevel::AVX), DetectSIMDLevel());
    EXPECT_EQ(ClampSIMDLevel(DetectSIMDLevel()), DetectSIMDLevel());
#endif
}

TEST(SimdLevelTest, SetSIMDLevelAppliesTheClampedTier)
{
    const ScalarDispatchGuard guard;
    for (const SIMDLevel requested : {SIMDLevel::Scalar, SIMDLevel::SSE2, SIMDLevel::SSE41, SIMDLevel::AVX, SIMDLevel::NEON})
    {
        SetSIMDLevel(requested);
        EXPECT_EQ(Vector3::GetSIMDLevel(), ClampSIMDLevel(requested)) << LevelName(requested);
        EXPECT_EQ(Vector4::GetSIMDLevel(), ClampSIMDLevel(requested)) << LevelName(requested);
    }
}

TEST(SimdLevelTest, InitializeSIMDSelectsTheDetectedTier)
{
    const ScalarDispatchGuard guard;
    InitializeSIMD();
    EXPECT_EQ(Vector3::GetSIMDLevel(), DetectSIMDLevel());
    EXPECT_EQ(Vector4::GetSIMDLevel(), DetectSIMDLevel());
}

// ============================================================================
// Vector2 batch normalization (compile-time AVX, no runtime dispatch)
// ============================================================================

TEST(Vector2BatchTest, NormalizeBatchMatchesNormalized)
{
    // 11 vectors: two full AVX batches of four plus a scalar tail of three
    const std::vector<Vector2> samples = {
        {3.0f, 4.0f}, {-1.0f, 0.0f}, {0.1f, -0.7f}, {1234.5f, -6789.25f},
        {1e-3f, 2e-3f}, {-5.5f, -5.5f}, {0.0f, 2.0f}, {7.0f, 1e-4f},
        {2.0f, -3.0f}, {-0.25f, 0.5f}, {100.0f, 0.0f},
    };
    std::vector<Vector2> batch = samples;
    Vector2::NormalizeBatch(batch.data(), batch.size());

    for (size_t i = 0; i < samples.size(); ++i)
    {
        const Vector2 expected = samples[i].Normalized();
        const std::string at = "vector " + std::to_string(i);
        // Full precision: the 12-bit rsqrt approximation this replaced was off by ~1e-4
        EXPECT_NEAR(batch[i].x, expected.x, 1e-6f) << at;
        EXPECT_NEAR(batch[i].y, expected.y, 1e-6f) << at;
        EXPECT_NEAR(batch[i].Length(), 1.0f, 1e-6f) << at;
    }
}

TEST(Vector2BatchTest, NormalizeBatchZeroAndNaN)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Each special value in an AVX batch (first four) and in the scalar tail (last two)
    std::vector<Vector2> batch = {
        {0.0f, 0.0f}, {1e-9f, -1e-9f}, {nan, 1.0f}, {0.0f, -3.0f},
        {0.0f, 0.0f}, {nan, nan},
    };
    Vector2::NormalizeBatch(batch.data(), batch.size());

    for (const size_t i : {size_t{0}, size_t{1}, size_t{4}})
    {
        EXPECT_EQ(batch[i].x, 0.0f) << "near-zero vector " << i << " becomes Zero";
        EXPECT_EQ(batch[i].y, 0.0f) << "near-zero vector " << i << " becomes Zero";
    }
    for (const size_t i : {size_t{2}, size_t{5}})
    {
        EXPECT_TRUE(std::isnan(batch[i].x)) << "NaN vector " << i << " stays NaN, as Normalized() keeps it";
        EXPECT_TRUE(std::isnan(Vector2(batch[i]).Normalized().x));
    }
    EXPECT_NEAR(batch[3].x, 0.0f, 1e-6f);
    EXPECT_NEAR(batch[3].y, -1.0f, 1e-6f);
}

TEST(Vector2BatchTest, NormalizeBatchHandlesShortCounts)
{
    for (size_t count = 0; count <= 9; ++count)
    {
        std::vector<Vector2> batch(count, Vector2{6.0f, -8.0f});
        Vector2::NormalizeBatch(batch.data(), count);
        for (size_t i = 0; i < count; ++i)
        {
            EXPECT_NEAR(batch[i].x, 0.6f, 1e-6f) << "count " << count << " index " << i;
            EXPECT_NEAR(batch[i].y, -0.8f, 1e-6f) << "count " << count << " index " << i;
        }
    }
}

// Every count around the batch widths (two per NEON register, four per AVX register), for the batches the tests above
// do not cover. Starting at element 1 puts the data at 8 bytes past 16-byte alignment (Vector2 only needs 4)
TEST(Vector2BatchTest, BatchesMatchScalarForEveryCountAndOffset)
{
    std::vector<Vector2> pool;
    for (size_t i = 0; i < 40; ++i)
    {
        const float f = static_cast<float>(i);
        pool.emplace_back(std::sin(f * 1.7f) * 8.0f, std::cos(f * 0.9f) * 3.0f - 1.0f);
    }
    pool[2] = Vector2(0.0f, 0.0f);
    pool[5] = Vector2(1e-6f, -1e-6f);
    pool[7] = Vector2(-0.0f, -0.0f);

    const Vector2 sentinel(99.0f, 98.0f);
    for (size_t count = 0; count <= 11; ++count)
    {
        for (const size_t offset : {size_t{0}, size_t{1}})
        {
            const Vector2 *a = pool.data() + offset;
            const Vector2 *b = pool.data() + 15 + offset;
            const std::string at = " count=" + std::to_string(count) + " offset=" + std::to_string(offset);

            std::vector<Vector2> sum(count + 2, sentinel), difference(count + 2, sentinel), scaled(count + 2, sentinel);
            std::vector<float> dots(count + 2, -5.0f), lengths(count + 2, -5.0f);
            Vector2::AddBatch(a, b, sum.data(), count);
            Vector2::SubBatch(a, b, difference.data(), count);
            Vector2::ScalarMulBatch(a, scaled.data(), -1.5f, count);
            Vector2::DotBatch(a, b, dots.data(), count);
            Vector2::LengthBatch(a, lengths.data(), count);

            std::vector<Vector2> normalized(a, a + count);
            normalized.insert(normalized.end(), 2, sentinel);
            Vector2::NormalizeBatch(normalized.data(), count);

            for (size_t i = 0; i < count + 2; ++i)
            {
                const std::string where = at + " index=" + std::to_string(i);
                if (i >= count)
                {
                    // Past the last element nothing is written
                    EXPECT_EQ(sum[i].x, sentinel.x) << where;
                    EXPECT_EQ(difference[i].y, sentinel.y) << where;
                    EXPECT_EQ(scaled[i].x, sentinel.x) << where;
                    EXPECT_EQ(dots[i], -5.0f) << where;
                    EXPECT_EQ(lengths[i], -5.0f) << where;
                    EXPECT_EQ(normalized[i].x, sentinel.x) << where;
                    continue;
                }

                const Vector2 expectedSum = a[i] + b[i];
                const Vector2 expectedDifference = a[i] - b[i];
                const Vector2 expectedScaled = a[i] * -1.5f;
                const Vector2 expectedNormalized = a[i].Normalized();
                ExpectClose(sum[i].x, expectedSum.x, "Add x" + where);
                ExpectClose(sum[i].y, expectedSum.y, "Add y" + where);
                ExpectClose(difference[i].x, expectedDifference.x, "Sub x" + where);
                ExpectClose(difference[i].y, expectedDifference.y, "Sub y" + where);
                ExpectClose(scaled[i].x, expectedScaled.x, "ScalarMul x" + where);
                ExpectClose(scaled[i].y, expectedScaled.y, "ScalarMul y" + where);
                ExpectClose(dots[i], a[i].Dot(b[i]), "Dot" + where);
                ExpectClose(lengths[i], a[i].Length(), "Length" + where);
                ExpectClose(normalized[i].x, expectedNormalized.x, "Normalize x" + where);
                ExpectClose(normalized[i].y, expectedNormalized.y, "Normalize y" + where);
            }
        }
    }
}

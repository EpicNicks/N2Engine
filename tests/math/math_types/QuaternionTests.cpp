#include <gtest/gtest.h>
#include <cmath>
#include <numbers>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <math/Matrix.hpp>
#include <math/Constants.hpp>

using namespace N2Engine::Math;

class QuaternionTest : public ::testing::Test
{
protected:
    static constexpr float EPSILON = Constants::EPSILON;

    static bool NearEqual(float a, float b, float epsilon = EPSILON)
    {
        return std::abs(a - b) < epsilon;
    }

    static bool NearEqual(const Quaternion& a, const Quaternion& b, float epsilon = EPSILON)
    {
        return NearEqual(a.GetW(), b.GetW(), epsilon) &&
               NearEqual(a.GetX(), b.GetX(), epsilon) &&
               NearEqual(a.GetY(), b.GetY(), epsilon) &&
               NearEqual(a.GetZ(), b.GetZ(), epsilon);
    }

    static bool NearEqual(const Vector3& a, const Vector3& b, float epsilon = EPSILON)
    {
        return NearEqual(a.x, b.x, epsilon) &&
               NearEqual(a.y, b.y, epsilon) &&
               NearEqual(a.z, b.z, epsilon);
    }

    // Quaternions can represent the same rotation with opposite signs
    static bool NearEqualRotation(const Quaternion& a, const Quaternion& b, float epsilon = EPSILON)
    {
        return NearEqual(a, b, epsilon) ||
               NearEqual(a, Quaternion(-b.GetW(), -b.GetX(), -b.GetY(), -b.GetZ()), epsilon);
    }
};

// Constructor Tests
TEST_F(QuaternionTest, DefaultConstructor_IsIdentity)
{
    Quaternion q;

    EXPECT_FLOAT_EQ(q.GetW(), 1.0f);
    EXPECT_FLOAT_EQ(q.GetX(), 0.0f);
    EXPECT_FLOAT_EQ(q.GetY(), 0.0f);
    EXPECT_FLOAT_EQ(q.GetZ(), 0.0f);
}

TEST_F(QuaternionTest, ComponentConstructor)
{
    Quaternion q(0.5f, 0.5f, 0.5f, 0.5f);

    EXPECT_FLOAT_EQ(q.GetW(), 0.5f);
    EXPECT_FLOAT_EQ(q.GetX(), 0.5f);
    EXPECT_FLOAT_EQ(q.GetY(), 0.5f);
    EXPECT_FLOAT_EQ(q.GetZ(), 0.5f);
}

TEST_F(QuaternionTest, AxisAngleConstructor_XAxis90Degrees)
{
    Vector3 axis(1.0f, 0.0f, 0.0f);
    float angle = std::numbers::pi_v<float> / 2.0f; // 90 degrees

    Quaternion q(axis, angle);

    float expected_w = std::cos(angle / 2.0f);
    float expected_x = std::sin(angle / 2.0f);

    EXPECT_TRUE(NearEqual(q.GetW(), expected_w));
    EXPECT_TRUE(NearEqual(q.GetX(), expected_x));
    EXPECT_TRUE(NearEqual(q.GetY(), 0.0f));
    EXPECT_TRUE(NearEqual(q.GetZ(), 0.0f));
}

TEST_F(QuaternionTest, AxisAngleConstructor_YAxis180Degrees)
{
    Vector3 axis(0.0f, 1.0f, 0.0f);
    float angle = std::numbers::pi_v<float>; // 180 degrees

    Quaternion q(axis, angle);

    EXPECT_TRUE(NearEqual(q.GetW(), 0.0f));
    EXPECT_TRUE(NearEqual(q.GetX(), 0.0f));
    EXPECT_TRUE(NearEqual(q.GetY(), 1.0f));
    EXPECT_TRUE(NearEqual(q.GetZ(), 0.0f));
}

TEST_F(QuaternionTest, AxisAngleConstructor_ZeroAngle)
{
    Vector3 axis(1.0f, 0.0f, 0.0f);
    Quaternion q(axis, 0.0f);

    EXPECT_TRUE(NearEqual(q, Quaternion::Identity));
}

TEST_F(QuaternionTest, EulerAnglesConstructor_ZeroAngles)
{
    Quaternion q(0.0f, 0.0f, 0.0f);

    EXPECT_TRUE(NearEqual(q, Quaternion::Identity));
}

TEST_F(QuaternionTest, EulerAnglesConstructor_Pitch90)
{
    float pitch = std::numbers::pi_v<float> / 2.0f;
    Quaternion q(pitch, 0.0f, 0.0f);

    EXPECT_TRUE(q.IsNormalized());
}

TEST_F(QuaternionTest, EulerAnglesConstructor_Yaw90)
{
    float yaw = std::numbers::pi_v<float> / 2.0f;
    Quaternion q(0.0f, yaw, 0.0f);

    EXPECT_TRUE(q.IsNormalized());
}

TEST_F(QuaternionTest, EulerAnglesConstructor_Roll90)
{
    float roll = std::numbers::pi_v<float> / 2.0f;
    Quaternion q(0.0f, 0.0f, roll);

    EXPECT_TRUE(q.IsNormalized());
}

// Static Factory Method Tests
TEST_F(QuaternionTest, Identity_IsIdentity)
{
    Quaternion q = Quaternion::Identity;

    EXPECT_FLOAT_EQ(q.GetW(), 1.0f);
    EXPECT_FLOAT_EQ(q.GetX(), 0.0f);
    EXPECT_FLOAT_EQ(q.GetY(), 0.0f);
    EXPECT_FLOAT_EQ(q.GetZ(), 0.0f);
}

TEST_F(QuaternionTest, FromAxisAngle_MatchesConstructor)
{
    Vector3 axis(0.0f, 1.0f, 0.0f);
    float angle = std::numbers::pi_v<float> / 4.0f;

    Quaternion q1(axis, angle);
    Quaternion q2 = Quaternion::FromAxisAngle(axis, angle);

    EXPECT_TRUE(NearEqual(q1, q2));
}

TEST_F(QuaternionTest, FromEulerAngles_MatchesConstructor)
{
    float pitch = 0.5f;
    float yaw = 0.3f;
    float roll = 0.1f;

    Quaternion q1(pitch, yaw, roll);
    Quaternion q2 = Quaternion::FromEulerAngles(pitch, yaw, roll);

    EXPECT_TRUE(NearEqual(q1, q2));
}

TEST_F(QuaternionTest, FromEulerAngles_Vector3Overload)
{
    Vector3 euler(0.5f, 0.3f, 0.1f);

    Quaternion q1 = Quaternion::FromEulerAngles(euler.x, euler.y, euler.z);
    Quaternion q2 = Quaternion::FromEulerAngles(euler);

    EXPECT_TRUE(NearEqual(q1, q2));
}

TEST_F(QuaternionTest, LookRotation_ForwardZ)
{
    Vector3 forward(0.0f, 0.0f, 1.0f);
    Vector3 up(0.0f, 1.0f, 0.0f);

    Quaternion q = Quaternion::LookRotation(forward, up);

    EXPECT_TRUE(q.IsNormalized());
}

TEST_F(QuaternionTest, LookRotation_ForwardX)
{
    Vector3 forward(1.0f, 0.0f, 0.0f);
    Vector3 up(0.0f, 1.0f, 0.0f);

    Quaternion q = Quaternion::LookRotation(forward, up);

    EXPECT_TRUE(q.IsNormalized());
}

// Arithmetic Operator Tests
TEST_F(QuaternionTest, Addition)
{
    Quaternion a(1.0f, 2.0f, 3.0f, 4.0f);
    Quaternion b(0.5f, 1.5f, 2.5f, 3.5f);

    Quaternion result = a + b;

    EXPECT_FLOAT_EQ(result.GetW(), 1.5f);
    EXPECT_FLOAT_EQ(result.GetX(), 3.5f);
    EXPECT_FLOAT_EQ(result.GetY(), 5.5f);
    EXPECT_FLOAT_EQ(result.GetZ(), 7.5f);
}

TEST_F(QuaternionTest, Subtraction)
{
    Quaternion a(1.0f, 2.0f, 3.0f, 4.0f);
    Quaternion b(0.5f, 1.0f, 1.5f, 2.0f);

    Quaternion result = a - b;

    EXPECT_FLOAT_EQ(result.GetW(), 0.5f);
    EXPECT_FLOAT_EQ(result.GetX(), 1.0f);
    EXPECT_FLOAT_EQ(result.GetY(), 1.5f);
    EXPECT_FLOAT_EQ(result.GetZ(), 2.0f);
}

TEST_F(QuaternionTest, ScalarMultiplication)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);

    Quaternion result = q * 2.0f;

    EXPECT_FLOAT_EQ(result.GetW(), 2.0f);
    EXPECT_FLOAT_EQ(result.GetX(), 4.0f);
    EXPECT_FLOAT_EQ(result.GetY(), 6.0f);
    EXPECT_FLOAT_EQ(result.GetZ(), 8.0f);
}

TEST_F(QuaternionTest, ScalarMultiplication_LeftSide)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);

    Quaternion result = 2.0f * q;

    EXPECT_FLOAT_EQ(result.GetW(), 2.0f);
    EXPECT_FLOAT_EQ(result.GetX(), 4.0f);
    EXPECT_FLOAT_EQ(result.GetY(), 6.0f);
    EXPECT_FLOAT_EQ(result.GetZ(), 8.0f);
}

TEST_F(QuaternionTest, ScalarDivision)
{
    Quaternion q(2.0f, 4.0f, 6.0f, 8.0f);

    Quaternion result = q / 2.0f;

    EXPECT_FLOAT_EQ(result.GetW(), 1.0f);
    EXPECT_FLOAT_EQ(result.GetX(), 2.0f);
    EXPECT_FLOAT_EQ(result.GetY(), 3.0f);
    EXPECT_FLOAT_EQ(result.GetZ(), 4.0f);
}

TEST_F(QuaternionTest, QuaternionMultiplication_Identity)
{
    Quaternion q(0.5f, 0.5f, 0.5f, 0.5f);
    Quaternion identity = Quaternion::Identity;

    Quaternion result1 = q * identity;
    Quaternion result2 = identity * q;

    EXPECT_TRUE(NearEqual(result1, q));
    EXPECT_TRUE(NearEqual(result2, q));
}

TEST_F(QuaternionTest, QuaternionMultiplication_Inverse)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 4.0f);
    Quaternion qInv = q.Inverse();

    Quaternion result = q * qInv;

    EXPECT_TRUE(NearEqual(result, Quaternion::Identity));
}

TEST_F(QuaternionTest, QuaternionMultiplication_NonCommutative)
{
    Quaternion a = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), std::numbers::pi_v<float> / 4.0f);
    Quaternion b = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 4.0f);

    Quaternion ab = a * b;
    Quaternion ba = b * a;

    // Quaternion multiplication is NOT commutative
    EXPECT_FALSE(NearEqual(ab, ba));
}

TEST_F(QuaternionTest, AdditionAssignment)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);
    q += Quaternion(0.5f, 0.5f, 0.5f, 0.5f);

    EXPECT_FLOAT_EQ(q.GetW(), 1.5f);
    EXPECT_FLOAT_EQ(q.GetX(), 2.5f);
    EXPECT_FLOAT_EQ(q.GetY(), 3.5f);
    EXPECT_FLOAT_EQ(q.GetZ(), 4.5f);
}

TEST_F(QuaternionTest, SubtractionAssignment)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);
    q -= Quaternion(0.5f, 0.5f, 0.5f, 0.5f);

    EXPECT_FLOAT_EQ(q.GetW(), 0.5f);
    EXPECT_FLOAT_EQ(q.GetX(), 1.5f);
    EXPECT_FLOAT_EQ(q.GetY(), 2.5f);
    EXPECT_FLOAT_EQ(q.GetZ(), 3.5f);
}

TEST_F(QuaternionTest, MultiplicationAssignment_Scalar)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);
    q *= 2.0f;

    EXPECT_FLOAT_EQ(q.GetW(), 2.0f);
    EXPECT_FLOAT_EQ(q.GetX(), 4.0f);
    EXPECT_FLOAT_EQ(q.GetY(), 6.0f);
    EXPECT_FLOAT_EQ(q.GetZ(), 8.0f);
}

TEST_F(QuaternionTest, MultiplicationAssignment_Quaternion)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), std::numbers::pi_v<float> / 4.0f);
    Quaternion original = q;
    Quaternion other = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 4.0f);

    q *= other;

    EXPECT_TRUE(NearEqual(q, original * other));
}

TEST_F(QuaternionTest, DivisionAssignment)
{
    Quaternion q(2.0f, 4.0f, 6.0f, 8.0f);
    q /= 2.0f;

    EXPECT_FLOAT_EQ(q.GetW(), 1.0f);
    EXPECT_FLOAT_EQ(q.GetX(), 2.0f);
    EXPECT_FLOAT_EQ(q.GetY(), 3.0f);
    EXPECT_FLOAT_EQ(q.GetZ(), 4.0f);
}

// Comparison Operator Tests
TEST_F(QuaternionTest, Equality_Same)
{
    Quaternion a(1.0f, 2.0f, 3.0f, 4.0f);
    Quaternion b(1.0f, 2.0f, 3.0f, 4.0f);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST_F(QuaternionTest, Equality_Different)
{
    Quaternion a(1.0f, 2.0f, 3.0f, 4.0f);
    Quaternion b(1.0f, 2.0f, 3.0f, 4.1f);

    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST_F(QuaternionTest, Equality_NearlyEqual)
{
    Quaternion a(1.0f, 2.0f, 3.0f, 4.0f);
    Quaternion b(1.0f + 1e-7f, 2.0f, 3.0f, 4.0f);

    // Should be equal within epsilon
    EXPECT_TRUE(a == b);
}

// Length and Normalization Tests
TEST_F(QuaternionTest, Length_UnitQuaternion)
{
    Quaternion q = Quaternion::Identity;

    EXPECT_TRUE(NearEqual(q.Length(), 1.0f));
}

TEST_F(QuaternionTest, Length_NonUnitQuaternion)
{
    Quaternion q(1.0f, 1.0f, 1.0f, 1.0f);

    EXPECT_TRUE(NearEqual(q.Length(), 2.0f));
}

TEST_F(QuaternionTest, LengthSquared)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);

    float expected = 1.0f + 4.0f + 9.0f + 16.0f; // 30
    EXPECT_TRUE(NearEqual(q.LengthSquared(), expected));
}

TEST_F(QuaternionTest, Normalized)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);

    Quaternion normalized = q.Normalized();

    EXPECT_TRUE(NearEqual(normalized.Length(), 1.0f));
}

TEST_F(QuaternionTest, Normalized_PreservesDirection)
{
    Quaternion q(2.0f, 0.0f, 0.0f, 0.0f);

    Quaternion normalized = q.Normalized();

    EXPECT_TRUE(NearEqual(normalized.GetW(), 1.0f));
    EXPECT_TRUE(NearEqual(normalized.GetX(), 0.0f));
    EXPECT_TRUE(NearEqual(normalized.GetY(), 0.0f));
    EXPECT_TRUE(NearEqual(normalized.GetZ(), 0.0f));
}

TEST_F(QuaternionTest, Normalize_InPlace)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);

    q.Normalize();

    EXPECT_TRUE(NearEqual(q.Length(), 1.0f));
}

TEST_F(QuaternionTest, Normalized_ZeroQuaternion)
{
    Quaternion q(0.0f, 0.0f, 0.0f, 0.0f);

    Quaternion normalized = q.Normalized();

    // Should return identity for zero quaternion
    EXPECT_TRUE(NearEqual(normalized, Quaternion::Identity));
}

TEST_F(QuaternionTest, IsNormalized_True)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 1.0f);

    EXPECT_TRUE(q.IsNormalized());
}

TEST_F(QuaternionTest, IsNormalized_False)
{
    Quaternion q(1.0f, 1.0f, 1.0f, 1.0f);

    EXPECT_FALSE(q.IsNormalized());
}

// Conjugate and Inverse Tests
TEST_F(QuaternionTest, Conjugate)
{
    Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);

    Quaternion conj = q.Conjugate();

    EXPECT_FLOAT_EQ(conj.GetW(), 1.0f);
    EXPECT_FLOAT_EQ(conj.GetX(), -2.0f);
    EXPECT_FLOAT_EQ(conj.GetY(), -3.0f);
    EXPECT_FLOAT_EQ(conj.GetZ(), -4.0f);
}

TEST_F(QuaternionTest, Conjugate_Identity)
{
    Quaternion identity = Quaternion::Identity;

    Quaternion conj = identity.Conjugate();

    EXPECT_TRUE(NearEqual(conj, identity));
}

TEST_F(QuaternionTest, Inverse_UnitQuaternion)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 3.0f);

    Quaternion inv = q.Inverse();
    Quaternion result = q * inv;

    EXPECT_TRUE(NearEqual(result, Quaternion::Identity));
}

TEST_F(QuaternionTest, Inverse_NonUnitQuaternion)
{
    Quaternion q(2.0f, 1.0f, 0.0f, 0.0f);

    Quaternion inv = q.Inverse();
    Quaternion result = q * inv;

    EXPECT_TRUE(NearEqual(result, Quaternion::Identity));
}

TEST_F(QuaternionTest, Inverse_Identity)
{
    Quaternion identity = Quaternion::Identity;

    Quaternion inv = identity.Inverse();

    EXPECT_TRUE(NearEqual(inv, identity));
}

// Dot Product and Angle Tests
TEST_F(QuaternionTest, Dot_SameQuaternion)
{
    Quaternion q(0.5f, 0.5f, 0.5f, 0.5f);

    float dot = q.Dot(q);

    EXPECT_TRUE(NearEqual(dot, q.LengthSquared()));
}

TEST_F(QuaternionTest, Dot_PerpendicularQuaternions)
{
    Quaternion a(1.0f, 0.0f, 0.0f, 0.0f);
    Quaternion b(0.0f, 1.0f, 0.0f, 0.0f);

    float dot = a.Dot(b);

    EXPECT_TRUE(NearEqual(dot, 0.0f));
}

TEST_F(QuaternionTest, Dot_OppositeQuaternions)
{
    Quaternion a(0.5f, 0.5f, 0.5f, 0.5f);
    Quaternion b(-0.5f, -0.5f, -0.5f, -0.5f);

    float dot = a.Dot(b);

    EXPECT_TRUE(NearEqual(dot, -1.0f));
}

TEST_F(QuaternionTest, Angle_SameQuaternion)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 1.0f);

    float angle = q.Angle(q);

    EXPECT_TRUE(NearEqual(angle, 0.0f));
}

TEST_F(QuaternionTest, Angle_OppositeQuaternions)
{
    Vector3 axis(0.0f, 1.0f, 0.0f);
    Quaternion a = Quaternion::FromAxisAngle(axis, 0.0f);
    Quaternion b = Quaternion::FromAxisAngle(axis, std::numbers::pi_v<float>);

    float angle = a.Angle(b);

    EXPECT_TRUE(NearEqual(angle, std::numbers::pi_v<float>));
}

// Rotation Tests
TEST_F(QuaternionTest, Rotate_Identity)
{
    Quaternion identity = Quaternion::Identity;
    Vector3 v(1.0f, 2.0f, 3.0f);

    Vector3 result = identity.Rotate(v);

    EXPECT_TRUE(NearEqual(result, v));
}

TEST_F(QuaternionTest, Rotate_90DegreesAroundY)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 2.0f);
    Vector3 v(1.0f, 0.0f, 0.0f);

    Vector3 result = q.Rotate(v);

    // Rotating (1,0,0) 90 degrees around Y should give (0,0,-1)
    EXPECT_TRUE(NearEqual(result, Vector3(0.0f, 0.0f, -1.0f)));
}

TEST_F(QuaternionTest, Rotate_90DegreesAroundX)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), std::numbers::pi_v<float> / 2.0f);
    Vector3 v(0.0f, 1.0f, 0.0f);

    Vector3 result = q.Rotate(v);

    // Rotating (0,1,0) 90 degrees around X should give (0,0,1)
    EXPECT_TRUE(NearEqual(result, Vector3(0.0f, 0.0f, 1.0f)));
}

TEST_F(QuaternionTest, Rotate_90DegreesAroundZ)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 0.0f, 1.0f), std::numbers::pi_v<float> / 2.0f);
    Vector3 v(1.0f, 0.0f, 0.0f);

    Vector3 result = q.Rotate(v);

    // Rotating (1,0,0) 90 degrees around Z should give (0,1,0)
    EXPECT_TRUE(NearEqual(result, Vector3(0.0f, 1.0f, 0.0f)));
}

TEST_F(QuaternionTest, Rotate_180Degrees)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float>);
    Vector3 v(1.0f, 0.0f, 0.0f);

    Vector3 result = q.Rotate(v);

    // Rotating (1,0,0) 180 degrees around Y should give (-1,0,0)
    EXPECT_TRUE(NearEqual(result, Vector3(-1.0f, 0.0f, 0.0f)));
}

TEST_F(QuaternionTest, Rotate_VectorMultiplicationOperator)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 2.0f);
    Vector3 v(1.0f, 0.0f, 0.0f);

    Vector3 result1 = q.Rotate(v);
    Vector3 result2 = q * v;

    EXPECT_TRUE(NearEqual(result1, result2));
}

// Euler Angles Conversion Tests
TEST_F(QuaternionTest, ToEulerAngles_Identity)
{
    Quaternion identity = Quaternion::Identity;

    Vector3 euler = identity.ToEulerAngles();

    EXPECT_TRUE(NearEqual(euler.x, 0.0f));
    EXPECT_TRUE(NearEqual(euler.y, 0.0f));
    EXPECT_TRUE(NearEqual(euler.z, 0.0f));
}

TEST_F(QuaternionTest, EulerAngles_RoundTrip)
{
    float pitch = 0.3f;
    float yaw = 0.5f;
    float roll = 0.2f;

    Quaternion q = Quaternion::FromEulerAngles(pitch, yaw, roll);
    Vector3 euler = q.ToEulerAngles();
    Quaternion q2 = Quaternion::FromEulerAngles(euler.x, euler.y, euler.z);

    // The quaternions should represent the same rotation
    EXPECT_TRUE(NearEqualRotation(q, q2));
}

// Matrix Conversion Tests
TEST_F(QuaternionTest, ToMatrix_Identity)
{
    Quaternion identity = Quaternion::Identity;

    auto matrix = identity.ToMatrix();

    // Should be identity matrix
    EXPECT_TRUE(NearEqual(matrix(0, 0), 1.0f));
    EXPECT_TRUE(NearEqual(matrix(1, 1), 1.0f));
    EXPECT_TRUE(NearEqual(matrix(2, 2), 1.0f));
    EXPECT_TRUE(NearEqual(matrix(3, 3), 1.0f));

    EXPECT_TRUE(NearEqual(matrix(0, 1), 0.0f));
    EXPECT_TRUE(NearEqual(matrix(0, 2), 0.0f));
    EXPECT_TRUE(NearEqual(matrix(1, 0), 0.0f));
    EXPECT_TRUE(NearEqual(matrix(1, 2), 0.0f));
    EXPECT_TRUE(NearEqual(matrix(2, 0), 0.0f));
    EXPECT_TRUE(NearEqual(matrix(2, 1), 0.0f));
}

TEST_F(QuaternionTest, ToMatrix_RotationMatchesQuaternion)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 2.0f);
    Vector3 v(1.0f, 0.0f, 0.0f);

    Vector3 rotatedByQuat = q.Rotate(v);

    auto matrix = q.ToMatrix();
    Vector3 rotatedByMatrix(
        matrix(0, 0) * v.x + matrix(0, 1) * v.y + matrix(0, 2) * v.z,
        matrix(1, 0) * v.x + matrix(1, 1) * v.y + matrix(1, 2) * v.z,
        matrix(2, 0) * v.x + matrix(2, 1) * v.y + matrix(2, 2) * v.z
    );

    EXPECT_TRUE(NearEqual(rotatedByQuat, rotatedByMatrix));
}

// Interpolation Tests
TEST_F(QuaternionTest, Lerp_t0)
{
    Quaternion a = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    Quaternion b = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), std::numbers::pi_v<float>);

    Quaternion result = Quaternion::Lerp(a, b, 0.0f);

    EXPECT_TRUE(NearEqualRotation(result, a));
}

TEST_F(QuaternionTest, Lerp_t1)
{
    Quaternion a = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    Quaternion b = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), std::numbers::pi_v<float>);

    Quaternion result = Quaternion::Lerp(a, b, 1.0f);

    EXPECT_TRUE(NearEqualRotation(result, b));
}

TEST_F(QuaternionTest, Lerp_ResultIsNormalized)
{
    Quaternion a = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    Quaternion b = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 2.0f);

    Quaternion result = Quaternion::Lerp(a, b, 0.5f);

    EXPECT_TRUE(result.IsNormalized());
}

TEST_F(QuaternionTest, Slerp_t0)
{
    Quaternion a = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    Quaternion b = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), std::numbers::pi_v<float>);

    Quaternion result = Quaternion::Slerp(a, b, 0.0f);

    EXPECT_TRUE(NearEqualRotation(result, a));
}

TEST_F(QuaternionTest, Slerp_t1)
{
    Quaternion a = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    Quaternion b = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), std::numbers::pi_v<float>);

    Quaternion result = Quaternion::Slerp(a, b, 1.0f);

    EXPECT_TRUE(NearEqualRotation(result, b));
}

TEST_F(QuaternionTest, Slerp_ResultIsNormalized)
{
    Quaternion a = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    Quaternion b = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 2.0f);

    Quaternion result = Quaternion::Slerp(a, b, 0.5f);

    EXPECT_TRUE(result.IsNormalized());
}

TEST_F(QuaternionTest, Slerp_MidpointRotation)
{
    // Rotate from 0 to 90 degrees around Y
    Quaternion a = Quaternion::Identity;
    Quaternion b = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 2.0f);

    Quaternion mid = Quaternion::Slerp(a, b, 0.5f);

    // Mid should be 45 degree rotation
    Quaternion expected = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), std::numbers::pi_v<float> / 4.0f);

    EXPECT_TRUE(NearEqualRotation(mid, expected));
}

// Utility Tests

TEST_F(QuaternionTest, IsIdentity_True)
{
    Quaternion q = Quaternion::Identity;

    EXPECT_TRUE(q.IsIdentity());
}

TEST_F(QuaternionTest, IsIdentity_False)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.1f);

    EXPECT_FALSE(q.IsIdentity());
}

TEST_F(QuaternionTest, IsIdentity_NearlyIdentity)
{
    Quaternion q(1.0f - 1e-7f, 1e-8f, 1e-8f, 1e-8f);

    EXPECT_TRUE(q.IsIdentity());
}

// Edge Cases
TEST_F(QuaternionTest, Normalize_VerySmallQuaternion)
{
    Quaternion q(1e-8f, 1e-8f, 1e-8f, 1e-8f);

    Quaternion normalized = q.Normalized();

    // Should return identity for near-zero quaternion
    EXPECT_TRUE(NearEqual(normalized, Quaternion::Identity));
}

TEST_F(QuaternionTest, Inverse_VerySmallQuaternion)
{
    Quaternion q(1e-8f, 1e-8f, 1e-8f, 1e-8f);

    Quaternion inv = q.Inverse();

    // Should return identity for near-zero quaternion
    EXPECT_TRUE(NearEqual(inv, Quaternion::Identity));
}

TEST_F(QuaternionTest, Slerp_NearlyParallelQuaternions)
{
    Quaternion a = Quaternion::Identity;
    Quaternion b(1.0f - 1e-6f, 1e-7f, 1e-7f, 1e-7f);
    b = b.Normalized();

    // Should not crash and should return a valid quaternion
    Quaternion result = Quaternion::Slerp(a, b, 0.5f);

    EXPECT_TRUE(result.IsNormalized());
}

TEST_F(QuaternionTest, Slerp_OppositeQuaternions)
{
    Quaternion a(0.5f, 0.5f, 0.5f, 0.5f);
    Quaternion b(-0.5f, -0.5f, -0.5f, -0.5f);

    // Both represent the same rotation, slerp should handle this
    Quaternion result = Quaternion::Slerp(a, b, 0.5f);

    EXPECT_TRUE(result.IsNormalized());
}

// Consistency Tests
TEST_F(QuaternionTest, MultipleRotations_Associative)
{
    Quaternion a = Quaternion::FromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.5f);
    Quaternion b = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 0.3f);
    Quaternion c = Quaternion::FromAxisAngle(Vector3(0.0f, 0.0f, 1.0f), 0.7f);

    Quaternion result1 = (a * b) * c;
    Quaternion result2 = a * (b * c);

    EXPECT_TRUE(NearEqual(result1, result2));
}

TEST_F(QuaternionTest, RotateVector_ConsistentWithMultiplication)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(1.0f, 1.0f, 1.0f).Normalized(), 1.0f);
    Vector3 v(1.0f, 2.0f, 3.0f);

    // q * v should equal q.Rotate(v)
    Vector3 result1 = q * v;
    Vector3 result2 = q.Rotate(v);

    EXPECT_TRUE(NearEqual(result1, result2));
}

TEST_F(QuaternionTest, ConjugateInverse_UnitQuaternion)
{
    Quaternion q = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), 1.0f);

    // For unit quaternions, conjugate == inverse
    Quaternion conj = q.Conjugate();
    Quaternion inv = q.Inverse();

    EXPECT_TRUE(NearEqual(conj, inv));
}
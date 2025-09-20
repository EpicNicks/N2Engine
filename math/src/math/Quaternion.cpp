#include "math/Quaternion.hpp"
#include "math/Vector3.hpp"
#include "math/Matrix.hpp"
#include <cmath>
#include <algorithm>
#include <numbers>

using namespace N2Engine::Math;

// Constructor implementations
Quaternion::Quaternion(const Vector3 &axis, float angle)
{
    float halfAngle = angle * 0.5f;
    float sinHalfAngle = std::sin(halfAngle);

    w = std::cos(halfAngle);
    x = axis.x * sinHalfAngle;
    y = axis.y * sinHalfAngle;
    z = axis.z * sinHalfAngle;
}

Quaternion::Quaternion(float pitch, float yaw, float roll)
{
    float cy = std::cos(yaw * 0.5f);
    float sy = std::sin(yaw * 0.5f);
    float cp = std::cos(pitch * 0.5f);
    float sp = std::sin(pitch * 0.5f);
    float cr = std::cos(roll * 0.5f);
    float sr = std::sin(roll * 0.5f);

    w = cr * cp * cy + sr * sp * sy;
    x = sr * cp * cy - cr * sp * sy;
    y = cr * sp * cy + sr * cp * sy;
    z = cr * cp * sy - sr * sp * cy;
}

// Static factory methods
Quaternion Quaternion::LookRotation(const Vector3 &forward, const Vector3 &up)
{
    Vector3 f = forward.Normalized();
    Vector3 r = up.Cross(f).Normalized();
    Vector3 u = f.Cross(r);

    float trace = r.x + u.y + f.z;
    Quaternion result;

    if (trace > 0.0f)
    {
        float s = std::sqrt(trace + 1.0f) * 2.0f; // s = 4 * w
        result.w = 0.25f * s;
        result.x = (u.z - f.y) / s;
        result.y = (f.x - r.z) / s;
        result.z = (r.y - u.x) / s;
    }
    else if (r.x > u.y && r.x > f.z)
    {
        float s = std::sqrt(1.0f + r.x - u.y - f.z) * 2.0f; // s = 4 * x
        result.w = (u.z - f.y) / s;
        result.x = 0.25f * s;
        result.y = (u.x + r.y) / s;
        result.z = (f.x + r.z) / s;
    }
    else if (u.y > f.z)
    {
        float s = std::sqrt(1.0f + u.y - r.x - f.z) * 2.0f; // s = 4 * y
        result.w = (f.x - r.z) / s;
        result.x = (u.x + r.y) / s;
        result.y = 0.25f * s;
        result.z = (f.y + u.z) / s;
    }
    else
    {
        float s = std::sqrt(1.0f + f.z - r.x - u.y) * 2.0f; // s = 4 * z
        result.w = (r.y - u.x) / s;
        result.x = (f.x + r.z) / s;
        result.y = (f.y + u.z) / s;
        result.z = 0.25f * s;
    }

    return result;
}

Quaternion Quaternion::Slerp(const Quaternion &a, const Quaternion &b, float t)
{
    float dot = a.Dot(b);
    Quaternion b_copy = b;

    if (dot < 0.0f)
    {
        b_copy = Quaternion(-b.w, -b.x, -b.y, -b.z);
        dot = -dot;
    }

    if (dot > 0.9995f)
    {
        return Lerp(a, b_copy, t);
    }

    float theta0 = std::acos(std::abs(dot));
    float theta = theta0 * t;
    float sinTheta = std::sin(theta);
    float sinTheta0 = std::sin(theta0);

    float s0 = std::cos(theta) - dot * sinTheta / sinTheta0;
    float s1 = sinTheta / sinTheta0;

    return (s0 * a) + (s1 * b_copy);
}

Quaternion Quaternion::Lerp(const Quaternion &a, const Quaternion &b, float t)
{
    return ((1.0f - t) * a + t * b).Normalized();
}

// Non-SIMD operations that require Vector3
Vector3 Quaternion::operator*(const Vector3 &vector) const
{
    return Rotate(vector);
}

Quaternion Quaternion::operator/(float scalar) const
{
    float invScalar = 1.0f / scalar;
    return *this * invScalar;
}

Quaternion &Quaternion::operator/=(float scalar)
{
    float invScalar = 1.0f / scalar;
    *this *= invScalar;
    return *this;
}

bool Quaternion::operator==(const Quaternion &other) const
{
    return std::abs(w - other.w) < EPSILON &&
           std::abs(x - other.x) < EPSILON &&
           std::abs(y - other.y) < EPSILON &&
           std::abs(z - other.z) < EPSILON;
}

bool Quaternion::operator!=(const Quaternion &other) const
{
    return !(*this == other);
}

Quaternion Quaternion::Inverse() const
{
    float lengthSq = LengthSquared();
    if (lengthSq < EPSILON)
        return Identity();
    return Conjugate() / lengthSq;
}

float Quaternion::Angle(const Quaternion &other) const
{
    float dot = std::abs(Dot(other));
    return std::acos(std::min(dot, 1.0f)) * 2.0f;
}

Vector3 Quaternion::Rotate(const Vector3 &vector) const
{
    // Optimized quaternion-vector rotation
    Vector3 qvec{x, y, z};
    Vector3 uv = qvec.Cross(vector);
    Vector3 uuv = qvec.Cross(uv);

    return vector + ((uv * w) + uuv) * 2.0f;
}

Vector3 Quaternion::ToEulerAngles() const
{
    Vector3 euler;

    // Roll (x-axis rotation)
    float sinr_cosp = 2 * (w * x + y * z);
    float cosr_cosp = 1 - 2 * (x * x + y * y);
    euler.x = std::atan2(sinr_cosp, cosr_cosp);

    // Pitch (y-axis rotation)
    float sinp = 2 * (w * y - z * x);
    if (std::abs(sinp) >= 1)
        euler.y = std::copysign(std::numbers::pi / 2, sinp);
    else
        euler.y = std::asin(sinp);

    // Yaw (z-axis rotation)
    float siny_cosp = 2 * (w * z + x * y);
    float cosy_cosp = 1 - 2 * (y * y + z * z);
    euler.z = std::atan2(siny_cosp, cosy_cosp);

    return euler;
}

Quaternion::Matrix4f Quaternion::ToMatrix() const
{
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float wx = w * x;
    float wy = w * y;
    float wz = w * z;

    Matrix4f result;

    result(0, 0) = 1.0f - 2.0f * (yy + zz);
    result(0, 1) = 2.0f * (xy - wz);
    result(0, 2) = 2.0f * (xz + wy);
    result(0, 3) = 0.0f;

    result(1, 0) = 2.0f * (xy + wz);
    result(1, 1) = 1.0f - 2.0f * (xx + zz);
    result(1, 2) = 2.0f * (yz - wx);
    result(1, 3) = 0.0f;

    result(2, 0) = 2.0f * (xz - wy);
    result(2, 1) = 2.0f * (yz + wx);
    result(2, 2) = 1.0f - 2.0f * (xx + yy);
    result(2, 3) = 0.0f;

    result(3, 0) = 0.0f;
    result(3, 1) = 0.0f;
    result(3, 2) = 0.0f;
    result(3, 3) = 1.0f;

    return result;
}

bool Quaternion::IsNormalized(float tolerance) const
{
    return std::abs(LengthSquared() - 1.0f) <= tolerance;
}

bool Quaternion::IsIdentity(float tolerance) const
{
    return std::abs(w - 1.0f) <= tolerance &&
           std::abs(x) <= tolerance &&
           std::abs(y) <= tolerance &&
           std::abs(z) <= tolerance;
}

// SIMD initialization
void Quaternion::InitializeSIMD()
{
    if (initialized)
        return;

    // Use the same CPU features detection as Matrix class
    static struct
    {
        bool sse2_support = false;
        bool sse41_support = false;

        void DetectFeatures()
        {
#ifdef _WIN32
            int cpui[4];
            __cpuid(cpui, 1);
            sse2_support = (cpui[3] & (1 << 26)) != 0;
            sse41_support = (cpui[2] & (1 << 19)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
            unsigned int eax, ebx, ecx, edx;
            if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
            {
                sse2_support = (edx & (1 << 26)) != 0;
                sse41_support = (ecx & (1 << 19)) != 0;
            }
#endif
        }

        bool HasSSE2() const { return sse2_support; }
        bool HasSSE41() const { return sse41_support; }
    } features;

    static bool features_detected = false;
    if (!features_detected)
    {
        features.DetectFeatures();
        features_detected = true;
    }

    if (features.HasSSE41())
    {
        add_func = &AddSSE2;
        sub_func = &SubSSE2;
        mul_func = &MulSSE2;
        scalar_mul_func = &ScalarMulSSE2;
        dot_func = &DotSSE41;             // SSE4.1 has better dot product
        length_func = &LengthSSE41;       // SSE4.1 has better sqrt
        normalize_func = &NormalizeSSE41; // SSE4.1 has better normalization
    }
    else if (features.HasSSE2())
    {
        add_func = &AddSSE2;
        sub_func = &SubSSE2;
        mul_func = &MulSSE2;
        scalar_mul_func = &ScalarMulSSE2;
        dot_func = &DotSSE2;
        length_func = &LengthSSE2;
        normalize_func = &NormalizeSSE2;
    }
    else
    {
        // Function pointers already initialized to scalar versions
        // No need to change anything
    }

    initialized = true;
}
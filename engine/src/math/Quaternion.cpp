#include "Quaternion.hpp"
#include "Vector3.hpp"
#include "Matrix.hpp"
#include <cmath>
#include <algorithm>

namespace N2Engine
{
    namespace Math
    {
        const float Quaternion::EPSILON = 1e-6f;

        // Constructors
        Quaternion::Quaternion() : w(1.0f), x(0.0f), y(0.0f), z(0.0f)
        {
        }

        Quaternion::Quaternion(float w, float x, float y, float z) : w(w), x(x), y(y), z(z)
        {
        }

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

        Quaternion Quaternion::Identity()
        {
            return Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
        }

        Quaternion Quaternion::FromAxisAngle(const Vector3 &axis, float angle)
        {
            return Quaternion(axis, angle);
        }

        Quaternion Quaternion::FromEulerAngles(float pitch, float yaw, float roll)
        {
            return Quaternion(pitch, yaw, roll);
        }

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

        Quaternion Quaternion::operator+(const Quaternion &other) const
        {
            return Quaternion(w + other.w, x + other.x, y + other.y, z + other.z);
        }

        Quaternion Quaternion::operator-(const Quaternion &other) const
        {
            return Quaternion(w - other.w, x - other.x, y - other.y, z - other.z);
        }

        Quaternion Quaternion::operator*(const Quaternion &other) const
        {
            return Quaternion(
                w * other.w - x * other.x - y * other.y - z * other.z,
                w * other.x + x * other.w + y * other.z - z * other.y,
                w * other.y - x * other.z + y * other.w + z * other.x,
                w * other.z + x * other.y - y * other.x + z * other.w);
        }

        Quaternion Quaternion::operator*(float scalar) const
        {
            return Quaternion(w * scalar, x * scalar, y * scalar, z * scalar);
        }

        Quaternion Quaternion::operator/(float scalar) const
        {
            float invScalar = 1.0f / scalar;
            return Quaternion(w * invScalar, x * invScalar, y * invScalar, z * invScalar);
        }

        Quaternion &Quaternion::operator+=(const Quaternion &other)
        {
            w += other.w;
            x += other.x;
            y += other.y;
            z += other.z;
            return *this;
        }

        Quaternion &Quaternion::operator-=(const Quaternion &other)
        {
            w -= other.w;
            x -= other.x;
            y -= other.y;
            z -= other.z;
            return *this;
        }

        Quaternion &Quaternion::operator*=(const Quaternion &other)
        {
            *this = *this * other;
            return *this;
        }

        Quaternion &Quaternion::operator*=(float scalar)
        {
            w *= scalar;
            x *= scalar;
            y *= scalar;
            z *= scalar;
            return *this;
        }

        Quaternion &Quaternion::operator/=(float scalar)
        {
            float invScalar = 1.0f / scalar;
            w *= invScalar;
            x *= invScalar;
            y *= invScalar;
            z *= invScalar;
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

        float Quaternion::Length() const
        {
            return std::sqrt(w * w + x * x + y * y + z * z);
        }

        float Quaternion::LengthSquared() const
        {
            return w * w + x * x + y * y + z * z;
        }

        Quaternion Quaternion::Normalized() const
        {
            float length = Length();
            if (length < EPSILON)
                return Identity();
            return *this / length;
        }

        Quaternion &Quaternion::Normalize()
        {
            float length = Length();
            if (length >= EPSILON)
            {
                *this /= length;
            }
            else
            {
                *this = Identity();
            }
            return *this;
        }

        Quaternion Quaternion::Conjugate() const
        {
            return Quaternion(w, -x, -y, -z);
        }

        Quaternion Quaternion::Inverse() const
        {
            float lengthSq = LengthSquared();
            if (lengthSq < EPSILON)
                return Identity();
            return Conjugate() / lengthSq;
        }

        float Quaternion::Dot(const Quaternion &other) const
        {
            return w * other.w + x * other.x + y * other.y + z * other.z;
        }

        float Quaternion::Angle(const Quaternion &other) const
        {
            float dot = std::abs(Dot(other));
            return std::acos(std::min(dot, 1.0f)) * 2.0f;
        }

        Vector3 Quaternion::Rotate(const Vector3 &vector) const
        {
            Vector3 qvec(x, y, z);
            Vector3 uv = qvec.Cross(vector);
            Vector3 uuv = qvec.Cross(uv);

            return vector + ((uv * w) + uuv) * 2.0f;
        }

        Vector3 Quaternion::ToEulerAngles() const
        {
            Vector3 euler;

            float sinr_cosp = 2 * (w * x + y * z);
            float cosr_cosp = 1 - 2 * (x * x + y * y);
            euler.x = std::atan2(sinr_cosp, cosr_cosp);

            float sinp = 2 * (w * y - z * x);
            if (std::abs(sinp) >= 1)
                euler.y = std::copysign(M_PI / 2, sinp);
            else
                euler.y = std::asin(sinp);

            float siny_cosp = 2 * (w * z + x * y);
            float cosy_cosp = 1 - 2 * (y * y + z * z);
            euler.z = std::atan2(siny_cosp, cosy_cosp);

            return euler;
        }

        Matrix4f Quaternion::ToMatrix() const
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

        Quaternion operator*(float scalar, const Quaternion &q)
        {
            return q * scalar;
        }
    }
}
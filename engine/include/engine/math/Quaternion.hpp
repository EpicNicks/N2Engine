#pragma once
#include <cmath>

namespace N2Engine
{
    namespace Math
    {
        class Vector3; // Forward declaration
        template <typename T, std::size_t M, std::size_t N>
        class Matrix; // Forward declaration
        using Matrix4f = Matrix<float, 4, 4>;

        class Quaternion
        {
        public:
            float w, x, y, z;

            // Constructors
            Quaternion();
            Quaternion(float w, float x, float y, float z);
            explicit Quaternion(const Vector3 &axis, float angle);
            explicit Quaternion(float pitch, float yaw, float roll);

            // Static factory methods
            static Quaternion Identity();
            static Quaternion FromAxisAngle(const Vector3 &axis, float angle);
            static Quaternion FromEulerAngles(float pitch, float yaw, float roll);
            static Quaternion LookRotation(const Vector3 &forward, const Vector3 &up);
            static Quaternion Slerp(const Quaternion &a, const Quaternion &b, float t);
            static Quaternion Lerp(const Quaternion &a, const Quaternion &b, float t);

            // Basic operations
            Quaternion operator+(const Quaternion &other) const;
            Quaternion operator-(const Quaternion &other) const;
            Quaternion operator*(const Quaternion &other) const;
            Quaternion operator*(float scalar) const;
            Quaternion operator/(float scalar) const;

            Quaternion &operator+=(const Quaternion &other);
            Quaternion &operator-=(const Quaternion &other);
            Quaternion &operator*=(const Quaternion &other);
            Quaternion &operator*=(float scalar);
            Quaternion &operator/=(float scalar);

            bool operator==(const Quaternion &other) const;
            bool operator!=(const Quaternion &other) const;

            // Quaternion specific operations
            float Length() const;
            float LengthSquared() const;
            Quaternion Normalized() const;
            Quaternion &Normalize();
            Quaternion Conjugate() const;
            Quaternion Inverse() const;

            float Dot(const Quaternion &other) const;
            float Angle(const Quaternion &other) const;

            // Rotation operations
            Vector3 Rotate(const Vector3 &vector) const;
            Vector3 ToEulerAngles() const;
            Matrix4f ToMatrix() const;

            // Utility
            bool IsNormalized(float tolerance = 1e-6f) const;
            bool IsIdentity(float tolerance = 1e-6f) const;

        private:
            static const float EPSILON;
        };

        // Non-member operators
        Quaternion operator*(float scalar, const Quaternion &q);
    }
}
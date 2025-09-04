#pragma once

#include "math/VectorN.hpp"

namespace N2Engine
{
    namespace Math
    {
        struct Vector3 : public VectorN<float, 3>
        {
            using VectorN<float, 3>::VectorN;

        public:
            float &x = vector[0];
            float &y = vector[1];
            float &z = vector[2];

            Vector3 &operator=(const Vector3 &other);
            Vector3(const Vector3 &other);
            Vector3(const VectorN<float, 3> &base);

            static Vector3 Up();
            static Vector3 Down();
            static Vector3 Left();
            static Vector3 Right();
            static Vector3 Forward();
            static Vector3 Back();

            Vector3 Cross(const Vector3 &other) const;
            static Vector3 Cross(const Vector3 &a, const Vector3 &b);

            float AngleTo(const Vector3 &other) const;

            Vector3 Project(const Vector3 &onto) const;
            Vector3 Reject(const Vector3 &onto) const;
            Vector3 Reflect(const Vector3 &normal) const;

            Vector3 RotateAroundAxis(const Vector3 &axis, float radians) const;
            Vector3 &RotateAroundAxisInPlace(const Vector3 &axis, float radians);

            static float ScalarTriple(const Vector3 &a, const Vector3 &b, const Vector3 &c);

            static Vector3 VectorTriple(const Vector3 &a, const Vector3 &b, const Vector3 &c);

            static Vector3 FromSpherical(float radius, float theta, float phi);
            void ToSpherical(float &radius, float &theta, float &phi) const;

            bool IsParallel(const Vector3 &other, float tolerance = 1e-6f) const;
            bool IsPerpendicular(const Vector3 &other, float tolerance = 1e-6f) const;
            Vector3 GetOrthogonal() const;
        };

        using Vector3f = Vector3;
    }
}
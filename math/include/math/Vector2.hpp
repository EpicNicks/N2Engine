#pragma once

#include "math/VectorN.hpp"

namespace N2Engine
{
    namespace Math
    {
        struct Vector2 : public VectorN<float, 2>
        {
            using VectorN<float, 2>::VectorN;

        public:
            float &x = vector[0];
            float &y = vector[1];

            Vector2(float x, float y);
            Vector2(const VectorN<float, 2> &base);
            Vector2(const Vector2 &other);
            Vector2 &operator=(const Vector2 &other);

            static Vector2 Up();
            static Vector2 Down();
            static Vector2 Left();
            static Vector2 Right();

            float Cross(const Vector2 &other) const;
            static float Cross(const Vector2 &a, const Vector2 &b);

            Vector2 Perpendicular() const;
            Vector2 PerpendicularCW() const;

            float Angle() const;
            float AngleTo(const Vector2 &other) const;
            static Vector2 FromAngle(float radians);
            static Vector2 FromAngle(float radians, float magnitude);

            Vector2 Rotated(float radians) const;
            Vector2 &Rotate(float radians);
        };
    }
}
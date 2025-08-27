#pragma once

namespace N2Engine
{
    namespace Math
    {
        struct Vector2
        {
        public:
            float x = 0.0f, y = 0.0f;

            float Magnitude() const;
            float Length() const;

            static float Distance(const Vector2 &a, const Vector2 &b);

            Vector2 operator+(const Vector2 &other) const;
            Vector2 operator-(const Vector2 &other) const;
            Vector2 operator*(float other) const;
            Vector2 operator/(float other) const;

            bool operator==(const Vector2 &other) const;
            bool operator!=(const Vector2 &other) const;
        };

        Vector2 operator*(float f, Vector2 v);
    }
}
#include <cmath>

#include "engine/math/Vector2.hpp"

using namespace N2Engine::Math;

Vector2::Vector2(const VectorN<float, 2> &base) : VectorN<float, 2>(base) {}
Vector2::Vector2(const Vector2 &other) : VectorN<float, 2>(other) {}

Vector2 &Vector2::operator=(const Vector2 &other)
{
    if (this != &other)
    {
        vector[0] = other.vector[0];
        vector[1] = other.vector[1];
    }
    return *this;
}

Vector2 Vector2::Up()
{
    return {0.0f, 1.0f};
}

Vector2 Vector2::Down()
{
    return {0.0f, -1.0f};
}

Vector2 Vector2::Left()
{
    return {-1.0f, 0.0f};
}

Vector2 Vector2::Right()
{
    return {1.0f, 0.0f};
}

float Vector2::Cross(const Vector2 &other) const
{
    return x * other.y - y * other.x;
}

float Vector2::Cross(const Vector2 &a, const Vector2 &b)
{
    return a.Cross(b);
}

Vector2 Vector2::Perpendicular() const
{
    return {-y, x};
}

Vector2 Vector2::PerpendicularCW() const
{
    return {y, -x};
}

float Vector2::Angle() const
{
    return std::atan2(y, x);
}

float Vector2::AngleTo(const Vector2 &other) const
{
    float cross = Cross(other);
    float dot = Dot(other);
    return std::atan2(cross, dot);
}

Vector2 Vector2::FromAngle(float radians)
{
    return {std::cos(radians), std::sin(radians)};
}

Vector2 Vector2::FromAngle(float radians, float magnitude)
{
    return FromAngle(radians) * magnitude;
}

Vector2 Vector2::Rotated(float radians) const
{
    float cos_r = std::cos(radians);
    float sin_r = std::sin(radians);
    return {x * cos_r - y * sin_r, x * sin_r + y * cos_r};
}

Vector2 &Vector2::Rotate(float radians)
{
    *this = Rotated(radians);
    return *this;
}
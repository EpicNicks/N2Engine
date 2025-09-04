#include "math/Vector3.hpp"
#include <cmath>
#include <algorithm>

using namespace N2Engine::Math;

Vector3 &Vector3::operator=(const Vector3 &other)
{
    if (this != &other)
    {
        vector[0] = other.vector[0];
        vector[1] = other.vector[1];
        vector[2] = other.vector[2];
    }
    return *this;
}

Vector3::Vector3(const Vector3 &other) : VectorN<float, 3>(other) {}

Vector3::Vector3(const VectorN<float, 3> &base) : VectorN<float, 3>(base) {}

Vector3 Vector3::Up()
{
    return Vector3{0.0f, 1.0f, 0.0f};
}

Vector3 Vector3::Down()
{
    return Vector3{0.0f, -1.0f, 0.0f};
}

Vector3 Vector3::Left()
{
    return Vector3{-1.0f, 0.0f, 0.0f};
}

Vector3 Vector3::Right()
{
    return Vector3{1.0f, 0.0f, 0.0f};
}

Vector3 Vector3::Forward()
{
    return Vector3{0.0f, 0.0f, 1.0f};
}

Vector3 Vector3::Back()
{
    return Vector3{0.0f, 0.0f, -1.0f};
}

Vector3 Vector3::Cross(const Vector3 &other) const
{
    return Vector3{
        y * other.z - z * other.y,
        z * other.x - x * other.z,
        x * other.y - y * other.x};
}

Vector3 Vector3::Cross(const Vector3 &a, const Vector3 &b)
{
    return a.Cross(b);
}

float Vector3::AngleTo(const Vector3 &other) const
{
    float dot = Dot(other);
    float magProduct = Magnitude() * other.Magnitude();
    if (magProduct == 0.0f)
        return 0.0f;
    return std::acos(std::clamp(dot / magProduct, -1.0f, 1.0f));
}

Vector3 Vector3::Project(const Vector3 &onto) const
{
    float dot = Dot(onto);
    float ontoMagSq = onto.Dot(onto);
    if (ontoMagSq == 0.0f)
        return Vector3::Zero();
    return onto * (dot / ontoMagSq);
}

Vector3 Vector3::Reject(const Vector3 &onto) const
{
    return *this - Project(onto);
}

Vector3 Vector3::Reflect(const Vector3 &normal) const
{
    return *this - normal * (2.0f * Dot(normal));
}

Vector3 Vector3::RotateAroundAxis(const Vector3 &axis, float radians) const
{
    Vector3 normalizedAxis = axis.Normalized();
    float cosAngle = std::cos(radians);
    float sinAngle = std::sin(radians);
    float oneMinusCos = 1.0f - cosAngle;

    Vector3 parallel = normalizedAxis * Dot(normalizedAxis);
    Vector3 perpendicular = *this - parallel;
    Vector3 w = normalizedAxis.Cross(*this);

    return parallel + perpendicular * cosAngle + w * sinAngle;
}

Vector3 &Vector3::RotateAroundAxisInPlace(const Vector3 &axis, float radians)
{
    *this = RotateAroundAxis(axis, radians);
    return *this;
}

float Vector3::ScalarTriple(const Vector3 &a, const Vector3 &b, const Vector3 &c)
{
    return a.Dot(b.Cross(c));
}

Vector3 Vector3::VectorTriple(const Vector3 &a, const Vector3 &b, const Vector3 &c)
{
    return b * a.Dot(c) - c * a.Dot(b);
}

Vector3 Vector3::FromSpherical(float radius, float theta, float phi)
{
    float sinPhi = std::sin(phi);
    return Vector3{
        radius * sinPhi * std::cos(theta),
        radius * std::cos(phi),
        radius * sinPhi * std::sin(theta)};
}

void Vector3::ToSpherical(float &radius, float &theta, float &phi) const
{
    radius = Magnitude();
    if (radius == 0.0f)
    {
        theta = 0.0f;
        phi = 0.0f;
        return;
    }

    theta = std::atan2(z, x);
    phi = std::acos(std::clamp(y / radius, -1.0f, 1.0f));
}

bool Vector3::IsParallel(const Vector3 &other, float tolerance) const
{
    Vector3 cross = Cross(other);
    return cross.Magnitude() < tolerance;
}

bool Vector3::IsPerpendicular(const Vector3 &other, float tolerance) const
{
    return std::abs(Dot(other)) < tolerance;
}

Vector3 Vector3::GetOrthogonal() const
{
    if (std::abs(x) < 0.9f)
    {
        return Vector3{1.0f, 0.0f, 0.0f}.Cross(*this).Normalized();
    }
    else
    {
        return Vector3{0.0f, 1.0f, 0.0f}.Cross(*this).Normalized();
    }
}
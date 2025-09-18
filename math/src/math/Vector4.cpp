#include "math/Vector4.hpp"

using namespace N2Engine::Math;

Vector4 &Vector4::operator=(const Vector4 &other)
{
    if (this != &other)
    {
        vector = other.vector;
    }
    return *this;
}
Vector4::Vector4(const Vector4 &other) : VectorN<float, 4>(other) {}

Vector4::Vector4(const VectorN<float, 4> &base) : VectorN<float, 4>({base[0], base[1], base[2], base[3]}) {}

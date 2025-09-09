#include "engine/Transform.hpp"
#include <math/Matrix.hpp>

using namespace N2Engine;

constexpr Transform Transform::Identity()
{
    Transform transform;
    transform._transform = Matrix4::identity();
    return transform;
}
Transform Transform::Combine(const Transform &parent, const Transform &child)
{
    Transform result;
    result._transform = parent._transform * child._transform;
    return result;
}
bool Transform::operator==(const Transform &other) const
{
    return _transform == other._transform;
}
bool Transform::operator!=(const Transform &other) const
{
    return _transform != other._transform;
}
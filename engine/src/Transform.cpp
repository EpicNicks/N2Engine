#include "engine/Transform.hpp"
#include <math/Matrix.hpp>

using namespace N2Engine;
using namespace N2Engine::Math;

void Transform::BuildMatrix() const
{
    if (!_matrixDirty)
        return;

    // Create rotation matrix
    Matrix4 rotMatrix = _rotation.ToMatrix();

    // Apply scale and translation
    _cachedMatrix = Matrix4{
        rotMatrix(0, 0) * _scale.x, rotMatrix(0, 1) * _scale.y, rotMatrix(0, 2) * _scale.z, _position.x,
        rotMatrix(1, 0) * _scale.x, rotMatrix(1, 1) * _scale.y, rotMatrix(1, 2) * _scale.z, _position.y,
        rotMatrix(2, 0) * _scale.x, rotMatrix(2, 1) * _scale.y, rotMatrix(2, 2) * _scale.z, _position.z,
        0.0f, 0.0f, 0.0f, 1.0f};

    _matrixDirty = false;
}

const Transform::Matrix4 &Transform::GetMatrix() const
{
    BuildMatrix();
    return _cachedMatrix;
}

Transform Transform::Identity()
{
    Transform transform;
    return transform;
}

Transform Transform::Combine(const Transform &parent, const Transform &child)
{
    Transform result;

    Vector3 scaledChildPos = child._position.Scale(parent._scale);
    Vector3 rotatedChildPos = parent._rotation * scaledChildPos;
    result._position = parent._position + rotatedChildPos;

    result._rotation = parent._rotation * child._rotation;

    result._scale = parent._scale.Scale(child._scale);

    result._matrixDirty = true;
    return result;
}

bool Transform::operator==(const Transform &other) const
{
    return _position == other._position && _rotation == other._rotation && _scale == other._scale;
}

bool Transform::operator!=(const Transform &other) const
{
    return !(*this == other);
}

Math::Vector3 Transform::GetPosition() const
{
    return _position;
}

Math::Quaternion Transform::GetRotation() const
{
    return _rotation;
}

Math::Vector3 Transform::GetScale() const
{
    return _scale;
}

void Transform::SetPosition(Math::Vector3 position)
{
    if (_position != position)
    {
        _position = position;
        _matrixDirty = true;
    }
}

void Transform::SetRotation(Math::Quaternion rotation)
{
    if (_rotation != rotation)
    {
        _rotation = rotation;
        _matrixDirty = true;
    }
}

void Transform::SetScale(Math::Vector3 scale)
{
    if (_scale != scale)
    {
        _scale = scale;
        _matrixDirty = true;
    }
}

void Transform::SetPositionAndRotation(Math::Vector3 position, Math::Quaternion rotation)
{
    bool posChanged = (_position != position);
    bool rotChanged = (_rotation != rotation);

    _position = position;
    _rotation = rotation;

    if (posChanged || rotChanged)
    {
        _matrixDirty = true;
    }
}
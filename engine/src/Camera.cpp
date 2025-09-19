#include "engine/Camera.hpp"
#include <cmath>
#include <algorithm>

using namespace N2Engine;

float N2Engine::DistanceToPlane(const Math::Vector4 &plane, const BoundingBox &bbox)
{
    // Your Vector4 has order w,x,y,z, so plane normal is (x,y,z) and distance is w
    Math::Vector3 normal{plane.x, plane.y, plane.z};

    // Find the vertex that's furthest in the direction of the plane normal
    Math::Vector3 positiveVertex{
        (normal.x > 0) ? bbox.max.x : bbox.min.x,
        (normal.y > 0) ? bbox.max.y : bbox.min.y,
        (normal.z > 0) ? bbox.max.z : bbox.min.z};

    // Calculate distance from positive vertex to plane
    return positiveVertex.Dot(normal) + plane.w;
}

BoundingBox::BoundingBox() {}
BoundingBox::BoundingBox(const Math::Vector3 &min_, const Math::Vector3 &max_) : min(min_), max(max_) {}

Math::Vector3 BoundingBox::GetCenter() const
{
    return Math::Vector3{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
}

Math::Vector3 BoundingBox::GetExtents() const
{
    return Math::Vector3{(max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f, (max.z - min.z) * 0.5f};
}

Math::Vector3 BoundingBox::GetCorner(int index) const
{
    return Math::Vector3{
        (index & 1) ? max.x : min.x,
        (index & 2) ? max.y : min.y,
        (index & 4) ? max.z : min.z};
}

bool Frustum::IsVisible(const BoundingBox &bbox) const
{
    for (int i = 0; i < 6; i++)
    {
        if (DistanceToPlane(planes[i], bbox) < 0)
        {
            return false;
        }
    }
    return true;
}

void Camera::SetPosition(const Math::Vector3 &position)
{
    _position = position;
    _viewDirty = true;
}

void Camera::SetRotation(const Math::Quaternion &rotation)
{
    _rotation = rotation;
    _viewDirty = true;
}

void Camera::SetOrthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane)
{
    _projectionType = ProjectionType::Orthographic;
    _orthoLeft = left;
    _orthoRight = right;
    _orthoBottom = bottom;
    _orthoTop = top;
    _nearPlane = nearPlane;
    _farPlane = farPlane;
    _projectionDirty = true;
}

void Camera::SetPerspective(float fov, float aspect, float nearPlane, float farPlane)
{
    _projectionType = ProjectionType::Perspective;
    _fov = fov;
    _aspectRatio = aspect;
    _nearPlane = nearPlane;
    _farPlane = farPlane;
    _projectionDirty = true;
}

void Camera::UpdateProjectionMatrix() const
{
    // Clear the matrix
    _projectionMatrix = Matrix4();

    if (_projectionType == ProjectionType::Perspective)
    {
        // Perspective projection (existing code)
        float tanHalfFov = std::tan(_fov * 0.5f * (3.14159f / 180.0f));

        _projectionMatrix(0, 0) = 1.0f / (_aspectRatio * tanHalfFov);
        _projectionMatrix(1, 1) = 1.0f / tanHalfFov;
        _projectionMatrix(2, 2) = -(_farPlane + _nearPlane) / (_farPlane - _nearPlane);
        _projectionMatrix(2, 3) = -(2.0f * _farPlane * _nearPlane) / (_farPlane - _nearPlane);
        _projectionMatrix(3, 2) = -1.0f;
    }
    else
    { // Orthographic projection
        _projectionMatrix(0, 0) = 2.0f / (_orthoRight - _orthoLeft);
        _projectionMatrix(0, 3) = -(_orthoRight + _orthoLeft) / (_orthoRight - _orthoLeft);

        _projectionMatrix(1, 1) = 2.0f / (_orthoTop - _orthoBottom);
        _projectionMatrix(1, 3) = -(_orthoTop + _orthoBottom) / (_orthoTop - _orthoBottom);

        _projectionMatrix(2, 2) = -2.0f / (_farPlane - _nearPlane);
        _projectionMatrix(2, 3) = -(_farPlane + _nearPlane) / (_farPlane - _nearPlane);

        _projectionMatrix(3, 3) = 1.0f;
    }
}

void Camera::LookAt(const Math::Vector3 &target, const Math::Vector3 &up)
{
    Math::Vector3 forward = (target - _position).Normalized();
    _rotation = Math::Quaternion::LookRotation(forward, up);
    _viewDirty = true;
}

const Matrix4 &Camera::GetViewMatrix() const
{
    if (_viewDirty)
    {
        UpdateViewMatrix();
        _viewDirty = false;
    }
    return _viewMatrix;
}

const Matrix4 &Camera::GetProjectionMatrix() const
{
    if (_projectionDirty)
    {
        UpdateProjectionMatrix();
        _projectionDirty = false;
    }
    return _projectionMatrix;
}

Matrix4 Camera::GetViewProjectionMatrix() const
{
    return GetProjectionMatrix() * GetViewMatrix();
}

void Camera::UpdateViewMatrix() const
{
    // Create translation and rotation matrices separately
    Matrix4 translation = Matrix4::Translation(-_position);
    Matrix4 rotation = _rotation.Inverse().ToMatrix(); // Inverse rotation

    // View matrix = inverse rotation * inverse translation
    _viewMatrix = rotation * translation;
}

Frustum Camera::GetViewFrustum() const
{
    Frustum frustum;
    Matrix4 viewProj = GetViewProjectionMatrix();

    // Extract 6 frustum planes from view-projection matrix
    // Using your matrix(row, col) indexing

    // Left plane: clip.w + clip.x < 0
    frustum.planes[0] = Math::Vector4{
        viewProj(3, 3) + viewProj(0, 3), // w component (distance)
        viewProj(3, 0) + viewProj(0, 0), // x component (normal.x)
        viewProj(3, 1) + viewProj(0, 1), // y component (normal.y)
        viewProj(3, 2) + viewProj(0, 2)  // z component (normal.z)
    };

    // Right plane: clip.w - clip.x < 0
    frustum.planes[1] = Math::Vector4{
        viewProj(3, 3) - viewProj(0, 3),
        viewProj(3, 0) - viewProj(0, 0),
        viewProj(3, 1) - viewProj(0, 1),
        viewProj(3, 2) - viewProj(0, 2)};

    // Bottom plane: clip.w + clip.y < 0
    frustum.planes[2] = Math::Vector4{
        viewProj(3, 3) + viewProj(1, 3),
        viewProj(3, 0) + viewProj(1, 0),
        viewProj(3, 1) + viewProj(1, 1),
        viewProj(3, 2) + viewProj(1, 2)};

    // Top plane: clip.w - clip.y < 0
    frustum.planes[3] = Math::Vector4{
        viewProj(3, 3) - viewProj(1, 3),
        viewProj(3, 0) - viewProj(1, 0),
        viewProj(3, 1) - viewProj(1, 1),
        viewProj(3, 2) - viewProj(1, 2)};

    // Near plane: clip.w + clip.z < 0
    frustum.planes[4] = Math::Vector4{
        viewProj(3, 3) + viewProj(2, 3),
        viewProj(3, 0) + viewProj(2, 0),
        viewProj(3, 1) + viewProj(2, 1),
        viewProj(3, 2) + viewProj(2, 2)};

    // Far plane: clip.w - clip.z < 0
    frustum.planes[5] = Math::Vector4{
        viewProj(3, 3) - viewProj(2, 3),
        viewProj(3, 0) - viewProj(2, 0),
        viewProj(3, 1) - viewProj(2, 1),
        viewProj(3, 2) - viewProj(2, 2)};

    // Normalize planes
    for (int i = 0; i < 6; i++)
    {
        float length = std::sqrt(frustum.planes[i].x * frustum.planes[i].x +
                                 frustum.planes[i].y * frustum.planes[i].y +
                                 frustum.planes[i].z * frustum.planes[i].z);
        if (length > 0)
        {
            frustum.planes[i].x /= length;
            frustum.planes[i].y /= length;
            frustum.planes[i].z /= length;
            frustum.planes[i].w /= length;
        }
    }

    return frustum;
}
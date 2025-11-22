#pragma once

#include <math/Matrix.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <math/Quaternion.hpp>

namespace N2Engine
{
    using Matrix4 = Math::Matrix<float, 4, 4>;

    struct BoundingBox
    {
        Math::Vector3 min; // Minimum corner
        Math::Vector3 max; // Maximum corner

        BoundingBox();
        BoundingBox(const Math::Vector3 &min_, const Math::Vector3 &max_);

        // Get center point
        [[nodiscard]] Math::Vector3 GetCenter() const;
        // Get extents (half-sizes)
        [[nodiscard]] Math::Vector3 GetExtents() const;
        // Get corner point (0-7, where bits represent min/max for each axis)
        [[nodiscard]] Math::Vector3 GetCorner(int index) const;
    };

    struct Frustum
    {
        Math::Vector4 planes[6]; // left, right, bottom, top, near, far

        [[nodiscard]] bool IsVisible(const BoundingBox &bbox) const;
    };

    class Camera
    {
    public:
        enum class OrthographicResizeMode
        {
            MaintainVertical,   // Keep vertical size, adjust horizontal
            MaintainHorizontal, // Keep horizontal size, adjust vertical
            MaintainLarger      // Keep larger dimension, adjust smaller
        };

    private:
        mutable Matrix4 _viewMatrix;
        mutable Matrix4 _projectionMatrix;
        mutable bool _viewDirty = true;
        mutable bool _projectionDirty = true;

        Math::Vector3 _position{0.0f, 0.0f, 0.0f};
        Math::Quaternion _rotation = Math::Quaternion::Identity();
        float _fov = 45.0f;
        float _aspectRatio = 16.0f / 9.0f;
        float _nearPlane = 0.1f;
        float _farPlane = 100.0f;

        void UpdateViewMatrix() const;
        void UpdateProjectionMatrix() const;

        enum class ProjectionType
        {
            Perspective,
            Orthographic
        };

        ProjectionType _projectionType = ProjectionType::Perspective;
        float _orthoLeft = -1.0f;
        float _orthoRight = 1.0f;
        float _orthoBottom = -1.0f;
        float _orthoTop = 1.0f;

        OrthographicResizeMode _orthoResizeMode = OrthographicResizeMode::MaintainVertical;

    public:
        // Position and orientation
        void SetPosition(const Math::Vector3 &position);
        void SetRotation(const Math::Quaternion &rotation);
        void LookAt(const Math::Vector3 &target, const Math::Vector3 &up = Math::Vector3{0, 1, 0});

        // Projection settings
        void SetPerspective(float fov, float aspect, float nearPlane, float farPlane);
        void SetOrthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane);

        void SetOrthographicResizeMode(OrthographicResizeMode mode) { _orthoResizeMode = mode; }

        void UpdateAspectRatio(float newAspect);
        float GetAspectRatio() const { return _aspectRatio; }

        // Matrix getters
        const Matrix4 &GetViewMatrix() const;
        const Matrix4 &GetProjectionMatrix() const;
        Matrix4 GetViewProjectionMatrix() const;

        // For frustum culling
        Frustum GetViewFrustum() const;

        // Camera properties
        float GetNearPlane() const
        {
            return _nearPlane;
        }
        float GetFarPlane() const
        {
            return _farPlane;
        }
        float GetFOV() const
        {
            return _fov;
        }

        Math::Vector3 GetPosition() const
        {
            return _position;
        }
        Math::Quaternion GetRotation() const
        {
            return _rotation;
        }
    };

    // Helper function
    float DistanceToPlane(const Math::Vector4 &plane, const BoundingBox &bbox);
}
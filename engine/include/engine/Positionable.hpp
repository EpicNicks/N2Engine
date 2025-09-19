#pragma once

#include <math/Vector3.hpp>
#include <math/Quaternion.hpp>
#include "engine/Transform.hpp"
#include <memory>

namespace N2Engine
{
    class GameObject;

    /**
     * Handles spatial transformation for GameObjects that need positioning.
     * Uses lazy evaluation and dirty flagging for optimal performance.
     */
    class Positionable
    {
    public:
        using Matrix4 = Math::Matrix<float, 4, 4>;

    private:
        // Only store local transform - global is calculated on demand
        Transform _localTransform;
        GameObject &_gameObject;

        // Cached global transform with dirty tracking
        mutable Transform _cachedGlobalTransform;
        mutable bool _globalTransformDirty = true;
        mutable bool _globalTransformValid = false;

        // Hierarchy change detection
        mutable uint32_t _hierarchyVersion = 0;
        mutable uint32_t _cachedHierarchyVersion = 0;

        // Internal methods
        void MarkGlobalTransformDirty() const;
        void MarkChildrenGlobalTransformDirty() const;
        void UpdateGlobalTransform() const;
        Transform CalculateGlobalTransform() const;
        std::shared_ptr<Positionable> GetParentPositionable() const;

    public:
        explicit Positionable(GameObject &gameObject);

        // Local transform access (stored directly)
        const Math::Vector3 GetLocalPosition() const { return _localTransform.GetPosition(); }
        const Math::Quaternion GetLocalRotation() const { return _localTransform.GetRotation(); }
        const Math::Vector3 GetLocalScale() const { return _localTransform.GetScale(); }

        void SetLocalPosition(const Math::Vector3 &position);
        void SetLocalRotation(const Math::Quaternion &rotation);
        void SetLocalScale(const Math::Vector3 &scale);

        // World/Global transform access (calculated on demand)
        Math::Vector3 GetPosition() const;
        Math::Quaternion GetRotation() const;
        Math::Vector3 GetScale() const;

        void SetPosition(const Math::Vector3 &position);
        void SetRotation(const Math::Quaternion &rotation);
        void SetScale(const Math::Vector3 &scale);

        // Efficient batch operations
        void SetLocalPositionAndRotation(const Math::Vector3 &position, const Math::Quaternion &rotation);
        void SetPositionAndRotation(const Math::Vector3 &position, const Math::Quaternion &rotation);
        void SetLocalTransform(const Transform &transform);

        // Transform matrix access
        const Transform &GetLocalTransform() const;
        const Transform &GetGlobalTransform() const;
        Matrix4 GetLocalToWorldMatrix() const;
        Matrix4 GetWorldToLocalMatrix() const;

        // Utility methods
        Math::Vector3 TransformPoint(const Math::Vector3 &point) const;
        Math::Vector3 TransformDirection(const Math::Vector3 &direction) const;
        Math::Vector3 InverseTransformPoint(const Math::Vector3 &point) const;
        Math::Vector3 InverseTransformDirection(const Math::Vector3 &direction) const;

        // Hierarchy change notification
        void OnHierarchyChanged();

        // Debug/Editor support
        bool IsGlobalTransformDirty() const;
    };
}
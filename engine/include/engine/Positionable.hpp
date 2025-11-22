#pragma once

#include <math/Vector3.hpp>
#include <math/Quaternion.hpp>

#include <nlohmann/json.hpp>

#include "engine/Transform.hpp"

namespace N2Engine
{
    class GameObject;

    namespace Physics
    {
        class Rigidbody;
    }

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

        mutable Physics::Rigidbody* _attachedRigidbody = nullptr;

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
        Positionable* GetParentPositionable() const;

        void NotifyPhysicsComponents() const;

    public:
        explicit Positionable(GameObject &gameObject);

        // Local transform access (stored directly)
        Math::Vector3 GetLocalPosition() const { return _localTransform.GetPosition(); }
        Math::Quaternion GetLocalRotation() const { return _localTransform.GetRotation(); }
        Math::Vector3 GetLocalScale() const { return _localTransform.GetScale(); }

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

        // Serialization
        nlohmann::json Serialize() const;
        void Deserialize(const nlohmann::json &j);

        // Hierarchy change notification
        void OnHierarchyChanged() const;

        // Debug/Editor support
        bool IsGlobalTransformDirty() const;
    };
}
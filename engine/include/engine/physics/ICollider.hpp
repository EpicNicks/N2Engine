#pragma once

#include "engine/Component.hpp"
#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include <math/Vector3.hpp>

#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Physics
{
    class IPhysicsBackend;
    struct Collision;
    struct Trigger;

    class ICollider : public SerializableComponent
    {
    public:
        explicit ICollider(GameObject& gameObject);

        void OnAttach() override;
        void OnDestroy() override;
        /// While the collider is disabled or its object inactive, its shapes are taken off the body (no
        /// contacts, triggers or query hits); enabling it puts them back with its current settings
        void OnEnable() override;
        void OnDisable() override;

        void SetIsTrigger(bool isTrigger);
        [[nodiscard]] bool IsTrigger() const { return _isTrigger; }

        void SetMaterial(const PhysicsMaterial& material);
        [[nodiscard]] PhysicsMaterial GetMaterial() const { return _material; }

        void SetOffset(const Math::Vector3& offset);
        [[nodiscard]] Math::Vector3 GetOffset() const { return _offset; }

        void OnCollisionEnter(const Collision& collision) override {}
        void OnCollisionStay(const Collision& collision) override {}
        void OnCollisionExit(const Collision& collision) override {}

        void OnTriggerEnter(Trigger trigger) override {}
        void OnTriggerStay(Trigger trigger) override {}
        void OnTriggerExit(Trigger trigger) override {}

        void OnTransformChanged() const;
        /// GameObject::SetLayer calls it: moves this collider's shapes to the object's new layer
        void OnLayerChanged();

        [[nodiscard]] PhysicsBodyHandle GetHandle() const { return _handle; }

    protected:
        void OnActiveFlagChanged() override;

        virtual void AttachShape(IPhysicsBackend* backend) = 0;
        virtual void UpdateShapeGeometry() = 0;

        bool _isTrigger = false;
        PhysicsMaterial _material = PhysicsMaterial::Default();
        Math::Vector3 _offset = Math::Vector3::Zero;

    private:
        /// Adds this collider's shapes to its body or removes them (a no-op if they already are, or without a body)
        void SetShapesAttached(bool attached);

        PhysicsBodyHandle _handle;
        bool _ownsBody = false;
        bool _shapesAttached = false;
    };
}

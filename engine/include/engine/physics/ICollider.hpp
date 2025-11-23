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

        [[nodiscard]] PhysicsBodyHandle GetHandle() const { return _handle; }

    protected:
        virtual void AttachShape(IPhysicsBackend* backend) = 0;
        virtual void UpdateShapeGeometry() = 0;

        bool _isTrigger = false;
        PhysicsMaterial _material = PhysicsMaterial::Default();
        Math::Vector3 _offset = Math::Vector3::Zero();

    private:
        PhysicsBodyHandle _handle;
        bool _ownsBody = false;
    };
}

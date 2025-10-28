#pragma once

#include "engine/Component.hpp"
#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include <math/Vector3.hpp>

namespace N2Engine::Physics
{

    class IPhysicsBackend;
    /**
     * Base class for all collider components
     * Can exist without a Rigidbody (creates static body)
     * Can have Rigidbody (uses Rigidbody's body)
     */
    class ICollider : public Component
    {
    public:
        virtual ~ICollider() = default;

        void OnAttach() override;
        void OnDestroy() override;

        // Configuration
        void SetIsTrigger(bool isTrigger);
        bool IsTrigger() const { return _isTrigger; }

        void SetMaterial(const Physics::PhysicsMaterial &material);
        Physics::PhysicsMaterial GetMaterial() const { return _material; }

        void SetOffset(const Math::Vector3 &offset) { _offset = offset; }
        Math::Vector3 GetOffset() const { return _offset; }

        // Collision callbacks - override in derived classes
        virtual void OnCollisionEnter(const Physics::Collision &collision) {}
        virtual void OnCollisionStay(const Physics::Collision &collision) {}
        virtual void OnCollisionExit(const Physics::Collision &collision) {}

        virtual void OnTriggerEnter(const Physics::Trigger &trigger) {}
        virtual void OnTriggerExit(const Physics::Trigger &trigger) {}

        void OnTransformChanged();

        // Internal
        Physics::PhysicsBodyHandle GetHandle() const { return _handle; }

    protected:
        // Derived classes implement this to add their specific shape
        virtual void AttachShape(Physics::IPhysicsBackend *backend) = 0;

        bool _isTrigger = false;
        Physics::PhysicsMaterial _material = Physics::PhysicsMaterial::Default();
        Math::Vector3 _offset = Math::Vector3::Zero();

    private:
        Physics::PhysicsBodyHandle _handle;
        bool _ownsBody = false; // True if we created the body (no Rigidbody present)
    };

}
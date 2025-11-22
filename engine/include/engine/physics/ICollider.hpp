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
        explicit ICollider(GameObject &gameObject);

        void OnAttach() override;
        void OnDestroy() override;

        // Configuration
        void SetIsTrigger(bool isTrigger);
        [[nodiscard]] bool IsTrigger() const { return _isTrigger; }

        void SetMaterial(const Physics::PhysicsMaterial &material);
        [[nodiscard]] PhysicsMaterial GetMaterial() const { return _material; }

        void SetOffset(const Math::Vector3 &offset) { _offset = offset; }
        [[nodiscard]] Math::Vector3 GetOffset() const { return _offset; }

        void OnTransformChanged() const;

        // Internal
        [[nodiscard]] PhysicsBodyHandle GetHandle() const { return _handle; }

    protected:
        // Derived classes implement this to add their specific shape
        virtual void AttachShape(IPhysicsBackend *backend) = 0;

        bool _isTrigger = false;
        PhysicsMaterial _material = PhysicsMaterial::Default();
        Math::Vector3 _offset = Math::Vector3::Zero();

    private:
        PhysicsBodyHandle _handle;
        bool _ownsBody = false; // True if we created the body (no Rigidbody present)
    };

}
#pragma once

#include "engine/Component.hpp"
#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include <math/Vector3.hpp>

namespace N2Engine::Physics
{
    enum class BodyType
    {
        Static,   // Doesn't move (ground, walls)
        Dynamic,  // Affected by forces (player, enemies, projectiles)
        Kinematic // Moves via transform, not affected by forces (moving platforms)
    };

    /**
     * Rigidbody component - adds physics simulation to a GameObject
     * Automatically creates a physics body when the component starts
     */
    class Rigidbody : public Component
    {
    public:
        Rigidbody(GameObject &gameObject);
        ~Rigidbody();

        void OnAttach() override;
        void OnDestroy() override;

        // ========== Body Configuration ==========
        void SetBodyType(BodyType type);
        BodyType GetBodyType() const { return _bodyType; }

        void SetMass(float mass);
        float GetMass() const;

        void SetGravityEnabled(bool enabled);
        bool IsGravityEnabled() const { return _gravityEnabled; }

        // ========== Forces and Motion ==========
        void AddForce(const Math::Vector3 &force);
        void AddImpulse(const Math::Vector3 &impulse);
        void SetVelocity(const Math::Vector3 &velocity);
        void SetAngularVelocity(const Math::Vector3 &velocity);

        // ========== Queries ==========
        Math::Vector3 GetVelocity() const;
        Math::Vector3 GetAngularVelocity() const;

        // ========== Handle Access ==========
        Physics::PhysicsBodyHandle GetHandle() const { return _handle; }
        bool IsInitialized() const { return _initialized; }

        // ========== Collision Callbacks ==========
        // Override these in derived classes or scripts
        virtual void OnCollisionEnter(const Physics::Collision &collision) {}
        virtual void OnCollisionStay(const Physics::Collision &collision) {}
        virtual void OnCollisionExit(const Physics::Collision &collision) {}

        // ========== Trigger Callbacks ==========
        virtual void OnTriggerEnter(const Physics::Trigger &trigger) {}
        virtual void OnTriggerExit(const Physics::Trigger &trigger) {}

        // ========== Notify Rigidbody of Transform Changes ==========
        void OnTransformChanged();

    private:
        Physics::PhysicsBodyHandle _handle;
        BodyType _bodyType = BodyType::Dynamic;
        float _mass = 1.0f;
        bool _gravityEnabled = true;
        bool _initialized = false;
    };
}
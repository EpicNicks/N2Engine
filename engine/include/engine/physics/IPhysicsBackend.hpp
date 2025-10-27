#pragma once

#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include <math/Vector3.hpp>
#include <math/Quaternion.hpp>

namespace N2Engine::Physics
{
    class Rigidbody;
    class Collider;
    /**
     * Abstract interface for physics backends
     * Allows swapping between PhysX, Bullet, Jolt, etc.
     */
    class IPhysicsBackend
    {
    public:
        virtual ~IPhysicsBackend() = default;

        // ========== Lifecycle ==========
        virtual bool Initialize() = 0;
        virtual void Update(float deltaTime) = 0;
        virtual void Shutdown() = 0;

        // ========== Deferred Modifications ==========
        virtual void ApplyPendingChanges() = 0;

        // ========== Transform Syncing and Callbacks ==========
        virtual void SyncTransforms() = 0;
        virtual void ProcessCollisionCallbacks() = 0;

        // ========== Body Creation/Destruction ==========
        virtual PhysicsBodyHandle CreateDynamicBody(
            const Math::Vector3 &position,
            const Math::Quaternion &rotation,
            float mass,
            Rigidbody *rigidbody = nullptr) = 0;

        virtual PhysicsBodyHandle CreateStaticBody(
            const Math::Vector3 &position,
            const Math::Quaternion &rotation,
            Rigidbody *rigidbody = nullptr) = 0;

        virtual void DestroyBody(PhysicsBodyHandle handle) = 0;

        // ========== Component Registration ==========
        // Allow Colliders to register themselves with an existing body
        virtual void RegisterCollider(PhysicsBodyHandle handle, Collider *collider) = 0;
        virtual void UnregisterCollider(PhysicsBodyHandle handle, Collider *collider) = 0;

        // ========== Shape Attachment ==========
        virtual void AddSphereCollider(
            PhysicsBodyHandle body,
            float radius,
            const Math::Vector3 &localOffset = Math::Vector3::Zero(),
            const PhysicsMaterial &material = PhysicsMaterial::Default()) = 0;

        virtual void AddBoxCollider(
            PhysicsBodyHandle body,
            const Math::Vector3 &halfExtents,
            const Math::Vector3 &localOffset = Math::Vector3::Zero(),
            const PhysicsMaterial &material = PhysicsMaterial::Default()) = 0;

        virtual void AddCapsuleCollider(
            PhysicsBodyHandle body,
            float radius,
            float height,
            const Math::Vector3 &localOffset = Math::Vector3::Zero(),
            const PhysicsMaterial &material = PhysicsMaterial::Default()) = 0;

        // ========== Trigger Configuration ==========
        virtual void SetIsTrigger(PhysicsBodyHandle body, bool isTrigger) = 0;

        // ========== Forces and Motion ==========
        virtual void AddForce(PhysicsBodyHandle body, const Math::Vector3 &force) = 0;
        virtual void AddImpulse(PhysicsBodyHandle body, const Math::Vector3 &impulse) = 0;
        virtual void SetVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity) = 0;
        virtual void SetAngularVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity) = 0;

        // ========== Queries ==========
        virtual Math::Vector3 GetPosition(PhysicsBodyHandle body) = 0;
        virtual Math::Quaternion GetRotation(PhysicsBodyHandle body) = 0;
        virtual Math::Vector3 GetVelocity(PhysicsBodyHandle body) = 0;
        virtual Math::Vector3 GetAngularVelocity(PhysicsBodyHandle body) = 0;

        // ========== Body Properties ==========
        virtual void SetMass(PhysicsBodyHandle body, float mass) = 0;
        virtual float GetMass(PhysicsBodyHandle body) = 0;
        virtual void SetGravityEnabled(PhysicsBodyHandle body, bool enabled) = 0;

        // ========== Scene Settings ==========
        virtual void SetGravity(const Math::Vector3 &gravity) = 0;
        virtual Math::Vector3 GetGravity() const = 0;
    };

}
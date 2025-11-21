#pragma once

#include <math/Vector3.hpp>
#include <math/Quaternion.hpp>

#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsMaterial.hpp"

namespace N2Engine::Physics
{
    class Rigidbody;
    class ICollider;
    /**
     * Abstract interface for physics backends
     * Allows swapping between PhysX, Bullet, Jolt, etc.
     */
    class IPhysicsBackend
    {
    public:
        virtual ~IPhysicsBackend() = default;

        virtual bool Initialize() = 0;
        virtual void Update(float deltaTime) = 0;
        virtual void Shutdown() = 0;

        virtual void ApplyPendingChanges() = 0;

        virtual void SyncTransforms() = 0;
        virtual void ProcessCollisionCallbacks() = 0;

        virtual PhysicsBodyHandle CreateDynamicBody(const Math::Vector3& position, const Math::Quaternion& rotation,
                                                    float mass, Rigidbody* rigidbody,
                                                    bool isKinematic) = 0;

        virtual PhysicsBodyHandle CreateStaticBody(
            const Math::Vector3& position,
            const Math::Quaternion& rotation,
            Rigidbody* rigidbody) = 0;

        virtual void DestroyBody(PhysicsBodyHandle handle) = 0;

        virtual void RegisterCollider(PhysicsBodyHandle handle, ICollider* collider) = 0;
        virtual void UnregisterCollider(PhysicsBodyHandle handle, ICollider* collider) = 0;

        virtual void SetBodyTransform(PhysicsBodyHandle handle, const Math::Vector3& position,
                                      const Math::Quaternion& rotation) = 0;

        virtual void SetStaticBodyTransform(PhysicsBodyHandle handle, const Math::Vector3& position,
                                            const Math::Quaternion& rotation) = 0;

        virtual void AddSphereCollider(PhysicsBodyHandle body, float radius,
                                       const Math::Vector3& localOffset,
                                       const PhysicsMaterial& material) = 0;

        virtual void AddBoxCollider(
            PhysicsBodyHandle body,
            const Math::Vector3& halfExtents,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) = 0;

        virtual void AddCapsuleCollider(
            PhysicsBodyHandle body,
            float radius,
            float height,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) = 0;

        virtual void SetIsTrigger(PhysicsBodyHandle body, bool isTrigger) = 0;

        virtual void AddForce(PhysicsBodyHandle body, const Math::Vector3& force) = 0;
        virtual void AddImpulse(PhysicsBodyHandle body, const Math::Vector3& impulse) = 0;
        virtual void SetVelocity(PhysicsBodyHandle body, const Math::Vector3& velocity) = 0;
        virtual void SetAngularVelocity(PhysicsBodyHandle body, const Math::Vector3& velocity) = 0;

        virtual Math::Vector3 GetPosition(PhysicsBodyHandle body) = 0;
        virtual Math::Quaternion GetRotation(PhysicsBodyHandle body) = 0;
        virtual Math::Vector3 GetVelocity(PhysicsBodyHandle body) = 0;
        virtual Math::Vector3 GetAngularVelocity(PhysicsBodyHandle body) = 0;

        virtual void SetMass(PhysicsBodyHandle body, float mass) = 0;
        virtual float GetMass(PhysicsBodyHandle body) = 0;
        virtual void SetGravityEnabled(PhysicsBodyHandle body, bool enabled) = 0;

        virtual void SetGravity(const Math::Vector3& gravity) = 0;
        virtual Math::Vector3 GetGravity() const = 0;
    };
}

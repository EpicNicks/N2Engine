#pragma once

#include <math/Vector3.hpp>
#include <math/Quaternion.hpp>
#include "engine/physics/PhysicsHandle.hpp"

namespace N2Engine
{
    namespace Physics
    {
        class IPhysicsBackend
        {
        public:
            virtual ~IPhysicsBackend() = default;

            virtual void Initialize() = 0;
            virtual void Update(float deltaTime) = 0;
            virtual void Shutdown() = 0;

            // Body creation
            virtual PhysicsBodyHandle CreateDynamicBody(
                const Math::Vector3 &position,
                const Math::Quaternion &rotation,
                float mass) = 0;

            virtual PhysicsBodyHandle CreateStaticBody(
                const Math::Vector3 &position,
                const Math::Quaternion &rotation) = 0;

            virtual void DestroyBody(PhysicsBodyHandle handle) = 0;

            // Collider shapes
            virtual void AddBoxCollider(PhysicsBodyHandle body, const Math::Vector3 &halfExtents) = 0;
            virtual void AddSphereCollider(PhysicsBodyHandle body, float radius) = 0;
            virtual void AddCapsuleCollider(PhysicsBodyHandle body, float radius, float height) = 0;

            // Forces and motion
            virtual void AddForce(PhysicsBodyHandle body, const Math::Vector3 &force) = 0;
            virtual void AddImpulse(PhysicsBodyHandle body, const Math::Vector3 &impulse) = 0;
            virtual void SetVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity) = 0;

            // Queries
            virtual Math::Vector3 GetPosition(PhysicsBodyHandle body) = 0;
            virtual Math::Quaternion GetRotation(PhysicsBodyHandle body) = 0;
            virtual Math::Vector3 GetVelocity(PhysicsBodyHandle body) = 0;

            // Collision callbacks
            virtual void SetCollisionCallback(PhysicsBodyHandle body,
                                              std::function<void(PhysicsBodyHandle)> callback) = 0;
        };
    }
}
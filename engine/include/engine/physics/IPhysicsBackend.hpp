#pragma once

#include <math/Vector3.hpp>
#include <math/Quaternion.hpp>
#include <cstdint>
#include <vector>

#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsMaterial.hpp"

namespace N2Engine::Physics
{
    class IParticleBackend;
    class Rigidbody;
    class ICollider;
    struct RaycastHit;
    enum class QueryTriggers : std::uint8_t; // Raycast.hpp

    class IPhysicsBackend
    {
    public:
        virtual ~IPhysicsBackend() = default;

        /// physics.gpu: call before Initialize (the scene is built there and its GPU flag is immutable). A backend
        /// without GPU support ignores it.
        virtual void SetGpuMode(GpuMode) {}
        /// What Initialize made of the GPU request. Disabled for a backend that never asks.
        [[nodiscard]] virtual GpuStatus GetGpuStatus() const
        {
            return {GpuState::Disabled, "This physics backend has no GPU support"};
        }

        /// The GPU particle backend, when this backend runs particles in its own (GPU) scene: nullptr by default and
        /// whenever the GPU scene isn't up. A ParticleSystem with backend Auto or PhysX uses it when it is there and
        /// falls back to the CPU backend otherwise. The backend belongs to this object and is valid until Shutdown.
        [[nodiscard]] virtual IParticleBackend *GetParticleBackend() { return nullptr; }

        virtual bool Initialize() = 0;
        virtual void Update(float deltaTime) = 0;
        virtual void Shutdown() = 0;

        virtual void ApplyPendingChanges() = 0;

        virtual void SyncTransforms() = 0;
        virtual void ProcessCollisionCallbacks() = 0;

        virtual PhysicsBodyHandle CreateDynamicBody(
            const Math::Vector3& position,
            const Math::Quaternion& rotation,
            float mass,
            Rigidbody* rigidbody,
            bool isKinematic) = 0;

        virtual PhysicsBodyHandle CreateStaticBody(
            const Math::Vector3& position,
            const Math::Quaternion& rotation,
            Rigidbody* rigidbody) = 0;

        virtual void DestroyBody(PhysicsBodyHandle handle) = 0;

        /// Takes the body out of the simulation and scene queries (false) or puts it back (true), for a
        /// deactivated Rigidbody or GameObject. Its pairs end with an Exit; when it comes back it's moved to
        /// its GameObject's current pose, and pairs that still overlap start again with an Enter.
        virtual void SetBodyEnabled(PhysicsBodyHandle handle, bool enabled) = 0;

        virtual void RegisterCollider(PhysicsBodyHandle handle, ICollider* collider) = 0;
        virtual void UnregisterCollider(PhysicsBodyHandle handle, ICollider* collider) = 0;

        virtual void SetBodyTransform(
            PhysicsBodyHandle handle,
            const Math::Vector3& position,
            const Math::Quaternion& rotation) = 0;

        virtual void SetStaticBodyTransform(
            PhysicsBodyHandle handle,
            const Math::Vector3& position,
            const Math::Quaternion& rotation) = 0;

        // Shapes are recorded per owning collider, so they can be updated, toggled and removed
        // without touching other colliders on the same body
        virtual void AddSphereCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            float radius,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) = 0;

        virtual void AddBoxCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            const Math::Vector3& halfExtents,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) = 0;

        virtual void AddCapsuleCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            float radius,
            float height,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) = 0;

        virtual void RemoveColliderShapes(PhysicsBodyHandle body, ICollider* collider) = 0;

        virtual void UpdateSphereCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            float radius,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) = 0;

        virtual void UpdateBoxCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            const Math::Vector3& halfExtents,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) = 0;

        virtual void UpdateCapsuleCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            float radius,
            float height,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) = 0;

        /// Only the given collider's shapes; other colliders on the body keep their own setting
        virtual void SetIsTrigger(PhysicsBodyHandle body, ICollider* collider, bool isTrigger) = 0;

        /// Moves the collider's shapes to a layer (see Layers): its query layer, and which layers it collides
        /// with. Existing pairs are filtered again in the next step; one the matrix now rules out ends with an
        /// Exit, and one it now allows starts with an Enter if the shapes overlap.
        virtual void SetColliderLayer(PhysicsBodyHandle body, ICollider* collider, int layer) = 0;
        /// Re-reads Layers' collision matrix into every shape and filters the affected pairs again, as
        /// SetColliderLayer does. Layers calls it whenever the matrix changes.
        virtual void RefreshCollisionMatrix() = 0;

        virtual void AddForce(PhysicsBodyHandle body, const Math::Vector3& force) = 0;
        virtual void AddImpulse(PhysicsBodyHandle body, const Math::Vector3& impulse) = 0;
        virtual void SetVelocity(PhysicsBodyHandle body, const Math::Vector3& velocity) = 0;
        virtual void SetAngularVelocity(PhysicsBodyHandle body, const Math::Vector3& velocity) = 0;

        virtual Math::Vector3 GetPosition(PhysicsBodyHandle body) = 0;
        virtual Math::Quaternion GetRotation(PhysicsBodyHandle body) = 0;
        virtual Math::Vector3 GetVelocity(PhysicsBodyHandle body) = 0;
        virtual Math::Vector3 GetAngularVelocity(PhysicsBodyHandle body) = 0;

        /// Total mass; the backend keeps the inertia and centre of mass in step with the body's
        /// non-trigger shapes as they're added, changed or removed
        virtual void SetMass(PhysicsBodyHandle body, float mass) = 0;
        virtual float GetMass(PhysicsBodyHandle body) = 0;
        virtual void SetGravityEnabled(PhysicsBodyHandle body, bool enabled) = 0;

        virtual void SetGravity(const Math::Vector3& gravity) = 0;
        [[nodiscard]] virtual Math::Vector3 GetGravity() const = 0;

        // Queries: layerMask selects the layers hit (see Layers); 0 hits nothing. Raycast's wrappers default
        // it to Layers::DefaultRaycastMask. `triggers` (QueryTriggers::Ignore) skips trigger colliders; the
        // wrappers default it to Collide.
        virtual bool Raycast(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance,
            uint32_t layerMask,
            QueryTriggers triggers) = 0;

        virtual int RaycastAll(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            std::vector<RaycastHit>& hits,
            float maxDistance,
            uint32_t layerMask,
            QueryTriggers triggers) = 0;

        virtual bool SphereCast(
            const Math::Vector3& origin,
            float radius,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance,
            uint32_t layerMask) = 0;
    };
}
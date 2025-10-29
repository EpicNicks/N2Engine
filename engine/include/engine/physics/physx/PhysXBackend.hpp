// engine/include/engine/physics/physx/PhysXBackend.hpp
#pragma once

#include "engine/physics/IPhysicsBackend.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>

#ifdef N2ENGINE_PHYSX_ENABLED
#include <PxSimulationEventCallback.h>
#include <foundation/PxSimpleTypes.h>

#include <extensions/PxDefaultAllocator.h>
#include <extensions/PxDefaultErrorCallback.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxDefaultSimulationFilterShader.h>
#include <extensions/PxRigidBodyExt.h>

// Try to include PVD header - make it optional
#if __has_include(<pvd/PxPvd.h>)
#include <pvd/PxPvd.h>
#define N2ENGINE_PHYSX_HAS_PVD 1
#else
#define N2ENGINE_PHYSX_HAS_PVD 0
// Forward declare as incomplete type if not available
namespace physx
{
    class PxPvd;
}
#endif

#endif

namespace N2Engine
{
    class Rigidbody;
    class GameObject;
}

namespace N2Engine::Physics
{
    class ICollider;
    struct Collision;
    struct Trigger;

    class PhysXBackend : public IPhysicsBackend
#ifdef N2ENGINE_PHYSX_ENABLED
        ,
                         public physx::PxSimulationEventCallback
#endif
    {
    public:
        PhysXBackend();
        ~PhysXBackend() override;

        // ========== IPhysicsBackend Implementation ==========
        bool Initialize() override;
        void Update(float deltaTime) override;
        void Shutdown() override;

        void ApplyPendingChanges() override;
        void SyncTransforms() override;
        void ProcessCollisionCallbacks() override;

        PhysicsBodyHandle CreateDynamicBody(const Math::Vector3 &position, const Math::Quaternion &rotation, float mass, Rigidbody *rigidbody = nullptr, bool isKinematic = false) override;

        PhysicsBodyHandle CreateStaticBody(const Math::Vector3 &position, const Math::Quaternion &rotation, Rigidbody *rigidbody = nullptr) override;

        void DestroyBody(PhysicsBodyHandle handle) override;

        void RegisterCollider(PhysicsBodyHandle handle, ICollider *collider) override;
        void UnregisterCollider(PhysicsBodyHandle handle, ICollider *collider) override;

        void SetBodyTransform(PhysicsBodyHandle handle, const Math::Vector3 &position, const Math::Quaternion &rotation) override;
        void SetStaticBodyTransform(PhysicsBodyHandle handle, const Math::Vector3 &position, const Math::Quaternion &rotation) override;

        void AddSphereCollider(
            PhysicsBodyHandle body,
            float radius,
            const Math::Vector3 &localOffset = Math::Vector3::Zero(),
            const PhysicsMaterial &material = PhysicsMaterial::Default()) override;

        void AddBoxCollider(
            PhysicsBodyHandle body,
            const Math::Vector3 &halfExtents,
            const Math::Vector3 &localOffset = Math::Vector3::Zero(),
            const PhysicsMaterial &material = PhysicsMaterial::Default()) override;

        void AddCapsuleCollider(
            PhysicsBodyHandle body,
            float radius,
            float height,
            const Math::Vector3 &localOffset = Math::Vector3::Zero(),
            const PhysicsMaterial &material = PhysicsMaterial::Default()) override;

        void SetIsTrigger(PhysicsBodyHandle body, bool isTrigger) override;

        void AddForce(PhysicsBodyHandle body, const Math::Vector3 &force) override;
        void AddImpulse(PhysicsBodyHandle body, const Math::Vector3 &impulse) override;
        void SetVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity) override;
        void SetAngularVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity) override;

        Math::Vector3 GetPosition(PhysicsBodyHandle body) override;
        Math::Quaternion GetRotation(PhysicsBodyHandle body) override;
        Math::Vector3 GetVelocity(PhysicsBodyHandle body) override;
        Math::Vector3 GetAngularVelocity(PhysicsBodyHandle body) override;

        void SetMass(PhysicsBodyHandle body, float mass) override;
        float GetMass(PhysicsBodyHandle body) override;
        void SetGravityEnabled(PhysicsBodyHandle body, bool enabled) override;

        void SetGravity(const Math::Vector3 &gravity) override;
        Math::Vector3 GetGravity() const override;

#ifdef N2ENGINE_PHYSX_ENABLED
        // ========== PxSimulationEventCallback Interface ==========
        void onConstraintBreak(physx::PxConstraintInfo *constraints, physx::PxU32 count) override;
        void onWake(physx::PxActor **actors, physx::PxU32 count) override;
        void onSleep(physx::PxActor **actors, physx::PxU32 count) override;
        void onContact(const physx::PxContactPairHeader &pairHeader, const physx::PxContactPair *pairs, physx::PxU32 nbPairs) override;
        void onTrigger(physx::PxTriggerPair *pairs, physx::PxU32 count) override;
        void onAdvance(const physx::PxRigidBody *const *bodyBuffer, const physx::PxTransform *poseBuffer, const physx::PxU32 count) override;

    private:
        // ========== PhysX Objects ==========
        physx::PxFoundation *_foundation = nullptr;
        physx::PxPhysics *_physics = nullptr;
        physx::PxScene *_scene = nullptr;
        physx::PxMaterial *_defaultMaterial = nullptr;
        physx::PxDefaultCpuDispatcher *_dispatcher = nullptr;

        // PVD is optional - only available if header exists
#if N2ENGINE_PHYSX_HAS_PVD
        physx::PxPvd *_pvd = nullptr;
#endif

        // ========== Body Management ==========
        struct BodyData
        {
            physx::PxRigidActor *actor = nullptr;
            uint32_t generation = 0;
            bool active = false;

            // Component references
            Rigidbody *rigidbody = nullptr;     // Can be null (static objects)
            std::vector<ICollider *> colliders; // Can be multiple colliders per body
        };

        std::vector<BodyData> _bodies;
        std::vector<uint32_t> _freeList;

        PhysicsBodyHandle AllocateHandle();
        BodyData *GetBodyData(PhysicsBodyHandle handle);
        const BodyData *GetBodyData(PhysicsBodyHandle handle) const;

        // ========== Material Caching ==========
        struct MaterialKey
        {
            float staticFriction;
            float dynamicFriction;
            float restitution;

            bool operator==(const MaterialKey &other) const
            {
                return staticFriction == other.staticFriction &&
                       dynamicFriction == other.dynamicFriction &&
                       restitution == other.restitution;
            }
        };

        struct MaterialKeyHash
        {
            size_t operator()(const MaterialKey &k) const
            {
                return std::hash<float>()(k.staticFriction) ^
                       (std::hash<float>()(k.dynamicFriction) << 1) ^
                       (std::hash<float>()(k.restitution) << 2);
            }
        };

        std::unordered_map<MaterialKey, physx::PxMaterial *, MaterialKeyHash> _materialCache;
        physx::PxMaterial *GetOrCreateMaterial(const PhysicsMaterial &material);

        // ========== Deferred Scene Modifications ==========
        enum class ChangeType
        {
            SetGravity
        };

        std::unordered_map<ChangeType, std::function<void()>> _pendingChanges;
        Math::Vector3 _currentGravity{0.0f, -9.81f, 0.0f};

        // ========== Collision Tracking ==========
        struct CollisionPair
        {
            PhysicsBodyHandle bodyA;
            PhysicsBodyHandle bodyB;

            bool operator==(const CollisionPair &other) const
            {
                return (bodyA == other.bodyA && bodyB == other.bodyB) ||
                       (bodyA == other.bodyB && bodyB == other.bodyA);
            }
        };

        struct CollisionPairHash
        {
            size_t operator()(const CollisionPair &pair) const
            {
                size_t h1 = std::hash<uint32_t>()(pair.bodyA.index) ^
                            (std::hash<uint32_t>()(pair.bodyA.generation) << 1);
                size_t h2 = std::hash<uint32_t>()(pair.bodyB.index) ^
                            (std::hash<uint32_t>()(pair.bodyB.generation) << 1);
                return h1 ^ (h2 << 1);
            }
        };

        struct CollisionEvent
        {
            CollisionPair pair;
            Collision *data = nullptr; // Full collision data with contact points
        };

        struct TriggerEvent
        {
            CollisionPair pair;
        };

        std::vector<CollisionEvent> _newCollisions; // OnCollisionEnter
        std::unordered_set<CollisionPair, CollisionPairHash> _activeCollisions;
        std::vector<CollisionEvent> _endedCollisions; // OnCollisionExit
        std::vector<TriggerEvent> _newTriggers;       // OnTriggerEnter
        std::unordered_set<CollisionPair, CollisionPairHash> _activeTriggers;
        std::vector<TriggerEvent> _endedTriggers; // OnTriggerExit

        // Helper to create Collision/Trigger data with resolved references
        Collision CreateCollisionData(
            const CollisionPair &pair,
            const Collision &baseData,
            bool isForBodyA);

        Trigger CreateTriggerData(
            const CollisionPair &pair,
            bool isForBodyA);
#endif
    };
}
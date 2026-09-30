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
#include <foundation/PxTransform.h>
#include <geometry/PxGeometry.h>

#include <extensions/PxDefaultAllocator.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxDefaultErrorCallback.h>
#include <extensions/PxDefaultSimulationFilterShader.h>
#include <extensions/PxRigidBodyExt.h>

#if __has_include(<pvd/PxPvd.h>)
#include <pvd/PxPvd.h>
#define N2ENGINE_PHYSX_HAS_PVD 1
#else
#define N2ENGINE_PHYSX_HAS_PVD 0
namespace physx
{
    class PxPvd;
}
#endif

#endif

namespace N2Engine
{
    class GameObject;
    class Component;
}

namespace N2Engine::Physics
{
    class ICollider;
    struct Collision;
    struct Trigger;

    class PhysXBackend final : public IPhysicsBackend
#ifdef N2ENGINE_PHYSX_ENABLED
        , public physx::PxSimulationEventCallback
#endif
    {
    public:
        PhysXBackend();
        ~PhysXBackend() override;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Shutdown() override;

        void ApplyPendingChanges() override;
        void SyncTransforms() override;
        void ProcessCollisionCallbacks() override;

        PhysicsBodyHandle CreateDynamicBody(
            const Math::Vector3& position,
            const Math::Quaternion& rotation,
            float mass,
            Rigidbody* rigidbody,
            bool isKinematic) override;

        PhysicsBodyHandle CreateStaticBody(
            const Math::Vector3& position,
            const Math::Quaternion& rotation,
            Rigidbody* rigidbody) override;

        void DestroyBody(PhysicsBodyHandle handle) override;

        void RegisterCollider(PhysicsBodyHandle handle, ICollider* collider) override;
        void UnregisterCollider(PhysicsBodyHandle handle, ICollider* collider) override;

        void SetBodyTransform(
            PhysicsBodyHandle handle,
            const Math::Vector3& position,
            const Math::Quaternion& rotation) override;

        void SetStaticBodyTransform(
            PhysicsBodyHandle handle,
            const Math::Vector3& position,
            const Math::Quaternion& rotation) override;

        void AddSphereCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            float radius,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) override;

        void AddBoxCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            const Math::Vector3& halfExtents,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) override;

        void AddCapsuleCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            float radius,
            float height,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) override;

        void RemoveColliderShapes(PhysicsBodyHandle body, ICollider* collider) override;

        void UpdateSphereCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            float radius,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) override;

        void UpdateBoxCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            const Math::Vector3& halfExtents,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) override;

        void UpdateCapsuleCollider(
            PhysicsBodyHandle body,
            ICollider* collider,
            float radius,
            float height,
            const Math::Vector3& localOffset,
            const PhysicsMaterial& material) override;

        void SetIsTrigger(PhysicsBodyHandle body, ICollider* collider, bool isTrigger) override;

        void AddForce(PhysicsBodyHandle body, const Math::Vector3& force) override;
        void AddImpulse(PhysicsBodyHandle body, const Math::Vector3& impulse) override;
        void SetVelocity(PhysicsBodyHandle body, const Math::Vector3& velocity) override;
        void SetAngularVelocity(PhysicsBodyHandle body, const Math::Vector3& velocity) override;

        Math::Vector3 GetPosition(PhysicsBodyHandle body) override;
        Math::Quaternion GetRotation(PhysicsBodyHandle body) override;
        Math::Vector3 GetVelocity(PhysicsBodyHandle body) override;
        Math::Vector3 GetAngularVelocity(PhysicsBodyHandle body) override;

        void SetMass(PhysicsBodyHandle body, float mass) override;
        float GetMass(PhysicsBodyHandle body) override;
        void SetGravityEnabled(PhysicsBodyHandle body, bool enabled) override;

        void SetGravity(const Math::Vector3& gravity) override;
        [[nodiscard]] Math::Vector3 GetGravity() const override;

        bool Raycast(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance,
            uint32_t layerMask) override;

        int RaycastAll(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            std::vector<RaycastHit>& hits,
            float maxDistance,
            uint32_t layerMask) override;

        bool SphereCast(
            const Math::Vector3& origin,
            float radius,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance,
            uint32_t layerMask) override;

#ifdef N2ENGINE_PHYSX_ENABLED
        void onConstraintBreak(physx::PxConstraintInfo* constraints, physx::PxU32 count) override;
        void onWake(physx::PxActor** actors, physx::PxU32 count) override;
        void onSleep(physx::PxActor** actors, physx::PxU32 count) override;
        void onContact(const physx::PxContactPairHeader& pairHeader, const physx::PxContactPair* pairs, physx::PxU32 nbPairs) override;
        void onTrigger(physx::PxTriggerPair* pairs, physx::PxU32 count) override;
        void onAdvance(const physx::PxRigidBody* const* bodyBuffer, const physx::PxTransform* poseBuffer, const physx::PxU32 count) override;

    private:
        physx::PxDefaultAllocator _allocator;
        physx::PxDefaultErrorCallback _errorCallback;
        physx::PxFoundation* _foundation = nullptr;
        physx::PxPhysics* _physics = nullptr;
        physx::PxScene* _scene = nullptr;
        physx::PxMaterial* _defaultMaterial = nullptr;
        physx::PxDefaultCpuDispatcher* _dispatcher = nullptr;

#if N2ENGINE_PHYSX_HAS_PVD
        physx::PxPvd* _pvd = nullptr;
#endif

        struct BodyData
        {
            physx::PxRigidActor* actor = nullptr;
            uint32_t generation = 0;
            bool active = false;

            Rigidbody* rigidbody = nullptr;
            std::vector<ICollider*> colliders;
        };

        std::vector<BodyData> _bodies;
        std::vector<uint32_t> _freeList;
        std::unordered_map<ICollider*, std::vector<physx::PxShape*>> _colliderShapes;

        PhysicsBodyHandle AllocateHandle();

        /// Creates a shape on the body, owned by (and recorded under) the collider that asked for it
        void AttachColliderShape(PhysicsBodyHandle body, ICollider* collider, const physx::PxGeometry& geometry,
                                 const physx::PxTransform& localPose, const PhysicsMaterial& material);
        /// Drops active collision/trigger pairs involving the body (it was destroyed or lost its shapes);
        /// PhysX's own "touch lost" for them refers to released actors and is ignored
        void ForgetPairsWithBody(PhysicsBodyHandle handle);
        /// The GameObject a body belongs to: its Rigidbody's, else its first collider's
        [[nodiscard]] GameObject* GetBodyOwner(PhysicsBodyHandle handle);
        /// Calls fn on each component of the body's GameObject. Uses a snapshot and re-checks ownership,
        /// so handlers can add or remove components (or destroy the body) safely
        void DispatchToBody(PhysicsBodyHandle handle, const std::function<void(Component&)>& fn);

        BodyData* GetBodyData(PhysicsBodyHandle handle);
        [[nodiscard]] const BodyData* GetBodyData(PhysicsBodyHandle handle) const;

        struct MaterialKey
        {
            float staticFriction;
            float dynamicFriction;
            float restitution;

            bool operator==(const MaterialKey& other) const
            {
                return staticFriction == other.staticFriction &&
                       dynamicFriction == other.dynamicFriction &&
                       restitution == other.restitution;
            }
        };

        struct MaterialKeyHash
        {
            size_t operator()(const MaterialKey& k) const
            {
                return std::hash<float>()(k.staticFriction) ^
                       (std::hash<float>()(k.dynamicFriction) << 1) ^
                       (std::hash<float>()(k.restitution) << 2);
            }
        };

        std::unordered_map<MaterialKey, physx::PxMaterial*, MaterialKeyHash> _materialCache;
        physx::PxMaterial* GetOrCreateMaterial(const PhysicsMaterial& material);

        enum class ChangeType
        {
            SetGravity
        };

        std::unordered_map<ChangeType, std::function<void()>> _pendingChanges;
        Math::Vector3 _currentGravity{0.0f, -9.81f, 0.0f};

        struct CollisionPair
        {
            PhysicsBodyHandle bodyA;
            PhysicsBodyHandle bodyB;

            bool operator==(const CollisionPair& other) const
            {
                return (bodyA == other.bodyA && bodyB == other.bodyB) ||
                       (bodyA == other.bodyB && bodyB == other.bodyA);
            }
        };

        struct CollisionPairHash
        {
            size_t operator()(const CollisionPair& pair) const
            {
                const size_t h1 = std::hash<uint32_t>()(pair.bodyA.index) ^
                            (std::hash<uint32_t>()(pair.bodyA.generation) << 1);
                const size_t h2 = std::hash<uint32_t>()(pair.bodyB.index) ^
                            (std::hash<uint32_t>()(pair.bodyB.generation) << 1);
                // Order-independent, like operator==: (A, B) and (B, A) must land in the same bucket
                return std::min(h1, h2) ^ (std::max(h1, h2) << 1);
            }
        };

        struct CollisionEvent
        {
            CollisionPair pair;
            Collision* data = nullptr; // collider/otherCollider are pair.bodyA's and pair.bodyB's
        };

        struct TriggerEvent
        {
            CollisionPair pair;
            ICollider* colliderA = nullptr; // on pair.bodyA
            ICollider* colliderB = nullptr; // on pair.bodyB
        };

        // Events are per body pair, but PhysX reports touches per shape pair. Each body pair maps to
        // the shape pairs currently touching (oriented to the key's bodyA/bodyB): Enter fires when the
        // first starts touching and Exit when the last stops, so a compound body touching through two
        // shapes no longer exits while one is still in contact.
        struct ShapePair
        {
            const physx::PxShape* onA;
            const physx::PxShape* onB;
        };
        using TouchMap = std::unordered_map<CollisionPair, std::vector<ShapePair>, CollisionPairHash>;

        std::vector<CollisionEvent> _newCollisions;
        TouchMap _collisionTouches;
        std::vector<CollisionEvent> _endedCollisions;
        std::vector<TriggerEvent> _newTriggers;
        TouchMap _triggerTouches;
        std::vector<TriggerEvent> _endedTriggers;

        /// @returns true if this is the body pair's first touching shape pair
        static bool AddTouch(TouchMap& touches, const CollisionPair& pair,
                             const physx::PxShape* shapeOnA, const physx::PxShape* shapeOnB);
        /// @returns true if the body pair has no touching shape pairs left
        static bool RemoveTouch(TouchMap& touches, const CollisionPair& pair,
                                const physx::PxShape* shapeOnA, const physx::PxShape* shapeOnB);
        /// Drops a shape that's being removed from every touch list (PhysX's later "lost" event for it
        /// refers to a removed shape and is ignored). A body pair left with nothing ends silently.
        void ForgetShape(const physx::PxShape* shape);
        /// The collider that owns a shape (stored in the shape's userData)
        static ICollider* ColliderOf(const physx::PxShape* shape);
        /// The collider if it's still registered on the body, else null (a handler may have removed it)
        ICollider* LiveCollider(PhysicsBodyHandle body, ICollider* collider);

        Collision CreateCollisionData(
            const CollisionPair& pair,
            const Collision& baseData,
            bool isForBodyA);

        Trigger CreateTriggerData(
            const CollisionPair& pair,
            ICollider* colliderA,
            ICollider* colliderB,
            bool isForBodyA);

        void FillRaycastHit(
            RaycastHit& hit,
            const physx::PxRaycastHit& pxHit) const;
#endif
    };
}
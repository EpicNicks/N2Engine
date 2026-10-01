#pragma once

#include "engine/physics/IPhysicsBackend.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <memory>

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
#include <PxQueryReport.h>

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
        void SetBodyEnabled(PhysicsBodyHandle handle, bool enabled) override;

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

        /// For diagnostics and tests. A dynamic body's principal moments of inertia in the mass frame
        /// (PxRigidBody::getMassSpaceInertiaTensor): the axes are those of the mass pose's rotation,
        /// not necessarily the actor's. Zero for a static or unknown body.
        [[nodiscard]] Math::Vector3 GetInertiaTensor(PhysicsBodyHandle body) const;
        /// For diagnostics and tests. A dynamic body's centre of mass relative to the actor (the position
        /// of the mass pose; its rotation isn't returned). Zero for a static or unknown body.
        [[nodiscard]] Math::Vector3 GetCenterOfMass(PhysicsBodyHandle body) const;

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

            // The Rigidbody's configured mass; a dynamic actor's inertia and centre of mass are
            // derived from its shapes and rescaled to this (see UpdateMassProperties)
            float mass = 1.0f;
        };

        std::vector<BodyData> _bodies;
        std::vector<uint32_t> _freeList;
        std::unordered_map<ICollider*, std::vector<physx::PxShape*>> _colliderShapes;

        PhysicsBodyHandle AllocateHandle();

        /// Creates a shape on the body, owned by (and recorded under) the collider that asked for it
        void AttachColliderShape(PhysicsBodyHandle body, ICollider* collider, const physx::PxGeometry& geometry,
                                 const physx::PxTransform& localPose, const PhysicsMaterial& material);
        /// Recomputes a dynamic body's inertia tensor and centre of mass from its simulation shapes
        /// (triggers don't count), keeping its configured mass. Called whenever the shapes or the mass
        /// change, since PhysX doesn't derive them itself.
        void UpdateMassProperties(PhysicsBodyHandle body);
        /// Ends active collision/trigger pairs involving the body (it was destroyed, or taken out of the
        /// scene), queueing their Exit for both sides; PhysX's own "touch lost" for them is flagged as a
        /// removed actor and ignored. shapesReleased: the body's shapes are already freed, so only the
        /// other side's collider can be read.
        void ForgetPairsWithBody(PhysicsBodyHandle handle, bool shapesReleased);
        /// The GameObject a body belongs to: its Rigidbody's, else its first collider's
        [[nodiscard]] GameObject* GetBodyOwner(PhysicsBodyHandle handle) const;
        /// Calls fn on each component (enabled or not, as in Unity; none while the object is inactive in the
        /// hierarchy) of the body's GameObject, or of
        /// fallbackOwner if the body is gone and that object survives (see PairOwners). Uses a snapshot
        /// and re-checks ownership, so handlers can add or remove components (or destroy the body) safely
        void DispatchToBody(PhysicsBodyHandle handle, const std::function<void(Component&)>& fn,
                            const std::weak_ptr<GameObject>& fallbackOwner = {});

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

        // The GameObjects that owned each body when a pair was ended by removing a body or collider. A
        // side whose body is gone by dispatch (destroyed, or re-created by SetBodyType / adding or removing
        // a Rigidbody) still gets its Exit through its GameObject, unless that is being destroyed. With the
        // re-created body's later Enter, Enter and Exit stay balanced on both sides.
        struct PairOwners
        {
            std::weak_ptr<GameObject> a;
            std::weak_ptr<GameObject> b;
        };

        struct CollisionEvent
        {
            CollisionPair pair;
            Collision* data = nullptr; // collider/otherCollider are pair.bodyA's and pair.bodyB's
            PairOwners owners;         // only for forgotten pairs
        };

        struct TriggerEvent
        {
            CollisionPair pair;
            ICollider* colliderA = nullptr; // on pair.bodyA
            ICollider* colliderB = nullptr; // on pair.bodyB
            PairOwners owners;              // only for forgotten pairs
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
        // Ended by removing a body or collider rather than reported by PhysX; dispatched before the
        // step's Enter events
        std::vector<CollisionEvent> _forgottenCollisions;
        std::vector<TriggerEvent> _forgottenTriggers;

        /// Dispatches and clears a queue of ended pairs (OnCollisionExit / OnTriggerExit to both sides)
        void DispatchCollisionExits(std::vector<CollisionEvent>& queue);
        void DispatchTriggerExits(std::vector<TriggerEvent>& queue);

        /// @returns true if this is the body pair's first touching shape pair
        static bool AddTouch(TouchMap& touches, const CollisionPair& pair,
                             const physx::PxShape* shapeOnA, const physx::PxShape* shapeOnB);
        /// @returns true if the body pair has no touching shape pairs left
        static bool RemoveTouch(TouchMap& touches, const CollisionPair& pair,
                                const physx::PxShape* shapeOnA, const physx::PxShape* shapeOnB);
        /// Drops a shape that's being removed from every touch list (PhysX's later "lost" event for it
        /// refers to a removed shape and is ignored). A body pair left with nothing queues its Exit.
        void ForgetShape(const physx::PxShape* shape);
        /// The collider that owns a shape (stored in the shape's userData)
        static ICollider* ColliderOf(const physx::PxShape* shape);
        /// The collider if it's still registered on the body, else null (a handler may have removed it)
        [[nodiscard]] ICollider* LiveCollider(PhysicsBodyHandle body, ICollider* collider) const;
        /// Records who owns each body of a pair that's being forgotten
        [[nodiscard]] PairOwners OwnersOf(const CollisionPair& pair) const;
        /// The GameObject and Rigidbody to report for one side of a pair: the body's, or if the body is gone,
        /// the recorded owner's (with no Rigidbody)
        void DescribeSide(PhysicsBodyHandle handle, const std::weak_ptr<GameObject>& fallbackOwner,
                          GameObject*& gameObject, Rigidbody*& rigidbody) const;

        Collision CreateCollisionData(
            const CollisionPair& pair,
            const Collision& baseData,
            bool isForBodyA,
            const PairOwners& owners = {});

        Trigger CreateTriggerData(
            const CollisionPair& pair,
            ICollider* colliderA,
            ICollider* colliderB,
            bool isForBodyA,
            const PairOwners& owners = {});

        /// Fills a query hit from scratch (no field of a reused RaycastHit survives), reporting the collider
        /// that owns the hit shape. Raycast and sweep hits both carry a location and an actor/shape.
        void FillRaycastHit(
            RaycastHit& hit,
            const physx::PxLocationHit& location,
            const physx::PxActorShape& actorShape) const;
#endif
    };
}
#pragma once

#include "engine/physics/IPhysicsBackend.hpp"
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <memory>
#include <optional>

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
#ifdef N2ENGINE_PLATFORM_WEB
#include "engine/physics/physx/PhysXLoggerErrorCallback.hpp"
#endif

// The PhysX Visual Debugger connection is Windows-only: a Linux (server) build never opens the PVD socket, even though
// the PVD SDK library is linked, so the backend has no PxPvd there
#if defined(_WIN32) && __has_include(<pvd/PxPvd.h>)
#include <pvd/PxPvd.h>
#define N2ENGINE_PHYSX_HAS_PVD 1
#else
#define N2ENGINE_PHYSX_HAS_PVD 0
namespace physx
{
    class PxPvd;
}
#endif

namespace physx
{
    class PxCudaContextManager;
}

#endif

namespace N2Engine
{
    class GameObject;
    class Component;
}

namespace N2Engine::Particles
{
    class PbdParticleBackend;
    class IPbdDriver;
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

        void SetGpuMode(GpuMode mode) override { _gpuMode = mode; }
        /// physics.deterministic: the scene gets PxSceneFlag::eENABLE_ENHANCED_DETERMINISM and the CPU dispatcher has
        /// 0 worker threads (tasks run on the thread that calls Update). Not supported by GPU dynamics, so with
        /// physics.gpu auto/on the GPU probe is skipped with one warning and the scene is the CPU scene.
        void SetDeterministic(bool deterministic) override { _deterministic = deterministic; }
        /// What a PhysXBackend declares for a physics.deterministic value and an optional worker-thread override
        /// (SetWorkerThreadsForTesting): SameBinary when it is on and the dispatcher has 0 worker threads (the scene
        /// then has enhanced determinism and is the CPU scene: PhysX supports neither together with GPU dynamics),
        /// None otherwise. Never CrossPlatform: Windows and Linux use different PhysX binaries and their hashes
        /// differ (see docs/physics.html). Static so Application::Init can ask before it creates a backend.
        [[nodiscard]] static constexpr DeterminismLevel DeclaredDeterminism(
            const bool deterministic, const std::optional<unsigned> workerThreadsOverride = std::nullopt)
        {
            return deterministic && workerThreadsOverride.value_or(0u) == 0u ? DeterminismLevel::SameBinary
                                                                             : DeterminismLevel::None;
        }
        /// SameBinary or None as DeclaredDeterminism says for this backend's configuration; the same before and
        /// after Initialize
        [[nodiscard]] DeterminismLevel GetDeterminism() const override
        {
            return DeclaredDeterminism(_deterministic, _workerThreadsOverride);
        }
        /// What the PhysX backend supports of snapshot and set-state (static, like DeclaredDeterminism): set-state
        /// only
        static constexpr bool kSupportsSnapshot = false;
        static constexpr bool kSupportsSetState = true;
        /// False: nothing saves or restores the PhysX scene (SaveState, RestoreState and HashState stay unsupported)
        [[nodiscard]] bool SupportsSnapshot() const override { return kSupportsSnapshot; }
        /// True: GetBodyState and SetBodyState teleport a dynamic body and set its velocities and sleep state
        [[nodiscard]] bool SupportsSetState() const override { return kSupportsSetState; }
        /// "physx"
        [[nodiscard]] std::string_view BackendId() const override { return "physx"; }
        /// For tests: the dispatcher worker-thread count to use instead of the one SetDeterministic implies (4 by
        /// default, 0 when deterministic). Call before Initialize.
        void SetWorkerThreadsForTesting(std::optional<unsigned> threads) { _workerThreadsOverride = threads; }
        /// The scene (probe) status only: a particle or soft-body failure is kept apart (below), so this stays Ready
        /// while the GPU scene runs and Physics.IsGpuAvailable does not flip when one feature gives up
        [[nodiscard]] GpuStatus GetGpuStatus() const override { return _gpuStatus; }
        /// The scene status, or this feature's own failure when the scene is Ready (see ComposeFeatureGpuStatus)
        [[nodiscard]] GpuStatus GetParticlesGpuStatus() const override
        {
            return ComposeFeatureGpuStatus(_gpuStatus, _particlesFailure);
        }
        /// As GetParticlesGpuStatus, for soft bodies. There is no PhysX soft-body backend yet, so a Ready scene
        /// reports Unavailable here (see ComposeSoftBodiesGpuStatus).
        [[nodiscard]] GpuStatus GetSoftBodiesGpuStatus() const override
        {
            return ComposeSoftBodiesGpuStatus(_gpuStatus, _softBodiesFailure, false);
        }

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

        /// Pose, velocities and sleep state of a body that is in the scene (false for a stale handle or a disabled
        /// body). A kinematic or static body reports zero velocities; a static one is never asleep.
        bool GetBodyState(PhysicsBodyHandle handle, BodyState& out) const override;
        /// What each kind takes (a non-finite value or a zero-length rotation is refused with nothing changed):
        ///   dynamic    teleports with setGlobalPose, then setLinearVelocity and setAngularVelocity, all without
        ///              waking (autowake false); then sleeping puts it to sleep (putToSleep, which also zeroes its
        ///              velocities, so a sleeping body keeps none) and not sleeping wakes it (wakeUp: the wake counter
        ///              is reset, so it does not fall asleep in the next step). PhysX applies the sleep change when the
        ///              scene next steps, so GetBodyState's sleeping flag follows after the next Update.
        ///   kinematic  only the pose, as a kinematic target (the body moves there in the next step, like
        ///              SetBodyTransform); velocities and sleeping do not apply to it
        ///   static     only the pose (setGlobalPose, which is not cheap: it re-inserts the actor in the broadphase)
        /// Never call it from inside a PhysX callback or while Update runs; the engine's callbacks only queue events,
        /// so game code never does.
        bool SetBodyState(PhysicsBodyHandle handle, const BodyState& state) override;

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
        void SetColliderLayer(PhysicsBodyHandle body, ICollider* collider, int layer) override;
        void RefreshCollisionMatrix() override;

        /// Test seam: called once per body that RefreshCollisionMatrix re-filters, in the order it does so, with
        /// the object that owns the body's first collider. Pass an empty function to remove it.
        void SetRefilterObserver(std::function<void(const GameObject&)> observer) { _refilterObserver = std::move(observer); }

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
            uint32_t layerMask,
            QueryTriggers triggers) override;

        int RaycastAll(
            const Math::Vector3& origin,
            const Math::Vector3& direction,
            std::vector<RaycastHit>& hits,
            float maxDistance,
            uint32_t layerMask,
            QueryTriggers triggers) override;

        bool SphereCast(
            const Math::Vector3& origin,
            float radius,
            const Math::Vector3& direction,
            RaycastHit& hit,
            float maxDistance,
            uint32_t layerMask) override;

    private:
        // Outside the PhysX guard so the inline GetGpuStatus compiles without PhysX too
        GpuMode _gpuMode = GpuMode::Off;
        bool _deterministic = false;
        std::optional<unsigned> _workerThreadsOverride;
        GpuStatus _gpuStatus{GpuState::Disabled, "physics.gpu is off"};
        // Per-feature failure state, apart from the scene status above: set when a feature that runs in the GPU
        // scene gives up (Failed with physics.gpu=on, else Unavailable), cleared by Initialize. The feature's health
        // row is the scene status with its own failure on top; no feature's failure touches another's or the scene's.
        std::optional<GpuStatus> _particlesFailure;
        std::optional<GpuStatus> _softBodiesFailure;
        // Outside the PhysX guard like _gpuMode: the inline SetRefilterObserver is declared in every build
        std::function<void(const GameObject&)> _refilterObserver;

    public:
#ifdef N2ENGINE_PHYSX_ENABLED
        /// The PhysX PBD particle backend: only with a GPU scene (physics.gpu auto/on, a working probe, a build with
        /// the GPU library) and until the GPU refuses a particle system or transfer; nullptr otherwise (the
        /// ParticleSystems then use the CPU backend). It is created with the scene and destroyed in Shutdown.
        [[nodiscard]] IParticleBackend* GetParticleBackend() override;

        /// Test seam, not used by the engine: puts a particle backend over `driver` (a fake in the tests) in place
        /// of the one a real GPU scene would create, and reports the GPU as Ready, so the failure chain
        /// (PollParticleFailure / FailParticles) can run without a GPU. Needs an initialised backend that has no
        /// particle backend yet; returns false (and does nothing) otherwise.
        bool InstallParticleDriverForTesting(std::unique_ptr<Particles::IPbdDriver> driver);
        /// Records that the soft-body feature stopped working (or never could): Failed (physics.gpu=on) or
        /// Unavailable (auto) on the SoftBodies status only, one warning. The scene status, the particle backend and
        /// the Particles row are untouched. The PhysX soft-body backend (a later stage) calls it when its GPU work
        /// fails; the tests call it directly, as there is no such backend yet.
        void FailSoftBodies(const std::string& reason);
        /// For tests: the scene has eENABLE_GPU_DYNAMICS set
        [[nodiscard]] bool SceneUsesGpuDynamics() const;
        /// For tests: the scene has eENABLE_ENHANCED_DETERMINISM set
        [[nodiscard]] bool SceneUsesEnhancedDeterminism() const;
        /// For tests: the CPU dispatcher's worker-thread count (0 when there is none)
        [[nodiscard]] unsigned DispatcherWorkerCount() const;
        /// For tests: the scene's broad phase is the GPU one
        [[nodiscard]] bool SceneUsesGpuBroadPhase() const;
        /// For tests: a CUDA context manager exists (physics.gpu auto/on with a working probe)
        [[nodiscard]] bool HasCudaContextManager() const { return _cudaContextManager != nullptr; }

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
        /// For diagnostics and tests. How many shapes the body's actor holds, trigger shapes included (zero for an
        /// unknown body). The typed physics tests read it through the same name on JoltBackend, where it counts the
        /// solid colliders only: they compare it only for bodies without triggers.
        [[nodiscard]] std::size_t GetShapeCount(PhysicsBodyHandle body) const;

    private:
        physx::PxDefaultAllocator _allocator;
#ifdef N2ENGINE_PLATFORM_WEB
        // The web build reports PhysX's errors through the engine Logger (PhysXLoggerErrorCallback.hpp)
        PhysXLoggerErrorCallback _errorCallback;
#else
        physx::PxDefaultErrorCallback _errorCallback;
#endif
        physx::PxFoundation* _foundation = nullptr;
        physx::PxPhysics* _physics = nullptr;
        physx::PxScene* _scene = nullptr;
        physx::PxMaterial* _defaultMaterial = nullptr;
        physx::PxDefaultCpuDispatcher* _dispatcher = nullptr;
        physx::PxCudaContextManager* _cudaContextManager = nullptr; // only with physics.gpu auto/on and a working probe

        /// With physics.gpu auto/on: creates the CUDA context manager (true when the GPU scene can be built) and
        /// sets _gpuStatus. With off it does nothing, and makes no CUDA call.
        bool ProbeGpu();
        /// Drops the CUDA context, sets Failed (physics.gpu=on) or Unavailable (auto) and logs one warning
        void FailGpu(const std::string& reason);

        std::unique_ptr<Particles::PbdParticleBackend> _particles; // only on a GPU scene
        /// The particle backend stopped working (or never could): drops its systems, sets the PARTICLES failure
        /// (Failed with physics.gpu=on, Unavailable with auto) and logs one warning. The scene status and the
        /// GPU scene itself keep going.
        void FailParticles(const std::string& reason);
        /// Drops the particle backend (once) if it has failed, as FailParticles
        void PollParticleFailure();

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

        // Orders pairs by their bodies' handles, smaller side first (A,B and B,A are the same key, like
        // operator==). Handle indices come from a deterministic free list, so iteration order depends only
        // on the sequence of body creations and destructions, never on addresses or the standard library's
        // hash. (Slot reuse means it follows the slot, not strictly the age, of a body.)
        struct CollisionPairLess
        {
            bool operator()(const CollisionPair& a, const CollisionPair& b) const
            {
                const auto key = [](const CollisionPair& pair)
                {
                    const auto lessHandle = [](const PhysicsBodyHandle& x, const PhysicsBodyHandle& y)
                    {
                        return x.index != y.index ? x.index < y.index : x.generation < y.generation;
                    };
                    return lessHandle(pair.bodyB, pair.bodyA) ? std::pair{pair.bodyB, pair.bodyA}
                                                              : std::pair{pair.bodyA, pair.bodyB};
                };
                const auto [a0, a1] = key(a);
                const auto [b0, b1] = key(b);
                if (a0.index != b0.index) return a0.index < b0.index;
                if (a0.generation != b0.generation) return a0.generation < b0.generation;
                if (a1.index != b1.index) return a1.index < b1.index;
                return a1.generation < b1.generation;
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
        // std::map, not unordered_map: the touch maps are iterated to wake partners of re-filtered shapes and to
        // emit the Exit events of removed bodies and shapes, and that order must not depend on hashing.
        using TouchMap = std::map<CollisionPair, std::vector<ShapePair>, CollisionPairLess>;

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

        // ===== Layer re-filtering =====
        // resetFiltering makes PhysX report every pair of a re-filtered shape as lost, and the pairs the
        // filter still allows as found again, in the next step. Reports for those shapes are held back
        // and settled after the step by each shape pair's last report, so a pair that stays allowed gets
        // no Exit/Enter and one the matrix now rules out gets its Exit.
        struct RefilterReport
        {
            CollisionPair pair;
            const physx::PxShape* onA = nullptr; // on pair.bodyA
            const physx::PxShape* onB = nullptr;
            bool found = false;                  // else lost
            bool trigger = false;
            Collision* data = nullptr;           // contacts only
        };
        std::unordered_set<const physx::PxShape*> _pendingRefilter; // reset since the last step
        std::unordered_set<const physx::PxShape*> _refiltering;     // being re-filtered in this step
        std::vector<RefilterReport> _refilterReports;
        // A touch whose last report was "lost" although the new filter data still allows it: PhysX may
        // only report it found again a step later (e.g. once a sleeping partner wakes). Its Exit waits a
        // step and is dropped if "found" arrives meanwhile.
        std::vector<RefilterReport> _deferredLost;

        /// Re-filters these shapes of the actor in the next step (a no-op for an actor outside the scene),
        /// waking the dynamic bodies they touch so PhysX re-finds those pairs promptly
        void RefilterShapes(physx::PxRigidActor& actor, const std::vector<physx::PxShape*>& shapes);
        [[nodiscard]] bool IsRefiltering(const physx::PxShape* a, const physx::PxShape* b) const;
        /// After a step that re-filtered shapes: applies each held-back shape pair's last report, defers a
        /// "lost" the filter still allows, then ends any touch of those shapes the filter data now rules
        /// out (if PhysX didn't report it lost)
        void ReconcileRefilteredPairs();
        /// Wakes the shape's actor if it's a sleeping, simulated dynamic body in the scene
        static void WakeIfAsleep(const physx::PxShape* shape);
        /// Forgets deferred "lost" reports involving a shape that's going away
        void DropDeferred(const physx::PxShape* shape);

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
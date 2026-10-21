#pragma once

#include "engine/physics/IPhysicsBackend.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// The Jolt backend exists only in a build with N2ENGINE_USE_JOLT (the engine target then defines
// N2ENGINE_JOLT_ENABLED and links Jolt::Jolt). The header holds no Jolt type: the library's headers stay inside
// engine/src/physics/jolt/JoltBackend.cpp.
#ifdef N2ENGINE_JOLT_ENABLED

namespace N2Engine
{
    class GameObject;
}

namespace N2Engine::Physics
{
    /**
     * IPhysicsBackend on Jolt Physics (external/JoltPhysics, built cross-platform deterministic).
     *
     * Stages J3 (the core) and J4 (layers, triggers, events) of the physics plan. Implemented: bodies (static,
     * dynamic, kinematic), ONE shape per body (a compound of all the body's colliders, trigger ones included as
     * sub-shapes; a single collider at the origin is a plain shape), mass (from the solid colliders only), gravity,
     * forces, impulses and velocities, enable and disable, kinematic and static moves, transform sync, the set-state
     * calls, the 32 layers and the collision matrix, trigger colliders, and OnCollision* / OnTrigger* events through the
     * ContactTracker that the PhysX backend uses too. NOT implemented yet (each logs ONE warning when first used and
     * otherwise answers "nothing found"): scene queries (J5), snapshot (J6).
     *
     * Determinism: body ids are the engine's deterministic handles, index and generation (CreateBodyWithID), transforms are synced in
     * handle order (never in Jolt's active-body order), and the job system is single-threaded on the web and whenever
     * physics.deterministic asks for it (SetDeterministic). Jolt reports contacts from worker threads in no fixed order:
     * they are buffered and sorted by (handle pair, sub-shape pair) before they become touches, so the events of a
     * step come out in one order whatever the thread count.
     */
    class JoltBackend final : public IPhysicsBackend
    {
    public:
        /// The most bodies one backend holds (Jolt's table is sized at Initialize). A body whose handle index would
        /// reach it is refused (CreateDynamicBody and CreateStaticBody return INVALID_PHYSICS_HANDLE, with an error).
        static constexpr std::uint32_t kMaxBodies = 10240;
        /// Jolt's body-pair and contact buffers (PhysicsSystem::Init); Update logs once when one overflows
        static constexpr std::uint32_t kMaxBodyPairs = 65536;
        static constexpr std::uint32_t kMaxContactConstraints = 20480;
        /// For tests: the body limit to use instead of kMaxBodies (call before Initialize)
        void SetMaxBodiesForTesting(std::uint32_t maxBodies) { _maxBodies = maxBodies; }
        /// Native worker threads of the thread-pool job system: -1 is Jolt's own choice (the hardware thread count
        /// minus one, at least one). Not used on the web and while the job system is single-threaded.
        static constexpr int kWorkerThreads = -1;
        /// Jolt's scratch memory for one Update (TempAllocatorImpl), in bytes
        static constexpr std::size_t kTempAllocatorBytes = 16u * 1024u * 1024u;
        /// The collision steps of one Update. Always 1: the engine's fixed step is already the physics step.
        static constexpr int kCollisionSteps = 1;

        JoltBackend();
        ~JoltBackend() override;
        JoltBackend(const JoltBackend &) = delete;
        JoltBackend &operator=(const JoltBackend &) = delete;

        /// physics.deterministic: call before Initialize. True makes the job system single-threaded (what the
        /// Deterministic preset needs until the thread-count independence of Jolt is proven in CI); false is the
        /// thread pool on a native build. The web build is always single-threaded.
        void SetDeterministic(bool deterministic) override { _deterministic = deterministic; }

        /// What a JoltBackend declares for a physics.deterministic value (static so Application::Init can ask before
        /// it creates a backend):
        ///   None           a native build with the thread-pool job system (physics.deterministic off): thread-count
        ///                  independence is not proven yet, so nothing is promised
        ///   SameBinary     single-threaded job system (the web build, or physics.deterministic on), but the build is
        ///                  not the strict floating-point profile (N2ENGINE_STRICT_FP, N2_STRICT_FP) or Jolt is not
        ///                  built cross-platform deterministic
        ///   CrossPlatform  single-threaded job system AND Jolt built with JPH_CROSS_PLATFORM_DETERMINISTIC AND the
        ///                  strict floating-point profile on: the conditions of the design (docs/physics.html)
        [[nodiscard]] static DeterminismLevel DeclaredDeterminism(bool deterministic);
        [[nodiscard]] DeterminismLevel GetDeterminism() const override { return DeclaredDeterminism(_deterministic); }
        /// Set-state yes (GetBodyState, SetBodyState); snapshot not until J6 (SaveState, RestoreState, HashState)
        static constexpr bool kSupportsSnapshot = false;
        static constexpr bool kSupportsSetState = true;
        [[nodiscard]] bool SupportsSnapshot() const override { return kSupportsSnapshot; }
        [[nodiscard]] bool SupportsSetState() const override { return kSupportsSetState; }
        /// J4's groups: layers and the collision matrix, triggers, and contact and trigger events. Scene queries come
        /// with J5 (each stage adds its groups here). The Deterministic preset refuses Jolt until every group is in.
        static constexpr PhysicsFeatureSet kImplementedFeatures = PhysicsFeatureSet::None()
                                                                      .With(PhysicsFeature::Layers)
                                                                      .With(PhysicsFeature::Triggers)
                                                                      .With(PhysicsFeature::Events);
        [[nodiscard]] PhysicsFeatureSet GetImplementedFeatures() const override { return kImplementedFeatures; }
        /// "jolt"
        [[nodiscard]] std::string_view BackendId() const override { return "jolt"; }

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Shutdown() override;

        void ApplyPendingChanges() override;
        void SyncTransforms() override;
        /// Delivers the events of the last Update (OnCollisionEnter/Stay/Exit, OnTriggerEnter/Stay/Exit) through the
        /// ContactTracker: forgotten pairs' Exit, collision Enter, Stay, Exit, then the trigger events. A solid contact's
        /// Enter carries the contact points, separation, relative velocity, and an ESTIMATE of the impulse: Jolt calls
        /// its contact listener before the solver, so the applied impulse does not exist yet (docs/physics.html).
        void ProcessCollisionCallbacks() override;

        PhysicsBodyHandle CreateDynamicBody(const Math::Vector3 &position, const Math::Quaternion &rotation,
                                            float mass, Rigidbody *rigidbody, bool isKinematic) override;
        PhysicsBodyHandle CreateStaticBody(const Math::Vector3 &position, const Math::Quaternion &rotation,
                                           Rigidbody *rigidbody) override;
        void DestroyBody(PhysicsBodyHandle handle) override;
        void SetBodyEnabled(PhysicsBodyHandle handle, bool enabled) override;

        void RegisterCollider(PhysicsBodyHandle handle, ICollider *collider) override;
        void UnregisterCollider(PhysicsBodyHandle handle, ICollider *collider) override;

        /// A kinematic body: the pose becomes the target the next Update moves it to (Jolt's MoveKinematic, so it
        /// pushes dynamic bodies). A dynamic body ignores it, as in the PhysX backend.
        void SetBodyTransform(PhysicsBodyHandle handle, const Math::Vector3 &position,
                              const Math::Quaternion &rotation) override;
        /// A static body is moved at once (SetPositionAndRotation)
        void SetStaticBodyTransform(PhysicsBodyHandle handle, const Math::Vector3 &position,
                                    const Math::Quaternion &rotation) override;

        /// Pose, velocities and sleep state of a body that is in the simulation (false for a stale handle or a
        /// disabled body). A kinematic or static body reports zero velocities; a static one is never asleep.
        bool GetBodyState(PhysicsBodyHandle handle, BodyState &out) const override;
        /// A non-finite value or a zero-length rotation is refused with nothing changed (a rotation is normalised).
        ///   dynamic    teleports with its velocities (SetPositionRotationAndVelocity); sleeping puts it to sleep
        ///              (a sleeping body keeps no velocity), not sleeping wakes it
        ///   kinematic  only the pose, as the kinematic target of the next Update; velocities and sleeping do not apply
        ///   static     only the pose
        bool SetBodyState(PhysicsBodyHandle handle, const BodyState &state) override;

        void AddSphereCollider(PhysicsBodyHandle body, ICollider *collider, float radius,
                               const Math::Vector3 &localOffset, const PhysicsMaterial &material) override;
        void AddBoxCollider(PhysicsBodyHandle body, ICollider *collider, const Math::Vector3 &halfExtents,
                            const Math::Vector3 &localOffset, const PhysicsMaterial &material) override;
        void AddCapsuleCollider(PhysicsBodyHandle body, ICollider *collider, float radius, float height,
                                const Math::Vector3 &localOffset, const PhysicsMaterial &material) override;
        void RemoveColliderShapes(PhysicsBodyHandle body, ICollider *collider) override;
        void UpdateSphereCollider(PhysicsBodyHandle body, ICollider *collider, float radius,
                                  const Math::Vector3 &localOffset, const PhysicsMaterial &material) override;
        void UpdateBoxCollider(PhysicsBodyHandle body, ICollider *collider, const Math::Vector3 &halfExtents,
                               const Math::Vector3 &localOffset, const PhysicsMaterial &material) override;
        void UpdateCapsuleCollider(PhysicsBodyHandle body, ICollider *collider, float radius, float height,
                                   const Math::Vector3 &localOffset, const PhysicsMaterial &material) override;

        /// A trigger collider stays a sub-shape of its body's Jolt shape; its contacts are sensor contacts (no collision
        /// response) that become trigger events, and it adds nothing to the mass. Toggling it ends the collider's
        /// touches (they begin again as the other kind).
        void SetIsTrigger(PhysicsBodyHandle body, ICollider *collider, bool isTrigger) override;
        /// The collider's layer (its object layer is the layer when all the body's colliders share it). A touch the
        /// matrix now rules out ends with an Exit at the next ProcessCollisionCallbacks; one it still allows is
        /// undisturbed. The bodies involved wake up so Jolt looks at their pairs again.
        void SetColliderLayer(PhysicsBodyHandle body, ICollider *collider, int layer) override;
        /// Takes the engine's matrix (Layers::CollisionMaskFor) as the filters' copy; ends the touches it rules out
        void RefreshCollisionMatrix() override;

        /// Test seam, as on the PhysX backend: called once per body that RefreshCollisionMatrix re-filters (one with a
        /// collider on a layer whose row changed), in handle order, with the object that owns the body's first collider.
        /// Pass an empty function to remove it.
        void SetRefilterObserver(std::function<void(const GameObject &)> observer);

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
        [[nodiscard]] Math::Vector3 GetGravity() const override;

        /// J5: not implemented. Answer "nothing hit" (false, 0) with one warning.
        bool Raycast(const Math::Vector3 &origin, const Math::Vector3 &direction, RaycastHit &hit, float maxDistance,
                     std::uint32_t layerMask, QueryTriggers triggers) override;
        int RaycastAll(const Math::Vector3 &origin, const Math::Vector3 &direction, std::vector<RaycastHit> &hits,
                       float maxDistance, std::uint32_t layerMask, QueryTriggers triggers) override;
        bool SphereCast(const Math::Vector3 &origin, float radius, const Math::Vector3 &direction, RaycastHit &hit,
                        float maxDistance, std::uint32_t layerMask) override;

        /// J6: not implemented. Refused (false, nullopt) with one warning; SupportsSnapshot is false.
        bool SaveState(std::vector<std::byte> &out) const override;
        bool RestoreState(std::span<const std::byte> in) override;
        [[nodiscard]] std::optional<std::uint64_t> HashState() const override;

        // ===== For diagnostics and tests =====

        /// A dynamic or kinematic body's principal moments of inertia about its centre of mass, along the body's
        /// own axes (the diagonal of the inertia tensor Jolt was given): zero for a static or unknown body
        [[nodiscard]] Math::Vector3 GetInertiaTensor(PhysicsBodyHandle body) const;
        /// A body's centre of mass relative to its origin (zero for a static or unknown body)
        [[nodiscard]] Math::Vector3 GetCenterOfMass(PhysicsBodyHandle body) const;
        /// How many collider shapes the body's Jolt shape is made of (trigger colliders are not part of it)
        [[nodiscard]] std::size_t GetShapeCount(PhysicsBodyHandle body) const;
        /// The index part of a live body's Jolt id, which is the handle's index: UINT32_MAX for a stale handle
        [[nodiscard]] std::uint32_t GetJoltBodyIndex(PhysicsBodyHandle body) const;
        /// A live body's whole Jolt id (index and sequence number, BodyID::GetIndexAndSequenceNumber): the index of the
        /// handle and its generation (modulo 256) as the sequence number; UINT32_MAX for a stale handle
        [[nodiscard]] std::uint32_t GetJoltBodyId(PhysicsBodyHandle body) const;
        /// The body is in the simulation (not disabled)
        [[nodiscard]] bool IsBodyEnabled(PhysicsBodyHandle body) const;
        /// Initialize chose the single-threaded job system (the web build, or SetDeterministic(true))
        [[nodiscard]] bool UsesSingleThreadedJobSystem() const;
        /// How many "not implemented yet" warnings this backend has logged (one per feature, ever: queries, snapshot)
        [[nodiscard]] unsigned GetNotImplementedWarningCount() const;
        /// How many backends hold Jolt's process-wide state (allocator, type factory, registered types) right now
        [[nodiscard]] static int GlobalInitCount();

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
        bool _deterministic = false;
        std::uint32_t _maxBodies = kMaxBodies;
    };
}

#endif // N2ENGINE_JOLT_ENABLED

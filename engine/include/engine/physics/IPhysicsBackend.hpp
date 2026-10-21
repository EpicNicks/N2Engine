#pragma once

#include <math/Vector3.hpp>
#include <math/Quaternion.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/physics/BodyState.hpp"
#include "engine/physics/DeterminismLevel.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/PhysicsFeatures.hpp"
#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsMaterial.hpp"

namespace N2Engine::Physics
{
    class IParticleBackend;
    class ISoftBodyBackend;
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
        /// physics.deterministic: call before Initialize (the scene's flags are immutable once it exists). Asks for
        /// the backend's reproducible-replay configuration; a backend without one ignores it. Default off, and off
        /// changes nothing.
        virtual void SetDeterministic(bool) {}

        /// What this backend promises about reproducing its results, as configured now (see DeterminismLevel).
        /// None by default: a backend declares more only when it can prove it. The network presets read it at
        /// startup (the Deterministic preset needs CrossPlatform).
        [[nodiscard]] virtual DeterminismLevel GetDeterminism() const { return DeterminismLevel::None; }
        /// Whether the whole simulation can be saved and restored exactly (what rollback needs). False by default.
        [[nodiscard]] virtual bool SupportsSnapshot() const { return false; }
        /// Whether a body's full state (pose, velocities, sleep state) can be written back, including a dynamic
        /// body (what client reconciliation needs). False by default.
        [[nodiscard]] virtual bool SupportsSetState() const { return false; }
        /// Which feature groups this backend implements (layers and the collision matrix, triggers, contact and trigger
        /// events, scene queries; PhysicsFeatures.hpp). None by default, like the other declarations: a backend
        /// declares a group only once it works. The Deterministic preset refuses to start unless every group is there.
        [[nodiscard]] virtual PhysicsFeatureSet GetImplementedFeatures() const { return PhysicsFeatureSet::None(); }

        /// Which backend this is: "physx" or "jolt" (the physics.backend setting's values, PhysicsBackendKind), or
        /// "unknown" for a backend that does not say (the default, so mocks keep compiling). It is the name the Physics
        /// and Network health rows give, and the start of the id the determinism fingerprint will carry.
        [[nodiscard]] virtual std::string_view BackendId() const { return "unknown"; }

        /// Reads a body's full state into `out`: pose, both velocities and whether it is asleep. False (and `out`
        /// untouched) for a stale or invalid handle, a body that is not in the simulation (disabled), or a backend
        /// without SupportsSetState (the default). A static body reports zero velocities and awake. Main thread,
        /// between steps.
        virtual bool GetBodyState(PhysicsBodyHandle /*body*/, BodyState & /*out*/) const { return false; }
        /// Writes a body's full state back, including a dynamic body's pose (what a plain SetBodyTransform cannot do: it
        /// only sets a kinematic target). False, with nothing changed, for the cases GetBodyState refuses and for a
        /// state with a non-finite value or a rotation of no length (a rotation is normalised). Never call it while
        /// the backend is stepping. What each body kind takes is up to the backend (PhysXBackend documents it); the
        /// default does nothing and returns false.
        virtual bool SetBodyState(PhysicsBodyHandle /*body*/, const BodyState & /*state*/) { return false; }

        /// Snapshot hooks (what rollback needs; SupportsSnapshot says whether they work). SaveState replaces `out` with
        /// the whole simulation's state, between steps only; RestoreState puts a saved state back into a simulation
        /// that has the same bodies as at save time. Both return false for "unsupported" (the default).
        virtual bool SaveState(std::vector<std::byte> & /*out*/) const { return false; }
        virtual bool RestoreState(std::span<const std::byte> /*in*/) { return false; }
        /// A canonical hash of the simulation state (bodies in handle order), for desync detection; nullopt for
        /// "unsupported" (the default). Not 0 for unsupported: 0 is a legal hash.
        [[nodiscard]] virtual std::optional<std::uint64_t> HashState() const { return std::nullopt; }
        /// What Initialize made of the GPU request: the SCENE (and probe) status, Disabled for a backend that never
        /// asks. It does not change when a feature that runs in the GPU scene (particles, soft bodies) fails, so
        /// Physics.IsGpuAvailable reflects the scene only; a feature's own status is GetParticlesGpuStatus and
        /// GetSoftBodiesGpuStatus.
        [[nodiscard]] virtual GpuStatus GetGpuStatus() const
        {
            return {GpuState::Disabled, "This physics backend has no GPU support"};
        }
        /// The scene status with the GPU particle backend's own failure on top (see GpuStatus.hpp); the status the
        /// Particles health row shows. A particle failure never changes the soft-body status.
        [[nodiscard]] virtual GpuStatus GetParticlesGpuStatus() const { return GetGpuStatus(); }
        /// The scene status with the GPU soft-body backend's own failure on top; the status the SoftBodies health
        /// row shows. A soft-body failure never changes the particle status.
        [[nodiscard]] virtual GpuStatus GetSoftBodiesGpuStatus() const
        {
            return ComposeSoftBodiesGpuStatus(GetGpuStatus(), std::nullopt, false);
        }

        /// The GPU particle backend, when this backend runs particles in its own (GPU) scene: nullptr by default and
        /// whenever the GPU scene isn't up. A ParticleSystem with backend Auto or PhysX uses it when it is there and
        /// falls back to the CPU backend otherwise. The backend belongs to this object and is valid until Shutdown.
        [[nodiscard]] virtual IParticleBackend *GetParticleBackend() { return nullptr; }

        /// The GPU soft-body backend, when this backend simulates soft bodies in its own (GPU) scene: nullptr by
        /// default and for every backend until the PhysX one exists. A SoftBody component then uses its CPU backend.
        /// The backend belongs to this object and is valid until Shutdown.
        [[nodiscard]] virtual ISoftBodyBackend *GetSoftBodyBackend() { return nullptr; }

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
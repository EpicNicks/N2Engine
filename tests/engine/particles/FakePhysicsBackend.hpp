#pragma once

#include <cstddef>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include "engine/particles/CpuParticleBackend.hpp"
#include "engine/physics/CpuSoftBodyBackend.hpp"
#include "engine/physics/IParticleBackend.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/physics/ISoftBodyBackend.hpp"

namespace N2Engine::TestSupport
{
    /// A stand-in for the PhysX GPU soft-body backend: the engine's CPU simulation under the name "PhysX", counting
    /// the calls the component makes, so tests can tell that the seam (not the component's own CPU backend) is used
    class FakeGpuSoftBodyBackend final : public Physics::ISoftBodyBackend
    {
    public:
        // The inner simulation issues the handles, so it takes this backend's generation
        FakeGpuSoftBodyBackend() : _inner(GetGeneration()) {}

        [[nodiscard]] std::string_view GetName() const override { return "PhysX"; }
        [[nodiscard]] Physics::SoftBodyHandle CreateBody(const Physics::SoftBodyDesc &desc) override
        {
            if (refuseCreate)
            {
                return Physics::kInvalidSoftBody;
            }
            ++created;
            return _inner.CreateBody(desc);
        }
        void Destroy(const Physics::SoftBodyHandle handle) override
        {
            ++destroyed;
            _inner.Destroy(handle);
        }
        void SetParams(const Physics::SoftBodyHandle handle, const Physics::SoftBodyParams &params) override
        {
            _inner.SetParams(handle, params);
        }
        void AddVelocity(const Physics::SoftBodyHandle handle, const Math::Vector3 &velocity) override
        {
            ++velocityAdded;
            _inner.AddVelocity(handle, velocity);
        }
        void Step(const float dt) override
        {
            ++steps;
            _inner.Step(dt);
        }
        void CopySurfacePositions(const Physics::SoftBodyHandle handle, std::vector<Math::Vector3> &out) const override
        {
            _inner.CopySurfacePositions(handle, out);
        }

        int created = 0;
        int destroyed = 0;
        int steps = 0;
        int velocityAdded = 0;
        /// CreateBody fails, as when the GPU is out of memory
        bool refuseCreate = false;

    private:
        Physics::CpuSoftBodyBackend _inner;
    };

    /// A stand-in for the PhysX GPU particle backend: the engine's CPU simulation under the name "PhysX", counting
    /// the calls the component makes, so tests can tell that the seam (not the component's own CPU backend) is used
    class FakeGpuParticleBackend final : public Physics::IParticleBackend
    {
    public:
        // The inner simulation issues the handles, so it takes this backend's generation
        FakeGpuParticleBackend() : _inner(GetGeneration()) {}

        [[nodiscard]] std::string_view GetName() const override { return "PhysX"; }
        [[nodiscard]] Physics::ParticleSystemHandle CreateSystem(const Physics::ParticleSystemDesc &desc) override
        {
            if (refuseCreate)
            {
                return Physics::kInvalidParticleSystem;
            }
            ++created;
            return _inner.CreateSystem(desc);
        }
        void Destroy(const Physics::ParticleSystemHandle handle) override
        {
            ++destroyed;
            _inner.Destroy(handle);
        }
        void SetParams(const Physics::ParticleSystemHandle handle, const Physics::ParticleParams &params) override
        {
            _inner.SetParams(handle, params);
        }
        std::size_t Spawn(const Physics::ParticleSystemHandle handle, const std::span<const Math::Vector3> positions,
                          const std::span<const Math::Vector3> velocities) override
        {
            return _inner.Spawn(handle, positions, velocities);
        }
        void Clear(const Physics::ParticleSystemHandle handle) override { _inner.Clear(handle); }
        void Step(const float dt) override { _inner.Step(dt); }
        [[nodiscard]] std::size_t GetAliveCount(const Physics::ParticleSystemHandle handle) const override
        {
            return _inner.GetAliveCount(handle);
        }
        void CopyPositions(const Physics::ParticleSystemHandle handle, std::vector<Math::Vector4> &out) const override
        {
            _inner.CopyPositions(handle, out);
        }

        int created = 0;
        int destroyed = 0;
        /// CreateSystem fails, as when the GPU is out of memory
        bool refuseCreate = false;

    private:
        Particles::CpuParticleBackend _inner;
    };

    /// A physics backend that does nothing but report a GPU particle backend (or none) and a gravity
    class FakePhysicsBackend final : public Physics::IPhysicsBackend
    {
    public:
        explicit FakePhysicsBackend(const bool withGpuParticles, const Physics::GpuStatus status = {})
            : _withGpuParticles(withGpuParticles), _status(status)
        {
        }

        [[nodiscard]] Physics::GpuStatus GetGpuStatus() const override { return _status; }
        [[nodiscard]] Physics::IParticleBackend *GetParticleBackend() override
        {
            return _withGpuParticles ? &gpu : nullptr;
        }
        [[nodiscard]] Physics::ISoftBodyBackend *GetSoftBodyBackend() override
        {
            return withGpuSoftBodies ? &softGpu : nullptr;
        }
        [[nodiscard]] Physics::GpuStatus GetSoftBodiesGpuStatus() const override
        {
            return Physics::ComposeSoftBodiesGpuStatus(GetGpuStatus(), std::nullopt, withGpuSoftBodies);
        }

        FakeGpuParticleBackend gpu;
        /// Offers softGpu as the GPU soft-body backend (none by default, as every real backend until the PhysX one)
        bool withGpuSoftBodies = false;
        FakeGpuSoftBodyBackend softGpu;

        /// Destroys the GPU soft-body backend and constructs a new one in the same place (see RecreateGpu)
        void RecreateSoftGpu()
        {
            softGpu.~FakeGpuSoftBodyBackend();
            new (&softGpu) FakeGpuSoftBodyBackend();
        }

        /// Destroys the GPU particle backend and constructs a new one in the same place: the same address, a new
        /// backend (and so a new generation). What an allocator can do when a backend is replaced.
        void RecreateGpu()
        {
            gpu.~FakeGpuParticleBackend();
            new (&gpu) FakeGpuParticleBackend();
        }
        Math::Vector3 gravity{0.0f, -9.81f, 0.0f};
        /// The feature groups it declares (GetImplementedFeatures): none by default, as the interface's default
        Physics::PhysicsFeatureSet implementedFeatures = Physics::PhysicsFeatureSet::None();
        [[nodiscard]] Physics::PhysicsFeatureSet GetImplementedFeatures() const override { return implementedFeatures; }

        bool Initialize() override { return true; }
        void Update(float) override {}
        void Shutdown() override {}
        void ApplyPendingChanges() override {}
        void SyncTransforms() override {}
        void ProcessCollisionCallbacks() override {}

        Physics::PhysicsBodyHandle CreateDynamicBody(const Math::Vector3 &, const Math::Quaternion &, float,
                                                     Physics::Rigidbody *, bool) override
        {
            return Physics::INVALID_PHYSICS_HANDLE;
        }
        Physics::PhysicsBodyHandle CreateStaticBody(const Math::Vector3 &, const Math::Quaternion &,
                                                    Physics::Rigidbody *) override
        {
            return Physics::INVALID_PHYSICS_HANDLE;
        }
        void DestroyBody(Physics::PhysicsBodyHandle) override {}
        void SetBodyEnabled(Physics::PhysicsBodyHandle, bool) override {}
        void RegisterCollider(Physics::PhysicsBodyHandle, Physics::ICollider *) override {}
        void UnregisterCollider(Physics::PhysicsBodyHandle, Physics::ICollider *) override {}
        void SetBodyTransform(Physics::PhysicsBodyHandle, const Math::Vector3 &, const Math::Quaternion &) override {}
        void SetStaticBodyTransform(Physics::PhysicsBodyHandle, const Math::Vector3 &,
                                    const Math::Quaternion &) override
        {
        }
        void AddSphereCollider(Physics::PhysicsBodyHandle, Physics::ICollider *, float, const Math::Vector3 &,
                               const Physics::PhysicsMaterial &) override
        {
        }
        void AddBoxCollider(Physics::PhysicsBodyHandle, Physics::ICollider *, const Math::Vector3 &,
                            const Math::Vector3 &, const Physics::PhysicsMaterial &) override
        {
        }
        void AddCapsuleCollider(Physics::PhysicsBodyHandle, Physics::ICollider *, float, float,
                                const Math::Vector3 &, const Physics::PhysicsMaterial &) override
        {
        }
        void RemoveColliderShapes(Physics::PhysicsBodyHandle, Physics::ICollider *) override {}
        void UpdateSphereCollider(Physics::PhysicsBodyHandle, Physics::ICollider *, float, const Math::Vector3 &,
                                  const Physics::PhysicsMaterial &) override
        {
        }
        void UpdateBoxCollider(Physics::PhysicsBodyHandle, Physics::ICollider *, const Math::Vector3 &,
                               const Math::Vector3 &, const Physics::PhysicsMaterial &) override
        {
        }
        void UpdateCapsuleCollider(Physics::PhysicsBodyHandle, Physics::ICollider *, float, float,
                                   const Math::Vector3 &, const Physics::PhysicsMaterial &) override
        {
        }
        void SetIsTrigger(Physics::PhysicsBodyHandle, Physics::ICollider *, bool) override {}
        void SetColliderLayer(Physics::PhysicsBodyHandle, Physics::ICollider *, int) override {}
        void RefreshCollisionMatrix() override {}
        void AddForce(Physics::PhysicsBodyHandle, const Math::Vector3 &) override {}
        void AddImpulse(Physics::PhysicsBodyHandle, const Math::Vector3 &) override {}
        void SetVelocity(Physics::PhysicsBodyHandle, const Math::Vector3 &) override {}
        void SetAngularVelocity(Physics::PhysicsBodyHandle, const Math::Vector3 &) override {}
        Math::Vector3 GetPosition(Physics::PhysicsBodyHandle) override { return {}; }
        Math::Quaternion GetRotation(Physics::PhysicsBodyHandle) override { return {}; }
        Math::Vector3 GetVelocity(Physics::PhysicsBodyHandle) override { return {}; }
        Math::Vector3 GetAngularVelocity(Physics::PhysicsBodyHandle) override { return {}; }
        void SetMass(Physics::PhysicsBodyHandle, float) override {}
        float GetMass(Physics::PhysicsBodyHandle) override { return 1.0f; }
        void SetGravityEnabled(Physics::PhysicsBodyHandle, bool) override {}
        void SetGravity(const Math::Vector3 &value) override { gravity = value; }
        [[nodiscard]] Math::Vector3 GetGravity() const override { return gravity; }

        bool Raycast(const Math::Vector3 &, const Math::Vector3 &, Physics::RaycastHit &, float, uint32_t,
                     Physics::QueryTriggers) override
        {
            return false;
        }
        int RaycastAll(const Math::Vector3 &, const Math::Vector3 &, std::vector<Physics::RaycastHit> &, float,
                       uint32_t, Physics::QueryTriggers) override
        {
            return 0;
        }
        bool SphereCast(const Math::Vector3 &, float, const Math::Vector3 &, Physics::RaycastHit &, float,
                        uint32_t) override
        {
            return false;
        }

    private:
        bool _withGpuParticles;
        Physics::GpuStatus _status;
    };
}

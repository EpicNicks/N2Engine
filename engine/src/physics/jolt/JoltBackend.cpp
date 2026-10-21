// The Jolt backend (stage J3, the core; J5, the scene queries). Jolt's headers are included here and nowhere else in the
// engine.
// Everything that touches Jolt state lives in JoltBackend::Impl so the public header holds no Jolt type.

#include "engine/physics/jolt/JoltBackend.hpp"

#ifdef N2ENGINE_JOLT_ENABLED

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#ifndef N2ENGINE_SINGLE_THREADED
#include <Jolt/Core/JobSystemThreadPool.h>
#endif

#include "../QueryUtil.hpp"
#include "engine/GameObject.hpp"
#include "engine/Layers.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/physics/Rigidbody.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace N2Engine::Physics
{
    namespace
    {
        // ===== Jolt's process-wide state =====
        // The allocator, the type factory and the registered types exist once per process: two backends alive at the
        // same time (the tests do that) share them, and the last one to leave tears them down.
        std::mutex &GlobalsMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        int g_globalRefs = 0;

#ifdef JPH_ENABLE_ASSERTS
        // Jolt's asserts (a Debug build has them) say what failed on stderr before the build breaks into the debugger
        bool JoltAssertFailed(const char *expression, const char *message, const char *file, JPH::uint line)
        {
            std::fprintf(stderr, "[JOLT ASSERT] %s:%u: (%s) %s\n", file, static_cast<unsigned>(line), expression,
                         message ? message : "");
            std::fflush(stderr);
            return true; // break
        }
#endif

        void AcquireJoltGlobals()
        {
            const std::lock_guard lock(GlobalsMutex());
            if (g_globalRefs++ == 0)
            {
                JPH::RegisterDefaultAllocator();
#ifdef JPH_ENABLE_ASSERTS
                JPH::AssertFailed = &JoltAssertFailed;
#endif
                JPH::Factory::sInstance = new JPH::Factory();
                JPH::RegisterTypes();
            }
        }

        void ReleaseJoltGlobals()
        {
            const std::lock_guard lock(GlobalsMutex());
            if (g_globalRefs > 0 && --g_globalRefs == 0)
            {
                JPH::UnregisterTypes();
                delete JPH::Factory::sInstance;
                JPH::Factory::sInstance = nullptr;
            }
        }

        // ===== Layers (J3: two object layers, static and moving; J4 replaces them with the engine's 32 layers) =====
        namespace ObjLayers
        {
            constexpr JPH::ObjectLayer NonMoving = 0;
            constexpr JPH::ObjectLayer Moving = 1;
            constexpr JPH::uint Count = 2;
        }

        namespace BpLayers
        {
            constexpr JPH::BroadPhaseLayer NonMoving(0);
            constexpr JPH::BroadPhaseLayer Moving(1);
            constexpr JPH::uint Count = 2;
        }

        // Static bodies collide with moving ones only; moving ones collide with everything
        class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
        {
        public:
            bool ShouldCollide(const JPH::ObjectLayer a, const JPH::ObjectLayer b) const override
            {
                if (a == ObjLayers::NonMoving)
                {
                    return b == ObjLayers::Moving;
                }
                return true;
            }
        };

        class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface
        {
        public:
            BroadPhaseLayerMap()
            {
                _map[ObjLayers::NonMoving] = BpLayers::NonMoving;
                _map[ObjLayers::Moving] = BpLayers::Moving;
            }

            JPH::uint GetNumBroadPhaseLayers() const override { return BpLayers::Count; }
            JPH::BroadPhaseLayer GetBroadPhaseLayer(const JPH::ObjectLayer layer) const override { return _map[layer]; }

        private:
            JPH::BroadPhaseLayer _map[ObjLayers::Count];
        };

        class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide(const JPH::ObjectLayer layer, const JPH::BroadPhaseLayer broadPhaseLayer) const override
            {
                if (layer == ObjLayers::NonMoving)
                {
                    return broadPhaseLayer == BpLayers::Moving;
                }
                return true;
            }
        };

        // ===== Contact combine rules =====
        // PhysX combines two materials' friction and restitution as the average by default (PxCombineMode::eAVERAGE,
        // PxMaterial.h); Jolt's defaults are the geometric mean and the maximum. The backend installs the average for
        // both, as plain functions of the two bodies' values (so the result does not depend on any order).
        float CombineAverage(const float a, const float b)
        {
            return 0.5f * (a + b);
        }

        float CombineFriction(const JPH::Body &a, const JPH::SubShapeID &, const JPH::Body &b, const JPH::SubShapeID &)
        {
            return CombineAverage(a.GetFriction(), b.GetFriction());
        }

        float CombineRestitution(const JPH::Body &a, const JPH::SubShapeID &, const JPH::Body &b,
                                 const JPH::SubShapeID &)
        {
            return CombineAverage(a.GetRestitution(), b.GetRestitution());
        }

        // ===== Conversions =====
        bool IsFinite(const Math::Vector3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        JPH::Vec3 ToJolt(const Math::Vector3 &v)
        {
            return JPH::Vec3(v.x, v.y, v.z);
        }

        Math::Vector3 ToEngine(const JPH::Vec3 &v)
        {
            return Math::Vector3(v.GetX(), v.GetY(), v.GetZ());
        }

        // Jolt wants unit rotations. A rotation that is one already (within float rounding) is passed on untouched; a
        // zero-length or non-finite one becomes the identity.
        JPH::Quat ToJolt(const Math::Quaternion &q)
        {
            const float x = q.GetX();
            const float y = q.GetY();
            const float z = q.GetZ();
            const float w = q.GetW();
            const float lengthSquared = x * x + y * y + z * z + w * w;
            if (!std::isfinite(lengthSquared) || lengthSquared < 1e-12f)
            {
                return JPH::Quat::sIdentity();
            }
            if (std::abs(lengthSquared - 1.0f) <= 1e-6f)
            {
                return JPH::Quat(x, y, z, w);
            }
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            return JPH::Quat(x * inverseLength, y * inverseLength, z * inverseLength, w * inverseLength);
        }

        // The engine's quaternion constructor takes (w, x, y, z)
        Math::Quaternion ToEngine(const JPH::Quat &q)
        {
            return Math::Quaternion(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
        }

        // Rigidbody::SetMass clamps, but a deserialized mass reaches the backend unchecked; zero would make a dynamic
        // body immovable and a negative or non-finite one is invalid in Jolt (the same rule as the PhysX backend)
        float SanitizedMass(const float mass)
        {
            if (mass > 0.0f && std::isfinite(mass))
            {
                return mass;
            }

            Logger::Warn(std::format("Invalid Rigidbody mass {}, using 1", mass));
            return 1.0f;
        }

        // A shape needs a volume: Jolt computes mass and inertia from it, and a zero extent would make both zero
        constexpr float kMinExtent = 0.001f;
        // The capsule's cylinder never gets shorter than this (the PhysX backend's rule too)
        constexpr float kMinCylinderHalfHeight = 0.01f;

        // What a body that has no solid shape (no collider yet, or only triggers) weighs and how it turns: the
        // configured mass, a centre of mass on the origin and the inertia of a unit radius of gyration (I = m on each
        // axis), so it scales with the mass like a shaped body's does. The PhysX backend does the same.
        JPH::MassProperties ShapelessMassProperties(const float mass)
        {
            JPH::MassProperties properties;
            properties.mMass = mass;
            properties.mInertia = JPH::Mat44::sScale(mass);
            return properties;
        }

        enum class ColliderKind : std::uint8_t
        {
            Sphere,
            Box,
            Capsule
        };

        // One collider as the engine described it. The body's Jolt shape is rebuilt from the list of these whenever one
        // changes; the index of a spec in the list is the user data of its shape (what J4 maps a hit sub-shape back
        // to), so it is the position among ALL the body's colliders, trigger ones included.
        struct ColliderSpec
        {
            ICollider *collider = nullptr;
            ColliderKind kind = ColliderKind::Sphere;
            float radius = 0.5f;
            float height = 2.0f; // capsule: the whole height, caps included
            Math::Vector3 halfExtents{0.5f, 0.5f, 0.5f};
            Math::Vector3 offset = Math::Vector3::Zero;
            PhysicsMaterial material;
            bool isTrigger = false;
        };

        enum class MotionKind : std::uint8_t
        {
            Static,
            Dynamic,
            Kinematic
        };

        // The features this stage does not have yet, one warning each
        enum NotImplemented : unsigned
        {
            kLayers = 1u << 0,
            kTriggers = 1u << 1,
            kEvents = 1u << 2,
            kSnapshot = 1u << 3
        };
    }

    struct JoltBackend::Impl
    {
        struct BodyData
        {
            bool active = false;
            std::uint32_t generation = 0;
            JPH::BodyID id;
            MotionKind motion = MotionKind::Static;
            bool enabled = true; // in the simulation (added to Jolt's broad phase)

            Rigidbody *rigidbody = nullptr;
            std::vector<ICollider *> colliders;
            std::vector<ColliderSpec> specs;
            JPH::RefConst<JPH::Shape> shape;
            std::size_t shapeCount = 0; // non-trigger colliders in `shape`

            float mass = 1.0f;
            bool gravityEnabled = true;
            // Mass frame as last given to Jolt (kept here because a kinematic body's motion properties hold none)
            Math::Vector3 inertia = Math::Vector3::Zero;
            Math::Vector3 centerOfMass = Math::Vector3::Zero;

            // Kinematic bodies: the pose the next Update moves the body to, and whether the last Update moved it (its
            // velocity must then be cleared when no new target comes, or the body would drift on)
            bool hasKinematicTarget = false;
            Math::Vector3 targetPosition = Math::Vector3::Zero;
            Math::Quaternion targetRotation = Math::Quaternion::Identity;
            bool kinematicMoving = false;
        };

        // The layer objects are referenced by the physics system for its whole life: declared first, destroyed last
        BroadPhaseLayerMap broadPhaseLayers;
        ObjectVsBroadPhaseFilter objectVsBroadPhase;
        ObjectPairFilter objectPairs;
        std::unique_ptr<JPH::PhysicsSystem> system;
        std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
        std::unique_ptr<JPH::JobSystem> jobSystem;

        std::uint32_t maxBodies = kMaxBodies;
        bool initialized = false;
        bool holdsGlobals = false;
        bool singleThreaded = true;

        std::vector<BodyData> bodies;
        std::vector<std::uint32_t> freeList;

        Math::Vector3 currentGravity{0.0f, -9.81f, 0.0f};
        std::optional<Math::Vector3> pendingGravity;

        unsigned warned = 0;
        unsigned warnCount = 0;
        bool updateErrorLogged = false;

        JPH::BodyInterface &Bodies() { return system->GetBodyInterface(); }
        const JPH::BodyInterface &Bodies() const { return system->GetBodyInterface(); }

        void WarnOnce(const NotImplemented feature, const char *text)
        {
            if ((warned & feature) != 0)
            {
                return;
            }
            warned |= feature;
            ++warnCount;
            Logger::Warn(std::format("Jolt backend: {} (not implemented yet; see docs/physics.html)", text));
        }

        // ===== Handles (the PhysX backend's scheme, so both give the same handles for the same sequence) =====
        PhysicsBodyHandle AllocateHandle()
        {
            PhysicsBodyHandle handle;
            if (!freeList.empty())
            {
                handle.index = freeList.back();
                freeList.pop_back();
                bodies[handle.index].generation++;
            }
            else
            {
                if (bodies.size() >= maxBodies)
                {
                    return INVALID_PHYSICS_HANDLE;
                }
                handle.index = static_cast<std::uint32_t>(bodies.size());
                bodies.emplace_back();
            }

            handle.generation = bodies[handle.index].generation;
            bodies[handle.index].active = true;
            return handle;
        }

        void FreeHandle(const PhysicsBodyHandle handle)
        {
            BodyData &data = bodies[handle.index];
            data.active = false;
            data.rigidbody = nullptr;
            data.colliders.clear();
            data.specs.clear();
            data.shape = nullptr;
            data.shapeCount = 0;
            data.hasKinematicTarget = false;
            data.kinematicMoving = false;
            freeList.push_back(handle.index);
        }

        BodyData *GetData(const PhysicsBodyHandle handle)
        {
            if (!initialized || handle.index >= bodies.size())
            {
                return nullptr;
            }
            BodyData &data = bodies[handle.index];
            if (!data.active || data.generation != handle.generation)
            {
                return nullptr;
            }
            return &data;
        }

        const BodyData *GetData(const PhysicsBodyHandle handle) const
        {
            return const_cast<Impl *>(this)->GetData(handle);
        }

        // The GameObject a body belongs to: its Rigidbody's, else its first collider's
        static GameObject *OwnerOf(const BodyData &data)
        {
            if (data.rigidbody)
            {
                return &data.rigidbody->GetGameObject();
            }
            if (!data.colliders.empty())
            {
                return &data.colliders.front()->GetGameObject();
            }
            return nullptr;
        }

        // ===== Bodies =====
        PhysicsBodyHandle CreateBody(const Math::Vector3 &position, const Math::Quaternion &rotation, const float mass,
                                     Rigidbody *rigidbody, const MotionKind motion)
        {
            if (!initialized)
            {
                return INVALID_PHYSICS_HANDLE;
            }
            if (!IsFinite(position))
            {
                Logger::Error("Failed to create a physics body: its position is not finite");
                return INVALID_PHYSICS_HANDLE;
            }

            const PhysicsBodyHandle handle = AllocateHandle();
            if (!handle.IsValid())
            {
                Logger::Error(std::format("Failed to create a physics body: the Jolt backend holds at most {} bodies",
                                          maxBodies));
                return INVALID_PHYSICS_HANDLE;
            }
            BodyData &data = bodies[handle.index];
            data.motion = motion;
            data.rigidbody = rigidbody;
            data.mass = motion == MotionKind::Static ? 1.0f : SanitizedMass(mass);

            const JPH::EMotionType type = motion == MotionKind::Static      ? JPH::EMotionType::Static
                                          : motion == MotionKind::Kinematic ? JPH::EMotionType::Kinematic
                                                                            : JPH::EMotionType::Dynamic;
            const JPH::Ref<JPH::Shape> empty = new JPH::EmptyShape();
            JPH::BodyCreationSettings settings(empty.GetPtr(), ToJolt(position), ToJolt(rotation), type,
                                               motion == MotionKind::Static ? ObjLayers::NonMoving : ObjLayers::Moving);
            const PhysicsMaterial defaults = PhysicsMaterial::Default();
            settings.mFriction = defaults.dynamicFriction;
            settings.mRestitution = defaults.restitution;
            settings.mAllowSleeping = true;
            // The PhysX defaults for a rigid dynamic (PxRigidBody.h): no linear damping, angular damping 0.05 (also
            // Jolt's), a maximum angular velocity of 100 rad/s. Jolt's maximum linear velocity (500 m/s) stays: PhysX's
            // is 1e16 and not a limit anyone relies on.
            settings.mLinearDamping = 0.0f;
            settings.mAngularDamping = 0.05f;
            settings.mMaxAngularVelocity = 100.0f;
            if (motion != MotionKind::Static)
            {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                settings.mMassPropertiesOverride = ShapelessMassProperties(data.mass);
            }

            // The id is the handle's index plus its generation as the sequence number, which is what Jolt's own ids carry:
            // its broad phase tells a body that was removed from a new one that reuses the index within one step by the
            // sequence number (the engine reuses an index at once, so a bare index would be the same id twice in a step)
            JPH::Body *body = Bodies().CreateBodyWithID(
                JPH::BodyID(handle.index, static_cast<JPH::uint8>(handle.generation & 0xFFu)), settings);
            if (!body)
            {
                Logger::Error("Failed to create a physics body: Jolt refused the body id");
                FreeHandle(handle);
                return INVALID_PHYSICS_HANDLE;
            }
            data.id = body->GetID();
            data.shape = empty.GetPtr();
            data.inertia = Math::Vector3(data.mass, data.mass, data.mass);
            data.enabled = true;
            Bodies().AddBody(data.id, motion == MotionKind::Static ? JPH::EActivation::DontActivate
                                                                     : JPH::EActivation::Activate);
            return handle;
        }

        void DestroyBodyData(BodyData &data)
        {
            if (data.enabled && Bodies().IsAdded(data.id))
            {
                Bodies().RemoveBody(data.id);
            }
            Bodies().DestroyBody(data.id);
            data.enabled = false;
        }

        // ===== Shapes and mass =====
        JPH::Ref<JPH::Shape> MakeLeaf(const ColliderSpec &spec, const std::uint32_t index) const
        {
            JPH::Ref<JPH::Shape> leaf;
            switch (spec.kind)
            {
            case ColliderKind::Sphere:
                leaf = new JPH::SphereShape(std::max(spec.radius, kMinExtent));
                break;
            case ColliderKind::Box:
            {
                const JPH::Vec3 half(std::max(spec.halfExtents.x, kMinExtent), std::max(spec.halfExtents.y, kMinExtent),
                                     std::max(spec.halfExtents.z, kMinExtent));
                // The convex radius rounds the box's edges in Jolt's collision tests: never more than the smallest half
                // extent, or the box would be bigger than its size says
                leaf = new JPH::BoxShape(half, std::min(JPH::cDefaultConvexRadius, half.ReduceMin()));
                break;
            }
            case ColliderKind::Capsule:
            {
                const float radius = std::max(spec.radius, kMinExtent);
                const float halfCylinder = std::max(kMinCylinderHalfHeight, (spec.height - 2.0f * radius) * 0.5f);
                // Jolt's capsule stands along Y, as the engine's does: no rotation is needed (PhysX's lies along X)
                leaf = new JPH::CapsuleShape(halfCylinder, radius);
                break;
            }
            }
            leaf->SetUserData(index);
            return leaf;
        }

        // Rebuilds the body's Jolt shape from its colliders: the non-trigger ones, each at its offset, as one compound
        // (a single collider at the origin is the plain shape itself: StaticCompoundShapeSettings::Create returns it);
        // then the mass properties and the material of the body.
        void RebuildBody(BodyData &data)
        {
            JPH::StaticCompoundShapeSettings compound;
            std::size_t solidCount = 0;
            float friction = 0.0f;
            float restitution = 0.0f;
            for (std::size_t i = 0; i < data.specs.size(); ++i)
            {
                const ColliderSpec &spec = data.specs[i];
                if (spec.isTrigger)
                {
                    continue; // J4: a trigger becomes a sensor; until then it has no shape
                }
                const JPH::Ref<JPH::Shape> leaf = MakeLeaf(spec, static_cast<std::uint32_t>(i));
                compound.AddShape(ToJolt(spec.offset), JPH::Quat::sIdentity(), leaf.GetPtr(),
                                  static_cast<std::uint32_t>(i));
                friction += spec.material.dynamicFriction;
                restitution += spec.material.restitution;
                ++solidCount;
            }

            JPH::RefConst<JPH::Shape> shape;
            if (solidCount > 0)
            {
                const JPH::ShapeSettings::ShapeResult result = compound.Create();
                if (result.HasError())
                {
                    Logger::Error(std::format("Jolt backend: could not build a body's shape: {}",
                                              std::string(result.GetError().c_str())));
                    return;
                }
                shape = result.Get();
            }
            else
            {
                shape = new JPH::EmptyShape();
            }

            const bool inBroadPhase = data.enabled;
            Bodies().SetShape(data.id, shape.GetPtr(), false,
                              inBroadPhase && data.motion != MotionKind::Static ? JPH::EActivation::Activate
                                                                                : JPH::EActivation::DontActivate);
            data.shape = shape;
            data.shapeCount = solidCount;

            // One friction and one restitution per body in Jolt (docs/physics.html): the average of the solid colliders'
            // values (dynamic friction; Jolt has no static friction), the default material when there is none
            const PhysicsMaterial defaults = PhysicsMaterial::Default();
            const float count = static_cast<float>(solidCount);
            Bodies().SetFriction(data.id, solidCount > 0 ? std::max(0.0f, friction / count) : defaults.dynamicFriction);
            Bodies().SetRestitution(data.id,
                                    solidCount > 0 ? std::max(0.0f, restitution / count) : defaults.restitution);

            ApplyMassProperties(data);
        }

        // The Rigidbody's mass is authoritative; the shape, at uniform density, gives the inertia tensor and the centre
        // of mass (the same computation as EOverrideMassProperties::CalculateInertia, applied to the live body because
        // the shape changes after the body exists). A body with no solid shape gets ShapelessMassProperties.
        void ApplyMassProperties(BodyData &data)
        {
            if (data.motion == MotionKind::Static)
            {
                return;
            }
            JPH::MassProperties properties;
            if (data.shapeCount > 0 && data.shape)
            {
                properties = data.shape->GetMassProperties();
                properties.ScaleToMass(data.mass);
                const JPH::Vec3 center = data.shape->GetCenterOfMass();
                data.centerOfMass = ToEngine(center);
            }
            else
            {
                properties = ShapelessMassProperties(data.mass);
                data.centerOfMass = Math::Vector3::Zero;
            }
            data.inertia = Math::Vector3(properties.mInertia(0, 0), properties.mInertia(1, 1), properties.mInertia(2, 2));

            JPH::BodyLockWrite lock(system->GetBodyLockInterface(), data.id);
            if (lock.Succeeded())
            {
                JPH::MotionProperties *motion = lock.GetBody().GetMotionProperties();
                motion->SetMassProperties(motion->GetAllowedDOFs(), properties);
            }
        }

        ColliderSpec *FindSpec(BodyData &data, const ICollider *collider)
        {
            if (!collider)
            {
                return nullptr;
            }
            const auto it = std::ranges::find_if(data.specs, [collider](const ColliderSpec &s)
                                                 { return s.collider == collider; });
            return it == data.specs.end() ? nullptr : &*it;
        }

        // Adds the collider's spec (or replaces the one it already has: a collider is one shape here) and rebuilds
        void SetSpec(const PhysicsBodyHandle handle, ColliderSpec spec)
        {
            BodyData *data = GetData(handle);
            if (!data)
            {
                return;
            }
            if (ColliderSpec *existing = FindSpec(*data, spec.collider))
            {
                spec.isTrigger = existing->isTrigger;
                *existing = std::move(spec);
            }
            else
            {
                data->specs.push_back(std::move(spec));
            }
            RebuildBody(*data);
        }

        // Changes the geometry of the collider's spec; nothing happens for a collider that has no spec
        void UpdateSpec(const PhysicsBodyHandle handle, const ColliderSpec &changed)
        {
            BodyData *data = GetData(handle);
            if (!data)
            {
                return;
            }
            ColliderSpec *existing = FindSpec(*data, changed.collider);
            if (!existing)
            {
                return;
            }
            const bool isTrigger = existing->isTrigger;
            *existing = changed;
            existing->isTrigger = isTrigger;
            RebuildBody(*data);
        }

        // ===== Queries (J5) =====
        // Scene queries go through NarrowPhaseQuery only (never the broad-phase query, whose order is not
        // deterministic). Every hit is collected, resolved to the collider it hit, filtered (layer mask, triggers) and
        // ordered by QueryRules in QueryUtil: nothing depends on the order Jolt reports hits in.

        /// A hit sub-shape resolved to the engine's side: the body, the collider and where it sits in the order
        struct ResolvedHit
        {
            const BodyData *data = nullptr;
            PhysicsBodyHandle handle;
            ICollider *collider = nullptr;
            std::size_t colliderOrder = 0;
        };

        /// What one query hit is until it is turned into a RaycastHit
        struct RawHit
        {
            ResolvedHit resolved;
            float distance = 0.0f;
            JPH::BodyID body;
            JPH::SubShapeID subShape;
            JPH::Vec3 normal = JPH::Vec3::sZero(); // sweeps: the surface normal at the hit (unit, or zero)
            JPH::Vec3 point = JPH::Vec3::sZero();  // sweeps: the contact point on the hit shape
        };

        static int LayerOf(const ICollider *collider)
        {
            return collider ? collider->GetGameObject().GetLayer() : Layers::Default;
        }

        /// The collider that owns sub-shape `subShape` of body `id`, or false: not a body of this backend, a collider
        /// the mask leaves out, or a trigger collider while triggers are ignored. (Trigger colliders have no Jolt
        /// shape until J4, so no query can hit one before then.)
        bool Resolve(const JPH::BodyID id, const JPH::SubShapeID &subShape, const std::uint32_t layerMask,
                     const QueryTriggers triggers, ResolvedHit &out) const
        {
            if (id.GetIndex() >= bodies.size())
            {
                return false;
            }
            const BodyData &data = bodies[id.GetIndex()];
            if (!data.active || !data.shape || data.id != id)
            {
                return false;
            }
            const std::uint64_t index = data.shape->GetSubShapeUserData(subShape);
            if (index >= data.specs.size())
            {
                return false;
            }
            const ColliderSpec &spec = data.specs[static_cast<std::size_t>(index)];
            if (!spec.collider || (spec.isTrigger && triggers == QueryTriggers::Ignore))
            {
                return false;
            }
            if ((layerMask & Layers::MaskOf(LayerOf(spec.collider))) == 0)
            {
                return false;
            }
            out.data = &data;
            out.handle = PhysicsBodyHandle{id.GetIndex(), data.generation};
            out.collider = spec.collider;
            out.colliderOrder = QueryUtil::ColliderOrder(data.colliders, spec.collider);
            return true;
        }

        /// Calls `accept(result)` for every hit of a query. The collector keeps nothing and never lowers the early-out
        /// fraction, so Jolt reports every hit along the whole ray or sweep.
        template <typename Base, typename Result, typename Accept>
        class FunctionCollector final : public Base
        {
        public:
            explicit FunctionCollector(Accept accept) : _accept(std::move(accept)) {}
            void AddHit(const Result &result) override { _accept(result); }

        private:
            Accept _accept;
        };

        static QueryUtil::HitKey KeyOfRaw(const RawHit &raw)
        {
            return QueryUtil::KeyOf(raw.distance, raw.resolved.handle, raw.resolved.colliderOrder);
        }

        /// Every collider a ray hits within `sweep.distance`
        std::vector<RawHit> CollectRay(const Math::Vector3 &origin, const QueryUtil::Sweep &sweep,
                                       const std::uint32_t layerMask, const QueryTriggers triggers) const
        {
            std::vector<RawHit> raws;
            // The defaults: a ray that starts inside a convex shape hits it at fraction 0 (mTreatConvexAsSolid), one
            // that leaves it reports nothing (back faces ignored)
            JPH::RayCastSettings settings;
            const JPH::RRayCast ray(ToJolt(origin), ToJolt(sweep.direction) * sweep.distance);
            const auto accept = [&](const JPH::RayCastResult &result)
            {
                RawHit raw;
                if (!Resolve(result.mBodyID, result.mSubShapeID2, layerMask, triggers, raw.resolved))
                {
                    return;
                }
                raw.distance = result.mFraction * sweep.distance;
                raw.body = result.mBodyID;
                raw.subShape = result.mSubShapeID2;
                raws.push_back(raw);
            };
            FunctionCollector<JPH::CastRayCollector, JPH::RayCastResult, decltype(accept)> collector(accept);
            system->GetNarrowPhaseQuery().CastRay(ray, settings, collector);
            return raws;
        }

        /// Every collider `shape` (placed at `position` with `rotation`, its centre of mass at the origin of the
        /// shape) hits when swept along `sweep` (a start inside a shape is a hit at distance 0)
        std::vector<RawHit> CollectCast(const JPH::Shape &shape, const Math::Vector3 &position,
                                        const JPH::Quat &rotation, const QueryUtil::Sweep &sweep,
                                        const std::uint32_t layerMask, const QueryTriggers triggers) const
        {
            std::vector<RawHit> raws;
            JPH::ShapeCastSettings settings;
            settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
            settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces; // report a start inside a shape
            settings.mReturnDeepestPoint = false;
            const JPH::Vec3 toward = ToJolt(sweep.direction);
            const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
                &shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(rotation, ToJolt(position)),
                toward * sweep.distance);
            const auto accept = [&](const JPH::ShapeCastResult &result)
            {
                RawHit raw;
                if (!Resolve(result.mBodyID2, result.mSubShapeID2, layerMask, triggers, raw.resolved))
                {
                    return;
                }
                raw.distance = result.mFraction * sweep.distance;
                raw.body = result.mBodyID2;
                raw.subShape = result.mSubShapeID2;
                raw.point = result.mContactPointOn2;
                // mPenetrationAxis points from the cast shape to the hit one: the surface normal is its opposite
                raw.normal = (-result.mPenetrationAxis).NormalizedOr(-toward);
                raws.push_back(raw);
            };
            FunctionCollector<JPH::CastShapeCollector, JPH::ShapeCastResult, decltype(accept)> collector(accept);
            system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector);
            return raws;
        }

        /// Every collider `shape` (at `position` and `rotation`) overlaps, each once, in QueryRules' order
        int CollectOverlap(const JPH::Shape &shape, const Math::Vector3 &position, const JPH::Quat &rotation,
                           const std::uint32_t layerMask, const QueryTriggers triggers,
                           std::vector<OverlapHit> &hits) const
        {
            std::vector<RawHit> raws;
            JPH::CollideShapeSettings settings;
            const auto accept = [&](const JPH::CollideShapeResult &result)
            {
                RawHit raw;
                if (Resolve(result.mBodyID2, result.mSubShapeID2, layerMask, triggers, raw.resolved))
                {
                    raws.push_back(raw);
                }
            };
            FunctionCollector<JPH::CollideShapeCollector, JPH::CollideShapeResult, decltype(accept)> collector(accept);
            system->GetNarrowPhaseQuery().CollideShape(
                &shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(rotation, ToJolt(position)),
                settings, JPH::RVec3::sZero(), collector);

            std::vector<QueryUtil::HitKey> keys;
            keys.reserve(raws.size());
            for (const RawHit &raw : raws)
            {
                keys.push_back(KeyOfRaw(raw));
            }
            QueryUtil::SortByKeys(raws, keys, QueryUtil::HandleThenDistanceLess);
            for (std::size_t i = 0; i < raws.size(); ++i)
            {
                // A collider is one Jolt sub-shape, but report it once whatever Jolt does
                if (i > 0 && keys[i].index == keys[i - 1].index && keys[i].generation == keys[i - 1].generation &&
                    keys[i].colliderOrder == keys[i - 1].colliderOrder)
                {
                    continue;
                }
                OverlapHit hit;
                hit.gameObject = OwnerOf(*raws[i].resolved.data);
                hit.rigidbody = raws[i].resolved.data->rigidbody;
                hit.collider = raws[i].resolved.collider;
                hit.bodyHandle = raws[i].resolved.handle;
                hits.push_back(hit);
            }
            return static_cast<int>(hits.size());
        }

        /// Fills `hit` from a raw hit of a ray (`isRay`: point and normal are computed here, from the hit sub-shape)
        /// or a sweep (they came from Jolt); a hit at distance 0 follows QueryRules' started-inside answer
        void FillHit(RaycastHit &hit, const RawHit &raw, const Math::Vector3 &origin, const QueryUtil::Sweep &sweep,
                     const bool isRay) const
        {
            hit = RaycastHit{};
            hit.hit = true;
            hit.distance = raw.distance;
            hit.bodyHandle = raw.resolved.handle;
            hit.gameObject = OwnerOf(*raw.resolved.data);
            hit.rigidbody = raw.resolved.data->rigidbody;
            hit.collider = raw.resolved.collider;
            if (raw.distance <= 0.0f)
            {
                QueryUtil::MarkStartedInside(hit, origin, sweep.direction);
                return;
            }
            if (isRay)
            {
                hit.point = Math::Vector3(origin.x + sweep.direction.x * raw.distance,
                                          origin.y + sweep.direction.y * raw.distance,
                                          origin.z + sweep.direction.z * raw.distance);
                const JPH::BodyLockRead lock(system->GetBodyLockInterface(), raw.body);
                JPH::Vec3 normal = JPH::Vec3::sZero();
                if (lock.Succeeded())
                {
                    normal = lock.GetBody().GetWorldSpaceSurfaceNormal(raw.subShape, ToJolt(hit.point));
                }
                hit.normal = ToEngine(normal);
            }
            else
            {
                hit.point = ToEngine(raw.point);
                hit.normal = ToEngine(raw.normal);
            }
        }

        /// The single-hit queries' answer: the nearest of `raws`, ties broken by handle (QueryRules)
        bool PickNearest(const std::vector<RawHit> &raws, RaycastHit &hit, const Math::Vector3 &origin,
                         const QueryUtil::Sweep &sweep, const bool isRay) const
        {
            std::vector<QueryUtil::HitKey> keys;
            keys.reserve(raws.size());
            for (const RawHit &raw : raws)
            {
                keys.push_back(KeyOfRaw(raw));
            }
            const std::size_t best = QueryUtil::PickClosest(keys);
            if (best == SIZE_MAX)
            {
                return false;
            }
            FillHit(hit, raws[best], origin, sweep, isRay);
            return true;
        }

        /// A capsule's Jolt shape: standing along Y at the origin, and the rotation that lays it along the segment
        /// (coincident end points: a sphere)
        static JPH::RefConst<JPH::Shape> MakeCapsuleQueryShape(const QueryUtil::CapsuleFrame &frame,
                                                               const float radius, JPH::Quat &rotation)
        {
            if (!(frame.halfLength > 1e-5f))
            {
                rotation = JPH::Quat::sIdentity();
                return new JPH::SphereShape(radius);
            }
            const QueryUtil::ShortestRotation turn =
                QueryUtil::RotationBetween(Math::Vector3(0.0f, 1.0f, 0.0f), frame.axis);
            rotation = JPH::Quat(turn.x, turn.y, turn.z, turn.w);
            return new JPH::CapsuleShape(frame.halfLength, radius);
        }
    };

    // ===== Construction, lifetime =====

    JoltBackend::JoltBackend() : _impl(std::make_unique<Impl>()) {}

    JoltBackend::~JoltBackend()
    {
        JoltBackend::Shutdown();
    }

    DeterminismLevel JoltBackend::DeclaredDeterminism(const bool deterministic)
    {
#ifdef N2ENGINE_SINGLE_THREADED
        (void)deterministic;
        constexpr bool singleThreaded = true;
#else
        const bool singleThreaded = deterministic;
#endif
        if (!singleThreaded)
        {
            return DeterminismLevel::None;
        }
#if defined(JPH_CROSS_PLATFORM_DETERMINISTIC) && defined(N2_STRICT_FP)
        return DeterminismLevel::CrossPlatform;
#else
        return DeterminismLevel::SameBinary;
#endif
    }

    bool JoltBackend::Initialize()
    {
        Impl &s = *_impl;
        if (s.initialized)
        {
            return true;
        }

        AcquireJoltGlobals();
        s.holdsGlobals = true;

        s.system = std::make_unique<JPH::PhysicsSystem>();
        s.maxBodies = std::max<std::uint32_t>(1, std::min(_maxBodies, kMaxBodies));
        s.system->Init(s.maxBodies, 0, kMaxBodyPairs, kMaxContactConstraints, s.broadPhaseLayers, s.objectVsBroadPhase,
                       s.objectPairs);
        s.system->SetGravity(ToJolt(s.currentGravity));
        s.system->SetCombineFriction(&CombineFriction);
        s.system->SetCombineRestitution(&CombineRestitution);

        // PhysX's bounce threshold (PxSceneDesc bounceThresholdVelocity: 0.2 * the tolerance scale's speed, which is 10)
        // is 2 m/s; Jolt's default is 1 m/s. Everything else of Jolt's settings stays at its default.
        JPH::PhysicsSettings settings = s.system->GetPhysicsSettings();
        settings.mMinVelocityForRestitution = 2.0f;
        s.system->SetPhysicsSettings(settings);

        s.tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(static_cast<JPH::uint>(kTempAllocatorBytes));

        // Single-threaded on the web (no pthreads) and when physics.deterministic asks for it; the thread pool
        // otherwise. Until CI proves that Jolt's results do not depend on the thread count, the Deterministic preset
        // must use the single-threaded one.
#ifdef N2ENGINE_SINGLE_THREADED
        s.singleThreaded = true;
#else
        s.singleThreaded = _deterministic;
#endif
        if (s.singleThreaded)
        {
            s.jobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        }
#ifndef N2ENGINE_SINGLE_THREADED
        else
        {
            s.jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                                     kWorkerThreads);
        }
#endif

        s.initialized = true;
        s.WarnOnce(kEvents, "contact and trigger events (OnCollisionEnter, OnTriggerEnter, ...) are not delivered");
        Logger::Info(std::format("Jolt backend initialized ({} job system)",
                                 s.singleThreaded ? "single-threaded" : "thread-pool"));
        return true;
    }

    void JoltBackend::Shutdown()
    {
        if (!_impl)
        {
            return;
        }
        Impl &s = *_impl;
        if (s.initialized)
        {
            // Bodies in handle order, out of the broad phase before they are destroyed
            for (Impl::BodyData &data : s.bodies)
            {
                if (data.active)
                {
                    s.DestroyBodyData(data);
                }
            }
            s.jobSystem.reset();
            s.tempAllocator.reset();
            s.system.reset();
            s.bodies.clear();
            s.freeList.clear();
            s.pendingGravity.reset();
            s.initialized = false;
        }
        if (s.holdsGlobals)
        {
            ReleaseJoltGlobals();
            s.holdsGlobals = false;
        }
    }

    void JoltBackend::Update(const float deltaTime)
    {
        Impl &s = *_impl;
        if (!s.initialized || !(deltaTime > 0.0f) || !std::isfinite(deltaTime))
        {
            return;
        }

        // Kinematic bodies move to the pose set since the last step, in handle order (Jolt sets the velocity that
        // reaches it, so the body pushes what it meets). One that was moved last step and has no new target stops.
        for (Impl::BodyData &data : s.bodies)
        {
            if (!data.active || data.motion != MotionKind::Kinematic || !data.enabled)
            {
                continue;
            }
            if (data.hasKinematicTarget)
            {
                s.Bodies().MoveKinematic(data.id, ToJolt(data.targetPosition), ToJolt(data.targetRotation), deltaTime);
                data.hasKinematicTarget = false;
                data.kinematicMoving = true;
            }
            else if (data.kinematicMoving)
            {
                s.Bodies().SetLinearAndAngularVelocity(data.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                data.kinematicMoving = false;
            }
        }

        const JPH::EPhysicsUpdateError error =
            s.system->Update(deltaTime, kCollisionSteps, s.tempAllocator.get(), s.jobSystem.get());
        if (error != JPH::EPhysicsUpdateError::None && !s.updateErrorLogged)
        {
            s.updateErrorLogged = true;
            Logger::Error(std::format("Jolt backend: a simulation buffer overflowed (error flags {}); some contacts were "
                                      "ignored. The limits are JoltBackend::kMaxBodyPairs and kMaxContactConstraints.",
                                      static_cast<std::uint32_t>(error)));
        }
    }

    void JoltBackend::ApplyPendingChanges()
    {
        Impl &s = *_impl;
        if (!s.initialized || !s.pendingGravity)
        {
            return;
        }
        s.currentGravity = *s.pendingGravity;
        s.pendingGravity.reset();
        s.system->SetGravity(ToJolt(s.currentGravity));
        Logger::Info(std::format("Gravity applied: ({}, {}, {})", s.currentGravity.x, s.currentGravity.y,
                                 s.currentGravity.z));
    }

    void JoltBackend::SyncTransforms()
    {
        Impl &s = *_impl;
        if (!s.initialized)
        {
            return;
        }
        // The backend's own handle-ordered list, never Jolt's active-body list (its order is not deterministic).
        // Only simulated dynamic bodies: a kinematic body is driven by its transform, and writing the physics pose back
        // would overwrite a script's move before the step consumed it. A disabled body did not move.
        for (const Impl::BodyData &data : s.bodies)
        {
            if (!data.active || !data.rigidbody || !data.enabled || data.motion != MotionKind::Dynamic)
            {
                continue;
            }
            JPH::RVec3 position;
            JPH::Quat rotation;
            s.Bodies().GetPositionAndRotation(data.id, position, rotation);
            if (Positionable *positionable = data.rigidbody->GetGameObject().GetPositionable())
            {
                positionable->SetPositionAndRotation(ToEngine(JPH::Vec3(position)), ToEngine(rotation));
            }
        }
    }

    void JoltBackend::ProcessCollisionCallbacks()
    {
        // J4: contact and trigger events. The warning was given by Initialize.
    }

    // ===== Bodies =====

    PhysicsBodyHandle JoltBackend::CreateDynamicBody(const Math::Vector3 &position, const Math::Quaternion &rotation,
                                                     const float mass, Rigidbody *rigidbody, const bool isKinematic)
    {
        return _impl->CreateBody(position, rotation, mass, rigidbody,
                                 isKinematic ? MotionKind::Kinematic : MotionKind::Dynamic);
    }

    PhysicsBodyHandle JoltBackend::CreateStaticBody(const Math::Vector3 &position, const Math::Quaternion &rotation,
                                                    Rigidbody *rigidbody)
    {
        return _impl->CreateBody(position, rotation, 1.0f, rigidbody, MotionKind::Static);
    }

    void JoltBackend::DestroyBody(const PhysicsBodyHandle handle)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(handle);
        if (!data)
        {
            return;
        }
        s.DestroyBodyData(*data);
        s.FreeHandle(handle);
    }

    void JoltBackend::SetBodyEnabled(const PhysicsBodyHandle handle, const bool enabled)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(handle);
        if (!data || data->enabled == enabled)
        {
            return;
        }

        if (!enabled)
        {
            // Out of the simulation: it neither moves nor shows up in queries. The id stays, so a re-enabled body is
            // the same Jolt body (ids must match across peers).
            data->hasKinematicTarget = false;
            if (data->motion == MotionKind::Kinematic)
            {
                s.Bodies().SetLinearAndAngularVelocity(data->id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                data->kinematicMoving = false;
            }
            if (s.Bodies().IsAdded(data->id))
            {
                s.Bodies().RemoveBody(data->id);
            }
            data->enabled = false;
            return;
        }

        // Back where its GameObject is now: it may have been moved while it was out
        if (const GameObject *owner = Impl::OwnerOf(*data))
        {
            if (const Positionable *positionable = owner->GetPositionable())
            {
                s.Bodies().SetPositionAndRotation(data->id, ToJolt(positionable->GetPosition()),
                                                  ToJolt(positionable->GetRotation()), JPH::EActivation::DontActivate);
            }
        }
        s.Bodies().AddBody(data->id, data->motion == MotionKind::Static ? JPH::EActivation::DontActivate
                                                                          : JPH::EActivation::Activate);
        data->enabled = true;
    }

    void JoltBackend::RegisterCollider(const PhysicsBodyHandle handle, ICollider *collider)
    {
        if (Impl::BodyData *data = _impl->GetData(handle); data && collider)
        {
            if (std::ranges::find(data->colliders, collider) == data->colliders.end())
            {
                data->colliders.push_back(collider);
            }
        }
    }

    void JoltBackend::UnregisterCollider(const PhysicsBodyHandle handle, ICollider *collider)
    {
        if (Impl::BodyData *data = _impl->GetData(handle); data && collider)
        {
            RemoveColliderShapes(handle, collider);
            std::erase(data->colliders, collider);
        }
    }

    void JoltBackend::SetBodyTransform(const PhysicsBodyHandle handle, const Math::Vector3 &position,
                                       const Math::Quaternion &rotation)
    {
        Impl::BodyData *data = _impl->GetData(handle);
        // A kinematic target needs a body in the simulation; a disabled body takes its pose when it is re-enabled
        if (!data || data->motion != MotionKind::Kinematic || !data->enabled || !IsFinite(position))
        {
            return;
        }
        data->hasKinematicTarget = true;
        data->targetPosition = position;
        data->targetRotation = rotation;
    }

    void JoltBackend::SetStaticBodyTransform(const PhysicsBodyHandle handle, const Math::Vector3 &position,
                                             const Math::Quaternion &rotation)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(handle);
        if (!data || data->motion != MotionKind::Static || !IsFinite(position))
        {
            return;
        }
        // Not cheap (the broad phase updates the body's bounds), as in the PhysX backend: moving a static body is a
        // thing to avoid
        s.Bodies().SetPositionAndRotation(data->id, ToJolt(position), ToJolt(rotation), JPH::EActivation::DontActivate);
    }

    bool JoltBackend::GetBodyState(const PhysicsBodyHandle handle, BodyState &out) const
    {
        const Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(handle);
        // A disabled body is out of the simulation: it has no simulation state
        if (!data || !data->enabled)
        {
            return false;
        }

        BodyState state;
        JPH::RVec3 position;
        JPH::Quat rotation;
        s.Bodies().GetPositionAndRotation(data->id, position, rotation);
        state.position = ToEngine(JPH::Vec3(position));
        state.rotation = ToEngine(rotation);
        if (data->motion != MotionKind::Static)
        {
            state.sleeping = !s.Bodies().IsActive(data->id);
            // A kinematic body is moved by its target, not by velocities: it has none to report
            if (data->motion == MotionKind::Dynamic)
            {
                state.linearVelocity = ToEngine(s.Bodies().GetLinearVelocity(data->id));
                state.angularVelocity = ToEngine(s.Bodies().GetAngularVelocity(data->id));
            }
        }
        out = state;
        return true;
    }

    bool JoltBackend::SetBodyState(const PhysicsBodyHandle handle, const BodyState &state)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(handle);
        if (!data || !data->enabled)
        {
            return false;
        }

        const float lengthSquared =
            state.rotation.GetX() * state.rotation.GetX() + state.rotation.GetY() * state.rotation.GetY() +
            state.rotation.GetZ() * state.rotation.GetZ() + state.rotation.GetW() * state.rotation.GetW();
        if (!IsFinite(state.position) || !IsFinite(state.linearVelocity) || !IsFinite(state.angularVelocity) ||
            !std::isfinite(lengthSquared) || lengthSquared < 1e-12f)
        {
            return false;
        }
        const JPH::Quat rotation = ToJolt(state.rotation);

        switch (data->motion)
        {
        case MotionKind::Static:
            s.Bodies().SetPositionAndRotation(data->id, ToJolt(state.position), rotation,
                                              JPH::EActivation::DontActivate);
            return true;
        case MotionKind::Kinematic:
            data->hasKinematicTarget = true;
            data->targetPosition = state.position;
            data->targetRotation = state.rotation;
            return true;
        case MotionKind::Dynamic:
            break;
        }

        // A sleeping body keeps no velocity
        const JPH::Vec3 linear = state.sleeping ? JPH::Vec3::sZero() : ToJolt(state.linearVelocity);
        const JPH::Vec3 angular = state.sleeping ? JPH::Vec3::sZero() : ToJolt(state.angularVelocity);
        s.Bodies().SetPositionRotationAndVelocity(data->id, ToJolt(state.position), rotation, linear, angular);
        if (state.sleeping)
        {
            s.Bodies().DeactivateBody(data->id);
        }
        else
        {
            s.Bodies().ActivateBody(data->id);
        }
        return true;
    }

    // ===== Colliders =====

    void JoltBackend::AddSphereCollider(const PhysicsBodyHandle body, ICollider *collider, const float radius,
                                        const Math::Vector3 &localOffset, const PhysicsMaterial &material)
    {
        ColliderSpec spec;
        spec.collider = collider;
        spec.kind = ColliderKind::Sphere;
        spec.radius = radius;
        spec.offset = localOffset;
        spec.material = material;
        _impl->SetSpec(body, std::move(spec));
    }

    void JoltBackend::AddBoxCollider(const PhysicsBodyHandle body, ICollider *collider,
                                     const Math::Vector3 &halfExtents, const Math::Vector3 &localOffset,
                                     const PhysicsMaterial &material)
    {
        ColliderSpec spec;
        spec.collider = collider;
        spec.kind = ColliderKind::Box;
        spec.halfExtents = halfExtents;
        spec.offset = localOffset;
        spec.material = material;
        _impl->SetSpec(body, std::move(spec));
    }

    void JoltBackend::AddCapsuleCollider(const PhysicsBodyHandle body, ICollider *collider, const float radius,
                                         const float height, const Math::Vector3 &localOffset,
                                         const PhysicsMaterial &material)
    {
        ColliderSpec spec;
        spec.collider = collider;
        spec.kind = ColliderKind::Capsule;
        spec.radius = radius;
        spec.height = height;
        spec.offset = localOffset;
        spec.material = material;
        _impl->SetSpec(body, std::move(spec));
    }

    void JoltBackend::RemoveColliderShapes(const PhysicsBodyHandle body, ICollider *collider)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(body);
        if (!data || !collider)
        {
            return;
        }
        if (std::erase_if(data->specs, [collider](const ColliderSpec &spec) { return spec.collider == collider; }) > 0)
        {
            s.RebuildBody(*data);
        }
    }

    void JoltBackend::UpdateSphereCollider(const PhysicsBodyHandle body, ICollider *collider, const float radius,
                                           const Math::Vector3 &localOffset, const PhysicsMaterial &material)
    {
        ColliderSpec spec;
        spec.collider = collider;
        spec.kind = ColliderKind::Sphere;
        spec.radius = radius;
        spec.offset = localOffset;
        spec.material = material;
        _impl->UpdateSpec(body, spec);
    }

    void JoltBackend::UpdateBoxCollider(const PhysicsBodyHandle body, ICollider *collider,
                                        const Math::Vector3 &halfExtents, const Math::Vector3 &localOffset,
                                        const PhysicsMaterial &material)
    {
        ColliderSpec spec;
        spec.collider = collider;
        spec.kind = ColliderKind::Box;
        spec.halfExtents = halfExtents;
        spec.offset = localOffset;
        spec.material = material;
        _impl->UpdateSpec(body, spec);
    }

    void JoltBackend::UpdateCapsuleCollider(const PhysicsBodyHandle body, ICollider *collider, const float radius,
                                            const float height, const Math::Vector3 &localOffset,
                                            const PhysicsMaterial &material)
    {
        ColliderSpec spec;
        spec.collider = collider;
        spec.kind = ColliderKind::Capsule;
        spec.radius = radius;
        spec.height = height;
        spec.offset = localOffset;
        spec.material = material;
        _impl->UpdateSpec(body, spec);
    }

    void JoltBackend::SetIsTrigger(const PhysicsBodyHandle body, ICollider *collider, const bool isTrigger)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(body);
        if (!data)
        {
            return;
        }
        ColliderSpec *spec = s.FindSpec(*data, collider);
        if (!spec)
        {
            return;
        }
        if (isTrigger)
        {
            s.WarnOnce(kTriggers, "trigger colliders have no effect (their shape is left out of the body)");
        }
        if (spec->isTrigger == isTrigger)
        {
            return;
        }
        spec->isTrigger = isTrigger;
        // A trigger adds nothing to the mass, so toggling one moves the body's centre of mass and inertia too
        s.RebuildBody(*data);
    }

    void JoltBackend::SetColliderLayer(PhysicsBodyHandle, ICollider *, int)
    {
        // The queries read the collider's layer when they run (J5); the simulation has no layers yet
        _impl->WarnOnce(kLayers, "the simulation ignores layers and the collision matrix (every body is static or "
                                 "moving; queries do honour layer masks)");
    }

    void JoltBackend::RefreshCollisionMatrix()
    {
        _impl->WarnOnce(kLayers, "the simulation ignores layers and the collision matrix (every body is static or "
                                 "moving; queries do honour layer masks)");
    }

    // ===== Forces and motion =====

    void JoltBackend::AddForce(const PhysicsBodyHandle body, const Math::Vector3 &force)
    {
        Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(body);
        // Out of the simulation (disabled) a body takes no force, as in the PhysX backend
        if (!data || !data->enabled || data->motion != MotionKind::Dynamic || !IsFinite(force))
        {
            return;
        }
        s.Bodies().AddForce(data->id, ToJolt(force));
    }

    void JoltBackend::AddImpulse(const PhysicsBodyHandle body, const Math::Vector3 &impulse)
    {
        Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(body);
        if (!data || !data->enabled || data->motion != MotionKind::Dynamic || !IsFinite(impulse))
        {
            return;
        }
        s.Bodies().AddImpulse(data->id, ToJolt(impulse));
    }

    void JoltBackend::SetVelocity(const PhysicsBodyHandle body, const Math::Vector3 &velocity)
    {
        Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(body);
        if (!data || data->motion != MotionKind::Dynamic || !IsFinite(velocity))
        {
            return;
        }
        s.Bodies().SetLinearVelocity(data->id, ToJolt(velocity));
    }

    void JoltBackend::SetAngularVelocity(const PhysicsBodyHandle body, const Math::Vector3 &velocity)
    {
        Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(body);
        if (!data || data->motion != MotionKind::Dynamic || !IsFinite(velocity))
        {
            return;
        }
        s.Bodies().SetAngularVelocity(data->id, ToJolt(velocity));
    }

    Math::Vector3 JoltBackend::GetPosition(const PhysicsBodyHandle body)
    {
        Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(body);
        if (!data)
        {
            return Math::Vector3::Zero;
        }
        return ToEngine(JPH::Vec3(s.Bodies().GetPosition(data->id)));
    }

    Math::Quaternion JoltBackend::GetRotation(const PhysicsBodyHandle body)
    {
        Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(body);
        if (!data)
        {
            return Math::Quaternion::Identity;
        }
        return ToEngine(s.Bodies().GetRotation(data->id));
    }

    Math::Vector3 JoltBackend::GetVelocity(const PhysicsBodyHandle body)
    {
        Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(body);
        if (!data || data->motion != MotionKind::Dynamic)
        {
            return Math::Vector3::Zero;
        }
        return ToEngine(s.Bodies().GetLinearVelocity(data->id));
    }

    Math::Vector3 JoltBackend::GetAngularVelocity(const PhysicsBodyHandle body)
    {
        Impl &s = *_impl;
        const Impl::BodyData *data = s.GetData(body);
        if (!data || data->motion != MotionKind::Dynamic)
        {
            return Math::Vector3::Zero;
        }
        return ToEngine(s.Bodies().GetAngularVelocity(data->id));
    }

    // ===== Properties =====

    void JoltBackend::SetMass(const PhysicsBodyHandle body, const float mass)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(body);
        if (!data)
        {
            return;
        }
        data->mass = SanitizedMass(mass);
        s.ApplyMassProperties(*data);
    }

    float JoltBackend::GetMass(const PhysicsBodyHandle body)
    {
        const Impl::BodyData *data = _impl->GetData(body);
        // Dynamic and kinematic bodies have the Rigidbody's mass; a static one has none (the PhysX backend says 0 too)
        if (!data || data->motion == MotionKind::Static)
        {
            return 0.0f;
        }
        return data->mass;
    }

    void JoltBackend::SetGravityEnabled(const PhysicsBodyHandle body, const bool enabled)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(body);
        if (!data || data->motion == MotionKind::Static)
        {
            return;
        }
        data->gravityEnabled = enabled;
        s.Bodies().SetGravityFactor(data->id, enabled ? 1.0f : 0.0f);
    }

    void JoltBackend::SetGravity(const Math::Vector3 &gravity)
    {
        // Deferred to ApplyPendingChanges, as in the PhysX backend
        if (IsFinite(gravity))
        {
            _impl->pendingGravity = gravity;
        }
    }

    Math::Vector3 JoltBackend::GetGravity() const
    {
        return _impl->currentGravity;
    }

    // ===== Queries (J5) =====

    bool JoltBackend::Raycast(const Math::Vector3 &origin, const Math::Vector3 &direction, RaycastHit &hit,
                              const float maxDistance, const std::uint32_t layerMask, const QueryTriggers triggers)
    {
        hit = RaycastHit{};
        Impl &s = *_impl;
        const auto sweep = QueryUtil::ReadSweep(origin, direction, maxDistance);
        if (!s.initialized || !sweep || layerMask == 0)
        {
            return false;
        }
        const std::vector<Impl::RawHit> raws = s.CollectRay(origin, *sweep, layerMask, triggers);
        return s.PickNearest(raws, hit, origin, *sweep, true);
    }

    int JoltBackend::RaycastAll(const Math::Vector3 &origin, const Math::Vector3 &direction,
                                std::vector<RaycastHit> &hits, const float maxDistance, const std::uint32_t layerMask,
                                const QueryTriggers triggers)
    {
        hits.clear();
        Impl &s = *_impl;
        const auto sweep = QueryUtil::ReadSweep(origin, direction, maxDistance);
        if (!s.initialized || !sweep || layerMask == 0)
        {
            return 0;
        }
        std::vector<Impl::RawHit> raws = s.CollectRay(origin, *sweep, layerMask, triggers);
        std::vector<QueryUtil::HitKey> keys;
        keys.reserve(raws.size());
        for (const Impl::RawHit &raw : raws)
        {
            keys.push_back(Impl::KeyOfRaw(raw));
        }
        QueryUtil::SortByKeys(raws, keys, QueryUtil::DistanceThenHandleLess);
        hits.reserve(raws.size());
        for (const Impl::RawHit &raw : raws)
        {
            RaycastHit hit;
            s.FillHit(hit, raw, origin, *sweep, true);
            hits.push_back(hit);
        }
        return static_cast<int>(hits.size());
    }

    bool JoltBackend::SphereCast(const Math::Vector3 &origin, const float radius, const Math::Vector3 &direction,
                                 RaycastHit &hit, const float maxDistance, const std::uint32_t layerMask)
    {
        hit = RaycastHit{};
        Impl &s = *_impl;
        const auto sweep = QueryUtil::ReadSweep(origin, direction, maxDistance, QueryRules::kMaxSweepDistance);
        if (!s.initialized || !sweep || layerMask == 0 || !QueryUtil::IsValidRadius(radius))
        {
            return false;
        }
        const JPH::RefConst<JPH::Shape> shape = new JPH::SphereShape(radius);
        const std::vector<Impl::RawHit> raws =
            s.CollectCast(*shape, origin, JPH::Quat::sIdentity(), *sweep, layerMask, QueryTriggers::Collide);
        return s.PickNearest(raws, hit, origin, *sweep, false);
    }

    bool JoltBackend::CapsuleCast(const Math::Vector3 &point0, const Math::Vector3 &point1, const float radius,
                                  const Math::Vector3 &direction, RaycastHit &hit, const float maxDistance,
                                  const std::uint32_t layerMask, const QueryTriggers triggers)
    {
        hit = RaycastHit{};
        Impl &s = *_impl;
        if (!QueryUtil::IsFinite(point0) || !QueryUtil::IsFinite(point1) || !QueryUtil::IsValidRadius(radius))
        {
            return false;
        }
        const QueryUtil::CapsuleFrame frame = QueryUtil::CapsuleFrameOf(point0, point1);
        const auto sweep = QueryUtil::ReadSweep(frame.center, direction, maxDistance, QueryRules::kMaxSweepDistance);
        if (!s.initialized || !sweep || layerMask == 0)
        {
            return false;
        }
        JPH::Quat rotation = JPH::Quat::sIdentity();
        const JPH::RefConst<JPH::Shape> shape = Impl::MakeCapsuleQueryShape(frame, radius, rotation);
        const std::vector<Impl::RawHit> raws =
            s.CollectCast(*shape, frame.center, rotation, *sweep, layerMask, triggers);
        return s.PickNearest(raws, hit, frame.center, *sweep, false);
    }

    int JoltBackend::Overlap(const OverlapShape &query, std::vector<OverlapHit> &hits, const std::uint32_t layerMask,
                             const QueryTriggers triggers)
    {
        hits.clear();
        Impl &s = *_impl;
        if (!s.initialized || layerMask == 0 || !QueryUtil::IsValidOverlapShape(query))
        {
            return 0;
        }
        JPH::Quat rotation = JPH::Quat::sIdentity();
        Math::Vector3 position = query.center;
        JPH::RefConst<JPH::Shape> shape;
        switch (query.kind)
        {
        case OverlapShape::Kind::Sphere:
            shape = new JPH::SphereShape(query.radius);
            break;
        case OverlapShape::Kind::Box:
            // No convex radius: the query box is exactly the size asked for
            shape = new JPH::BoxShape(ToJolt(query.halfExtents), 0.0f);
            rotation = ToJolt(query.rotation);
            break;
        case OverlapShape::Kind::Capsule:
        {
            const QueryUtil::CapsuleFrame frame = QueryUtil::CapsuleFrameOf(query.point0, query.point1);
            position = frame.center;
            shape = Impl::MakeCapsuleQueryShape(frame, query.radius, rotation);
            break;
        }
        }
        return s.CollectOverlap(*shape, position, rotation, layerMask, triggers, hits);
    }

    // ===== Snapshot (J6) =====

    bool JoltBackend::SaveState(std::vector<std::byte> &) const
    {
        _impl->WarnOnce(kSnapshot, "SaveState, RestoreState and HashState are unsupported");
        return false;
    }

    bool JoltBackend::RestoreState(std::span<const std::byte>)
    {
        _impl->WarnOnce(kSnapshot, "SaveState, RestoreState and HashState are unsupported");
        return false;
    }

    std::optional<std::uint64_t> JoltBackend::HashState() const
    {
        _impl->WarnOnce(kSnapshot, "SaveState, RestoreState and HashState are unsupported");
        return std::nullopt;
    }

    // ===== Diagnostics and tests =====

    Math::Vector3 JoltBackend::GetInertiaTensor(const PhysicsBodyHandle body) const
    {
        const Impl::BodyData *data = _impl->GetData(body);
        return data && data->motion != MotionKind::Static ? data->inertia : Math::Vector3::Zero;
    }

    Math::Vector3 JoltBackend::GetCenterOfMass(const PhysicsBodyHandle body) const
    {
        const Impl::BodyData *data = _impl->GetData(body);
        return data && data->motion != MotionKind::Static ? data->centerOfMass : Math::Vector3::Zero;
    }

    std::size_t JoltBackend::GetShapeCount(const PhysicsBodyHandle body) const
    {
        const Impl::BodyData *data = _impl->GetData(body);
        return data ? data->shapeCount : 0;
    }

    std::uint32_t JoltBackend::GetJoltBodyIndex(const PhysicsBodyHandle body) const
    {
        const Impl::BodyData *data = _impl->GetData(body);
        return data ? data->id.GetIndex() : UINT32_MAX;
    }

    std::uint32_t JoltBackend::GetJoltBodyId(const PhysicsBodyHandle body) const
    {
        const Impl::BodyData *data = _impl->GetData(body);
        return data ? data->id.GetIndexAndSequenceNumber() : UINT32_MAX;
    }

    bool JoltBackend::IsBodyEnabled(const PhysicsBodyHandle body) const
    {
        const Impl::BodyData *data = _impl->GetData(body);
        return data && data->enabled;
    }

    bool JoltBackend::UsesSingleThreadedJobSystem() const
    {
        return _impl->initialized && _impl->singleThreaded;
    }

    unsigned JoltBackend::GetNotImplementedWarningCount() const
    {
        return _impl->warnCount;
    }

    int JoltBackend::GlobalInitCount()
    {
        const std::lock_guard lock(GlobalsMutex());
        return g_globalRefs;
    }
}

#endif // N2ENGINE_JOLT_ENABLED

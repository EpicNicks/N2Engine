// The Jolt backend (stages J3, the core, and J4, layers, triggers and events). Jolt's headers are included here and nowhere else in the engine.
// Everything that touches Jolt state lives in JoltBackend::Impl so the public header holds no Jolt type.

#include "engine/physics/jolt/JoltBackend.hpp"

#ifdef N2ENGINE_JOLT_ENABLED

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/SimShapeFilter.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#ifndef N2ENGINE_SINGLE_THREADED
#include <Jolt/Core/JobSystemThreadPool.h>
#endif

#include "engine/GameObject.hpp"
#include "engine/Layers.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/ContactTracker.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/physics/Rigidbody.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
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

        // ===== Layers =====
        // ObjectLayer = (class << 1) | moving. The class is the engine layer (0..31) when every collider of the body is
        // on it, else kMixedClass (colliders on different layers, or none yet). Bit 0 is 1 for a dynamic or kinematic
        // body and 0 for a static one. There are two broad-phase layers, static and moving. The object layer only
        // prunes pairs early; the exact decision is per collider pair (PairShapeFilter), which also covers a body
        // whose colliders sit on different layers.
        namespace ObjLayers
        {
            constexpr JPH::uint kMixedClass = static_cast<JPH::uint>(Layers::Count);

            constexpr JPH::ObjectLayer Make(const JPH::uint layerClass, const bool moving)
            {
                return static_cast<JPH::ObjectLayer>((layerClass << 1) | (moving ? 1u : 0u));
            }

            constexpr JPH::uint ClassOf(const JPH::ObjectLayer layer)
            {
                return static_cast<JPH::uint>(layer) >> 1;
            }

            constexpr bool IsMoving(const JPH::ObjectLayer layer)
            {
                return (static_cast<JPH::uint>(layer) & 1u) != 0;
            }
        }

        namespace BpLayers
        {
            constexpr JPH::BroadPhaseLayer NonMoving(0);
            constexpr JPH::BroadPhaseLayer Moving(1);
            constexpr JPH::uint Count = 2;
        }

        // The collision matrix as the backend last saw it. The engine's Layers registry is main-thread state and
        // Jolt's filters run on worker threads during Update, so they read this copy, which RefreshCollisionMatrix
        // (called between steps) replaces. "A matrix change swaps the filter" is this copy being swapped.
        struct LayerMatrix
        {
            std::array<std::uint32_t, static_cast<std::size_t>(Layers::Count)> rows{};

            LayerMatrix() { rows.fill(Layers::AllLayers); }

            void Load()
            {
                for (int layer = 0; layer < Layers::Count; ++layer)
                {
                    rows[static_cast<std::size_t>(layer)] = Layers::CollisionMaskFor(layer);
                }
            }

            // Each side has to accept the other's layer (the PhysX backend's rule)
            [[nodiscard]] bool Collide(const int a, const int b) const
            {
                const auto ia = static_cast<std::size_t>(std::clamp(a, 0, Layers::Count - 1));
                const auto ib = static_cast<std::size_t>(std::clamp(b, 0, Layers::Count - 1));
                return ((rows[ia] >> ib) & 1u) != 0 && ((rows[ib] >> ia) & 1u) != 0;
            }
        };

        // Static bodies never collide with each other; a body of the mixed class passes every pair (the collider pair
        // filter decides); otherwise the engine's matrix decides
        class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
        {
        public:
            explicit ObjectPairFilter(const LayerMatrix &matrix) : _matrix(matrix) {}

            bool ShouldCollide(const JPH::ObjectLayer a, const JPH::ObjectLayer b) const override
            {
                if (!ObjLayers::IsMoving(a) && !ObjLayers::IsMoving(b))
                {
                    return false;
                }
                const JPH::uint classA = ObjLayers::ClassOf(a);
                const JPH::uint classB = ObjLayers::ClassOf(b);
                if (classA == ObjLayers::kMixedClass || classB == ObjLayers::kMixedClass)
                {
                    return true;
                }
                return _matrix.Collide(static_cast<int>(classA), static_cast<int>(classB));
            }

        private:
            const LayerMatrix &_matrix;
        };

        class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface
        {
        public:
            JPH::uint GetNumBroadPhaseLayers() const override { return BpLayers::Count; }

            JPH::BroadPhaseLayer GetBroadPhaseLayer(const JPH::ObjectLayer layer) const override
            {
                return ObjLayers::IsMoving(layer) ? BpLayers::Moving : BpLayers::NonMoving;
            }
        };

        class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide(const JPH::ObjectLayer layer, const JPH::BroadPhaseLayer broadPhaseLayer) const override
            {
                if (!ObjLayers::IsMoving(layer))
                {
                    return broadPhaseLayer == BpLayers::Moving;
                }
                return true;
            }
        };

        // PhysX's bounce threshold (PxSceneDesc bounceThresholdVelocity: 0.2 * the tolerance scale's speed, which is 10)
        // is 2 m/s; Jolt's default is 1 m/s
        constexpr float kMinVelocityForRestitution = 2.0f;

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
        // changes; the index of a spec in the list is the user data of its shape (what the contact listener and the
        // shape filter map a sub-shape back to), so it is the position among ALL the body's colliders, trigger ones
        // included.
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
            int layer = Layers::Default; // the layer of the collider's GameObject
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
            kQueries = 1u << 0,
            kSnapshot = 1u << 1
        };

        // What the contact listener buffers: one report of Jolt's per contact (body and sub-shape pair), with the data
        // the main thread cannot get after the step. Listener callbacks run on worker threads in no fixed order; the
        // main thread sorts them (Impl::FoldContacts).
        struct RawContact
        {
            enum class Kind : std::uint8_t
            {
                Added,
                Persisted,
                Removed
            };

            Kind kind = Kind::Added;
            std::uint32_t bodyId1 = 0; // BodyID::GetIndexAndSequenceNumber, bodies as Jolt ordered them
            std::uint32_t bodyId2 = 0;
            std::uint32_t sub1 = 0; // SubShapeID::GetValue
            std::uint32_t sub2 = 0;
            std::uint32_t collider1 = 0; // index into the body's specs (Added, Persisted)
            std::uint32_t collider2 = 0;
            bool trigger = false;     // one of the two sub-shapes is a trigger collider
            bool overlapping = false; // trigger pairs: the shapes really overlap (not only inside the speculative distance)
            // Solid contacts that were Added: from body 1's point of view, as the engine's Collision describes them
            std::vector<ContactPoint> contacts;
            Math::Vector3 relativeVelocity = Math::Vector3::Zero;
            Math::Vector3 impulse = Math::Vector3::Zero;
        };

        // A contact the main thread knows about, keyed by (lower handle, higher handle, lower sub-shape, higher
        // sub-shape): a total order that is the same on every run
        using ContactKey = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t>;

        std::uint64_t HandleOrder(const PhysicsBodyHandle handle)
        {
            return (static_cast<std::uint64_t>(handle.index) << 32) | handle.generation;
        }
    }

    struct JoltBackend::Impl
    {
        using Tracker = ContactTracker<const ICollider *>;

        struct BodyData
        {
            bool active = false;
            std::uint32_t generation = 0;
            JPH::BodyID id;
            MotionKind motion = MotionKind::Static;
            bool enabled = true;    // in the simulation (added to Jolt's broad phase)
            bool wasActive = false; // awake when the last Update began (CaptureActivity)
            JPH::ObjectLayer objectLayer = ObjLayers::Make(ObjLayers::kMixedClass, false);

            Rigidbody *rigidbody = nullptr;
            std::vector<ICollider *> colliders;
            std::vector<ColliderSpec> specs;
            JPH::RefConst<JPH::Shape> shape;      // the body's shape: every collider, trigger ones included
            JPH::RefConst<JPH::Shape> solidShape; // the solid colliders only: what the mass properties come from
            std::size_t shapeCount = 0;           // non-trigger colliders in `shape`

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

        // A contact the main thread tracks (see FoldContacts)
        struct ContactInfo
        {
            PhysicsBodyHandle low;  // the body with the lower handle: side A of a collision pair
            PhysicsBodyHandle high;
            const ICollider *colliderLow = nullptr;
            const ICollider *colliderHigh = nullptr;
            bool trigger = false;       // one of the two colliders is a trigger
            bool triggerOnHigh = false; // trigger pairs: the trigger collider is the high body's
            bool reported = false;      // the tracker holds this collider pair as touching
            bool dormant = false;       // Jolt dropped the contact because a body fell asleep; the pair still touches
            bool seen = false;          // Jolt reported the contact in this step
        };

        // The contact listener. Jolt calls it on worker threads, with all bodies locked: it only reads the bodies and
        // this backend's (unchanging during Update) collider specs, adjusts the contact's settings, and buffers what the
        // main thread needs. Nothing it reads changes while Update runs, and everything it buffers is sorted before use.
        class ContactBridge final : public JPH::ContactListener
        {
        public:
            explicit ContactBridge(Impl &impl) : _impl(impl) {}

            void OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                JPH::ContactSettings &settings) override
            {
                Handle(body1, body2, manifold, settings, RawContact::Kind::Added);
            }

            void OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2,
                                    const JPH::ContactManifold &manifold, JPH::ContactSettings &settings) override
            {
                Handle(body1, body2, manifold, settings, RawContact::Kind::Persisted);
            }

            void OnContactRemoved(const JPH::SubShapeIDPair &pair) override
            {
                RawContact raw;
                raw.kind = RawContact::Kind::Removed;
                raw.bodyId1 = pair.GetBody1ID().GetIndexAndSequenceNumber();
                raw.bodyId2 = pair.GetBody2ID().GetIndexAndSequenceNumber();
                raw.sub1 = pair.GetSubShapeID1().GetValue();
                raw.sub2 = pair.GetSubShapeID2().GetValue();
                const std::lock_guard lock(_impl.contactMutex);
                _impl.rawContacts.push_back(std::move(raw));
            }

        private:
            void Handle(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                        JPH::ContactSettings &settings, const RawContact::Kind kind) const
            {
                const std::uint64_t index1 = body1.GetShape()->GetSubShapeUserData(manifold.mSubShapeID1);
                const std::uint64_t index2 = body2.GetShape()->GetSubShapeUserData(manifold.mSubShapeID2);
                const ColliderSpec *collider1 = _impl.SpecFor(body1, index1);
                const ColliderSpec *collider2 = _impl.SpecFor(body2, index2);
                if (!collider1 || !collider2)
                {
                    return; // not a collider of this backend's (every shape is built from the specs, so not reachable)
                }

                if (!_impl.matrix.Collide(collider1->layer, collider2->layer))
                {
                    // Jolt keeps a resting pair's contacts in a cache and does not ask the shape filter about them again.
                    // A pair the matrix now rules out can get here that way (the object layers of two bodies on one layer
                    // each are filtered earlier, when Jolt looks for pairs): no collision response, nothing reported. The
                    // backend has ended its touch when the matrix changed.
                    settings.mIsSensor = true;
                    return;
                }

                const bool trigger = collider1->isTrigger || collider2->isTrigger;
                if (trigger)
                {
                    // No collision response, but the contact is still reported: this is what makes a trigger collider a
                    // sensor while its body keeps its solid colliders (docs/physics.html, "Triggers")
                    settings.mIsSensor = true;
                }
                else
                {
                    // Per collider, not per body: the average of the two colliders' materials (the PhysX default)
                    settings.mCombinedFriction = CombineAverage(std::max(0.0f, collider1->material.dynamicFriction),
                                                                std::max(0.0f, collider2->material.dynamicFriction));
                    settings.mCombinedRestitution = CombineAverage(std::max(0.0f, collider1->material.restitution),
                                                                   std::max(0.0f, collider2->material.restitution));
                }
                if (kind == RawContact::Kind::Persisted && !trigger)
                {
                    return; // a solid contact that carries on is not news
                }

                RawContact raw;
                raw.kind = kind;
                raw.bodyId1 = body1.GetID().GetIndexAndSequenceNumber();
                raw.bodyId2 = body2.GetID().GetIndexAndSequenceNumber();
                raw.sub1 = manifold.mSubShapeID1.GetValue();
                raw.sub2 = manifold.mSubShapeID2.GetValue();
                raw.collider1 = static_cast<std::uint32_t>(index1);
                raw.collider2 = static_cast<std::uint32_t>(index2);
                raw.trigger = trigger;
                // A trigger overlaps when the shapes penetrate; a contact inside the speculative distance (negative depth)
                // is only close. (PhysX reports a trigger on real overlap only.)
                raw.overlapping = manifold.mPenetrationDepth >= 0.0f;

                if (kind == RawContact::Kind::Added && !trigger)
                {
                    // What Jolt allows for the impulse (Q3): the solver has not run yet, so the impulse is Jolt's own
                    // estimate for this pair from the velocities before the collision. Point contacts, separation, normal
                    // and relative velocity are exact; ContactPoint::normalImpulse is the estimate; the friction
                    // impulses (tangentImpulse) are not filled in.
                    JPH::CollisionEstimationResult estimate;
                    JPH::EstimateCollisionResponse(body1, body2, manifold, estimate, settings.mCombinedFriction,
                                                   settings.mCombinedRestitution, kMinVelocityForRestitution);

                    // The engine's normal points from the other body to this one; Jolt's from body 1 to body 2
                    const Math::Vector3 normal = ToEngine(-manifold.mWorldSpaceNormal);
                    const JPH::uint count = manifold.mRelativeContactPointsOn1.size();
                    raw.contacts.reserve(count);
                    Math::Vector3 total = Math::Vector3::Zero;
                    for (JPH::uint i = 0; i < count; ++i)
                    {
                        const JPH::Vec3 on1(manifold.GetWorldSpaceContactPointOn1(i));
                        const JPH::Vec3 on2(manifold.GetWorldSpaceContactPointOn2(i));
                        ContactPoint contact;
                        contact.point = ToEngine(0.5f * (on1 + on2));
                        contact.normal = normal;
                        contact.separation = -manifold.mPenetrationDepth;
                        contact.normalImpulse = i < estimate.mContactImpulse.size() ? estimate.mContactImpulse[i] : 0.0f;
                        total = total + normal * contact.normalImpulse;
                        raw.contacts.push_back(contact);
                    }
                    raw.impulse = total;
                    const JPH::Vec3 velocity1 = body1.IsDynamic() ? body1.GetLinearVelocity() : JPH::Vec3::sZero();
                    const JPH::Vec3 velocity2 = body2.IsDynamic() ? body2.GetLinearVelocity() : JPH::Vec3::sZero();
                    raw.relativeVelocity = ToEngine(velocity1 - velocity2);
                }

                const std::lock_guard lock(_impl.contactMutex);
                _impl.rawContacts.push_back(std::move(raw));
            }

            Impl &_impl;
        };

        // Decides per pair of leaf shapes (colliders) whether they collide: the layer matrix, and no trigger against
        // trigger (PhysX never pairs two trigger shapes either). Called on worker threads: reads only.
        class PairShapeFilter final : public JPH::SimShapeFilter
        {
        public:
            explicit PairShapeFilter(const Impl &impl) : _impl(impl) {}

            bool ShouldCollide(const JPH::Body &body1, const JPH::Shape *shape1, const JPH::SubShapeID &,
                               const JPH::Body &body2, const JPH::Shape *shape2, const JPH::SubShapeID &) const override
            {
                // Called at every level of the shapes; only the leaves (the colliders) decide
                if (shape1->GetType() != JPH::EShapeType::Convex || shape2->GetType() != JPH::EShapeType::Convex)
                {
                    return true;
                }
                const ColliderSpec *collider1 = _impl.SpecFor(body1, shape1->GetUserData());
                const ColliderSpec *collider2 = _impl.SpecFor(body2, shape2->GetUserData());
                if (!collider1 || !collider2)
                {
                    return true;
                }
                if (collider1->isTrigger && collider2->isTrigger)
                {
                    return false; // triggers never see each other
                }
                if (!_impl.matrix.Collide(collider1->layer, collider2->layer))
                {
                    return false;
                }
                return true;
            }

        private:
            const Impl &_impl;
        };

        // The layer objects and the contact listener are referenced by the physics system for its whole life:
        // declared first, destroyed last
        LayerMatrix matrix;
        BroadPhaseLayerMap broadPhaseLayers;
        ObjectVsBroadPhaseFilter objectVsBroadPhase;
        ObjectPairFilter objectPairs{matrix};
        ContactBridge contactBridge{*this};
        PairShapeFilter pairShapeFilter{*this};
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

        // Events: the listener buffers under the mutex, FoldContacts feeds the tracker after Update
        std::mutex contactMutex;
        std::vector<RawContact> rawContacts;
        Tracker tracker;
        std::map<ContactKey, ContactInfo> contacts;
        std::function<void(const GameObject &)> refilterObserver;

        unsigned warned = 0;
        unsigned warnCount = 0;
        bool updateErrorLogged = false;

        Impl()
        {
            // The tracker finds a body's Rigidbody and colliders through this backend; a shape key is the collider
            tracker.SetHost(
                [this](const PhysicsBodyHandle handle, Tracker::BodyView &view)
                {
                    const BodyData *data = GetData(handle);
                    if (!data)
                    {
                        return false;
                    }
                    view.rigidbody = data->rigidbody;
                    view.colliders = &data->colliders;
                    return true;
                },
                [](const ICollider *collider) { return const_cast<ICollider *>(collider); });
        }

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
            data.solidShape = nullptr;
            data.shapeCount = 0;
            data.hasKinematicTarget = false;
            data.kinematicMoving = false;
            data.wasActive = false;
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

        // The handle of the live body that has this Jolt id (false for a stale id)
        bool HandleOfId(const std::uint32_t rawId, PhysicsBodyHandle &out) const
        {
            const JPH::BodyID id(rawId);
            const std::uint32_t index = id.GetIndex();
            if (index >= bodies.size())
            {
                return false;
            }
            const BodyData &data = bodies[index];
            if (!data.active || data.id != id)
            {
                return false;
            }
            out.index = index;
            out.generation = data.generation;
            return true;
        }

        // The spec of the collider a sub-shape's user data names, on the body Jolt hands the listener or the filter
        const ColliderSpec *SpecFor(const JPH::Body &body, const std::uint64_t colliderIndex) const
        {
            const std::uint32_t index = body.GetID().GetIndex();
            if (index >= bodies.size())
            {
                return nullptr;
            }
            const BodyData &data = bodies[index];
            if (!data.active || colliderIndex >= data.specs.size())
            {
                return nullptr;
            }
            return &data.specs[static_cast<std::size_t>(colliderIndex)];
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
            data.objectLayer = ObjLayers::Make(ObjLayers::kMixedClass, motion != MotionKind::Static);

            const JPH::EMotionType type = motion == MotionKind::Static      ? JPH::EMotionType::Static
                                          : motion == MotionKind::Kinematic ? JPH::EMotionType::Kinematic
                                                                            : JPH::EMotionType::Dynamic;
            const JPH::Ref<JPH::Shape> empty = new JPH::EmptyShape();
            JPH::BodyCreationSettings settings(empty.GetPtr(), ToJolt(position), ToJolt(rotation), type,
                                               data.objectLayer);
            const PhysicsMaterial defaults = PhysicsMaterial::Default();
            settings.mFriction = defaults.dynamicFriction;
            settings.mRestitution = defaults.restitution;
            settings.mAllowSleeping = true;
            // mCollideKinematicVsNonDynamic stays off: a kinematic body is not paired with a static or another
            // kinematic one, which is PhysX's default (no events between them, triggers included)
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

        // The object layer the body's colliders call for: their layer when they all share one, else the mixed class
        void UpdateObjectLayer(BodyData &data)
        {
            JPH::uint layerClass = ObjLayers::kMixedClass;
            if (!data.specs.empty() &&
                std::ranges::all_of(data.specs, [&data](const ColliderSpec &spec) { return spec.layer == data.specs.front().layer; }))
            {
                layerClass = static_cast<JPH::uint>(std::clamp(data.specs.front().layer, 0, Layers::Count - 1));
            }
            const JPH::ObjectLayer wanted = ObjLayers::Make(layerClass, data.motion != MotionKind::Static);
            if (wanted != data.objectLayer)
            {
                Bodies().SetObjectLayer(data.id, wanted);
                data.objectLayer = wanted;
            }
        }

        // Rebuilds the body's Jolt shape from its colliders: all of them, each at its offset, as one compound (a single
        // collider at the origin is the plain shape itself: StaticCompoundShapeSettings::Create returns it). A trigger
        // collider is a sub-shape like the others; what makes it a trigger is the contact listener (a sensor contact)
        // and the shape filter. Its mass does not count: the mass properties come from the solid colliders alone, and
        // when there are triggers the centre of mass of the body's shape is moved to the solid colliders' (an
        // OffsetCenterOfMassShape around the compound). Then the material and the object layer of the body.
        void RebuildBody(BodyData &data)
        {
            JPH::StaticCompoundShapeSettings solidCompound;
            JPH::StaticCompoundShapeSettings fullCompound;
            std::size_t solidCount = 0;
            std::size_t triggerCount = 0;
            float friction = 0.0f;
            float restitution = 0.0f;
            for (std::size_t i = 0; i < data.specs.size(); ++i)
            {
                const ColliderSpec &spec = data.specs[i];
                const JPH::Ref<JPH::Shape> leaf = MakeLeaf(spec, static_cast<std::uint32_t>(i));
                fullCompound.AddShape(ToJolt(spec.offset), JPH::Quat::sIdentity(), leaf.GetPtr(),
                                      static_cast<std::uint32_t>(i));
                if (spec.isTrigger)
                {
                    ++triggerCount;
                    continue;
                }
                solidCompound.AddShape(ToJolt(spec.offset), JPH::Quat::sIdentity(), leaf.GetPtr(),
                                       static_cast<std::uint32_t>(i));
                friction += spec.material.dynamicFriction;
                restitution += spec.material.restitution;
                ++solidCount;
            }

            JPH::RefConst<JPH::Shape> solid;
            if (solidCount > 0)
            {
                const JPH::ShapeSettings::ShapeResult result = solidCompound.Create();
                if (result.HasError())
                {
                    Logger::Error(std::format("Jolt backend: could not build a body's shape: {}",
                                              std::string(result.GetError().c_str())));
                    return;
                }
                solid = result.Get();
            }

            JPH::RefConst<JPH::Shape> shape;
            if (triggerCount > 0)
            {
                const JPH::ShapeSettings::ShapeResult result = fullCompound.Create();
                if (result.HasError())
                {
                    Logger::Error(std::format("Jolt backend: could not build a body's shape: {}",
                                              std::string(result.GetError().c_str())));
                    return;
                }
                const JPH::RefConst<JPH::Shape> full = result.Get();
                const JPH::Vec3 wantedCenter = solid ? solid->GetCenterOfMass() : JPH::Vec3::sZero();
                shape = new JPH::OffsetCenterOfMassShape(full.GetPtr(), wantedCenter - full->GetCenterOfMass());
            }
            else if (solid)
            {
                shape = solid;
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
            data.solidShape = solid;
            data.shapeCount = solidCount;

            // Manifold reduction merges the contacts of a body pair that face the same way into one manifold and keeps
            // the sub-shapes of one of them: a trigger sub-shape's contact would be merged into a solid one's and lose
            // its identity. A body with a trigger collider has it off (which switches it off for its pairs).
            Bodies().SetUseManifoldReduction(data.id, triggerCount == 0);

            // One friction and one restitution per body in Jolt (docs/physics.html): the average of the solid colliders'
            // values (dynamic friction; Jolt has no static friction), the default material when there is none. The
            // contact listener overrides the combined values per collider pair, so these only matter for a contact it
            // does not see.
            const PhysicsMaterial defaults = PhysicsMaterial::Default();
            const float count = static_cast<float>(solidCount);
            Bodies().SetFriction(data.id, solidCount > 0 ? std::max(0.0f, friction / count) : defaults.dynamicFriction);
            Bodies().SetRestitution(data.id,
                                    solidCount > 0 ? std::max(0.0f, restitution / count) : defaults.restitution);

            UpdateObjectLayer(data);
            ApplyMassProperties(data);
        }

        // The Rigidbody's mass is authoritative; the solid colliders, at uniform density, give the inertia tensor and the
        // centre of mass (the same computation as EOverrideMassProperties::CalculateInertia, applied to the live body
        // because the shape changes after the body exists). A body with no solid collider gets ShapelessMassProperties.
        void ApplyMassProperties(BodyData &data)
        {
            if (data.motion == MotionKind::Static)
            {
                return;
            }
            JPH::MassProperties properties;
            if (data.shapeCount > 0 && data.solidShape)
            {
                properties = data.solidShape->GetMassProperties();
                properties.ScaleToMass(data.mass);
                const JPH::Vec3 center = data.solidShape->GetCenterOfMass();
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

        static int LayerOfObject(const ICollider *collider)
        {
            return std::clamp(collider->GetGameObject().GetLayer(), 0, Layers::Count - 1);
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
                spec.layer = existing->layer;
                *existing = std::move(spec);
            }
            else
            {
                // A new collider is on the layer of its object, as the PhysX backend gives its new shapes
                spec.layer = spec.collider ? LayerOfObject(spec.collider) : Layers::Default;
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
            const int layer = existing->layer;
            *existing = changed;
            existing->isTrigger = isTrigger;
            existing->layer = layer;
            RebuildBody(*data);
        }

        // ===== Contacts and events =====

        // The collider of a body's spec index (null when the index is stale or the spec has no collider object)
        const ICollider *ColliderAt(const PhysicsBodyHandle handle, const std::uint32_t index) const
        {
            const BodyData *data = GetData(handle);
            return data && index < data->specs.size() ? data->specs[index].collider : nullptr;
        }

        const ColliderSpec *SpecOfCollider(const PhysicsBodyHandle handle, const ICollider *collider) const
        {
            const BodyData *data = GetData(handle);
            if (!data || !collider)
            {
                return nullptr;
            }
            const auto it = std::ranges::find_if(data->specs, [collider](const ColliderSpec &s)
                                                 { return s.collider == collider; });
            return it == data->specs.end() ? nullptr : &*it;
        }

        // Whether the matrix lets two colliders touch (true when either is unknown: nothing to end)
        bool CollidersCollide(const PhysicsBodyHandle handleA, const ICollider *colliderA,
                              const PhysicsBodyHandle handleB, const ICollider *colliderB) const
        {
            const ColliderSpec *a = SpecOfCollider(handleA, colliderA);
            const ColliderSpec *b = SpecOfCollider(handleB, colliderB);
            return !a || !b || matrix.Collide(a->layer, b->layer);
        }

        // The pair the tracker is told about: a collision pair is (lower handle, higher handle); a trigger pair starts
        // with the body that holds the trigger collider (as the PhysX backend reports it)
        struct Oriented
        {
            CollisionPair pair;
            const ICollider *onA = nullptr;
            const ICollider *onB = nullptr;
            bool lowIsA = true;
        };

        static Oriented Orient(const ContactInfo &info)
        {
            Oriented oriented;
            oriented.lowIsA = !(info.trigger && info.triggerOnHigh);
            oriented.pair = oriented.lowIsA ? CollisionPair{info.low, info.high} : CollisionPair{info.high, info.low};
            oriented.onA = oriented.lowIsA ? info.colliderLow : info.colliderHigh;
            oriented.onB = oriented.lowIsA ? info.colliderHigh : info.colliderLow;
            return oriented;
        }

        // The tracker's Enter: `raw` (may be null) holds the contact data from body 1's point of view, and body 1 is the
        // info's low body when raw1IsLow
        void ReportFound(const ContactInfo &info, const RawContact *raw, const bool raw1IsLow)
        {
            const Oriented oriented = Orient(info);
            if (info.trigger)
            {
                tracker.TriggerFound(oriented.pair, oriented.onA, oriented.onB);
                return;
            }
            auto *data = new Collision();
            data->collider = const_cast<ICollider *>(oriented.onA);
            data->otherCollider = const_cast<ICollider *>(oriented.onB);
            if (raw)
            {
                data->contacts = raw->contacts;
                data->impulse = raw->impulse;
                data->relativeVelocity = raw->relativeVelocity;
                // The data is from body 1's point of view and the pair's side A is the low body
                if (raw1IsLow != oriented.lowIsA)
                {
                    for (ContactPoint &contact : data->contacts)
                    {
                        contact.normal = contact.normal * -1.0f;
                    }
                    data->impulse = data->impulse * -1.0f;
                    data->relativeVelocity = data->relativeVelocity * -1.0f;
                }
            }
            tracker.ContactFound(oriented.pair, oriented.onA, oriented.onB, data);
        }

        void ReportLost(const ContactInfo &info)
        {
            const Oriented oriented = Orient(info);
            if (info.trigger)
            {
                tracker.TriggerLost(oriented.pair, oriented.onA, oriented.onB);
                return;
            }
            auto *data = new Collision();
            data->collider = const_cast<ICollider *>(oriented.onA);
            data->otherCollider = const_cast<ICollider *>(oriented.onB);
            tracker.ContactLost(oriented.pair, oriented.onA, oriented.onB, data);
        }

        // Which bodies are awake as a step begins: a contact Jolt reports removed because a body fell asleep is told
        // from a contact that really ended by that (a body that was awake and no longer is)
        void CaptureActivity()
        {
            const bool any = !contacts.empty();
            for (BodyData &data : bodies)
            {
                data.wasActive = any && data.active && data.enabled && data.motion != MotionKind::Static &&
                                 Bodies().IsActive(data.id);
            }
        }

        bool FellAsleep(const PhysicsBodyHandle handle) const
        {
            const BodyData *data = GetData(handle);
            return data && data->enabled && data->motion != MotionKind::Static && data->wasActive &&
                   !Bodies().IsActive(data->id);
        }

        // Whether Jolt looked at every contact of the body in the step that just ran: a static body has none of its own to
        // look at, any other must have been awake as the step began (Jolt only finds the contacts of an awake body)
        bool WasSearched(const PhysicsBodyHandle handle) const
        {
            const BodyData *data = GetData(handle);
            return !data || data->motion == MotionKind::Static || data->wasActive;
        }

        // After Update: sorts what the listener buffered into a total order, and turns it into found and lost touches
        // for the tracker. Jolt's callbacks come from worker threads in no fixed order, and a Removed that goes with an
        // Added for the same body pair must not become an Exit then an Enter: so the reports are grouped per contact
        // (body pair and sub-shape pair), taken in key order, and every touch that starts is reported before any
        // touch that ends.
        void FoldContacts()
        {
            std::vector<RawContact> raw;
            {
                const std::lock_guard lock(contactMutex);
                raw.swap(rawContacts);
            }

            struct Event
            {
                ContactKey key;
                RawContact::Kind kind = RawContact::Kind::Added;
                PhysicsBodyHandle low;
                PhysicsBodyHandle high;
                std::uint32_t colliderLow = 0;
                std::uint32_t colliderHigh = 0;
                bool raw1IsLow = true;
                const RawContact *raw = nullptr;
            };

            std::vector<Event> events;
            events.reserve(raw.size());
            for (const RawContact &report : raw)
            {
                PhysicsBodyHandle handle1;
                PhysicsBodyHandle handle2;
                if (!HandleOfId(report.bodyId1, handle1) || !HandleOfId(report.bodyId2, handle2))
                {
                    continue; // a body that is gone
                }
                Event event;
                event.kind = report.kind;
                event.raw = &report;
                event.raw1IsLow = HandleOrder(handle1) < HandleOrder(handle2);
                event.low = event.raw1IsLow ? handle1 : handle2;
                event.high = event.raw1IsLow ? handle2 : handle1;
                event.colliderLow = event.raw1IsLow ? report.collider1 : report.collider2;
                event.colliderHigh = event.raw1IsLow ? report.collider2 : report.collider1;
                event.key = ContactKey{HandleOrder(event.low), HandleOrder(event.high),
                                       event.raw1IsLow ? report.sub1 : report.sub2,
                                       event.raw1IsLow ? report.sub2 : report.sub1};
                events.push_back(event);
            }
            std::ranges::sort(events, [](const Event &a, const Event &b)
                              { return a.key != b.key ? a.key < b.key : a.kind < b.kind; });

            std::vector<ContactKey> ended;  // a Removed with nothing else for the contact
            std::vector<ContactKey> parted; // a trigger contact whose shapes no longer overlap
            for (std::size_t first = 0; first < events.size();)
            {
                std::size_t last = first;
                const Event *present = nullptr; // the Added, else the Persisted, of the group
                while (last < events.size() && events[last].key == events[first].key)
                {
                    if (!present && events[last].kind != RawContact::Kind::Removed)
                    {
                        present = &events[last];
                    }
                    ++last;
                }
                const ContactKey key = events[first].key;
                first = last;

                if (!present)
                {
                    ended.push_back(key);
                    continue;
                }

                const ICollider *colliderLow = ColliderAt(present->low, present->colliderLow);
                const ICollider *colliderHigh = ColliderAt(present->high, present->colliderHigh);
                const ColliderSpec *specLow = SpecOfCollider(present->low, colliderLow);
                const ColliderSpec *specHigh = SpecOfCollider(present->high, colliderHigh);
                if (!specLow || !specHigh)
                {
                    continue; // a collider that is gone, or one with no object to tell
                }

                ContactInfo &info = contacts[key];
                info.low = present->low;
                info.high = present->high;
                info.colliderLow = colliderLow;
                info.colliderHigh = colliderHigh;
                info.trigger = specLow->isTrigger || specHigh->isTrigger;
                info.triggerOnHigh = specHigh->isTrigger;
                info.seen = true;
                info.dormant = false;

                const bool touching = !info.trigger || present->raw->overlapping;
                if (touching && !info.reported)
                {
                    info.reported = true;
                    ReportFound(info, present->kind == RawContact::Kind::Added ? present->raw : nullptr,
                                present->raw1IsLow);
                }
                else if (!touching && info.reported)
                {
                    parted.push_back(key);
                }
            }

            for (const ContactKey &key : parted)
            {
                if (const auto it = contacts.find(key); it != contacts.end() && it->second.reported)
                {
                    it->second.reported = false;
                    ReportLost(it->second);
                }
            }

            for (const ContactKey &key : ended)
            {
                const auto it = contacts.find(key);
                if (it == contacts.end())
                {
                    continue; // forgotten already (a body disabled or destroyed, a collider removed)
                }
                ContactInfo &info = it->second;
                if (!info.reported)
                {
                    contacts.erase(it);
                }
                else if (FellAsleep(info.low) || FellAsleep(info.high))
                {
                    // Jolt reports no contacts of a body that fell asleep: the pair still touches (as in PhysX, where a
                    // sleeping body in contact is in contact), until the body is awake and does not find it again
                    info.dormant = true;
                }
                else
                {
                    ReportLost(info);
                    contacts.erase(it);
                }
            }

            // A dormant pair whose bodies Jolt searched this step and found no contact for has ended
            for (auto it = contacts.begin(); it != contacts.end();)
            {
                ContactInfo &info = it->second;
                if (info.dormant && !info.seen && WasSearched(info.low) && WasSearched(info.high))
                {
                    ReportLost(info);
                    it = contacts.erase(it);
                    continue;
                }
                info.seen = false;
                ++it;
            }
        }

        // The engine ended every touch of the body (it was disabled or destroyed): Jolt's later report of the contact
        // finds nothing
        void ForgetBody(const PhysicsBodyHandle handle, const bool shapesReleased)
        {
            tracker.ForgetPairsWithBody(handle, shapesReleased);
            std::erase_if(contacts, [handle](const auto &entry)
                          { return entry.second.low == handle || entry.second.high == handle; });
        }

        // The collider is going (removed from its body, or turned from a trigger into a solid or back): its touches end
        void EndColliderTouches(const ICollider *collider)
        {
            tracker.EndShape(collider);
            std::erase_if(contacts, [collider](const auto &entry)
                          { return entry.second.colliderLow == collider || entry.second.colliderHigh == collider; });
        }

        // After a layer or matrix change: every touch the matrix now rules out ends, with an Exit at the next dispatch
        // (PhysX reports the same pairs ended one step later: after its next Update, which comes before the dispatch)
        void EndRuledOutTouches()
        {
            tracker.EndTouches([this](const CollisionPair &pair, const Tracker::ShapePair &shapes)
                               { return !CollidersCollide(pair.bodyA, shapes.onA, pair.bodyB, shapes.onB); });
            std::erase_if(contacts, [this](const auto &entry)
                          { return !CollidersCollide(entry.second.low, entry.second.colliderLow, entry.second.high,
                                                     entry.second.colliderHigh); });
        }

        // Wakes a body that is simulated and not static (an asleep body is not looked at, so a pair it is in would keep
        // its old state)
        void WakeBody(const BodyData &data)
        {
            if (data.active && data.enabled && data.motion != MotionKind::Static)
            {
                Bodies().ActivateBody(data.id);
            }
        }

        // Wakes both bodies of every touch the selector picks
        void WakeTouching(const std::function<bool(const CollisionPair &, const Tracker::ShapePair &)> &selected)
        {
            for (const Tracker::TouchMap *touches : {&tracker.collisionTouches, &tracker.triggerTouches})
            {
                for (const auto &[pair, shapePairs] : *touches)
                {
                    for (const Tracker::ShapePair &shapes : shapePairs)
                    {
                        if (selected(pair, shapes))
                        {
                            if (const BodyData *a = GetData(pair.bodyA))
                            {
                                WakeBody(*a);
                            }
                            if (const BodyData *b = GetData(pair.bodyB))
                            {
                                WakeBody(*b);
                            }
                        }
                    }
                }
            }
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

        s.matrix.Load();
        s.system = std::make_unique<JPH::PhysicsSystem>();
        s.maxBodies = std::max<std::uint32_t>(1, std::min(_maxBodies, kMaxBodies));
        s.system->Init(s.maxBodies, 0, kMaxBodyPairs, kMaxContactConstraints, s.broadPhaseLayers, s.objectVsBroadPhase,
                       s.objectPairs);
        s.system->SetGravity(ToJolt(s.currentGravity));
        s.system->SetCombineFriction(&CombineFriction);
        s.system->SetCombineRestitution(&CombineRestitution);
        s.system->SetContactListener(&s.contactBridge);
        s.system->SetSimShapeFilter(&s.pairShapeFilter);

        // PhysX's bounce threshold (kMinVelocityForRestitution); everything else of Jolt's settings stays at its default
        JPH::PhysicsSettings settings = s.system->GetPhysicsSettings();
        settings.mMinVelocityForRestitution = kMinVelocityForRestitution;
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
            s.tracker.Clear();
            s.contacts.clear();
            s.rawContacts.clear();
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

        s.CaptureActivity();
        s.rawContacts.clear();
        const JPH::EPhysicsUpdateError error =
            s.system->Update(deltaTime, kCollisionSteps, s.tempAllocator.get(), s.jobSystem.get());
        if (error != JPH::EPhysicsUpdateError::None && !s.updateErrorLogged)
        {
            s.updateErrorLogged = true;
            Logger::Error(std::format("Jolt backend: a simulation buffer overflowed (error flags {}); some contacts were "
                                      "ignored. The limits are JoltBackend::kMaxBodyPairs and kMaxContactConstraints.",
                                      static_cast<std::uint32_t>(error)));
        }

        // The listener's reports of this step become touches, in a deterministic order
        s.FoldContacts();
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
        _impl->tracker.Dispatch();
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
        // Both sides of its touches get their Exit (as in the PhysX backend: after the body is gone, before its handle
        // is freed, so the owner is still known)
        s.ForgetBody(handle, true);
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
            // Its touches end with an Exit for both sides: Jolt reports nothing for a body that leaves
            s.ForgetBody(handle, false);
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
        if (s.FindSpec(*data, collider))
        {
            // Pairs through this collider end with an Exit (the tracker's forgotten pairs); the others carry on
            s.EndColliderTouches(collider);
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
        if (spec->isTrigger == isTrigger)
        {
            return;
        }
        spec->isTrigger = isTrigger;
        // The touches of this collider were of the other kind (contacts or trigger overlaps): they end, and begin again
        // as the new kind. A trigger adds nothing to the mass, so toggling one moves the body's centre of mass and
        // inertia too. Waking the body makes Jolt look at its pairs again.
        s.EndColliderTouches(collider);
        s.RebuildBody(*data);
        s.WakeBody(*data);
    }

    void JoltBackend::SetColliderLayer(const PhysicsBodyHandle body, ICollider *collider, const int layer)
    {
        Impl &s = *_impl;
        Impl::BodyData *data = s.GetData(body);
        ColliderSpec *spec = data ? s.FindSpec(*data, collider) : nullptr;
        if (!spec)
        {
            return; // no shape yet: it takes its object's layer when it is attached
        }
        const int clamped = std::clamp(layer, 0, Layers::Count - 1);
        if (spec->layer == clamped)
        {
            return;
        }
        spec->layer = clamped;
        s.UpdateObjectLayer(*data);

        // Jolt only looks at the pairs of awake bodies, so the bodies this collider touches wake up to find the new
        // rules; the pairs the rules now forbid end at once (PhysX ends and restarts them in its next step)
        s.WakeBody(*data);
        s.WakeTouching([collider](const CollisionPair &, const Impl::Tracker::ShapePair &shapes)
                       { return shapes.onA == collider || shapes.onB == collider; });
        s.EndRuledOutTouches();
    }

    void JoltBackend::RefreshCollisionMatrix()
    {
        Impl &s = *_impl;
        if (!s.initialized)
        {
            return;
        }
        const LayerMatrix before = s.matrix;
        s.matrix.Load();
        std::uint32_t changed = 0;
        for (int layer = 0; layer < Layers::Count; ++layer)
        {
            const auto index = static_cast<std::size_t>(layer);
            if (before.rows[index] != s.matrix.rows[index])
            {
                changed |= Layers::MaskOf(layer);
            }
        }
        if (changed == 0)
        {
            return;
        }

        // Every body that has a collider on a layer whose row changed, in handle order (the bodies' creation order):
        // told to the observer, and woken. Jolt reads the matrix (the filters' copy) during the next Update and only
        // for awake bodies.
        const auto layerChanged = [changed](const int layer) { return (changed & Layers::MaskOf(layer)) != 0; };
        for (Impl::BodyData &data : s.bodies)
        {
            if (!data.active ||
                !std::ranges::any_of(data.specs, [&layerChanged](const ColliderSpec &spec)
                                     { return layerChanged(spec.layer); }))
            {
                continue;
            }
            if (s.refilterObserver && !data.colliders.empty())
            {
                s.refilterObserver(data.colliders.front()->GetGameObject());
            }
            s.WakeBody(data);
        }
        // The bodies they touch wake too, so a resting pair the matrix now rules out is looked at (and falls apart)
        s.WakeTouching(
            [&s, &layerChanged](const CollisionPair &pair, const Impl::Tracker::ShapePair &shapes)
            {
                const ColliderSpec *a = s.SpecOfCollider(pair.bodyA, shapes.onA);
                const ColliderSpec *b = s.SpecOfCollider(pair.bodyB, shapes.onB);
                return (a && layerChanged(a->layer)) || (b && layerChanged(b->layer));
            });
        s.EndRuledOutTouches();
    }

    void JoltBackend::SetRefilterObserver(std::function<void(const GameObject &)> observer)
    {
        _impl->refilterObserver = std::move(observer);
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

    bool JoltBackend::Raycast(const Math::Vector3 &, const Math::Vector3 &, RaycastHit &, float, std::uint32_t,
                              QueryTriggers)
    {
        _impl->WarnOnce(kQueries, "scene queries (Raycast, RaycastAll, SphereCast) find nothing");
        return false;
    }

    int JoltBackend::RaycastAll(const Math::Vector3 &, const Math::Vector3 &, std::vector<RaycastHit> &hits, float,
                                std::uint32_t, QueryTriggers)
    {
        _impl->WarnOnce(kQueries, "scene queries (Raycast, RaycastAll, SphereCast) find nothing");
        hits.clear();
        return 0;
    }

    bool JoltBackend::SphereCast(const Math::Vector3 &, float, const Math::Vector3 &, RaycastHit &, float,
                                 std::uint32_t)
    {
        _impl->WarnOnce(kQueries, "scene queries (Raycast, RaycastAll, SphereCast) find nothing");
        return false;
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

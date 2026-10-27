// The Jolt backend (stages J3, the core; J4, layers, triggers and events; J5, the scene queries; J6, snapshots). Jolt's
// headers are included here and nowhere else in the engine.
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
#include <Jolt/Physics/StateRecorder.h>
#include <Jolt/RegisterTypes.h>
#ifndef N2ENGINE_SINGLE_THREADED
#include <Jolt/Core/JobSystemThreadPool.h>
#endif

#include "../QueryUtil.hpp"
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
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
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

        // ===== Snapshots (J6) =====
        // The backend's part of a snapshot is a small binary format (native byte order: the bytes are for the same build):
        //   u32 magic, u32 version
        //   u32 slot count, then per body slot: bool active, u32 generation and, for an active slot, the body (see
        //     Impl::WriteSnapshot)
        //   the layer matrix, gravity, the contact map, the tracker's touches and undelivered events
        //   u64 size of Jolt's own state, then those bytes (PhysicsSystem::SaveState, everything)
        //   u64 FNV-1a checksum of all the bytes before it
        // A collider is written as its index in the owning body's registered colliders (never as a pointer, so the bytes
        // do not depend on addresses), kNoCollider for none.
        constexpr std::uint32_t kSnapshotMagic = 0x4A53324Eu; // "N2SJ"
        constexpr std::uint32_t kSnapshotVersion = 1;
        constexpr std::uint32_t kNoCollider = UINT32_MAX;
        constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

        std::uint64_t Fnv1a(const std::span<const std::byte> bytes)
        {
            std::uint64_t hash = kFnvOffset;
            for (const std::byte byte : bytes)
            {
                hash ^= static_cast<std::uint64_t>(byte);
                hash *= kFnvPrime;
            }
            return hash;
        }

        // The bits a float hashes as: every NaN is one value and -0 is +0, so two states that differ only in the payload of
        // a NaN or the sign of a zero hash alike
        std::uint32_t CanonicalFloatBits(const float value)
        {
            if (std::isnan(value))
            {
                return 0x7FC00000u;
            }
            if (value == 0.0f)
            {
                return 0u;
            }
            return std::bit_cast<std::uint32_t>(value);
        }

        // FNV-1a over 32-bit words, byte by byte in little-endian order (no std::hash, nothing platform dependent)
        class StateHasher
        {
        public:
            void Word(const std::uint32_t word)
            {
                for (int i = 0; i < 4; ++i)
                {
                    _hash ^= static_cast<std::uint64_t>((word >> (8 * i)) & 0xFFu);
                    _hash *= kFnvPrime;
                }
            }
            void Float(const float value) { Word(CanonicalFloatBits(value)); }
            [[nodiscard]] std::uint64_t Value() const { return _hash; }

        private:
            std::uint64_t _hash = kFnvOffset;
        };

        class SnapshotWriter
        {
        public:
            explicit SnapshotWriter(std::vector<std::byte> &out) : _out(out) {}

            void Bytes(const void *data, const std::size_t count)
            {
                if (count == 0)
                {
                    return;
                }
                const auto *bytes = static_cast<const std::byte *>(data);
                _out.insert(_out.end(), bytes, bytes + count);
            }
            void U8(const std::uint8_t value) { Bytes(&value, sizeof(value)); }
            void U32(const std::uint32_t value) { Bytes(&value, sizeof(value)); }
            void I32(const std::int32_t value) { Bytes(&value, sizeof(value)); }
            void U64(const std::uint64_t value) { Bytes(&value, sizeof(value)); }
            void F32(const float value) { Bytes(&value, sizeof(value)); }
            void Bool(const bool value) { U8(static_cast<std::uint8_t>(value ? 1 : 0)); }
            void Vec3(const Math::Vector3 &value)
            {
                F32(value.x);
                F32(value.y);
                F32(value.z);
            }
            void Quat(const Math::Quaternion &value)
            {
                F32(value.GetW());
                F32(value.GetX());
                F32(value.GetY());
                F32(value.GetZ());
            }
            void Handle(const PhysicsBodyHandle handle)
            {
                U32(handle.index);
                U32(handle.generation);
            }

        private:
            std::vector<std::byte> &_out;
        };

        // Reads with bounds checks: after the first read past the end every read gives zero and Ok() is false
        class SnapshotReader
        {
        public:
            explicit SnapshotReader(const std::span<const std::byte> in) : _in(in) {}

            bool Bytes(void *out, const std::size_t count)
            {
                if (!_ok || count > _in.size() - _position)
                {
                    _ok = false;
                    return false;
                }
                if (count > 0)
                {
                    std::memcpy(out, _in.data() + _position, count);
                }
                _position += count;
                return true;
            }
            std::uint8_t U8()
            {
                std::uint8_t value = 0;
                Bytes(&value, sizeof(value));
                return value;
            }
            std::uint32_t U32()
            {
                std::uint32_t value = 0;
                Bytes(&value, sizeof(value));
                return value;
            }
            std::int32_t I32()
            {
                std::int32_t value = 0;
                Bytes(&value, sizeof(value));
                return value;
            }
            std::uint64_t U64()
            {
                std::uint64_t value = 0;
                Bytes(&value, sizeof(value));
                return value;
            }
            float F32()
            {
                float value = 0.0f;
                Bytes(&value, sizeof(value));
                return value;
            }
            bool Bool() { return U8() != 0; }
            Math::Vector3 Vec3()
            {
                const float x = F32();
                const float y = F32();
                const float z = F32();
                return Math::Vector3(x, y, z);
            }
            Math::Quaternion Quat()
            {
                const float w = F32();
                const float x = F32();
                const float y = F32();
                const float z = F32();
                return Math::Quaternion(w, x, y, z);
            }
            PhysicsBodyHandle Handle()
            {
                PhysicsBodyHandle handle;
                handle.index = U32();
                handle.generation = U32();
                return handle;
            }
            // The next `count` bytes as a view (empty and not Ok when there are fewer)
            std::span<const std::byte> Take(const std::size_t count)
            {
                if (!_ok || count > _in.size() - _position)
                {
                    _ok = false;
                    return {};
                }
                const std::span<const std::byte> part = _in.subspan(_position, count);
                _position += count;
                return part;
            }
            [[nodiscard]] std::size_t Remaining() const { return _in.size() - _position; }
            [[nodiscard]] bool Ok() const { return _ok; }

        private:
            std::span<const std::byte> _in;
            std::size_t _position = 0;
            bool _ok = true;
        };

        // Jolt's StateRecorder over a byte vector (to write) or a byte view (to read), without the stringstream of
        // StateRecorderImpl. A read past the end fails the recorder and gives zeros.
        class ByteRecorder final : public JPH::StateRecorder
        {
        public:
            explicit ByteRecorder(std::vector<std::byte> &out) : _out(&out) {}
            explicit ByteRecorder(const std::span<const std::byte> in) : _in(in) {}

            void WriteBytes(const void *data, const std::size_t count) override
            {
                if (!_out)
                {
                    _failed = true;
                    return;
                }
                if (count > 0)
                {
                    const auto *bytes = static_cast<const std::byte *>(data);
                    _out->insert(_out->end(), bytes, bytes + count);
                }
            }

            void ReadBytes(void *data, const std::size_t count) override
            {
                if (_failed || _out || count > _in.size() - _position)
                {
                    _failed = true;
                    if (count > 0)
                    {
                        std::memset(data, 0, count);
                    }
                    return;
                }
                if (count > 0)
                {
                    std::memcpy(data, _in.data() + _position, count);
                }
                _position += count;
            }

            bool IsEOF() const override { return _out || _position >= _in.size(); }
            bool IsFailed() const override { return _failed; }

        private:
            std::vector<std::byte> *_out = nullptr;
            std::span<const std::byte> _in;
            std::size_t _position = 0;
            bool _failed = false;
        };

        // What a snapshot holds of the backend's own state, parsed (no live pointer in it)
        struct SnapSpec
        {
            std::uint32_t collider = kNoCollider; // index into the body's registered colliders
            ColliderKind kind = ColliderKind::Sphere;
            float radius = 0.5f;
            float height = 2.0f;
            Math::Vector3 halfExtents{0.5f, 0.5f, 0.5f};
            Math::Vector3 offset = Math::Vector3::Zero;
            PhysicsMaterial material;
            bool isTrigger = false;
            int layer = Layers::Default;
        };

        struct SnapBody
        {
            bool active = false;
            std::uint32_t generation = 0;
            std::uint32_t joltId = 0;
            MotionKind motion = MotionKind::Static;
            bool enabled = false;
            float mass = 1.0f;
            bool gravityEnabled = true;
            std::uint32_t colliderCount = 0;
            bool hasKinematicTarget = false;
            Math::Vector3 targetPosition = Math::Vector3::Zero;
            Math::Quaternion targetRotation = Math::Quaternion::Identity;
            bool kinematicMoving = false;
            std::vector<SnapSpec> specs;
        };

        struct SnapContact
        {
            ContactKey key;
            PhysicsBodyHandle low;
            PhysicsBodyHandle high;
            std::uint32_t colliderLow = kNoCollider;
            std::uint32_t colliderHigh = kNoCollider;
            bool trigger = false;
            bool triggerOnHigh = false;
            bool reported = false;
            bool dormant = false;
            bool seen = false;
        };

        struct SnapShapePair
        {
            std::uint32_t onA = kNoCollider; // a collider of the touch's bodyA
            std::uint32_t onB = kNoCollider; // a collider of its bodyB
        };

        struct SnapTouch
        {
            PhysicsBodyHandle bodyA;
            PhysicsBodyHandle bodyB;
            std::vector<SnapShapePair> shapes;
        };

        struct SnapCollisionEvent
        {
            PhysicsBodyHandle bodyA;
            PhysicsBodyHandle bodyB;
            std::uint32_t colliderA = kNoCollider;
            std::uint32_t colliderB = kNoCollider;
            std::vector<ContactPoint> contacts;
            Math::Vector3 relativeVelocity = Math::Vector3::Zero;
            Math::Vector3 impulse = Math::Vector3::Zero;
        };

        struct SnapTriggerEvent
        {
            PhysicsBodyHandle bodyA;
            PhysicsBodyHandle bodyB;
            std::uint32_t colliderA = kNoCollider;
            std::uint32_t colliderB = kNoCollider;
        };

        struct Snapshot
        {
            std::vector<SnapBody> bodies; // by handle index
            std::array<std::uint32_t, static_cast<std::size_t>(Layers::Count)> matrixRows{};
            Math::Vector3 gravity = Math::Vector3::Zero;
            bool hasPendingGravity = false;
            Math::Vector3 pendingGravity = Math::Vector3::Zero;
            std::vector<SnapContact> contacts;
            std::vector<SnapTouch> collisionTouches;
            std::vector<SnapTouch> triggerTouches;
            std::vector<SnapCollisionEvent> newCollisions;
            std::vector<SnapCollisionEvent> endedCollisions;
            std::vector<SnapCollisionEvent> forgottenCollisions;
            std::vector<SnapTriggerEvent> newTriggers;
            std::vector<SnapTriggerEvent> endedTriggers;
            std::vector<SnapTriggerEvent> forgottenTriggers;
            std::span<const std::byte> jolt; // Jolt's own state, a view into the bytes that were parsed
        };

        // Whether `index` names a collider of the snapshot's body `handle` (kNoCollider always does)
        bool SnapColliderOk(const Snapshot &snap, const PhysicsBodyHandle handle, const std::uint32_t index)
        {
            if (index == kNoCollider)
            {
                return true;
            }
            if (handle.index >= snap.bodies.size())
            {
                return false;
            }
            const SnapBody &body = snap.bodies[handle.index];
            return body.active && body.generation == handle.generation && index < body.colliderCount;
        }

        bool ReadTouches(SnapshotReader &reader, const Snapshot &snap, std::vector<SnapTouch> &touches)
        {
            const std::uint32_t count = reader.U32();
            if (!reader.Ok() || count > reader.Remaining())
            {
                return false;
            }
            touches.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                SnapTouch touch;
                touch.bodyA = reader.Handle();
                touch.bodyB = reader.Handle();
                const std::uint32_t shapes = reader.U32();
                if (!reader.Ok() || shapes > reader.Remaining())
                {
                    return false;
                }
                for (std::uint32_t j = 0; j < shapes; ++j)
                {
                    SnapShapePair pair;
                    pair.onA = reader.U32();
                    pair.onB = reader.U32();
                    if (!reader.Ok() || !SnapColliderOk(snap, touch.bodyA, pair.onA) ||
                        !SnapColliderOk(snap, touch.bodyB, pair.onB))
                    {
                        return false;
                    }
                    touch.shapes.push_back(pair);
                }
                touches.push_back(std::move(touch));
            }
            return reader.Ok();
        }

        bool ReadCollisionEvents(SnapshotReader &reader, const Snapshot &snap, std::vector<SnapCollisionEvent> &events)
        {
            const std::uint32_t count = reader.U32();
            if (!reader.Ok() || count > reader.Remaining())
            {
                return false;
            }
            events.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                SnapCollisionEvent event;
                event.bodyA = reader.Handle();
                event.bodyB = reader.Handle();
                event.colliderA = reader.U32();
                event.colliderB = reader.U32();
                const std::uint32_t points = reader.U32();
                if (!reader.Ok() || points > reader.Remaining() || !SnapColliderOk(snap, event.bodyA, event.colliderA) ||
                    !SnapColliderOk(snap, event.bodyB, event.colliderB))
                {
                    return false;
                }
                for (std::uint32_t j = 0; j < points; ++j)
                {
                    ContactPoint point;
                    point.point = reader.Vec3();
                    point.normal = reader.Vec3();
                    point.separation = reader.F32();
                    point.normalImpulse = reader.F32();
                    point.tangentImpulse[0] = reader.F32();
                    point.tangentImpulse[1] = reader.F32();
                    event.contacts.push_back(point);
                }
                event.relativeVelocity = reader.Vec3();
                event.impulse = reader.Vec3();
                events.push_back(std::move(event));
            }
            return reader.Ok();
        }

        bool ReadTriggerEvents(SnapshotReader &reader, const Snapshot &snap, std::vector<SnapTriggerEvent> &events)
        {
            const std::uint32_t count = reader.U32();
            if (!reader.Ok() || count > reader.Remaining())
            {
                return false;
            }
            events.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                SnapTriggerEvent event;
                event.bodyA = reader.Handle();
                event.bodyB = reader.Handle();
                event.colliderA = reader.U32();
                event.colliderB = reader.U32();
                if (!reader.Ok() || !SnapColliderOk(snap, event.bodyA, event.colliderA) ||
                    !SnapColliderOk(snap, event.bodyB, event.colliderB))
                {
                    return false;
                }
                events.push_back(event);
            }
            return reader.Ok();
        }

        // Parses and checks the bytes of a snapshot (size, checksum, version, structure) without looking at the live
        // backend. False with the reason in `why`; `snap.jolt` then views `in`.
        bool ParseSnapshot(const std::span<const std::byte> in, Snapshot &snap, std::string &why)
        {
            constexpr std::size_t kChecksumBytes = sizeof(std::uint64_t);
            if (in.size() < 2 * sizeof(std::uint32_t) + kChecksumBytes)
            {
                why = std::format("the {} bytes are too short to be a snapshot", in.size());
                return false;
            }
            const std::span<const std::byte> body = in.first(in.size() - kChecksumBytes);
            std::uint64_t checksum = 0;
            std::memcpy(&checksum, in.data() + body.size(), kChecksumBytes);
            if (checksum != Fnv1a(body))
            {
                why = "the checksum does not match (the bytes are damaged, cut short, or not a snapshot of this backend)";
                return false;
            }

            SnapshotReader reader(body);
            const std::uint32_t magic = reader.U32();
            const std::uint32_t version = reader.U32();
            if (magic != kSnapshotMagic || version != kSnapshotVersion)
            {
                why = "the bytes are not a snapshot of this backend's version";
                return false;
            }

            const std::uint32_t slots = reader.U32();
            if (!reader.Ok() || slots > reader.Remaining())
            {
                why = "the body table is cut short";
                return false;
            }
            snap.bodies.resize(slots);
            for (std::uint32_t i = 0; i < slots; ++i)
            {
                SnapBody &data = snap.bodies[i];
                data.active = reader.Bool();
                data.generation = reader.U32();
                if (!data.active)
                {
                    continue;
                }
                data.joltId = reader.U32();
                const std::uint8_t motion = reader.U8();
                data.enabled = reader.Bool();
                data.mass = reader.F32();
                data.gravityEnabled = reader.Bool();
                data.colliderCount = reader.U32();
                data.hasKinematicTarget = reader.Bool();
                data.targetPosition = reader.Vec3();
                data.targetRotation = reader.Quat();
                data.kinematicMoving = reader.Bool();
                const std::uint32_t specs = reader.U32();
                if (!reader.Ok() || motion > static_cast<std::uint8_t>(MotionKind::Kinematic) ||
                    specs > reader.Remaining())
                {
                    why = "the body table is damaged";
                    return false;
                }
                data.motion = static_cast<MotionKind>(motion);
                for (std::uint32_t j = 0; j < specs; ++j)
                {
                    SnapSpec spec;
                    spec.collider = reader.U32();
                    const std::uint8_t kind = reader.U8();
                    spec.radius = reader.F32();
                    spec.height = reader.F32();
                    spec.halfExtents = reader.Vec3();
                    spec.offset = reader.Vec3();
                    spec.material.staticFriction = reader.F32();
                    spec.material.dynamicFriction = reader.F32();
                    spec.material.restitution = reader.F32();
                    spec.isTrigger = reader.Bool();
                    spec.layer = reader.I32();
                    if (!reader.Ok() || kind > static_cast<std::uint8_t>(ColliderKind::Capsule) ||
                        !Layers::IsValid(spec.layer) ||
                        (spec.collider != kNoCollider && spec.collider >= data.colliderCount))
                    {
                        why = "a collider description is damaged";
                        return false;
                    }
                    spec.kind = static_cast<ColliderKind>(kind);
                    data.specs.push_back(spec);
                }
            }

            const std::uint32_t rows = reader.U32();
            if (!reader.Ok() || rows != snap.matrixRows.size())
            {
                why = std::format("the layer matrix has {} rows and this build has {}", rows, snap.matrixRows.size());
                return false;
            }
            for (std::uint32_t &row : snap.matrixRows)
            {
                row = reader.U32();
            }
            snap.gravity = reader.Vec3();
            snap.hasPendingGravity = reader.Bool();
            snap.pendingGravity = reader.Vec3();

            const std::uint32_t contacts = reader.U32();
            if (!reader.Ok() || contacts > reader.Remaining())
            {
                why = "the contact table is damaged";
                return false;
            }
            snap.contacts.reserve(contacts);
            for (std::uint32_t i = 0; i < contacts; ++i)
            {
                SnapContact contact;
                const std::uint64_t lowOrder = reader.U64();
                const std::uint64_t highOrder = reader.U64();
                const std::uint32_t lowSub = reader.U32();
                const std::uint32_t highSub = reader.U32();
                contact.key = ContactKey{lowOrder, highOrder, lowSub, highSub};
                contact.low = reader.Handle();
                contact.high = reader.Handle();
                contact.colliderLow = reader.U32();
                contact.colliderHigh = reader.U32();
                contact.trigger = reader.Bool();
                contact.triggerOnHigh = reader.Bool();
                contact.reported = reader.Bool();
                contact.dormant = reader.Bool();
                contact.seen = reader.Bool();
                if (!reader.Ok() || !SnapColliderOk(snap, contact.low, contact.colliderLow) ||
                    !SnapColliderOk(snap, contact.high, contact.colliderHigh))
                {
                    why = "the contact table is damaged";
                    return false;
                }
                snap.contacts.push_back(contact);
            }

            if (!ReadTouches(reader, snap, snap.collisionTouches) || !ReadTouches(reader, snap, snap.triggerTouches) ||
                !ReadCollisionEvents(reader, snap, snap.newCollisions) ||
                !ReadCollisionEvents(reader, snap, snap.endedCollisions) ||
                !ReadCollisionEvents(reader, snap, snap.forgottenCollisions) ||
                !ReadTriggerEvents(reader, snap, snap.newTriggers) ||
                !ReadTriggerEvents(reader, snap, snap.endedTriggers) ||
                !ReadTriggerEvents(reader, snap, snap.forgottenTriggers))
            {
                why = "the touch and event tables are damaged";
                return false;
            }

            const std::uint64_t joltSize = reader.U64();
            if (!reader.Ok() || joltSize != reader.Remaining())
            {
                why = "Jolt's part of the state does not fit the rest";
                return false;
            }
            snap.jolt = reader.Take(static_cast<std::size_t>(joltSize));
            if (!reader.Ok())
            {
                why = "Jolt's part of the state is cut short";
                return false;
            }
            return true;
        }

        bool SameSpec(const ColliderSpec &a, const ColliderSpec &b)
        {
            return a.collider == b.collider && a.kind == b.kind && a.radius == b.radius && a.height == b.height &&
                   a.halfExtents.x == b.halfExtents.x && a.halfExtents.y == b.halfExtents.y &&
                   a.halfExtents.z == b.halfExtents.z && a.offset.x == b.offset.x && a.offset.y == b.offset.y &&
                   a.offset.z == b.offset.z && a.material.staticFriction == b.material.staticFriction &&
                   a.material.dynamicFriction == b.material.dynamicFriction &&
                   a.material.restitution == b.material.restitution && a.isTrigger == b.isTrigger &&
                   a.layer == b.layer;
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
                _impl.NoteContactThread();
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
                if (kind == RawContact::Kind::Persisted && !trigger && _impl.IsReported(body1, body2, manifold))
                {
                    return; // a solid contact that carries on, and that the main thread already knows, is not news
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
                    const JPH::Vec3 velocity1 = !body1.IsStatic() ? body1.GetLinearVelocity() : JPH::Vec3::sZero();
                    const JPH::Vec3 velocity2 = !body2.IsStatic() ? body2.GetLinearVelocity() : JPH::Vec3::sZero();
                    raw.relativeVelocity = ToEngine(velocity1 - velocity2);
                }

                const std::lock_guard lock(_impl.contactMutex);
                _impl.rawContacts.push_back(std::move(raw));
            }

            Impl &_impl;
        };

        // Decides per pair of leaf shapes (colliders) whether they collide: the layer matrix, no trigger against
        // trigger (PhysX never pairs two trigger shapes either), and no solid pair of which neither body is dynamic
        // (a kinematic body is let into the pair search so that it can trigger, see CreateBody). Called on worker
        // threads: reads only.
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
                if (!collider1->isTrigger && !collider2->isTrigger && !body1.IsDynamic() && !body2.IsDynamic())
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
        mutable std::mutex contactMutex;
        std::vector<RawContact> rawContacts;
        std::set<std::thread::id> contactThreads; // the threads that ran the listener (diagnostics), under the mutex
        Tracker tracker;
        std::map<ContactKey, ContactInfo> contacts;
        std::function<void(const GameObject &)> refilterObserver;

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

        // Called by the contact listener on its worker threads: remembers which threads ran it, so a test can tell that
        // the thread pool really took part in the steps
        void NoteContactThread()
        {
            const std::lock_guard lock(contactMutex);
            contactThreads.insert(std::this_thread::get_id());
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

        // Whether the main thread tracks this contact as touching. Called by the listener on worker threads during Update,
        // when the main thread does not change `contacts`: concurrent reads only. A Persisted report of a contact that is
        // not tracked (its touch was ended by a layer, matrix or trigger change while Jolt kept the contact in its cache)
        // must reach FoldContacts, which then begins the touch again.
        bool IsReported(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold) const
        {
            PhysicsBodyHandle handle1;
            PhysicsBodyHandle handle2;
            if (!HandleOfId(body1.GetID().GetIndexAndSequenceNumber(), handle1) ||
                !HandleOfId(body2.GetID().GetIndexAndSequenceNumber(), handle2))
            {
                return true; // nothing to tell about a body that is gone
            }
            const bool oneIsLow = HandleOrder(handle1) < HandleOrder(handle2);
            const ContactKey key{HandleOrder(oneIsLow ? handle1 : handle2), HandleOrder(oneIsLow ? handle2 : handle1),
                                 oneIsLow ? manifold.mSubShapeID1.GetValue() : manifold.mSubShapeID2.GetValue(),
                                 oneIsLow ? manifold.mSubShapeID2.GetValue() : manifold.mSubShapeID1.GetValue()};
            const auto it = contacts.find(key);
            return it != contacts.end() && it->second.reported && !it->second.dormant;
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
            // A kinematic body generates contacts against static and kinematic bodies too: that is how a moving
            // kinematic body triggers a static trigger zone, as it does in PhysX. PairShapeFilter drops the solid pairs
            // among them (PhysX gives no contact events there), so only trigger contacts come of it.
            settings.mCollideKinematicVsNonDynamic = motion == MotionKind::Kinematic;
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

        /// The collider that owns sub-shape `subShape` of body `id`, or false: not a body of this backend, a collider
        /// the mask leaves out (its layer is the one SetColliderLayer last gave it, which the simulation uses too, as
        /// the PhysX backend's shape filter data), or a trigger collider while triggers are ignored. A trigger
        /// collider is a sub-shape of its body's shape like a solid one (J4), so a query that collides with triggers
        /// hits it.
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
            if ((layerMask & Layers::MaskOf(spec.layer)) == 0)
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

        // A static body, or one that is not awake now. Jolt reports a sleeping body's contacts removed (in the step it
        // fell asleep or the next one), so a removal whose non-static bodies are all asleep is a sleep, not a
        // separation: a body that moved away is awake.
        bool AsleepOrStatic(const PhysicsBodyHandle handle) const
        {
            const BodyData *data = GetData(handle);
            return !data || data->motion == MotionKind::Static || !data->enabled || !Bodies().IsActive(data->id);
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

            // The collider pairs Jolt reported present in this step. A contact's key holds sub-shape ids, which change when a
            // body's shape is rebuilt (a collider added or removed): Jolt then reports the old key Removed and the new key
            // Added for the same collider pair, and the old key must not end the touch. Only membership is tested, and
            // only touching pairs count (a trigger pair that is only speculative does not hold the old touch).
            std::set<std::tuple<std::uint64_t, std::uint64_t, const ICollider *, const ICollider *>> presentPairs;

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
                if (touching)
                {
                    // A trigger contact inside the speculative distance only is not present: a tracked touch under the
                    // old key must end with an Exit, not be taken over by a key that never reports it
                    presentPairs.insert({HandleOrder(info.low), HandleOrder(info.high), colliderLow, colliderHigh});
                }
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
                else if (presentPairs.contains({HandleOrder(info.low), HandleOrder(info.high), info.colliderLow,
                                                info.colliderHigh}))
                {
                    contacts.erase(it); // the same collider pair under a new key: still touching
                }
                else if (AsleepOrStatic(info.low) && AsleepOrStatic(info.high))
                {
                    // Jolt reports no contacts of a body that is asleep: the pair still touches (as in PhysX, where a
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

        // ===== Snapshots (J6) =====

        // A collider's index among the body's registered colliders (what a snapshot stores), kNoCollider for none
        static std::uint32_t IndexOfCollider(const BodyData &data, const ICollider *collider)
        {
            if (!collider)
            {
                return kNoCollider;
            }
            const auto it = std::find(data.colliders.begin(), data.colliders.end(), collider);
            return it == data.colliders.end() ? kNoCollider : static_cast<std::uint32_t>(it - data.colliders.begin());
        }

        // The same for the body behind a handle; kNoCollider when the body is gone
        std::uint32_t EncodeCollider(const PhysicsBodyHandle handle, const ICollider *collider) const
        {
            const BodyData *data = GetData(handle);
            return data ? IndexOfCollider(*data, collider) : kNoCollider;
        }

        // The registered collider of a live body that an index names; null for kNoCollider, a body that is gone, or an
        // index past the list
        ICollider *DecodeCollider(const PhysicsBodyHandle handle, const std::uint32_t index) const
        {
            const BodyData *data = GetData(handle);
            return data && index < data->colliders.size() ? data->colliders[index] : nullptr;
        }

        // The GameObjects that own a pair's two bodies now, as the tracker records them on a forgotten pair (empty for a
        // body that is gone)
        PairOwners OwnersOfPair(const CollisionPair &pair) const
        {
            const auto ownerOf = [this](const PhysicsBodyHandle handle) -> std::weak_ptr<GameObject>
            {
                const BodyData *data = GetData(handle);
                GameObject *owner = data ? OwnerOf(*data) : nullptr;
                return owner ? owner->weak_from_this() : std::weak_ptr<GameObject>{};
            };
            return PairOwners{ownerOf(pair.bodyA), ownerOf(pair.bodyB)};
        }

        // Why a queue of forgotten pairs cannot be saved, or empty. An Exit that waits for a body that is already gone
        // reaches the GameObject that owned it through a weak reference, which the bytes cannot hold; one whose owner is
        // gone too needs nothing.
        template <typename Event>
        std::string UnsavableOwner(const std::vector<Event> &events) const
        {
            for (const Event &event : events)
            {
                if ((event.owners.a.lock() && !GetData(event.pair.bodyA)) ||
                    (event.owners.b.lock() && !GetData(event.pair.bodyB)))
                {
                    return "an Exit is still waiting for a body that is already gone (ProcessCollisionCallbacks delivers it)";
                }
            }
            return {};
        }

        void WriteTouches(SnapshotWriter &writer, const Tracker::TouchMap &touches) const
        {
            writer.U32(static_cast<std::uint32_t>(touches.size()));
            for (const auto &[pair, shapes] : touches)
            {
                writer.Handle(pair.bodyA);
                writer.Handle(pair.bodyB);
                writer.U32(static_cast<std::uint32_t>(shapes.size()));
                for (const Tracker::ShapePair &shape : shapes)
                {
                    writer.U32(EncodeCollider(pair.bodyA, shape.onA));
                    writer.U32(EncodeCollider(pair.bodyB, shape.onB));
                }
            }
        }

        void WriteCollisionEvents(SnapshotWriter &writer, const std::vector<Tracker::CollisionEvent> &events) const
        {
            static const Collision kNoData;
            writer.U32(static_cast<std::uint32_t>(events.size()));
            for (const Tracker::CollisionEvent &event : events)
            {
                const Collision &data = event.data ? *event.data : kNoData;
                writer.Handle(event.pair.bodyA);
                writer.Handle(event.pair.bodyB);
                writer.U32(EncodeCollider(event.pair.bodyA, data.collider));
                writer.U32(EncodeCollider(event.pair.bodyB, data.otherCollider));
                writer.U32(static_cast<std::uint32_t>(data.contacts.size()));
                for (const ContactPoint &point : data.contacts)
                {
                    writer.Vec3(point.point);
                    writer.Vec3(point.normal);
                    writer.F32(point.separation);
                    writer.F32(point.normalImpulse);
                    writer.F32(point.tangentImpulse[0]);
                    writer.F32(point.tangentImpulse[1]);
                }
                writer.Vec3(data.relativeVelocity);
                writer.Vec3(data.impulse);
            }
        }

        void WriteTriggerEvents(SnapshotWriter &writer, const std::vector<Tracker::TriggerEvent> &events) const
        {
            writer.U32(static_cast<std::uint32_t>(events.size()));
            for (const Tracker::TriggerEvent &event : events)
            {
                writer.Handle(event.pair.bodyA);
                writer.Handle(event.pair.bodyB);
                writer.U32(EncodeCollider(event.pair.bodyA, event.colliderA));
                writer.U32(EncodeCollider(event.pair.bodyB, event.colliderB));
            }
        }

        // Writes the whole snapshot into `out` (replacing what it held, keeping its capacity). `strict` refuses a state
        // that holds an Exit the bytes cannot carry; nothing is written then. False with the reason in `why`.
        bool WriteSnapshot(std::vector<std::byte> &out, const bool strict, std::string &why) const
        {
            if (!initialized)
            {
                why = "the backend is not initialized";
                return false;
            }
            if (strict)
            {
                std::string reason = UnsavableOwner(tracker.forgottenCollisions);
                if (reason.empty())
                {
                    reason = UnsavableOwner(tracker.forgottenTriggers);
                }
                if (!reason.empty())
                {
                    why = std::move(reason);
                    return false;
                }
            }

            out.clear();
            SnapshotWriter writer(out);
            writer.U32(kSnapshotMagic);
            writer.U32(kSnapshotVersion);

            // Every body slot, so a restore can tell which bodies are the same ones. What a restore puts back is what the
            // simulation does not carry itself: the mass, the gravity flag, the kinematic target, the collider specs.
            writer.U32(static_cast<std::uint32_t>(bodies.size()));
            for (const BodyData &data : bodies)
            {
                writer.Bool(data.active);
                writer.U32(data.generation);
                if (!data.active)
                {
                    continue;
                }
                writer.U32(data.id.GetIndexAndSequenceNumber());
                writer.U8(static_cast<std::uint8_t>(data.motion));
                writer.Bool(data.enabled);
                writer.F32(data.mass);
                writer.Bool(data.gravityEnabled);
                writer.U32(static_cast<std::uint32_t>(data.colliders.size()));
                writer.Bool(data.hasKinematicTarget);
                writer.Vec3(data.targetPosition);
                writer.Quat(data.targetRotation);
                writer.Bool(data.kinematicMoving);
                writer.U32(static_cast<std::uint32_t>(data.specs.size()));
                for (const ColliderSpec &spec : data.specs)
                {
                    writer.U32(IndexOfCollider(data, spec.collider));
                    writer.U8(static_cast<std::uint8_t>(spec.kind));
                    writer.F32(spec.radius);
                    writer.F32(spec.height);
                    writer.Vec3(spec.halfExtents);
                    writer.Vec3(spec.offset);
                    writer.F32(spec.material.staticFriction);
                    writer.F32(spec.material.dynamicFriction);
                    writer.F32(spec.material.restitution);
                    writer.Bool(spec.isTrigger);
                    writer.I32(spec.layer);
                }
            }

            writer.U32(static_cast<std::uint32_t>(matrix.rows.size()));
            for (const std::uint32_t row : matrix.rows)
            {
                writer.U32(row);
            }
            writer.Vec3(currentGravity);
            writer.Bool(pendingGravity.has_value());
            writer.Vec3(pendingGravity.value_or(Math::Vector3::Zero));

            writer.U32(static_cast<std::uint32_t>(contacts.size()));
            for (const auto &[key, info] : contacts)
            {
                writer.U64(std::get<0>(key));
                writer.U64(std::get<1>(key));
                writer.U32(std::get<2>(key));
                writer.U32(std::get<3>(key));
                writer.Handle(info.low);
                writer.Handle(info.high);
                writer.U32(EncodeCollider(info.low, info.colliderLow));
                writer.U32(EncodeCollider(info.high, info.colliderHigh));
                writer.Bool(info.trigger);
                writer.Bool(info.triggerOnHigh);
                writer.Bool(info.reported);
                writer.Bool(info.dormant);
                writer.Bool(info.seen);
            }

            WriteTouches(writer, tracker.collisionTouches);
            WriteTouches(writer, tracker.triggerTouches);
            WriteCollisionEvents(writer, tracker.newCollisions);
            WriteCollisionEvents(writer, tracker.endedCollisions);
            WriteCollisionEvents(writer, tracker.forgottenCollisions);
            WriteTriggerEvents(writer, tracker.newTriggers);
            WriteTriggerEvents(writer, tracker.endedTriggers);
            WriteTriggerEvents(writer, tracker.forgottenTriggers);

            // Jolt's own state, straight into the same buffer behind its size
            const std::size_t sizeAt = out.size();
            writer.U64(0);
            const std::size_t joltStart = out.size();
            ByteRecorder recorder(out);
            system->SaveState(recorder, JPH::EStateRecorderState::All);
            if (recorder.IsFailed())
            {
                out.clear();
                why = "Jolt could not save its state";
                return false;
            }
            const std::uint64_t joltSize = static_cast<std::uint64_t>(out.size() - joltStart);
            std::memcpy(out.data() + sizeAt, &joltSize, sizeof(joltSize));

            const std::uint64_t checksum = Fnv1a(std::span<const std::byte>(out.data(), out.size()));
            writer.U64(checksum);
            return true;
        }

        // Whether the live backend can take the parsed snapshot: the bodies in the simulation (enabled) must be the same
        // ones. A body that is disabled now and was disabled when saved (parked) may be anything. False with the reason.
        bool CheckSnapshot(const Snapshot &snap, std::string &why) const
        {
            const std::size_t slots = std::max(snap.bodies.size(), bodies.size());
            for (std::size_t i = 0; i < slots; ++i)
            {
                const BodyData *live = i < bodies.size() ? &bodies[i] : nullptr;
                const SnapBody *was = i < snap.bodies.size() ? &snap.bodies[i] : nullptr;
                const bool liveEnabled = live && live->active && live->enabled;
                const bool wasEnabled = was && was->active && was->enabled;
                if (!liveEnabled && !wasEnabled)
                {
                    continue;
                }
                if (liveEnabled != wasEnabled)
                {
                    why = std::format("the bodies in the simulation differ: body slot {} is {} now and was {} when the "
                                      "state was saved (a body spawned or despawned inside the window must be parked "
                                      "with SetBodyEnabled(false), not created or destroyed)",
                                      i, liveEnabled ? "in the simulation" : "out of the simulation",
                                      wasEnabled ? "in the simulation" : "out of the simulation");
                    return false;
                }
                if (live->generation != was->generation || live->id.GetIndexAndSequenceNumber() != was->joltId ||
                    live->motion != was->motion)
                {
                    why = std::format("body slot {} holds another body than when the state was saved (generation {} "
                                      "against {})",
                                      i, live->generation, was->generation);
                    return false;
                }
                if (live->colliders.size() != was->colliderCount)
                {
                    why = std::format("body slot {} has {} registered colliders and had {} when the state was saved", i,
                                      live->colliders.size(), was->colliderCount);
                    return false;
                }
            }
            return true;
        }

        // Puts back what the simulation does not carry for one body (see WriteSnapshot): it is the same body as in the
        // snapshot (CheckSnapshot, or a parked body that is still there)
        void RestoreBodyConfig(BodyData &live, const SnapBody &was)
        {
            live.hasKinematicTarget = was.hasKinematicTarget;
            live.targetPosition = was.targetPosition;
            live.targetRotation = was.targetRotation;
            live.kinematicMoving = was.kinematicMoving;

            if (live.gravityEnabled != was.gravityEnabled)
            {
                live.gravityEnabled = was.gravityEnabled;
                if (live.motion != MotionKind::Static)
                {
                    Bodies().SetGravityFactor(live.id, live.gravityEnabled ? 1.0f : 0.0f);
                }
            }
            const bool massChanged = live.mass != was.mass;
            live.mass = was.mass;

            // The collider specs: when any differs, the body's Jolt shape is rebuilt from the saved ones (a trigger flag, a
            // layer, a size or a material changed inside the window)
            bool specsChanged = false;
            if (live.colliders.size() == was.colliderCount)
            {
                std::vector<ColliderSpec> specs;
                specs.reserve(was.specs.size());
                for (const SnapSpec &saved : was.specs)
                {
                    ColliderSpec spec;
                    spec.collider = saved.collider == kNoCollider ? nullptr : live.colliders[saved.collider];
                    spec.kind = saved.kind;
                    spec.radius = saved.radius;
                    spec.height = saved.height;
                    spec.halfExtents = saved.halfExtents;
                    spec.offset = saved.offset;
                    spec.material = saved.material;
                    spec.isTrigger = saved.isTrigger;
                    spec.layer = saved.layer;
                    specs.push_back(spec);
                }
                specsChanged = specs.size() != live.specs.size() ||
                               !std::equal(specs.begin(), specs.end(), live.specs.begin(), SameSpec);
                if (specsChanged)
                {
                    live.specs = std::move(specs);
                }
            }
            if (specsChanged)
            {
                RebuildBody(live);
            }
            else if (massChanged)
            {
                ApplyMassProperties(live);
            }
        }

        void RestoreTouches(Tracker::TouchMap &touches, const std::vector<SnapTouch> &saved) const
        {
            for (const SnapTouch &touch : saved)
            {
                std::vector<Tracker::ShapePair> &shapes = touches[CollisionPair{touch.bodyA, touch.bodyB}];
                for (const SnapShapePair &pair : touch.shapes)
                {
                    Tracker::ShapePair shape;
                    shape.onA = DecodeCollider(touch.bodyA, pair.onA);
                    shape.onB = DecodeCollider(touch.bodyB, pair.onB);
                    shapes.push_back(shape);
                }
            }
        }

        // `withOwners`: a forgotten pair, which also carries the objects that own its bodies
        Tracker::CollisionEvent MakeCollisionEvent(const SnapCollisionEvent &saved, const bool withOwners) const
        {
            Tracker::CollisionEvent event;
            event.pair = CollisionPair{saved.bodyA, saved.bodyB};
            auto *data = new Collision();
            data->collider = DecodeCollider(saved.bodyA, saved.colliderA);
            data->otherCollider = DecodeCollider(saved.bodyB, saved.colliderB);
            data->contacts = saved.contacts;
            data->relativeVelocity = saved.relativeVelocity;
            data->impulse = saved.impulse;
            event.data = data;
            if (withOwners)
            {
                event.owners = OwnersOfPair(event.pair);
            }
            return event;
        }

        Tracker::TriggerEvent MakeTriggerEvent(const SnapTriggerEvent &saved, const bool withOwners) const
        {
            Tracker::TriggerEvent event;
            event.pair = CollisionPair{saved.bodyA, saved.bodyB};
            event.colliderA = DecodeCollider(saved.bodyA, saved.colliderA);
            event.colliderB = DecodeCollider(saved.bodyB, saved.colliderB);
            if (withOwners)
            {
                event.owners = OwnersOfPair(event.pair);
            }
            return event;
        }

        // Puts a parsed and checked snapshot back. Only Jolt's own restore can fail (false); the rest is assignment. The
        // order matters: shapes are rebuilt first (a rebuild may wake a body), then Jolt's restore sets every body's pose,
        // velocities and awake state, then the contact bookkeeping that refers to them.
        bool ApplySnapshot(const Snapshot &snap)
        {
            const std::size_t common = std::min(snap.bodies.size(), bodies.size());
            for (std::size_t i = 0; i < common; ++i)
            {
                BodyData &live = bodies[i];
                const SnapBody &was = snap.bodies[i];
                if (!live.active || !was.active || live.generation != was.generation || live.motion != was.motion)
                {
                    continue; // not the same body: a parked one that the snapshot does not know
                }
                RestoreBodyConfig(live, was);
            }

            matrix.rows = snap.matrixRows;
            currentGravity = snap.gravity;
            if (snap.hasPendingGravity)
            {
                pendingGravity = snap.pendingGravity;
            }
            else
            {
                pendingGravity.reset();
            }

            ByteRecorder recorder(snap.jolt);
            if (!system->RestoreState(recorder) || recorder.IsFailed())
            {
                return false;
            }

            tracker.Clear();
            contacts.clear();
            {
                const std::lock_guard lock(contactMutex);
                rawContacts.clear();
            }
            for (const SnapContact &saved : snap.contacts)
            {
                ContactInfo info;
                info.low = saved.low;
                info.high = saved.high;
                info.colliderLow = DecodeCollider(saved.low, saved.colliderLow);
                info.colliderHigh = DecodeCollider(saved.high, saved.colliderHigh);
                info.trigger = saved.trigger;
                info.triggerOnHigh = saved.triggerOnHigh;
                info.reported = saved.reported;
                info.dormant = saved.dormant;
                info.seen = saved.seen;
                contacts.emplace(saved.key, info);
            }
            RestoreTouches(tracker.collisionTouches, snap.collisionTouches);
            RestoreTouches(tracker.triggerTouches, snap.triggerTouches);
            for (const SnapCollisionEvent &saved : snap.newCollisions)
            {
                tracker.newCollisions.push_back(MakeCollisionEvent(saved, false));
            }
            for (const SnapCollisionEvent &saved : snap.endedCollisions)
            {
                tracker.endedCollisions.push_back(MakeCollisionEvent(saved, false));
            }
            for (const SnapCollisionEvent &saved : snap.forgottenCollisions)
            {
                tracker.forgottenCollisions.push_back(MakeCollisionEvent(saved, true));
            }
            for (const SnapTriggerEvent &saved : snap.newTriggers)
            {
                tracker.newTriggers.push_back(MakeTriggerEvent(saved, false));
            }
            for (const SnapTriggerEvent &saved : snap.endedTriggers)
            {
                tracker.endedTriggers.push_back(MakeTriggerEvent(saved, false));
            }
            for (const SnapTriggerEvent &saved : snap.forgottenTriggers)
            {
                tracker.forgottenTriggers.push_back(MakeTriggerEvent(saved, true));
            }
            return true;
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
        // otherwise. J6's tests prove same-binary 0-versus-N thread equality of the state, but the declaration stays
        // conservative (DeclaredDeterminism): the Deterministic preset still runs the single-threaded job system.
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
                                                                     _workerThreads);
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
            {
                const std::lock_guard lock(s.contactMutex);
                s.contactThreads.clear();
            }
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
        QueryUtil::SortByPick(raws, keys);
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

    bool JoltBackend::SaveState(std::vector<std::byte> &out) const
    {
        std::string why;
        if (!_impl->WriteSnapshot(out, true, why))
        {
            Logger::Error(std::format("Jolt backend: SaveState refused: {}", why));
            return false;
        }
        return true;
    }

    bool JoltBackend::RestoreState(const std::span<const std::byte> in)
    {
        Impl &s = *_impl;
        if (!s.initialized)
        {
            Logger::Error("Jolt backend: RestoreState refused: the backend is not initialized");
            return false;
        }

        // Everything that can be refused is decided before anything changes
        Snapshot snap;
        std::string why;
        if (!ParseSnapshot(in, snap, why) || !s.CheckSnapshot(snap, why))
        {
            Logger::Error(std::format("Jolt backend: RestoreState refused: {}", why));
            return false;
        }

        // The state as it is now, in case Jolt itself fails half way (it cannot after the checks above, unless its own
        // state and ours disagree): a failed restore puts this back
        std::vector<std::byte> rescue;
        std::string rescueWhy;
        const bool haveRescue = s.WriteSnapshot(rescue, false, rescueWhy);

        if (s.ApplySnapshot(snap))
        {
            return true;
        }
        Logger::Error("Jolt backend: RestoreState failed: Jolt could not restore its state");
        Snapshot before;
        if (haveRescue && ParseSnapshot(rescue, before, rescueWhy) && s.ApplySnapshot(before))
        {
            return false;
        }
        Logger::Error("Jolt backend: the simulation could not be put back as it was; the world is in an unknown state");
        return false;
    }

    std::optional<std::uint64_t> JoltBackend::HashState() const
    {
        const Impl &s = *_impl;
        if (!s.initialized)
        {
            return std::nullopt;
        }
        // Bodies in handle order (the slot order), what makes each of them what it is, and for one in the simulation its
        // pose, velocities and whether it is awake. Floats go in canonicalised (NaN, -0).
        StateHasher hasher;
        for (std::size_t i = 0; i < s.bodies.size(); ++i)
        {
            const Impl::BodyData &data = s.bodies[i];
            if (!data.active)
            {
                continue;
            }
            hasher.Word(static_cast<std::uint32_t>(i));
            hasher.Word(data.generation);
            hasher.Word(data.enabled ? 1u : 0u);
            hasher.Word(static_cast<std::uint32_t>(data.motion));
            if (!data.enabled)
            {
                continue;
            }
            JPH::RVec3 position;
            JPH::Quat rotation;
            s.Bodies().GetPositionAndRotation(data.id, position, rotation);
            hasher.Float(static_cast<float>(position.GetX()));
            hasher.Float(static_cast<float>(position.GetY()));
            hasher.Float(static_cast<float>(position.GetZ()));
            hasher.Float(rotation.GetX());
            hasher.Float(rotation.GetY());
            hasher.Float(rotation.GetZ());
            hasher.Float(rotation.GetW());
            if (data.motion != MotionKind::Static)
            {
                const JPH::Vec3 linear = s.Bodies().GetLinearVelocity(data.id);
                const JPH::Vec3 angular = s.Bodies().GetAngularVelocity(data.id);
                hasher.Float(linear.GetX());
                hasher.Float(linear.GetY());
                hasher.Float(linear.GetZ());
                hasher.Float(angular.GetX());
                hasher.Float(angular.GetY());
                hasher.Float(angular.GetZ());
                hasher.Word(s.Bodies().IsActive(data.id) ? 1u : 0u);
            }
        }
        return hasher.Value();
    }

    std::optional<std::uint64_t> JoltBackend::HashSimulatedState() const
    {
        const Impl &s = *_impl;
        if (!s.initialized)
        {
            return std::nullopt;
        }
        // Each body in the simulation on its own (no index, no generation), then an order-free sum: the slot a body
        // happens to occupy differs between a peer that parks despawned bodies and one that destroys them
        std::uint64_t sum = 0;
        std::uint32_t count = 0;
        for (const Impl::BodyData &data : s.bodies)
        {
            if (!data.active || !data.enabled)
            {
                continue;
            }
            StateHasher hasher;
            hasher.Word(static_cast<std::uint32_t>(data.motion));
            JPH::RVec3 position;
            JPH::Quat rotation;
            s.Bodies().GetPositionAndRotation(data.id, position, rotation);
            hasher.Float(static_cast<float>(position.GetX()));
            hasher.Float(static_cast<float>(position.GetY()));
            hasher.Float(static_cast<float>(position.GetZ()));
            hasher.Float(rotation.GetX());
            hasher.Float(rotation.GetY());
            hasher.Float(rotation.GetZ());
            hasher.Float(rotation.GetW());
            if (data.motion != MotionKind::Static)
            {
                const JPH::Vec3 linear = s.Bodies().GetLinearVelocity(data.id);
                const JPH::Vec3 angular = s.Bodies().GetAngularVelocity(data.id);
                hasher.Float(linear.GetX());
                hasher.Float(linear.GetY());
                hasher.Float(linear.GetZ());
                hasher.Float(angular.GetX());
                hasher.Float(angular.GetY());
                hasher.Float(angular.GetZ());
                hasher.Word(s.Bodies().IsActive(data.id) ? 1u : 0u);
            }
            sum += hasher.Value();
            ++count;
        }
        StateHasher total;
        total.Word(count);
        total.Word(static_cast<std::uint32_t>(sum & 0xFFFFFFFFu));
        total.Word(static_cast<std::uint32_t>(sum >> 32));
        return total.Value();
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

    int JoltBackend::GetJobSystemConcurrency() const
    {
        return _impl->jobSystem ? _impl->jobSystem->GetMaxConcurrency() : 0;
    }

    std::size_t JoltBackend::GetContactThreadCount() const
    {
        const std::lock_guard lock(_impl->contactMutex);
        return _impl->contactThreads.size();
    }

    int JoltBackend::GlobalInitCount()
    {
        const std::lock_guard lock(GlobalsMutex());
        return g_globalRefs;
    }
}

#endif // N2ENGINE_JOLT_ENABLED

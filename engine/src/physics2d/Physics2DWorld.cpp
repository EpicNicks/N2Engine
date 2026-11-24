// The only engine file that names Box2D. With N2ENGINE_USE_BOX2D=OFF, Create returns null and the rest of this file is not
// compiled, so a game that strips 2D physics carries no Box2D code (engine/CMakeLists.txt, docs/physics2d.html#stripping).

#include "engine/physics2d/Physics2DWorld.hpp"

#include "engine/Logger.hpp"

namespace N2Engine::Physics2D
{
    namespace
    {
        Math::Vector2 &DefaultGravity()
        {
            static Math::Vector2 gravity{0.0f, -9.81f};
            return gravity;
        }
    }

    Math::Vector2 Physics2DWorld::GetGravity()
    {
        return DefaultGravity();
    }
}

#ifndef N2ENGINE_BOX2D_ENABLED

namespace N2Engine::Physics2D
{
    std::shared_ptr<Physics2DWorld> Physics2DWorld::Create()
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Logger::Warn("This build has no 2D physics (N2ENGINE_USE_BOX2D is OFF): 2D components keep their data and do nothing");
        }
        return nullptr;
    }

    void Physics2DWorld::RefreshAllCollisionMatrices() {}

    void Physics2DWorld::SetGravity(const Math::Vector2 &gravity)
    {
        DefaultGravity() = gravity;
    }
}

#else

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>

#include <box2d/box2d.h>
#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include "ContactTracker2D.hpp"
#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Joint2D.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"

namespace N2Engine::Physics2D
{
    namespace
    {
        /// Above the 32 layers: every shape's mask has it and every query's category is it. Box2D also requires a shape's mask
        /// to accept a query's category, and without this the collision matrix would change what queries find.
        constexpr std::uint64_t kQueryCategory = 1ull << 32;
        constexpr int kSubSteps = 4;
        /// Every query and cast is clamped to this (QueryRules::kMaxSweepDistance): a longer translation loses Box2D's precision
        constexpr float kMaxDistance = Physics::QueryRules::kMaxSweepDistance;
        /// How far along a ray a start must still be inside a shape to count as started inside: above Box2D's 0.005 linear slop
        constexpr float kInsideProbe = 0.01f;
        /// A distance joint is never made shorter than this (Box2D clamps to its linear slop anyway)
        constexpr float kMinJointLength = 0.01f;
        /// PickRay: a ray whose unit direction has less z than this is taken as parallel to the 2D plane, which it cannot cross
        constexpr float kPickMinSlope = 1.0e-6f;

        using ShapeKey = ContactTracker2D::ShapeKey;

        b2Vec2 ToB2(const Math::Vector2 &v) { return b2Vec2{v.x, v.y}; }
        Math::Vector2 FromB2(const b2Vec2 v) { return Math::Vector2(v.x, v.y); }

        // The rollback state's bytes (SaveState): native byte order, they belong to the build that wrote them
        void PutBytes(std::vector<std::byte> &out, const void *data, const std::size_t size)
        {
            const auto *bytes = static_cast<const std::byte *>(data);
            out.insert(out.end(), bytes, bytes + size);
        }
        void PutU8(std::vector<std::byte> &out, const std::uint8_t value) { PutBytes(out, &value, sizeof(value)); }
        void PutU32(std::vector<std::byte> &out, const std::uint32_t value) { PutBytes(out, &value, sizeof(value)); }
        void PutU64(std::vector<std::byte> &out, const std::uint64_t value) { PutBytes(out, &value, sizeof(value)); }
        void PutF32(std::vector<std::byte> &out, const float value) { PutBytes(out, &value, sizeof(value)); }

        void PutTouches(std::vector<std::byte> &out, const ContactTracker2D::TouchMap &touches)
        {
            PutU32(out, static_cast<std::uint32_t>(touches.size()));
            for (const auto &entry : touches)
            {
                PutU32(out, entry.first.a.index);
                PutU32(out, entry.first.a.generation);
                PutU32(out, entry.first.b.index);
                PutU32(out, entry.first.b.generation);
                PutU32(out, static_cast<std::uint32_t>(entry.second.size()));
                for (const ContactTracker2D::ShapePair &pair : entry.second)
                {
                    PutU64(out, pair.onA);
                    PutU64(out, pair.onB);
                }
            }
        }

        /// Reads what the Put functions wrote; `ok` goes false at the first read past the end, and every later read gives 0
        struct StateReader
        {
            std::span<const std::byte> data;
            std::size_t position = 0;
            bool ok = true;

            [[nodiscard]] std::size_t Remaining() const { return data.size() - position; }

            template <typename T>
            T Get()
            {
                T value{};
                if (!ok || Remaining() < sizeof(T))
                {
                    ok = false;
                    return value;
                }
                std::memcpy(&value, data.data() + position, sizeof(T));
                position += sizeof(T);
                return value;
            }
            std::uint8_t U8() { return Get<std::uint8_t>(); }
            std::uint32_t U32() { return Get<std::uint32_t>(); }
            std::uint64_t U64() { return Get<std::uint64_t>(); }
            float F32() { return Get<float>(); }

            std::span<const std::byte> Take(const std::size_t size)
            {
                if (!ok || Remaining() < size)
                {
                    ok = false;
                    return {};
                }
                const std::span<const std::byte> taken = data.subspan(position, size);
                position += size;
                return taken;
            }

            ContactTracker2D::TouchMap Touches()
            {
                ContactTracker2D::TouchMap touches;
                const std::uint32_t count = U32();
                for (std::uint32_t i = 0; i < count && ok; ++i)
                {
                    ContactTracker2D::BodyPair pair;
                    pair.a.index = U32();
                    pair.a.generation = U32();
                    pair.b.index = U32();
                    pair.b.generation = U32();
                    const std::uint32_t shapes = U32();
                    if (!ok || shapes > Remaining() / 16)
                    {
                        ok = false;
                        break;
                    }
                    std::vector<ContactTracker2D::ShapePair> list;
                    for (std::uint32_t s = 0; s < shapes; ++s)
                    {
                        ContactTracker2D::ShapePair shape;
                        shape.onA = U64();
                        shape.onB = U64();
                        list.push_back(shape);
                    }
                    touches.emplace(pair, std::move(list));
                }
                return touches;
            }
        };

        b2Pos ToPos(const float x, const float y)
        {
            b2Pos pos{};
            pos.x = x;
            pos.y = y;
            return pos;
        }

        PhysicsBodyHandle ToHandle(const b2BodyId id)
        {
            PhysicsBodyHandle handle;
            handle.index = static_cast<decltype(handle.index)>(id.index1);
            handle.generation = static_cast<decltype(handle.generation)>(id.generation);
            return handle;
        }

        /// The rotation about z of any rotation, built without trigonometry so it is the same on every platform
        b2Rot ToRot(const Math::Quaternion &q)
        {
            const float w = q.GetW(), x = q.GetX(), y = q.GetY(), z = q.GetZ();
            const float cosine = 1.0f - 2.0f * (y * y + z * z);
            const float sine = 2.0f * (w * z + x * y);
            const float length = std::sqrt(cosine * cosine + sine * sine);
            if (!(length > 0.0f))
            {
                return b2Rot_identity;
            }
            return b2Rot{cosine / length, sine / length};
        }

        Math::Quaternion FromRot(const b2Rot rot)
        {
            // Half-angle formulas, from whichever of cosine and sine of the half angle is the larger
            if (rot.c >= 0.0f)
            {
                const float w = std::sqrt((1.0f + rot.c) * 0.5f);
                return Math::Quaternion(w, 0.0f, 0.0f, rot.s / (2.0f * w));
            }
            const float z = std::copysign(std::sqrt((1.0f - rot.c) * 0.5f), rot.s);
            return Math::Quaternion(rot.s / (2.0f * z), 0.0f, 0.0f, z);
        }

        b2Filter MakeFilter(const int layer)
        {
            b2Filter filter = b2DefaultFilter();
            filter.categoryBits = static_cast<std::uint64_t>(Layers::MaskOf(layer));
            filter.maskBits = static_cast<std::uint64_t>(Layers::CollisionMaskFor(layer)) | kQueryCategory;
            filter.groupIndex = 0;
            return filter;
        }

        b2BodyType ToB2(const BodyType2D type)
        {
            switch (type)
            {
            case BodyType2D::Dynamic:
                return b2_dynamicBody;
            case BodyType2D::Kinematic:
                return b2_kinematicBody;
            case BodyType2D::Static:
                break;
            }
            return b2_staticBody;
        }

        b2MotionLocks MakeLocks(const bool fixedRotation)
        {
            b2MotionLocks locks{};
            locks.angularZ = fixedRotation;
            return locks;
        }

        struct Sweep
        {
            b2Vec2 direction{};
            float distance = 0.0f;
        };

        /// False for a ray or cast that cannot hit (QueryRules)
        bool ReadSweep(const Math::Vector2 &origin, const Math::Vector2 &direction, const float maxDistance, Sweep &sweep)
        {
            if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(direction.x) || !std::isfinite(direction.y) ||
                !(maxDistance > 0.0f))
            {
                return false;
            }
            const float lengthSquared = direction.x * direction.x + direction.y * direction.y;
            if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared))
            {
                return false;
            }
            const float length = std::sqrt(lengthSquared);
            sweep.direction = b2Vec2{direction.x / length, direction.y / length};
            sweep.distance = std::min(maxDistance, kMaxDistance);
            return true;
        }

        bool IsPositiveFinite(const float value) { return value > 0.0f && std::isfinite(value); }
        bool IsFinite(const Math::Vector2 &v) { return std::isfinite(v.x) && std::isfinite(v.y); }

        /// `value`, or `fallback` with a warning: Box2D refuses negative and non-finite settings, and returns null ids for them
        float NonNegative(const float value, const float fallback, const char *what)
        {
            if (value >= 0.0f && std::isfinite(value))
            {
                return value;
            }
            Logger::Warn(std::format("2D physics: {} must be finite and not negative, not {}; using {}", what, value, fallback));
            return fallback;
        }

        float Finite(const float value, const float fallback, const char *what)
        {
            if (std::isfinite(value))
            {
                return value;
            }
            Logger::Warn(std::format("2D physics: {} must be finite; using {}", what, fallback));
            return fallback;
        }

        Math::Vector2 Finite(const Math::Vector2 &value, const char *what)
        {
            if (IsFinite(value))
            {
                return value;
            }
            Logger::Warn(std::format("2D physics: {} must be finite; using zero", what));
            return Math::Vector2();
        }

        struct RawHit
        {
            ShapeKey shape = 0;
            Math::Vector2 point;
            Math::Vector2 normal;
            float distance = 0.0f;
        };

        class Box2DWorld;

        struct QueryContext
        {
            const Box2DWorld *world = nullptr;
            QueryTriggers triggers = QueryTriggers::Collide;
            float sweepLength = 0.0f;
            std::vector<RawHit> hits;
            std::vector<ShapeKey> overlaps;
        };

        std::vector<Box2DWorld *> &LiveWorlds()
        {
            static std::vector<Box2DWorld *> worlds;
            return worlds;
        }

        class Box2DWorld final : public Physics2DWorld
        {
        public:
            Box2DWorld()
                : _tracker(ContactTracker2D::Host{
                      [this](const PhysicsBodyHandle handle, GameObject *&gameObject, Rigidbody2D *&rigidbody)
                      {
                          const auto it = _bodies.find(handle);
                          if (it == _bodies.end())
                          {
                              return false;
                          }
                          gameObject = it->second.owner;
                          rigidbody = it->second.rigidbody;
                          return true;
                      },
                      [this](const ShapeKey key) -> Collider2D *
                      {
                          const auto it = _shapes.find(key);
                          return it == _shapes.end() ? nullptr : it->second.collider;
                      },
                      [this](const ContactTracker2D::BodyPair &pair, const ContactTracker2D::ShapePair &shapes, ContactTracker2D::ContactData &data)
                      { ReadContacts(pair, shapes, data); }})
            {
                b2WorldDef def = b2DefaultWorldDef();
                def.gravity = ToB2(DefaultGravity());
                // One worker and no task callbacks: single-threaded, so the result cannot depend on a thread count
                def.workerCount = 1;
                def.enqueueTask = nullptr;
                def.finishTask = nullptr;
                _maxLinearSpeed = def.maximumLinearSpeed;
                _world = b2CreateWorld(&def);
                LiveWorlds().push_back(this);
            }

            ~Box2DWorld() override
            {
                std::erase(LiveWorlds(), this);
                b2DestroyWorld(_world);
            }

            void SetWorldGravity(const Math::Vector2 &gravity) override
            {
                if (!IsFinite(gravity))
                {
                    Logger::Warn("Physics2D: gravity must be finite; it was not changed");
                    return;
                }
                b2World_SetGravity(_world, ToB2(gravity));
                // Sleeping bodies would ignore it until something else woke them
                for (const auto &[handle, body] : _bodies)
                {
                    if (body.enabled && body.type == BodyType2D::Dynamic)
                    {
                        b2Body_SetAwake(body.id, true);
                    }
                }
            }

            [[nodiscard]] Math::Vector2 GetWorldGravity() const override { return FromB2(b2World_GetGravity(_world)); }

            void RefreshMatrix()
            {
                for (auto &[key, shape] : _shapes)
                {
                    const b2Filter filter = MakeFilter(shape.layer);
                    if (filter.maskBits != shape.maskBits)
                    {
                        shape.maskBits = filter.maskBits;
                        b2Shape_SetFilter(shape.id, filter);
                    }
                }
            }

            void ApplyPendingChanges() override
            {
                const float deltaTime = Time::GetFixedDeltaTime();
                for (auto &[handle, body] : _bodies)
                {
                    if (!body.dirty)
                    {
                        continue;
                    }
                    body.dirty = false;
                    if (!body.enabled || !body.owner)
                    {
                        continue; // enabling it reads the pose
                    }
                    const Positionable *positionable = body.owner->GetPositionable();
                    if (!positionable)
                    {
                        continue;
                    }
                    const Math::Vector3 position = positionable->GetPosition();
                    if (!std::isfinite(position.x) || !std::isfinite(position.y))
                    {
                        Logger::Warn(std::format("2D physics: '{}' has a position that is not finite; its body stays where it is", body.owner->GetName()));
                        continue;
                    }
                    const b2Rot rotation = ToRot(positionable->GetRotation());
                    // Box2D caps a body's speed, so a target further than one step at that speed would fall short, and the sync after
                    // the step would then pull the object back short of where the game put it. That move is a teleport instead.
                    const b2Pos current = b2Body_GetPosition(body.id);
                    const float moveX = position.x - static_cast<float>(current.x);
                    const float moveY = position.y - static_cast<float>(current.y);
                    const float reach = _maxLinearSpeed * deltaTime;
                    if (body.type == BodyType2D::Kinematic && deltaTime > 0.0f && moveX * moveX + moveY * moveY < reach * reach)
                    {
                        // The target replaces the velocity for one step; Step puts the game's velocity back
                        _targeted.push_back({handle, b2Body_GetLinearVelocity(body.id), b2Body_GetAngularVelocity(body.id)});
                        // A velocity that reaches the pose in one step, so it carries what stands on it
                        b2WorldTransform target{};
                        target.p = ToPos(position.x, position.y);
                        target.q = rotation;
                        b2Body_SetTargetTransform(body.id, target, deltaTime, true);
                    }
                    else
                    {
                        // b2Body_SetTransform wakes nothing: a sleeping body would hang where it was put, and one resting on a
                        // moved static body would float. Woken bodies also notice the contacts that this ended.
                        WakeBodiesTouching(body.id);
                        b2Body_SetTransform(body.id, ToPos(position.x, position.y), rotation);
                        if (body.type == BodyType2D::Dynamic)
                        {
                            b2Body_SetAwake(body.id, true);
                        }
                    }
                }
            }

            void Step(const float deltaTime) override
            {
                if (!(deltaTime > 0.0f))
                {
                    return;
                }
                EndUnclaimedJoints();
                b2World_Step(_world, deltaTime, kSubSteps);

                // The target's velocity lasts one step, or the body would carry on past its pose
                for (const Targeted &targeted : _targeted)
                {
                    if (const auto it = _bodies.find(targeted.body); it != _bodies.end())
                    {
                        b2Body_SetLinearVelocity(it->second.id, targeted.linearVelocity);
                        b2Body_SetAngularVelocity(it->second.id, targeted.angularVelocity);
                    }
                }
                _targeted.clear();

                SyncTransforms();
                ReadEvents();
                // Read before any handler runs: Box2D's event array does not survive a joint being destroyed
                const std::vector<JointHandle2D> broken = ReadBrokenJoints();
                _tracker.Dispatch();
                BreakJoints(broken);
            }

            PhysicsBodyHandle CreateBody(const BodyDesc2D &desc) override
            {
                b2BodyDef def = b2DefaultBodyDef();
                def.type = ToB2(desc.type);
                if (desc.owner)
                {
                    if (const Positionable *positionable = desc.owner->GetPositionable())
                    {
                        const Math::Vector3 position = positionable->GetPosition();
                        const Math::Vector2 start = Finite(Math::Vector2(position.x, position.y), "a body's position");
                        def.position = ToPos(start.x, start.y);
                        def.rotation = ToRot(positionable->GetRotation());
                    }
                }
                def.linearVelocity = ToB2(Finite(desc.linearVelocity, "linear velocity"));
                def.angularVelocity = Finite(desc.angularVelocity, 0.0f, "angular velocity");
                def.linearDamping = NonNegative(desc.linearDamping, 0.0f, "linear damping");
                def.angularDamping = NonNegative(desc.angularDamping, 0.0f, "angular damping");
                def.gravityScale = Finite(desc.gravityScale, 1.0f, "gravity scale");
                def.motionLocks = MakeLocks(desc.fixedRotation);
                def.isBullet = desc.bullet;
                def.isEnabled = desc.enabled;
                const b2BodyId id = b2CreateBody(_world, &def);
                if (!b2Body_IsValid(id))
                {
                    Logger::Warn("2D physics: Box2D refused to create a body");
                    return Physics::INVALID_PHYSICS_HANDLE;
                }
                const PhysicsBodyHandle handle = ToHandle(id);

                BodyRecord record;
                record.id = id;
                record.owner = desc.owner;
                record.rigidbody = desc.rigidbody;
                record.type = desc.type;
                record.fixedRotation = desc.fixedRotation;
                record.enabled = desc.enabled;
                _bodies.insert_or_assign(handle, std::move(record));
                return handle;
            }

            void DestroyBody(const PhysicsBodyHandle handle) override
            {
                const auto it = _bodies.find(handle);
                if (it == _bodies.end())
                {
                    return;
                }
                _tracker.EndBody(handle);
                for (Collider2D *collider : it->second.colliders)
                {
                    if (const auto shape = _colliderShapes.find(collider); shape != _colliderShapes.end())
                    {
                        _shapes.erase(shape->second);
                        _colliderShapes.erase(shape);
                    }
                }
                std::erase_if(_joints, [handle](const auto &entry) { return entry.second.self == handle || entry.second.other == handle; });
                b2DestroyBody(it->second.id); // Box2D ends the body's joints with it
                std::erase_if(_targeted, [handle](const Targeted &targeted) { return targeted.body == handle; });
                _bodies.erase(it);
            }

            void SetBodyEnabled(const PhysicsBodyHandle handle, const bool enabled) override
            {
                BodyRecord *body = Find(handle);
                if (!body || body->enabled == enabled)
                {
                    return;
                }
                body->enabled = enabled;
                if (enabled)
                {
                    _resyncTriggers = true;
                    // Back at its GameObject's current pose; pairs that still overlap start again with an Enter
                    if (const Positionable *positionable = body->owner ? body->owner->GetPositionable() : nullptr)
                    {
                        const Math::Vector3 position = positionable->GetPosition();
                        b2Body_SetTransform(body->id, ToPos(position.x, position.y), ToRot(positionable->GetRotation()));
                    }
                    b2Body_Enable(body->id);
                }
                else
                {
                    _tracker.EndBody(handle);
                    b2Body_Disable(body->id);
                }
            }

            void MarkTransformDirty(const PhysicsBodyHandle handle) override
            {
                if (BodyRecord *body = Find(handle))
                {
                    body->dirty = true;
                }
            }

            [[nodiscard]] bool IsSyncingTransforms(const PhysicsBodyHandle handle) const override
            {
                return _syncing && _syncingBody == handle;
            }

            void SetBodyType(const PhysicsBodyHandle handle, const BodyType2D type) override
            {
                if (BodyRecord *body = Find(handle); body && body->type != type)
                {
                    body->type = type;
                    b2Body_SetType(body->id, ToB2(type));
                    RefreshMass(*body);
                }
            }

            void SetLinearVelocity(const PhysicsBodyHandle handle, const Math::Vector2 &velocity) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_SetLinearVelocity(body->id, ToB2(Finite(velocity, "linear velocity")));
                }
            }

            [[nodiscard]] Math::Vector2 GetLinearVelocity(const PhysicsBodyHandle handle) const override
            {
                const BodyRecord *body = Find(handle);
                return body ? FromB2(b2Body_GetLinearVelocity(body->id)) : Math::Vector2();
            }

            void SetAngularVelocity(const PhysicsBodyHandle handle, const float radiansPerSecond) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_SetAngularVelocity(body->id, Finite(radiansPerSecond, 0.0f, "angular velocity"));
                }
            }

            [[nodiscard]] float GetAngularVelocity(const PhysicsBodyHandle handle) const override
            {
                const BodyRecord *body = Find(handle);
                return body ? b2Body_GetAngularVelocity(body->id) : 0.0f;
            }

            void SetDamping(const PhysicsBodyHandle handle, const float linear, const float angular) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_SetLinearDamping(body->id, NonNegative(linear, 0.0f, "linear damping"));
                    b2Body_SetAngularDamping(body->id, NonNegative(angular, 0.0f, "angular damping"));
                }
            }

            void SetGravityScale(const PhysicsBodyHandle handle, const float scale) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_SetGravityScale(body->id, Finite(scale, 1.0f, "gravity scale"));
                }
            }

            void SetFixedRotation(const PhysicsBodyHandle handle, const bool fixed) override
            {
                if (BodyRecord *body = Find(handle))
                {
                    body->fixedRotation = fixed;
                    b2Body_SetMotionLocks(body->id, MakeLocks(fixed));
                }
            }

            void SetBullet(const PhysicsBodyHandle handle, const bool bullet) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_SetBullet(body->id, bullet);
                }
            }

            void AddForce(const PhysicsBodyHandle handle, const Math::Vector2 &force) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_ApplyForceToCenter(body->id, ToB2(Finite(force, "a force")), true);
                }
            }

            void AddImpulse(const PhysicsBodyHandle handle, const Math::Vector2 &impulse) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_ApplyLinearImpulseToCenter(body->id, ToB2(Finite(impulse, "an impulse")), true);
                }
            }

            void AddTorque(const PhysicsBodyHandle handle, const float torque) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_ApplyTorque(body->id, Finite(torque, 0.0f, "a torque"), true);
                }
            }

            void AddAngularImpulse(const PhysicsBodyHandle handle, const float impulse) override
            {
                if (const BodyRecord *body = Find(handle))
                {
                    b2Body_ApplyAngularImpulse(body->id, Finite(impulse, 0.0f, "an angular impulse"), true);
                }
            }

            void SetMass(const PhysicsBodyHandle handle, const bool autoMass, const float mass) override
            {
                if (BodyRecord *body = Find(handle))
                {
                    body->autoMass = autoMass;
                    body->mass = mass;
                    RefreshMass(*body);
                }
            }

            [[nodiscard]] float GetMass(const PhysicsBodyHandle handle) const override
            {
                const BodyRecord *body = Find(handle);
                return body ? b2Body_GetMass(body->id) : 0.0f;
            }

            bool SetCollider(const PhysicsBodyHandle handle, Collider2D *collider, const ShapeDesc2D &desc) override
            {
                BodyRecord *body = Find(handle);
                if (!body)
                {
                    return false;
                }
                Geometry geometry;
                if (!BuildGeometry(desc, geometry))
                {
                    RemoveCollider(collider);
                    return false;
                }

                const auto existing = _colliderShapes.find(collider);
                if (existing != _colliderShapes.end())
                {
                    ShapeRecord &shape = _shapes.at(existing->second);
                    if (shape.body == handle && shape.isTrigger == desc.isTrigger && shape.isCircle == geometry.isCircle)
                    {
                        UpdateShape(shape, desc, geometry);
                        RefreshMass(*body);
                        return true;
                    }
                    RemoveCollider(collider); // Box2D cannot change a shape's sensor flag
                }

                b2ShapeDef def = b2DefaultShapeDef();
                def.material = MakeMaterial(desc);
                def.density = desc.isTrigger ? 0.0f : NonNegative(desc.density, 1.0f, "density"); // triggers add no mass, as in 3D
                def.filter = MakeFilter(desc.layer);
                def.isSensor = desc.isTrigger;
                def.enableSensorEvents = true;
                def.enableContactEvents = !desc.isTrigger;
                def.updateBodyMass = false;
                const b2ShapeId id = geometry.isCircle ? b2CreateCircleShape(body->id, &def, &geometry.circle)
                                                       : b2CreatePolygonShape(body->id, &def, &geometry.polygon);
                if (!b2Shape_IsValid(id))
                {
                    Logger::Warn("2D physics: Box2D refused to create a shape");
                    return false;
                }
                const ShapeKey key = b2StoreShapeId(id);

                ShapeRecord shape;
                shape.geometry = desc;
                shape.id = id;
                shape.collider = collider;
                shape.body = handle;
                shape.isTrigger = desc.isTrigger;
                shape.isCircle = geometry.isCircle;
                shape.layer = desc.layer;
                shape.maskBits = def.filter.maskBits;
                _shapes.insert_or_assign(key, shape);
                _colliderShapes[collider] = key;
                if (std::ranges::find(body->colliders, collider) == body->colliders.end())
                {
                    body->colliders.push_back(collider);
                }
                RefreshMass(*body);
                return true;
            }

            void RemoveCollider(Collider2D *collider) override
            {
                const auto existing = _colliderShapes.find(collider);
                if (existing == _colliderShapes.end())
                {
                    return;
                }
                const ShapeKey key = existing->second;
                const ShapeRecord shape = _shapes.at(key);
                _tracker.EndShape(key); // while the collider can still be read
                b2DestroyShape(shape.id, false);
                _shapes.erase(key);
                _colliderShapes.erase(existing);
                if (BodyRecord *body = Find(shape.body))
                {
                    std::erase(body->colliders, collider);
                    RefreshMass(*body);
                }
            }

            bool Raycast(const Math::Vector2 &origin, const Math::Vector2 &direction, RaycastHit2D &hit, const float maxDistance,
                         const std::uint32_t layerMask, const QueryTriggers triggers) override
            {
                hit = RaycastHit2D{};
                std::vector<RaycastHit2D> hits;
                if (RaycastAll(origin, direction, hits, maxDistance, layerMask, triggers) == 0)
                {
                    return false;
                }
                hit = hits.front();
                return true;
            }

            int RaycastAll(const Math::Vector2 &origin, const Math::Vector2 &direction, std::vector<RaycastHit2D> &hits,
                           const float maxDistance, const std::uint32_t layerMask, const QueryTriggers triggers) override
            {
                hits.clear();
                Sweep sweep;
                if (!ReadSweep(origin, direction, maxDistance, sweep) || layerMask == 0)
                {
                    return 0;
                }
                QueryContext context{this, triggers, sweep.distance, {}, {}};
                const b2Vec2 translation = b2MulSV(sweep.distance, sweep.direction);
                b2World_CastRay(_world, ToPos(origin.x, origin.y), translation, MakeQueryFilter(layerMask), &CollectCast, &context);

                // Box2D's ray misses a shape it starts inside, and QueryRules says that is a hit at distance 0
                const b2Vec2 point{0.0f, 0.0f};
                const b2ShapeProxy proxy = b2MakeProxy(&point, 1, 0.0f);
                b2World_OverlapShape(_world, ToPos(origin.x, origin.y), &proxy, MakeQueryFilter(layerMask), &CollectOverlap, &context);
                // A point on a surface (the last hit of a ricochet) counts as overlapping, so it must also still be inside just along the ray
                const b2Pos ahead = ToPos(origin.x + sweep.direction.x * kInsideProbe, origin.y + sweep.direction.y * kInsideProbe);
                // Box2D's ray reports that same surface too, at fraction 0
                std::erase_if(context.hits, [&ahead](const RawHit &raw)
                              { return !(raw.distance > 0.0f) && !b2Shape_TestPoint(b2LoadShapeId(raw.shape), ahead); });
                for (const ShapeKey shape : context.overlaps)
                {
                    if (!b2Shape_TestPoint(b2LoadShapeId(shape), ahead))
                    {
                        continue;
                    }
                    std::erase_if(context.hits, [shape](const RawHit &raw) { return raw.shape == shape; });
                    context.hits.push_back({shape, origin, Math::Vector2(-sweep.direction.x, -sweep.direction.y), 0.0f});
                }
                return FinishHits(context.hits, hits);
            }

            bool CircleCast(const Math::Vector2 &origin, const float radius, const Math::Vector2 &direction, RaycastHit2D &hit,
                            const float maxDistance, const std::uint32_t layerMask, const QueryTriggers triggers) override
            {
                hit = RaycastHit2D{};
                if (!IsPositiveFinite(radius))
                {
                    return false;
                }
                const b2Vec2 point{0.0f, 0.0f};
                return Cast(origin, b2MakeProxy(&point, 1, radius), direction, hit, maxDistance, layerMask, triggers);
            }

            bool BoxCast(const Math::Vector2 &origin, const Math::Vector2 &halfExtents, const float angleRadians, const Math::Vector2 &direction,
                         RaycastHit2D &hit, const float maxDistance, const std::uint32_t layerMask, const QueryTriggers triggers) override
            {
                hit = RaycastHit2D{};
                if (!IsPositiveFinite(halfExtents.x) || !IsPositiveFinite(halfExtents.y) || !std::isfinite(angleRadians))
                {
                    return false;
                }
                b2Vec2 corners[4];
                MakeBoxCorners(halfExtents, angleRadians, corners);
                return Cast(origin, b2MakeProxy(corners, 4, 0.0f), direction, hit, maxDistance, layerMask, triggers);
            }

            int OverlapCircle(const Math::Vector2 &center, const float radius, std::vector<OverlapHit2D> &hits, const std::uint32_t layerMask,
                              const QueryTriggers triggers) override
            {
                hits.clear();
                if (!IsPositiveFinite(radius))
                {
                    return 0;
                }
                const b2Vec2 point{0.0f, 0.0f};
                return Overlap(center, b2MakeProxy(&point, 1, radius), hits, layerMask, triggers);
            }

            int OverlapBox(const Math::Vector2 &center, const Math::Vector2 &halfExtents, const float angleRadians, std::vector<OverlapHit2D> &hits,
                           const std::uint32_t layerMask, const QueryTriggers triggers) override
            {
                hits.clear();
                if (!IsPositiveFinite(halfExtents.x) || !IsPositiveFinite(halfExtents.y) || !std::isfinite(angleRadians))
                {
                    return 0;
                }
                b2Vec2 corners[4];
                MakeBoxCorners(halfExtents, angleRadians, corners);
                return Overlap(center, b2MakeProxy(corners, 4, 0.0f), hits, layerMask, triggers);
            }

            // ===== Joints =====
            JointHandle2D CreateJoint(const JointDesc2D &desc) override
            {
                const BodyRecord *self = Find(desc.self);
                const bool connected = desc.type != JointType2D::Target && desc.other.IsValid();
                const BodyRecord *other = connected ? Find(desc.other) : nullptr;
                if (!self || (connected && (!other || desc.other == desc.self)))
                {
                    return 0;
                }
                const b2BodyId otherId = other ? other->id : Ground();
                if (!b2Body_IsValid(otherId))
                {
                    return 0;
                }

                // The frames: this body's at the anchor, turned to the axis; the other's the same frame now, seen from it. So
                // the current pose is the joint's zero.
                const b2Vec2 axis = UnitAxis(desc.axis);
                const b2Rot axisRotation = b2MakeRotFromUnitVector(axis);
                const b2Rot worldRotation = b2MulRot(b2Body_GetRotation(self->id), axisRotation);
                b2Transform frameSelf{ToB2(desc.anchor), axisRotation};
                b2Transform frameOther{ToB2(desc.connectedAnchor), b2InvMulRot(b2Body_GetRotation(otherId), worldRotation)};

                b2JointId id = b2_nullJointId;
                // Box2D's body B is this one, so an angle, translation or motor speed is this body's relative to the other (a
                // positive hinge angle turns it counterclockwise), and a slider's or wheel's axis is fixed to the other body
                const auto base = [&](b2JointDef &def)
                {
                    def.bodyIdA = otherId;
                    def.bodyIdB = self->id;
                    def.localFrameA = frameOther;
                    def.localFrameB = frameSelf;
                    def.collideConnected = desc.collideConnected;
                    def.forceThreshold = Threshold(desc.breakForce);
                    def.torqueThreshold = Threshold(desc.breakTorque);
                };
                switch (desc.type)
                {
                case JointType2D::Fixed:
                {
                    b2WeldJointDef def = b2DefaultWeldJointDef();
                    base(def.base);
                    def.linearHertz = desc.useSpring ? desc.springFrequency : 0.0f;
                    def.angularHertz = def.linearHertz;
                    def.linearDampingRatio = desc.springDamping;
                    def.angularDampingRatio = desc.springDamping;
                    id = b2CreateWeldJoint(_world, &def);
                    break;
                }
                case JointType2D::Hinge:
                {
                    b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
                    base(def.base);
                    def.enableSpring = desc.useSpring;
                    def.hertz = desc.springFrequency;
                    def.dampingRatio = desc.springDamping;
                    def.enableLimit = desc.useLimits;
                    def.lowerAngle = desc.limitMin;
                    def.upperAngle = desc.limitMax;
                    def.enableMotor = desc.useMotor;
                    def.motorSpeed = desc.motorSpeed;
                    def.maxMotorTorque = desc.maxMotorForce;
                    id = b2CreateRevoluteJoint(_world, &def);
                    break;
                }
                case JointType2D::Slider:
                {
                    b2PrismaticJointDef def = b2DefaultPrismaticJointDef();
                    base(def.base);
                    def.enableSpring = desc.useSpring;
                    def.hertz = desc.springFrequency;
                    def.dampingRatio = desc.springDamping;
                    def.enableLimit = desc.useLimits;
                    def.lowerTranslation = desc.limitMin;
                    def.upperTranslation = desc.limitMax;
                    def.enableMotor = desc.useMotor;
                    def.motorSpeed = desc.motorSpeed;
                    def.maxMotorForce = desc.maxMotorForce;
                    id = b2CreatePrismaticJoint(_world, &def);
                    break;
                }
                case JointType2D::Wheel:
                {
                    b2WheelJointDef def = b2DefaultWheelJointDef();
                    base(def.base);
                    def.enableSpring = desc.useSpring;
                    def.hertz = desc.springFrequency;
                    def.dampingRatio = desc.springDamping;
                    def.enableLimit = desc.useLimits;
                    def.lowerTranslation = desc.limitMin;
                    def.upperTranslation = desc.limitMax;
                    def.enableMotor = desc.useMotor;
                    def.motorSpeed = desc.motorSpeed;
                    def.maxMotorTorque = desc.maxMotorForce;
                    id = b2CreateWheelJoint(_world, &def);
                    break;
                }
                case JointType2D::Distance:
                {
                    b2DistanceJointDef def = b2DefaultDistanceJointDef();
                    base(def.base);
                    const b2Pos selfPoint = b2Body_GetWorldPoint(self->id, ToB2(desc.anchor));
                    const b2Pos otherPoint = b2Body_GetWorldPoint(otherId, ToB2(desc.connectedAnchor));
                    def.length = std::max(b2Length(b2SubPos(otherPoint, selfPoint)), kMinJointLength);
                    // A spring of 0 Hz with limits is a rope: free inside them
                    def.enableSpring = desc.useSpring || desc.useLimits;
                    def.hertz = desc.useSpring ? desc.springFrequency : 0.0f;
                    def.dampingRatio = desc.springDamping;
                    def.enableLimit = desc.useLimits;
                    if (desc.useLimits) // else Box2D's defaults: no limit
                    {
                        def.minLength = desc.limitMin;
                        def.maxLength = std::max(desc.limitMax, desc.limitMin);
                    }
                    def.enableMotor = desc.useMotor;
                    def.motorSpeed = desc.motorSpeed;
                    def.maxMotorForce = desc.maxMotorForce;
                    id = b2CreateDistanceJoint(_world, &def);
                    break;
                }
                case JointType2D::Target:
                {
                    // Box2D's motor joint from a frame at the target on the ground body to the anchor on this body, with only
                    // its linear spring: Box2D v3's replacement for the mouse joint
                    b2MotorJointDef def = b2DefaultMotorJointDef();
                    frameOther = b2Transform{ToB2(desc.connectedAnchor), b2Rot_identity};
                    frameSelf = b2Transform{ToB2(desc.anchor), b2Rot_identity};
                    base(def.base);
                    def.linearHertz = desc.springFrequency;
                    def.linearDampingRatio = desc.springDamping;
                    def.maxSpringForce = desc.maxForce;
                    id = b2CreateMotorJoint(_world, &def);
                    break;
                }
                }
                if (!b2Joint_IsValid(id))
                {
                    Logger::Warn("2D physics: Box2D refused to create a joint");
                    return 0;
                }
                const JointHandle2D key = b2StoreJointId(id);
                JointRecord record;
                record.id = id;
                record.type = desc.type;
                record.self = desc.self;
                record.other = connected ? desc.other : Physics::INVALID_PHYSICS_HANDLE;
                record.owner = desc.owner;
                record.breakable = desc.breakForce > 0.0f || desc.breakTorque > 0.0f;
                _joints.insert_or_assign(key, record);
                return key;
            }

            void DestroyJoint(const JointHandle2D joint) override
            {
                const auto it = _joints.find(joint);
                if (it == _joints.end())
                {
                    return;
                }
                if (b2Joint_IsValid(it->second.id))
                {
                    b2Joint_WakeBodies(it->second.id);
                    b2DestroyJoint(it->second.id);
                }
                _joints.erase(it);
            }

            void ConfigureJoint(const JointHandle2D joint, const JointDesc2D &desc) override
            {
                const auto it = _joints.find(joint);
                if (it == _joints.end() || !b2Joint_IsValid(it->second.id))
                {
                    return;
                }
                const b2JointId id = it->second.id;
                b2Joint_SetCollideConnected(id, desc.collideConnected);
                b2Joint_SetForceThreshold(id, Threshold(desc.breakForce));
                b2Joint_SetTorqueThreshold(id, Threshold(desc.breakTorque));
                it->second.breakable = desc.breakForce > 0.0f || desc.breakTorque > 0.0f;
                switch (it->second.type)
                {
                case JointType2D::Fixed:
                {
                    const float hertz = desc.useSpring ? desc.springFrequency : 0.0f;
                    b2WeldJoint_SetLinearHertz(id, hertz);
                    b2WeldJoint_SetAngularHertz(id, hertz);
                    b2WeldJoint_SetLinearDampingRatio(id, desc.springDamping);
                    b2WeldJoint_SetAngularDampingRatio(id, desc.springDamping);
                    break;
                }
                case JointType2D::Hinge:
                    b2RevoluteJoint_EnableSpring(id, desc.useSpring);
                    b2RevoluteJoint_SetSpringHertz(id, desc.springFrequency);
                    b2RevoluteJoint_SetSpringDampingRatio(id, desc.springDamping);
                    b2RevoluteJoint_EnableLimit(id, desc.useLimits);
                    if (desc.useLimits)
                    {
                        b2RevoluteJoint_SetLimits(id, desc.limitMin, desc.limitMax);
                    }
                    b2RevoluteJoint_EnableMotor(id, desc.useMotor);
                    b2RevoluteJoint_SetTargetMotorSpeed(id, desc.motorSpeed);
                    b2RevoluteJoint_SetMaxMotorTorque(id, desc.maxMotorForce);
                    break;
                case JointType2D::Slider:
                    b2PrismaticJoint_EnableSpring(id, desc.useSpring);
                    b2PrismaticJoint_SetSpringHertz(id, desc.springFrequency);
                    b2PrismaticJoint_SetSpringDampingRatio(id, desc.springDamping);
                    b2PrismaticJoint_EnableLimit(id, desc.useLimits);
                    if (desc.useLimits)
                    {
                        b2PrismaticJoint_SetLimits(id, desc.limitMin, desc.limitMax);
                    }
                    b2PrismaticJoint_EnableMotor(id, desc.useMotor);
                    b2PrismaticJoint_SetTargetMotorSpeed(id, desc.motorSpeed);
                    b2PrismaticJoint_SetMaxMotorForce(id, desc.maxMotorForce);
                    break;
                case JointType2D::Wheel:
                    b2WheelJoint_EnableSpring(id, desc.useSpring);
                    b2WheelJoint_SetSpringHertz(id, desc.springFrequency);
                    b2WheelJoint_SetSpringDampingRatio(id, desc.springDamping);
                    b2WheelJoint_EnableLimit(id, desc.useLimits);
                    if (desc.useLimits)
                    {
                        b2WheelJoint_SetLimits(id, desc.limitMin, desc.limitMax);
                    }
                    b2WheelJoint_EnableMotor(id, desc.useMotor);
                    b2WheelJoint_SetTargetMotorSpeed(id, desc.motorSpeed);
                    b2WheelJoint_SetMaxMotorTorque(id, desc.maxMotorForce);
                    break;
                case JointType2D::Distance:
                    b2DistanceJoint_EnableSpring(id, desc.useSpring || desc.useLimits);
                    b2DistanceJoint_SetSpringHertz(id, desc.useSpring ? desc.springFrequency : 0.0f);
                    b2DistanceJoint_SetSpringDampingRatio(id, desc.springDamping);
                    b2DistanceJoint_EnableLimit(id, desc.useLimits);
                    if (desc.useLimits)
                    {
                        b2DistanceJoint_SetLimits(id, desc.limitMin, std::max(desc.limitMax, desc.limitMin));
                    }
                    b2DistanceJoint_EnableMotor(id, desc.useMotor);
                    b2DistanceJoint_SetTargetMotorSpeed(id, desc.motorSpeed);
                    b2DistanceJoint_SetMaxMotorForce(id, desc.maxMotorForce);
                    break;
                case JointType2D::Target:
                    b2Joint_SetLocalFrameA(id, b2Transform{ToB2(desc.connectedAnchor), b2Rot_identity});
                    b2MotorJoint_SetLinearHertz(id, desc.springFrequency);
                    b2MotorJoint_SetLinearDampingRatio(id, desc.springDamping);
                    b2MotorJoint_SetMaxSpringForce(id, desc.maxForce);
                    break;
                }
                b2Joint_WakeBodies(id);
            }

            [[nodiscard]] bool JointHolds(const JointHandle2D joint, const JointDesc2D &desc) const override
            {
                const auto it = _joints.find(joint);
                const PhysicsBodyHandle other = desc.type == JointType2D::Target ? Physics::INVALID_PHYSICS_HANDLE : desc.other;
                return it != _joints.end() && it->second.owner == desc.owner && it->second.self == desc.self &&
                       it->second.other == other && b2Joint_IsValid(it->second.id);
            }

            JointHandle2D ClaimJoint(const JointDesc2D &desc) override
            {
                const PhysicsBodyHandle other = desc.type == JointType2D::Target ? Physics::INVALID_PHYSICS_HANDLE : desc.other;
                for (auto &[key, record] : _joints)
                {
                    if (!record.owner && record.type == desc.type && record.self == desc.self && record.other == other &&
                        b2Joint_IsValid(record.id))
                    {
                        // Its saved settings stay as they were: setting them again would wake its bodies, which the run that
                        // saved the state did not do
                        record.owner = desc.owner;
                        return key;
                    }
                }
                return 0;
            }

            [[nodiscard]] JointReading2D ReadJoint(const JointHandle2D joint) const override
            {
                JointReading2D reading;
                const auto it = _joints.find(joint);
                if (it == _joints.end() || !b2Joint_IsValid(it->second.id))
                {
                    return reading;
                }
                const b2JointId id = it->second.id;
                reading.reactionForce = FromB2(b2Joint_GetConstraintForce(id));
                reading.reactionTorque = b2Joint_GetConstraintTorque(id);
                switch (it->second.type)
                {
                case JointType2D::Hinge:
                    reading.angle = b2RevoluteJoint_GetCurrentAngle(id);
                    break;
                case JointType2D::Slider:
                    reading.translation = b2PrismaticJoint_GetCurrentTranslation(id);
                    break;
                case JointType2D::Distance:
                    reading.translation = b2DistanceJoint_GetCurrentLength(id);
                    break;
                default:
                    break;
                }
                return reading;
            }

            bool PickRay(const Math::Vector3 &origin, const Math::Vector3 &direction, RaycastHit2D &hit, const float maxDistance,
                         const std::uint32_t layerMask, const QueryTriggers triggers) override
            {
                hit = RaycastHit2D{};
                const bool finite = std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) &&
                                    std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z);
                const float lengthSquared = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
                if (!finite || !(maxDistance > 0.0f) || layerMask == 0 || !(lengthSquared > 0.0f) || !std::isfinite(lengthSquared))
                {
                    return false;
                }
                const float length = std::sqrt(lengthSquared);
                const float dx = direction.x / length;
                const float dy = direction.y / length;
                const float dz = direction.z / length;
                if (std::abs(dz) < kPickMinSlope)
                {
                    return false; // edge-on to every shape's plane
                }
                float reach = std::min(maxDistance, kMaxDistance);
                const float planar = std::sqrt(dx * dx + dy * dy);
                if (planar * reach > kMaxDistance)
                {
                    reach = kMaxDistance / planar;
                }

                // The candidates: every shape that contains a point of the ray's shadow on the plane, which is every shape the
                // ray can cross in its own plane. Those that contain the shadow's start (Box2D's ray misses a shape it starts
                // inside, and an orthographic camera's shadow is that single point), and those the shadow crosses.
                QueryContext context{this, triggers, reach, {}, {}};
                const b2QueryFilter filter = MakeQueryFilter(layerMask);
                const b2Vec2 point{0.0f, 0.0f};
                const b2ShapeProxy proxy = b2MakeProxy(&point, 1, 0.0f);
                b2World_OverlapShape(_world, ToPos(origin.x, origin.y), &proxy, filter, &CollectOverlap, &context);
                const b2Vec2 shadow{dx * reach, dy * reach};
                if (shadow.x != 0.0f || shadow.y != 0.0f)
                {
                    b2World_CastRay(_world, ToPos(origin.x, origin.y), shadow, filter, &CollectCast, &context);
                }
                std::vector<ShapeKey> candidates = std::move(context.overlaps);
                for (const RawHit &raw : context.hits)
                {
                    candidates.push_back(raw.shape);
                }
                std::ranges::sort(candidates);
                candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

                // Each candidate exactly: where the ray crosses its own plane, and whether the shape contains that point
                std::vector<RawHit> raw;
                for (const ShapeKey key : candidates)
                {
                    const ShapeRecord &shape = _shapes.at(key);
                    const BodyRecord *body = Find(shape.body);
                    GameObject *object = shape.collider ? &shape.collider->GetGameObject() : body ? body->owner : nullptr;
                    const Positionable *positionable = object ? object->GetPositionable() : nullptr;
                    if (!positionable)
                    {
                        continue;
                    }
                    const float distance = (positionable->GetPosition().z - origin.z) / dz;
                    if (!(distance >= 0.0f) || distance > reach)
                    {
                        continue; // behind the ray's start, or beyond its end
                    }
                    const Math::Vector2 at(origin.x + dx * distance, origin.y + dy * distance);
                    if (b2Shape_TestPoint(shape.id, ToPos(at.x, at.y)))
                    {
                        raw.push_back({key, at, Math::Vector2(), distance});
                    }
                }
                std::vector<RaycastHit2D> hits;
                if (FinishHits(raw, hits) == 0)
                {
                    return false;
                }
                hit = hits.front();
                return true;
            }

            [[nodiscard]] std::uint64_t HashState() const override { return b2World_GetStateHash(_world); }

            [[nodiscard]] std::uint64_t HashSimulatedState() const override
            {
                std::uint64_t hash = 14695981039346656037ull;
                const auto mix = [&hash](const void *data, const std::size_t size)
                {
                    const auto *bytes = static_cast<const unsigned char *>(data);
                    for (std::size_t i = 0; i < size; ++i)
                    {
                        hash = (hash ^ static_cast<std::uint64_t>(bytes[i])) * 1099511628211ull;
                    }
                };
                const auto mixFloat = [&mix](const float value) { mix(&value, sizeof(value)); };
                const auto mixDouble = [&mix](const double value) { mix(&value, sizeof(value)); };
                // _bodies is ordered by handle; the handle itself is not mixed, only what the body is
                for (const auto &entry : _bodies)
                {
                    const BodyRecord &body = entry.second;
                    if (!body.enabled)
                    {
                        continue;
                    }
                    const b2Pos position = b2Body_GetPosition(body.id);
                    const b2Rot rotation = b2Body_GetRotation(body.id);
                    const b2Vec2 velocity = b2Body_GetLinearVelocity(body.id);
                    mixDouble(static_cast<double>(position.x));
                    mixDouble(static_cast<double>(position.y));
                    mixFloat(rotation.c);
                    mixFloat(rotation.s);
                    mixFloat(velocity.x);
                    mixFloat(velocity.y);
                    mixFloat(b2Body_GetAngularVelocity(body.id));
                    const unsigned char flags = static_cast<unsigned char>((b2Body_IsAwake(body.id) ? 1u : 0u) |
                                                                           (static_cast<unsigned>(body.type) << 1));
                    mix(&flags, 1);
                }
                return hash;
            }

            // ===== Rollback =====
            // The state is Box2D's snapshot image plus what this class keeps beside it. Bodies are named by their Box2D id
            // and shapes by their shape key, both of which stay valid across b2World_RestoreFromSnapshot; the colliders
            // behind a body's shapes by their index among the Collider2D components of the body's GameObject (the same on
            // every build that makes the same scene), never by address.
            static constexpr std::uint32_t kStateMagic = 0x3250324Eu; // "N2P2"
            static constexpr std::uint32_t kStateVersion = 2; // 2: the joints and the ground body (issue #445)

            [[nodiscard]] bool SaveState(std::vector<std::byte> &out) const override
            {
                out.clear();
                const int size = b2World_GetSnapshot(_world, nullptr, 0);
                if (size <= 0)
                {
                    Logger::Warn("2D physics: Box2D would not take a snapshot (the world is mid-step)");
                    return false;
                }
                PutU32(out, kStateMagic);
                PutU32(out, kStateVersion);
                PutU32(out, static_cast<std::uint32_t>(size));
                const std::size_t at = out.size();
                out.resize(at + static_cast<std::size_t>(size));
                if (b2World_GetSnapshot(_world, reinterpret_cast<std::uint8_t *>(out.data() + at), size) != size)
                {
                    Logger::Warn("2D physics: Box2D wrote a snapshot of another size than it announced");
                    out.clear();
                    return false;
                }

                PutU32(out, static_cast<std::uint32_t>(_bodies.size()));
                for (const auto &entry : _bodies)
                {
                    const BodyRecord &body = entry.second;
                    PutU64(out, b2StoreBodyId(body.id));
                    PutU8(out, body.enabled ? 1 : 0);
                    PutU8(out, static_cast<std::uint8_t>(body.type));
                    PutU8(out, body.fixedRotation ? 1 : 0);
                    PutU8(out, body.autoMass ? 1 : 0);
                    PutF32(out, body.mass);
                    PutU32(out, static_cast<std::uint32_t>(body.colliders.size()));
                    const std::vector<Collider2D *> siblings =
                        body.owner ? body.owner->GetComponents<Collider2D>() : std::vector<Collider2D *>{};
                    for (Collider2D *collider : body.colliders)
                    {
                        const auto shape = _colliderShapes.find(collider);
                        const auto position = std::ranges::find(siblings, collider);
                        if (shape == _colliderShapes.end() || position == siblings.end())
                        {
                            Logger::Warn("2D physics: a shape's collider is not on its body's GameObject; the state cannot be saved");
                            out.clear();
                            return false;
                        }
                        PutU64(out, shape->second);
                        PutU32(out, static_cast<std::uint32_t>(position - siblings.begin()));
                    }
                }
                PutTouches(out, _tracker.Collisions());
                PutTouches(out, _tracker.Triggers());
                PutU8(out, _resyncTriggers ? 1 : 0);
                // The joints, by Box2D id, with their type and bodies (their owners are found again by ClaimJoint)
                PutU64(out, b2Body_IsValid(_ground) ? b2StoreBodyId(_ground) : 0);
                PutU32(out, static_cast<std::uint32_t>(_joints.size()));
                for (const auto &[key, record] : _joints)
                {
                    PutU64(out, key);
                    PutU8(out, static_cast<std::uint8_t>(record.type));
                    PutU8(out, record.breakable ? 1 : 0);
                    PutU32(out, record.self.index);
                    PutU32(out, record.self.generation);
                    PutU32(out, record.other.index);
                    PutU32(out, record.other.generation);
                }
                return true;
            }

            [[nodiscard]] bool RestoreState(const std::span<const std::byte> in) override { return RestoreImpl(in, nullptr); }

            [[nodiscard]] JoinRestore2D RestoreStateForJoin(const std::span<const std::byte> in,
                                                            const std::span<const BodyRename> renames) override
            {
                JoinRestore2D result;
                // Box2D refuses an image of another version, precision or layout before it touches the world: compare the
                // image's 16-byte header (magic, version, layout hash, flags) with one of this world's own, so a refusal
                // is told apart from a payload that breaks the world halfway
                {
                    StateReader header{in};
                    if (header.U32() != kStateMagic || header.U32() != kStateVersion)
                    {
                        Logger::Warn("2D physics: the joined state is not one of this build's 2D worlds");
                        return result;
                    }
                    const std::uint32_t imageSize = header.U32();
                    const std::span<const std::byte> image = header.Take(imageSize);
                    const int ownSize = b2World_GetSnapshot(_world, nullptr, 0);
                    std::vector<std::uint8_t> own(static_cast<std::size_t>(std::max(ownSize, 0)));
                    constexpr std::size_t kHeaderBytes = 16;
                    if (!header.ok || image.size() < kHeaderBytes || own.size() < kHeaderBytes ||
                        b2World_GetSnapshot(_world, own.data(), ownSize) != ownSize ||
                        std::memcmp(image.data(), own.data(), kHeaderBytes) != 0)
                    {
                        Logger::Warn("2D physics: the joined state's Box2D image is not one this build reads (another Box2D "
                                     "version, precision or layout); the 2D world is left as this peer built it");
                        return result;
                    }
                }

                // Every renamed body takes the handle the state gives it, with its components; one that is not renamed keeps
                // its own unless a renamed body takes it (then it is not the state's body and loses it). Worked out from the
                // handles before anything moves, so a chain of renames (a to b, b to c) is not confused.
                std::map<PhysicsBodyHandle, PhysicsBodyHandle, HandleLess> target;
                std::set<PhysicsBodyHandle, HandleLess> taken;
                // A body whose rename names a handle another body already took (a state that does not fit this world): it
                // is not the state's body under any handle, so it loses its body rather than keep a handle of the state
                std::set<PhysicsBodyHandle, HandleLess> refused;
                for (const BodyRename &rename : renames)
                {
                    if (!_bodies.contains(rename.here) || target.contains(rename.here))
                    {
                        continue;
                    }
                    if (rename.saved.IsValid() && !taken.contains(rename.saved))
                    {
                        target.emplace(rename.here, rename.saved);
                        taken.insert(rename.saved);
                    }
                    else
                    {
                        refused.insert(rename.here);
                    }
                }
                for (const PhysicsBodyHandle &handle : target | std::views::keys)
                {
                    refused.erase(handle);
                }
                std::map<PhysicsBodyHandle, BodyRecord, HandleLess> renamed;
                std::vector<std::pair<Rigidbody2D *, PhysicsBodyHandle>> rigidbodies;
                std::vector<std::pair<Collider2D *, PhysicsBodyHandle>> colliders;
                const auto collidersOf = [](const BodyRecord &record, const PhysicsBodyHandle handle)
                {
                    std::vector<Collider2D *> out;
                    if (record.owner)
                    {
                        for (Collider2D *collider : record.owner->GetComponents<Collider2D>())
                        {
                            if (collider->GetHandle() == handle)
                            {
                                out.push_back(collider);
                            }
                        }
                    }
                    return out;
                };
                for (auto &entry : _bodies)
                {
                    const auto to = target.find(entry.first);
                    const bool moves = to != target.end();
                    if (!moves && !taken.contains(entry.first) && !refused.contains(entry.first))
                    {
                        renamed.emplace(entry.first, std::move(entry.second));
                        continue;
                    }
                    BodyRecord &record = entry.second;
                    if (!moves)
                    {
                        // Its handle is another body's in the state, or its rename did not fit
                        if (record.rigidbody)
                        {
                            record.rigidbody->OnBodyLost();
                        }
                        for (Collider2D *collider : collidersOf(record, entry.first))
                        {
                            collider->OnBodyLost();
                        }
                        ++result.lost;
                        continue;
                    }
                    if (record.rigidbody)
                    {
                        rigidbodies.emplace_back(record.rigidbody, to->second);
                    }
                    for (Collider2D *collider : collidersOf(record, entry.first))
                    {
                        colliders.emplace_back(collider, to->second);
                    }
                    record.id = BodyIdOf(to->second);
                    renamed.emplace(to->second, std::move(record));
                }
                for (const auto &[rigidbody, handle] : rigidbodies)
                {
                    rigidbody->OnBodyRenamed(handle);
                }
                for (const auto &[collider, handle] : colliders)
                {
                    collider->OnBodyRenamed(handle);
                }
                _bodies = std::move(renamed);
                // The shape records name the old handles; RestoreImpl rebuilds them, the touches and the targets from the state
                _shapes.clear();
                _colliderShapes.clear();
                _targeted.clear();

                RestoreCounts counts;
                result.status = RestoreImpl(in, &counts) ? JoinRestore2D::Status::Restored : JoinRestore2D::Status::Broken;
                result.lost += counts.lost;
                result.removed = counts.removed;
                return result;
            }

            struct RestoreCounts
            {
                std::size_t lost = 0;
                std::size_t removed = 0;
            };

            /// This world's id for a body handle (a state from another peer names that peer's world in its ids)
            [[nodiscard]] b2BodyId BodyIdOf(const PhysicsBodyHandle handle) const
            {
                b2BodyId id{};
                id.index1 = static_cast<int32_t>(handle.index);
                id.world0 = static_cast<uint16_t>(_world.index1 - 1);
                id.generation = static_cast<uint16_t>(handle.generation);
                return id;
            }
            /// A shape key of a state, moved into this world (unchanged for a state this world saved)
            [[nodiscard]] ShapeKey Rehome(const ShapeKey key) const
            {
                return (key & ~0xFFFF0000ull) | (static_cast<std::uint64_t>(static_cast<uint16_t>(_world.index1 - 1)) << 16);
            }

            [[nodiscard]] bool RestoreImpl(const std::span<const std::byte> in, RestoreCounts *const counts)
            {
                StateReader reader{in};
                if (reader.U32() != kStateMagic || reader.U32() != kStateVersion)
                {
                    Logger::Warn("2D physics: the state is not one of this build's 2D worlds");
                    return false;
                }
                const std::uint32_t imageSize = reader.U32();
                const std::span<const std::byte> image = reader.Take(imageSize);
                struct SavedShape
                {
                    std::uint64_t key = 0;
                    std::uint32_t ordinal = 0;
                };
                struct SavedBody
                {
                    std::uint64_t id = 0;
                    bool enabled = true;
                    std::uint8_t type = 0;
                    bool fixedRotation = false;
                    bool autoMass = true;
                    float mass = 1.0f;
                    std::vector<SavedShape> shapes;
                };
                std::vector<SavedBody> saved;
                const std::uint32_t bodyCount = reader.U32();
                if (!reader.ok || bodyCount > reader.Remaining() / 8)
                {
                    Logger::Warn("2D physics: the state is cut short");
                    return false;
                }
                for (std::uint32_t i = 0; i < bodyCount && reader.ok; ++i)
                {
                    SavedBody body;
                    body.id = reader.U64();
                    body.enabled = reader.U8() != 0;
                    body.type = reader.U8();
                    body.fixedRotation = reader.U8() != 0;
                    body.autoMass = reader.U8() != 0;
                    body.mass = reader.F32();
                    const std::uint32_t shapeCount = reader.U32();
                    if (!reader.ok || shapeCount > reader.Remaining() / 12)
                    {
                        reader.ok = false;
                        break;
                    }
                    for (std::uint32_t s = 0; s < shapeCount; ++s)
                    {
                        SavedShape shape;
                        shape.key = Rehome(reader.U64());
                        shape.ordinal = reader.U32();
                        body.shapes.push_back(shape);
                    }
                    saved.push_back(std::move(body));
                }
                ContactTracker2D::TouchMap collisions = reader.Touches();
                ContactTracker2D::TouchMap triggers = reader.Touches();
                for (ContactTracker2D::TouchMap *const touches : {&collisions, &triggers})
                {
                    for (auto &entry : *touches)
                    {
                        for (ContactTracker2D::ShapePair &pair : entry.second)
                        {
                            pair.onA = Rehome(pair.onA);
                            pair.onB = Rehome(pair.onB);
                        }
                    }
                }
                const bool resyncTriggers = reader.U8() != 0;
                const std::uint64_t savedGround = reader.U64();
                std::map<JointHandle2D, JointRecord> joints;
                const std::uint32_t jointCount = reader.U32();
                if (!reader.ok || jointCount > reader.Remaining() / 26)
                {
                    Logger::Warn("2D physics: the state is cut short");
                    return false;
                }
                for (std::uint32_t i = 0; i < jointCount; ++i)
                {
                    const JointHandle2D key = Rehome(reader.U64());
                    JointRecord record;
                    record.id = b2LoadJointId(key);
                    record.type = static_cast<JointType2D>(reader.U8());
                    record.breakable = reader.U8() != 0;
                    record.self.index = reader.U32();
                    record.self.generation = reader.U32();
                    record.other.index = reader.U32();
                    record.other.generation = reader.U32();
                    joints.insert_or_assign(key, record); // unowned: each waits for its Joint2D to claim it
                }
                if (!reader.ok || reader.Remaining() != 0)
                {
                    Logger::Warn("2D physics: the state is cut short or has bytes after its end");
                    return false;
                }

                if (!b2World_RestoreFromSnapshot(_world, reinterpret_cast<const std::uint8_t *>(image.data()),
                                                 static_cast<int>(image.size())))
                {
                    Logger::Error("2D physics: Box2D refused the snapshot image; this world must not be used any more");
                    return false;
                }

                // The bodies of the saved state that this world still has keep their record; a body in the state with no
                // record was destroyed since (Box2D brought it back: it goes again)
                std::map<PhysicsBodyHandle, BodyRecord, HandleLess> bodies;
                std::vector<b2BodyId> destroyed;
                for (const SavedBody &savedBody : saved)
                {
                    const b2BodyId id = BodyIdOf(ToHandle(b2LoadBodyId(savedBody.id)));
                    const PhysicsBodyHandle handle = ToHandle(id);
                    auto node = _bodies.extract(handle);
                    if (node.empty())
                    {
                        destroyed.push_back(id);
                        continue;
                    }
                    BodyRecord record = std::move(node.mapped());
                    record.id = id;
                    record.enabled = savedBody.enabled;
                    record.type = static_cast<BodyType2D>(savedBody.type);
                    record.fixedRotation = savedBody.fixedRotation;
                    record.autoMass = savedBody.autoMass;
                    record.mass = savedBody.mass;
                    record.dirty = false; // Box2D's poses are the truth now
                    record.colliders.clear();
                    bodies.emplace(handle, std::move(record));
                }
                // What is left in _bodies did not exist at the saved instant: Box2D dropped it. Its components lose it.
                for (auto &entry : _bodies)
                {
                    BodyRecord &lost = entry.second;
                    if (counts != nullptr)
                    {
                        ++counts->lost;
                    }
                    if (lost.rigidbody)
                    {
                        lost.rigidbody->OnBodyLost();
                    }
                    if (lost.owner)
                    {
                        for (Collider2D *collider : lost.owner->GetComponents<Collider2D>())
                        {
                            if (collider->GetHandle() == entry.first)
                            {
                                collider->OnBodyLost();
                            }
                        }
                    }
                }
                for (const b2BodyId id : destroyed)
                {
                    b2DestroyBody(id);
                }
                if (counts != nullptr)
                {
                    counts->removed += destroyed.size();
                }
                _bodies = std::move(bodies);

                // The shapes: rebuilt from the state's list, each with its collider's current description
                _shapes.clear();
                _colliderShapes.clear();
                for (const SavedBody &savedBody : saved)
                {
                    const auto live = _bodies.find(ToHandle(b2LoadBodyId(savedBody.id)));
                    if (live == _bodies.end())
                    {
                        continue;
                    }
                    BodyRecord &record = live->second;
                    const std::vector<Collider2D *> siblings =
                        record.owner ? record.owner->GetComponents<Collider2D>() : std::vector<Collider2D *>{};
                    for (const SavedShape &savedShape : savedBody.shapes)
                    {
                        const b2ShapeId id = b2LoadShapeId(savedShape.key);
                        if (savedShape.ordinal >= siblings.size() || !b2Shape_IsValid(id))
                        {
                            Logger::Error("2D physics: the state names a collider or shape this world does not have; this world "
                                          "must not be used any more");
                            return false;
                        }
                        Collider2D *const collider = siblings[savedShape.ordinal];
                        ShapeRecord shape;
                        shape.id = id;
                        shape.collider = collider;
                        shape.body = live->first;
                        shape.geometry = collider->DescribeShape();
                        shape.isTrigger = b2Shape_IsSensor(id);
                        shape.isCircle = b2Shape_GetType(id) == b2_circleShape;
                        shape.layer = shape.geometry.layer;
                        shape.maskBits = b2Shape_GetFilter(id).maskBits;
                        _shapes.insert_or_assign(savedShape.key, std::move(shape));
                        _colliderShapes[collider] = savedShape.key;
                        record.colliders.push_back(collider);
                    }
                }

                // The touching pairs of the saved instant, without the ones of a body or shape that is gone
                const auto keep = [this](ContactTracker2D::TouchMap &touches)
                {
                    for (auto it = touches.begin(); it != touches.end();)
                    {
                        std::erase_if(it->second, [this](const ContactTracker2D::ShapePair &pair)
                                      { return !_shapes.contains(pair.onA) || !_shapes.contains(pair.onB); });
                        if (it->second.empty() || !_bodies.contains(it->first.a) || !_bodies.contains(it->first.b))
                        {
                            it = touches.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                };
                keep(collisions);
                keep(triggers);
                _tracker.RestoreTouches(std::move(collisions), std::move(triggers));
                _targeted.clear();
                _resyncTriggers = resyncTriggers;
                _ground = savedGround != 0 ? BodyIdOf(ToHandle(b2LoadBodyId(savedGround))) : b2_nullBodyId;
                if (savedGround != 0 && !b2Body_IsValid(_ground))
                {
                    Logger::Error("2D physics: the state names a joint anchor body this world does not have; this world must not be used any more");
                    return false;
                }
                // Joints of a body that is gone after the restore went with it in Box2D (b2DestroyBody)
                std::erase_if(joints, [this](const auto &entry)
                              { return !b2Joint_IsValid(entry.second.id) || !_bodies.contains(entry.second.self) ||
                                       (entry.second.other.IsValid() && !_bodies.contains(entry.second.other)); });
                _joints = std::move(joints);

                // The transforms follow the restored poses, as after a step; the writes are not the game moving the object
                _syncing = true;
                for (const auto &entry : _bodies)
                {
                    const BodyRecord &body = entry.second;
                    if (!body.enabled || body.type == BodyType2D::Static || !body.rigidbody || !body.owner)
                    {
                        continue;
                    }
                    if (Positionable *positionable = body.owner->GetPositionable())
                    {
                        _syncingBody = entry.first;
                        const b2Pos position = b2Body_GetPosition(body.id);
                        const Math::Vector3 current = positionable->GetPosition();
                        const Math::Vector3 moved(static_cast<float>(position.x), static_cast<float>(position.y), current.z);
                        positionable->SetPositionAndRotation(
                            moved, body.fixedRotation ? positionable->GetRotation() : FromRot(b2Body_GetRotation(body.id)));
                    }
                }
                _syncing = false;
                return true;
            }

            [[nodiscard]] bool KeepsShape(const ShapeKey key, const QueryTriggers triggers) const
            {
                const auto it = _shapes.find(key);
                return it != _shapes.end() && !(triggers == QueryTriggers::Ignore && it->second.isTrigger);
            }

        private:
            struct JointRecord
            {
                b2JointId id{};
                JointType2D type = JointType2D::Fixed;
                PhysicsBodyHandle self;
                PhysicsBodyHandle other; // invalid: the ground body
                Joint2D *owner = nullptr; // null after a restore until its component claims it
                bool breakable = false;
            };

            /// The static body that world joints and Target hold to: made with the first one, at the origin, with no shape
            b2BodyId Ground()
            {
                if (!b2Body_IsValid(_ground))
                {
                    b2BodyDef def = b2DefaultBodyDef();
                    def.type = b2_staticBody;
                    _ground = b2CreateBody(_world, &def);
                }
                return _ground;
            }

            static b2Vec2 UnitAxis(const Math::Vector2 &axis)
            {
                const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y);
                if (!(length > 0.0f) || !std::isfinite(length))
                {
                    return b2Vec2{1.0f, 0.0f};
                }
                return b2Vec2{axis.x / length, axis.y / length};
            }

            /// A break threshold for Box2D: 0 is never (Box2D's own "never" is FLT_MAX; its 0 reports every joint)
            static float Threshold(const float value)
            {
                return value > 0.0f && std::isfinite(value) ? value : std::numeric_limits<float>::max();
            }

            /// A restored joint nobody claimed in the FixedUpdate before this step: its component is gone or was disabled
            void EndUnclaimedJoints()
            {
                bool warned = false;
                for (auto it = _joints.begin(); it != _joints.end();)
                {
                    if (it->second.owner)
                    {
                        ++it;
                        continue;
                    }
                    if (!warned)
                    {
                        warned = true;
                        Logger::Warn("2D physics: a restored joint was not claimed by a Joint2D before the step; it is ended");
                    }
                    if (b2Joint_IsValid(it->second.id))
                    {
                        b2Joint_WakeBodies(it->second.id);
                        b2DestroyJoint(it->second.id);
                    }
                    it = _joints.erase(it);
                }
            }

            [[nodiscard]] std::vector<JointHandle2D> ReadBrokenJoints() const
            {
                std::vector<JointHandle2D> broken;
                const b2JointEvents events = b2World_GetJointEvents(_world);
                for (int i = 0; i < events.count; ++i)
                {
                    const JointHandle2D key = b2StoreJointId(events.jointEvents[i].jointId);
                    if (const auto it = _joints.find(key); it != _joints.end() && it->second.breakable)
                    {
                        broken.push_back(key);
                    }
                }
                std::ranges::sort(broken); // id order, the same on every peer
                broken.erase(std::unique(broken.begin(), broken.end()), broken.end());
                return broken;
            }

            void BreakJoints(const std::vector<JointHandle2D> &broken)
            {
                for (const JointHandle2D key : broken)
                {
                    const auto it = _joints.find(key);
                    if (it == _joints.end())
                    {
                        continue; // ended by a handler since
                    }
                    Joint2D *owner = it->second.owner;
                    DestroyJoint(key);
                    if (owner)
                    {
                        owner->OnJointBroken();
                    }
                }
            }

            struct BodyRecord
            {
                b2BodyId id{};
                GameObject *owner = nullptr;
                Rigidbody2D *rigidbody = nullptr;
                std::vector<Collider2D *> colliders; // in registration order, for the query order
                BodyType2D type = BodyType2D::Static;
                bool fixedRotation = false;
                bool enabled = true;
                bool dirty = false;
                bool autoMass = true;
                float mass = 1.0f;
            };

            struct ShapeRecord
            {
                b2ShapeId id{};
                Collider2D *collider = nullptr;
                PhysicsBodyHandle body;
                bool isTrigger = false;
                bool isCircle = false;
                int layer = 0;
                std::uint64_t maskBits = 0;
                ShapeDesc2D geometry; // the description it was last given, to tell whether its geometry changed
            };

            struct Targeted
            {
                PhysicsBodyHandle body;
                b2Vec2 linearVelocity{};
                float angularVelocity = 0.0f;
            };

            struct Geometry
            {
                bool isCircle = false;
                b2Circle circle{};
                b2Polygon polygon{};
            };

            [[nodiscard]] BodyRecord *Find(const PhysicsBodyHandle handle)
            {
                const auto it = _bodies.find(handle);
                return it == _bodies.end() ? nullptr : &it->second;
            }
            [[nodiscard]] const BodyRecord *Find(const PhysicsBodyHandle handle) const
            {
                const auto it = _bodies.find(handle);
                return it == _bodies.end() ? nullptr : &it->second;
            }

            static bool SameGeometry(const ShapeDesc2D &a, const ShapeDesc2D &b)
            {
                const auto same = [](const Math::Vector2 &x, const Math::Vector2 &y) { return x.x == y.x && x.y == y.y; };
                return a.kind == b.kind && same(a.offset, b.offset) && same(a.halfExtents, b.halfExtents) && a.radius == b.radius &&
                       std::ranges::equal(a.vertices, b.vertices, same);
            }

            void WakeBodiesTouching(const b2BodyId body)
            {
                const int capacity = b2Body_GetContactCapacity(body);
                if (capacity <= 0)
                {
                    return;
                }
                _contactBuffer.resize(static_cast<std::size_t>(capacity));
                const int count = b2Body_GetContactData(body, _contactBuffer.data(), capacity);
                for (int i = 0; i < count; ++i)
                {
                    const b2ContactData &contact = _contactBuffer[static_cast<std::size_t>(i)];
                    for (const b2ShapeId shape : {contact.shapeIdA, contact.shapeIdB})
                    {
                        const b2BodyId other = b2Shape_GetBody(shape);
                        if (b2Body_GetType(other) == b2_dynamicBody)
                        {
                            b2Body_SetAwake(other, true);
                        }
                    }
                }
            }

            static b2SurfaceMaterial MakeMaterial(const ShapeDesc2D &desc)
            {
                b2SurfaceMaterial material = b2DefaultSurfaceMaterial();
                material.friction = NonNegative(desc.material.dynamicFriction, material.friction, "friction");
                material.restitution = NonNegative(desc.material.restitution, material.restitution, "restitution");
                material.tangentSpeed = Finite(desc.surfaceSpeed, 0.0f, "surface speed");
                return material;
            }

            static b2QueryFilter MakeQueryFilter(const std::uint32_t layerMask)
            {
                b2QueryFilter filter{};
                filter.categoryBits = kQueryCategory;
                filter.maskBits = layerMask;
                return filter;
            }

            static void MakeBoxCorners(const Math::Vector2 &halfExtents, const float angleRadians, b2Vec2 corners[4])
            {
                const b2Rot rot = b2MakeRot(angleRadians);
                corners[0] = b2RotateVector(rot, b2Vec2{-halfExtents.x, -halfExtents.y});
                corners[1] = b2RotateVector(rot, b2Vec2{halfExtents.x, -halfExtents.y});
                corners[2] = b2RotateVector(rot, b2Vec2{halfExtents.x, halfExtents.y});
                corners[3] = b2RotateVector(rot, b2Vec2{-halfExtents.x, halfExtents.y});
            }

            bool BuildGeometry(const ShapeDesc2D &desc, Geometry &geometry) const
            {
                if (!IsFinite(desc.offset))
                {
                    Logger::Warn("Collider2D: the offset must be finite; it has no shape");
                    return false;
                }
                const b2Vec2 offset = ToB2(desc.offset);
                switch (desc.kind)
                {
                case ShapeDesc2D::Kind::Box:
                    if (!IsPositiveFinite(desc.halfExtents.x) || !IsPositiveFinite(desc.halfExtents.y))
                    {
                        Logger::Warn("BoxCollider2D: half extents must be above zero; it has no shape");
                        return false;
                    }
                    geometry.polygon = b2MakeOffsetBox(desc.halfExtents.x, desc.halfExtents.y, offset, b2Rot_identity);
                    return true;
                case ShapeDesc2D::Kind::Circle:
                    if (!IsPositiveFinite(desc.radius))
                    {
                        Logger::Warn("CircleCollider2D: the radius must be above zero; it has no shape");
                        return false;
                    }
                    geometry.isCircle = true;
                    geometry.circle.center = offset;
                    geometry.circle.radius = desc.radius;
                    return true;
                case ShapeDesc2D::Kind::Polygon:
                {
                    if (desc.vertices.size() < 3 || desc.vertices.size() > B2_MAX_POLYGON_VERTICES)
                    {
                        Logger::Warn(std::format("PolygonCollider2D: needs 3 to {} points, has {}; it has no shape", B2_MAX_POLYGON_VERTICES, desc.vertices.size()));
                        return false;
                    }
                    b2Vec2 points[B2_MAX_POLYGON_VERTICES];
                    for (std::size_t i = 0; i < desc.vertices.size(); ++i)
                    {
                        if (!IsFinite(desc.vertices[i]))
                        {
                            Logger::Warn("PolygonCollider2D: every point must be finite; it has no shape");
                            return false;
                        }
                        points[i] = b2Vec2{desc.vertices[i].x + offset.x, desc.vertices[i].y + offset.y};
                    }
                    const b2Hull hull = b2ComputeHull(points, static_cast<int>(desc.vertices.size()));
                    if (hull.count == 0)
                    {
                        Logger::Warn("PolygonCollider2D: the points do not make a convex polygon (too small or in a line); it has no shape");
                        return false;
                    }
                    geometry.polygon = b2MakePolygon(&hull, 0.0f);
                    return true;
                }
                }
                return false;
            }

            void UpdateShape(ShapeRecord &shape, const ShapeDesc2D &desc, const Geometry &geometry)
            {
                // Setting the geometry destroys the shape's contacts, so only when it changed: a new density or surface speed on a
                // crate at rest must not end its touch
                if (!SameGeometry(shape.geometry, desc))
                {
                    if (geometry.isCircle)
                    {
                        b2Shape_SetCircle(shape.id, &geometry.circle);
                    }
                    else
                    {
                        b2Shape_SetPolygon(shape.id, &geometry.polygon);
                    }
                }
                shape.geometry = desc;
                const b2SurfaceMaterial material = MakeMaterial(desc);
                b2Shape_SetSurfaceMaterial(shape.id, &material);
                b2Shape_SetDensity(shape.id, desc.isTrigger ? 0.0f : NonNegative(desc.density, 1.0f, "density"), false);
                const b2Filter filter = MakeFilter(desc.layer);
                if (desc.layer != shape.layer || filter.maskBits != shape.maskBits)
                {
                    shape.layer = desc.layer;
                    shape.maskBits = filter.maskBits;
                    b2Shape_SetFilter(shape.id, filter);
                }
            }

            static void RefreshMass(const BodyRecord &body)
            {
                // For every body type: a shape made or removed with updateBodyMass false leaves the body's mass marked dirty, and
                // Box2D asserts on that in the next step even for a kinematic or static body
                b2Body_UpdateMassFromShapes(body.id);
                if (body.type != BodyType2D::Dynamic)
                {
                    return;
                }
                b2MassData data = b2Body_GetMassData(body.id);
                if (body.autoMass)
                {
                    if (data.mass > 0.0f)
                    {
                        return;
                    }
                    // Only triggers, or no shape yet: with no mass it would ignore forces and gravity
                    data.mass = 1.0f;
                    data.rotationalInertia = 1.0f;
                    b2Body_SetMassData(body.id, data);
                    return;
                }
                const float mass = IsPositiveFinite(body.mass) ? body.mass : 1.0f;
                data.rotationalInertia = data.mass > 0.0f ? data.rotationalInertia * (mass / data.mass) : mass;
                data.mass = mass;
                b2Body_SetMassData(body.id, data);
            }

            void SyncTransforms()
            {
                // Set while writing, so the writes are not taken for a game moving the object
                _syncing = true;
                const b2BodyEvents events = b2World_GetBodyEvents(_world);
                for (int i = 0; i < events.moveCount; ++i)
                {
                    const b2BodyMoveEvent &move = events.moveEvents[i];
                    const auto it = _bodies.find(ToHandle(move.bodyId));
                    if (it == _bodies.end() || !it->second.rigidbody || !it->second.owner || it->second.dirty)
                    {
                        continue; // a dirty body was moved by the game since the step: its move wins
                    }
                    if (Positionable *positionable = it->second.owner->GetPositionable())
                    {
                        _syncingBody = it->first;
                        const Math::Vector3 position = positionable->GetPosition();
                        const Math::Vector3 moved(static_cast<float>(move.transform.p.x), static_cast<float>(move.transform.p.y), position.z);
                        positionable->SetPositionAndRotation(moved, it->second.fixedRotation ? positionable->GetRotation() : FromRot(move.transform.q));
                    }
                }
                _syncing = false;
            }

            void ReadEvents()
            {
                const auto bodyOf = [this](const ShapeKey key, PhysicsBodyHandle &body, bool &isTrigger)
                {
                    const auto it = _shapes.find(key);
                    if (it == _shapes.end())
                    {
                        return false; // removed since: its pairs already ended
                    }
                    body = it->second.body;
                    isTrigger = it->second.isTrigger;
                    return true;
                };
                // Copied into the tracker before any handler runs: Box2D's arrays do not survive a shape being destroyed
                const auto report = [&](const b2ShapeId shapeX, const b2ShapeId shapeY, auto &&reporter)
                {
                    PhysicsBodyHandle bodyX, bodyY;
                    bool triggerX = false, triggerY = false;
                    const ShapeKey keyX = b2StoreShapeId(shapeX), keyY = b2StoreShapeId(shapeY);
                    if (bodyOf(keyX, bodyX, triggerX) && bodyOf(keyY, bodyY, triggerY))
                    {
                        reporter(bodyX, keyX, triggerX, bodyY, keyY, triggerY);
                    }
                };

                const b2ContactEvents contacts = b2World_GetContactEvents(_world);
                for (int i = 0; i < contacts.endCount; ++i)
                {
                    report(contacts.endEvents[i].shapeIdA, contacts.endEvents[i].shapeIdB,
                           [this](const PhysicsBodyHandle x, const ShapeKey kx, bool, const PhysicsBodyHandle y, const ShapeKey ky, bool)
                           { _tracker.CollisionEnded(x, kx, y, ky); });
                }
                for (int i = 0; i < contacts.beginCount; ++i)
                {
                    report(contacts.beginEvents[i].shapeIdA, contacts.beginEvents[i].shapeIdB,
                           [this](const PhysicsBodyHandle x, const ShapeKey kx, bool, const PhysicsBodyHandle y, const ShapeKey ky, bool)
                           { _tracker.CollisionBegan(x, kx, y, ky); });
                }

                const b2SensorEvents sensors = b2World_GetSensorEvents(_world);
                for (int i = 0; i < sensors.endCount; ++i)
                {
                    report(sensors.endEvents[i].sensorShapeId, sensors.endEvents[i].visitorShapeId,
                           [this](const PhysicsBodyHandle x, const ShapeKey kx, bool, const PhysicsBodyHandle y, const ShapeKey ky, bool)
                           { _tracker.TriggerEnded(x, kx, y, ky); });
                }
                for (int i = 0; i < sensors.beginCount; ++i)
                {
                    report(sensors.beginEvents[i].sensorShapeId, sensors.beginEvents[i].visitorShapeId,
                           [this](const PhysicsBodyHandle x, const ShapeKey kx, const bool triggerX, const PhysicsBodyHandle y, const ShapeKey ky, const bool triggerY)
                           { ReportTriggerBegan(x, kx, triggerX, y, ky, triggerY); });
                }

                // A body disabled and enabled between two steps ended its pairs here, but Box2D's sensors kept their overlap lists and
                // so report no new begin. Ask them. A pair the tracker already has is left alone.
                if (_resyncTriggers)
                {
                    _resyncTriggers = false;
                    for (const auto &[key, shape] : _shapes)
                    {
                        if (!shape.isTrigger)
                        {
                            continue;
                        }
                        const int capacity = b2Shape_GetSensorCapacity(shape.id);
                        if (capacity <= 0)
                        {
                            continue;
                        }
                        std::vector<b2ShapeId> visitors(static_cast<std::size_t>(capacity));
                        const int count = b2Shape_GetSensorData(shape.id, visitors.data(), capacity);
                        for (int i = 0; i < count; ++i)
                        {
                            report(shape.id, visitors[static_cast<std::size_t>(i)],
                                   [this](const PhysicsBodyHandle x, const ShapeKey kx, const bool triggerX, const PhysicsBodyHandle y, const ShapeKey ky, const bool triggerY)
                                   { ReportTriggerBegan(x, kx, triggerX, y, ky, triggerY); });
                        }
                    }
                }
            }

            /// The 3D contract, which Box2D does not share: two triggers raise nothing, and two static bodies never meet
            void ReportTriggerBegan(const PhysicsBodyHandle x, const ShapeKey kx, const bool triggerX, const PhysicsBodyHandle y, const ShapeKey ky, const bool triggerY)
            {
                const BodyRecord *a = Find(x), *b = Find(y);
                const bool bothStatic = a && b && a->type == BodyType2D::Static && b->type == BodyType2D::Static;
                const bool bothEnabled = a && b && a->enabled && b->enabled;
                if (!(triggerX && triggerY) && !bothStatic && bothEnabled)
                {
                    _tracker.TriggerBegan(x, kx, y, ky);
                }
            }

            void ReadContacts(const ContactTracker2D::BodyPair &pair, const ContactTracker2D::ShapePair &shapes, ContactTracker2D::ContactData &data)
            {
                const auto shapeA = _shapes.find(shapes.onA);
                const BodyRecord *bodyA = Find(pair.a);
                const BodyRecord *bodyB = Find(pair.b);
                if (shapeA == _shapes.end() || !bodyA || !bodyB)
                {
                    return;
                }
                const b2Vec2 velocityA = b2Body_GetLinearVelocity(bodyA->id);
                const b2Vec2 velocityB = b2Body_GetLinearVelocity(bodyB->id);
                data.velocityAMinusB = Math::Vector2(velocityA.x - velocityB.x, velocityA.y - velocityB.y);

                const int capacity = b2Shape_GetContactCapacity(shapeA->second.id);
                if (capacity <= 0)
                {
                    return;
                }
                _contactBuffer.resize(static_cast<std::size_t>(capacity));
                const int count = b2Shape_GetContactData(shapeA->second.id, _contactBuffer.data(), capacity);
                for (int i = 0; i < count; ++i)
                {
                    const b2ContactData &contact = _contactBuffer[static_cast<std::size_t>(i)];
                    const ShapeKey first = b2StoreShapeId(contact.shapeIdA);
                    const ShapeKey second = b2StoreShapeId(contact.shapeIdB);
                    const bool sameOrder = first == shapes.onA && second == shapes.onB;
                    const bool swapped = first == shapes.onB && second == shapes.onA;
                    if (!sameOrder && !swapped)
                    {
                        continue;
                    }
                    // Box2D's normal goes from its shape A to its shape B; stored here from the pair's a to its b
                    const float sign = sameOrder ? 1.0f : -1.0f;
                    const Math::Vector2 normal(contact.manifold.normal.x * sign, contact.manifold.normal.y * sign);
                    // Box2D gives each point relative to its shape A's body's centre of mass
                    const b2Pos centerA = b2Body_GetWorldCenter(b2Shape_GetBody(contact.shapeIdA));
                    for (int p = 0; p < contact.manifold.pointCount; ++p)
                    {
                        const b2ManifoldPoint &source = contact.manifold.points[p];
                        ContactPoint2D point;
                        point.point = Math::Vector2(static_cast<float>(centerA.x) + source.anchorA.x, static_cast<float>(centerA.y) + source.anchorA.y);
                        point.normal = normal;
                        point.separation = source.separation;
                        point.normalImpulse = source.totalNormalImpulse;
                        point.tangentImpulse = source.tangentImpulse;
                        data.contacts.push_back(point);
                        data.impulseOnB = Math::Vector2(data.impulseOnB.x + normal.x * source.totalNormalImpulse,
                                                        data.impulseOnB.y + normal.y * source.totalNormalImpulse);
                    }
                    return;
                }
            }

            static float CollectCast(const b2ShapeId shape, const b2Pos point, const b2Vec2 normal, const float fraction, void *contextPointer)
            {
                auto &context = *static_cast<QueryContext *>(contextPointer);
                const ShapeKey key = b2StoreShapeId(shape);
                if (context.world->KeepsShape(key, context.triggers))
                {
                    context.hits.push_back({key, Math::Vector2(static_cast<float>(point.x), static_cast<float>(point.y)), FromB2(normal), fraction * context.sweepLength});
                }
                return 1.0f; // keep going: the order is decided afterwards, so tree order never shows
            }

            static bool CollectOverlap(const b2ShapeId shape, void *contextPointer)
            {
                auto &context = *static_cast<QueryContext *>(contextPointer);
                const ShapeKey key = b2StoreShapeId(shape);
                if (context.world->KeepsShape(key, context.triggers))
                {
                    context.overlaps.push_back(key);
                }
                return true;
            }

            bool Cast(const Math::Vector2 &origin, const b2ShapeProxy &proxy, const Math::Vector2 &direction, RaycastHit2D &hit, const float maxDistance,
                      const std::uint32_t layerMask, const QueryTriggers triggers)
            {
                Sweep sweep;
                if (!ReadSweep(origin, direction, maxDistance, sweep) || layerMask == 0)
                {
                    return false;
                }
                QueryContext context{this, triggers, sweep.distance, {}, {}};
                b2World_CastShape(_world, ToPos(origin.x, origin.y), &proxy, b2MulSV(sweep.distance, sweep.direction), MakeQueryFilter(layerMask), &CollectCast, &context);
                for (RawHit &raw : context.hits)
                {
                    if (!(raw.distance > 0.0f))
                    {
                        // Started inside: QueryRules' point and normal, as Box2D gives none
                        raw.point = origin;
                        raw.normal = Math::Vector2(-sweep.direction.x, -sweep.direction.y);
                        raw.distance = 0.0f;
                    }
                }
                std::vector<RaycastHit2D> hits;
                if (FinishHits(context.hits, hits) == 0)
                {
                    return false;
                }
                hit = hits.front();
                return true;
            }

            int Overlap(const Math::Vector2 &center, const b2ShapeProxy &proxy, std::vector<OverlapHit2D> &hits, const std::uint32_t layerMask,
                        const QueryTriggers triggers)
            {
                if (!std::isfinite(center.x) || !std::isfinite(center.y) || layerMask == 0)
                {
                    return 0;
                }
                QueryContext context{this, triggers, 0.0f, {}, {}};
                b2World_OverlapShape(_world, ToPos(center.x, center.y), &proxy, MakeQueryFilter(layerMask), &CollectOverlap, &context);
                std::ranges::sort(context.overlaps, [this](const ShapeKey a, const ShapeKey b) { return Precedes(a, b); });
                context.overlaps.erase(std::unique(context.overlaps.begin(), context.overlaps.end()), context.overlaps.end());
                for (const ShapeKey key : context.overlaps)
                {
                    const ShapeRecord &shape = _shapes.at(key);
                    const BodyRecord &body = _bodies.at(shape.body);
                    hits.push_back({body.owner, body.rigidbody, shape.collider, shape.body});
                }
                return static_cast<int>(hits.size());
            }

            /// By body handle, then the order the colliders were registered on it (QueryRules)
            [[nodiscard]] bool Precedes(const ShapeKey a, const ShapeKey b) const
            {
                const ShapeRecord &shapeA = _shapes.at(a);
                const ShapeRecord &shapeB = _shapes.at(b);
                if (!(shapeA.body == shapeB.body))
                {
                    return HandleLess{}(shapeA.body, shapeB.body);
                }
                const auto &colliders = _bodies.at(shapeA.body).colliders;
                return std::ranges::find(colliders, shapeA.collider) < std::ranges::find(colliders, shapeB.collider);
            }

            /// QueryRules' order: the single-hit pick taken again and again. Nearest first, and hits within the tie tolerance of the
            /// nearest by body handle, then collider order.
            int FinishHits(std::vector<RawHit> &raw, std::vector<RaycastHit2D> &hits) const
            {
                hits.clear();
                while (!raw.empty())
                {
                    float nearest = std::numeric_limits<float>::infinity();
                    for (const RawHit &candidate : raw)
                    {
                        nearest = std::min(nearest, candidate.distance);
                    }
                    auto pick = raw.end();
                    for (auto it = raw.begin(); it != raw.end(); ++it)
                    {
                        if (it->distance <= nearest + Physics::QueryRules::kQueryTieTolerance &&
                            (pick == raw.end() || Precedes(it->shape, pick->shape)))
                        {
                            pick = it;
                        }
                    }
                    const ShapeRecord &shape = _shapes.at(pick->shape);
                    const BodyRecord &body = _bodies.at(shape.body);
                    RaycastHit2D hit;
                    hit.hit = true;
                    hit.point = pick->point;
                    hit.normal = pick->normal;
                    hit.distance = pick->distance;
                    hit.gameObject = body.owner;
                    hit.rigidbody = body.rigidbody;
                    hit.collider = shape.collider;
                    hit.bodyHandle = shape.body;
                    hits.push_back(hit);
                    raw.erase(pick);
                }
                return static_cast<int>(hits.size());
            }

            b2WorldId _world{};
            float _maxLinearSpeed = 0.0f;
            // std::map: these are iterated, and that order must depend only on handles and ids, never on addresses or hashing
            std::map<PhysicsBodyHandle, BodyRecord, HandleLess> _bodies;
            std::map<ShapeKey, ShapeRecord> _shapes;
            // Only ever looked up, never iterated
            std::unordered_map<Collider2D *, ShapeKey> _colliderShapes;
            std::vector<Targeted> _targeted;
            std::vector<b2ContactData> _contactBuffer;
            ContactTracker2D _tracker;
            bool _syncing = false;
            PhysicsBodyHandle _syncingBody;
            bool _resyncTriggers = false;
            // Ordered by Box2D id: claims and breaks go in that order on every peer
            std::map<JointHandle2D, JointRecord> _joints;
            b2BodyId _ground = b2_nullBodyId;
        };
    }

    std::shared_ptr<Physics2DWorld> Physics2DWorld::Create()
    {
        if (Application::GetInstance().GetSimulationMode() == SimulationMode::Deterministic)
        {
            Logger::Warn("A 2D physics world was created in Deterministic mode. It is in the desync hash, in rollback "
                         "snapshots and in the serialised snapshot a late joiner or spectator starts from, but peers agree "
                         "only on a strict floating-point build, and a joiner takes Box2D's state exactly only from the "
                         "same build (docs/physics2d.html#network)");
        }
        return std::make_shared<Box2DWorld>();
    }

    void Physics2DWorld::RefreshAllCollisionMatrices()
    {
        for (Box2DWorld *world : LiveWorlds())
        {
            world->RefreshMatrix();
        }
    }

    void Physics2DWorld::SetGravity(const Math::Vector2 &gravity)
    {
        if (!std::isfinite(gravity.x) || !std::isfinite(gravity.y))
        {
            Logger::Warn("Physics2DWorld::SetGravity: gravity must be finite; it was not changed");
            return;
        }
        DefaultGravity() = gravity;
        for (Box2DWorld *world : LiveWorlds())
        {
            world->SetWorldGravity(gravity);
        }
    }
}

#endif

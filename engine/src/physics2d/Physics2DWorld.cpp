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
#include <format>
#include <limits>
#include <map>
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

        using ShapeKey = ContactTracker2D::ShapeKey;

        b2Vec2 ToB2(const Math::Vector2 &v) { return b2Vec2{v.x, v.y}; }
        Math::Vector2 FromB2(const b2Vec2 v) { return Math::Vector2(v.x, v.y); }

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
                _tracker.Dispatch();
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
                b2DestroyBody(it->second.id);
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

            [[nodiscard]] std::uint64_t HashState() const override { return b2World_GetStateHash(_world); }

            [[nodiscard]] bool KeepsShape(const ShapeKey key, const QueryTriggers triggers) const
            {
                const auto it = _shapes.find(key);
                return it != _shapes.end() && !(triggers == QueryTriggers::Ignore && it->second.isTrigger);
            }

        private:
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
        };
    }

    std::shared_ptr<Physics2DWorld> Physics2DWorld::Create()
    {
        if (Application::GetInstance().GetSimulationMode() == SimulationMode::Deterministic)
        {
            Logger::Warn("A 2D physics world was created in Deterministic mode. 2D physics state is not in the desync hash and a "
                         "rollback does not restore it (docs/physics2d.html#network)");
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

// The Lua side of 2D physics (docs/physics2d.html#lua). It names no Box2D type, so it is the same with N2ENGINE_USE_BOX2D=OFF: the
// components keep their data and do nothing, and the queries find nothing.

#include "engine/scripting/bindings/LuaBindings.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/GameObject.hpp"
#include "engine/Layers.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Physics2DQueries.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

namespace N2Engine::Scripting::Bindings
{
    using Rigidbody2DRef = ComponentRef<Physics2D::Rigidbody2D>;

    // What OnCollision*2D and OnTrigger*2D receive instead of Collision2D and Trigger2D, whose raw pointers would dangle in one a
    // script keeps past the callback (self.lastHit = collision). Every object in them is a checked handle.
    struct LuaCollision2D
    {
        sol::optional<GameObjectRef> gameObject;
        sol::optional<GameObjectRef> otherGameObject;
        sol::optional<Rigidbody2DRef> rigidbody;
        sol::optional<Rigidbody2DRef> otherRigidbody;
        // Handles, not Lua objects: making a Lua object here would take registry references the caller did not reserve, and
        // a script that never reads the collider would pay for it on every event
        std::optional<ComponentRef<Physics2D::Collider2D>> collider;
        std::optional<ComponentRef<Physics2D::Collider2D>> otherCollider;
        std::vector<Physics2D::ContactPoint2D> contacts;
        Math::Vector2 relativeVelocity;
        Math::Vector2 impulse;
        Math::Vector2 averageContactPoint;
    };

    struct LuaTrigger2D
    {
        sol::optional<GameObjectRef> gameObject;
        sol::optional<GameObjectRef> otherGameObject;
        sol::optional<Rigidbody2DRef> rigidbody;
        sol::optional<Rigidbody2DRef> otherRigidbody;
        std::optional<ComponentRef<Physics2D::Collider2D>> collider;
        std::optional<ComponentRef<Physics2D::Collider2D>> otherCollider;
    };

    namespace
    {
        sol::optional<GameObjectRef> RefOrNil(const GameObject *gameObject)
        {
            if (!gameObject)
            {
                return sol::nullopt;
            }
            return GameObjectRef(*gameObject);
        }

        sol::optional<Rigidbody2DRef> RefOrNil(Physics2D::Rigidbody2D *rigidbody)
        {
            if (!rigidbody)
            {
                return sol::nullopt;
            }
            return Rigidbody2DRef(*rigidbody);
        }

        std::optional<ComponentRef<Physics2D::Collider2D>> ColliderHandle(Physics2D::Collider2D *collider)
        {
            if (!collider)
            {
                return std::nullopt;
            }
            return ComponentRef<Physics2D::Collider2D>(*collider);
        }

        // The collider as its concrete Lua type, or nil when there is none or it is gone. Runs inside the script's own read.
        sol::object ColliderToLua(const std::optional<ComponentRef<Physics2D::Collider2D>> &collider, lua_State *state)
        {
            if (!collider || !collider->IsValid())
            {
                return sol::make_object(state, sol::lua_nil);
            }
            const auto live = collider->Pin();
            return ComponentToLua(*live, state);
        }

        Math::Vector2 AverageOf(const std::vector<Physics2D::ContactPoint2D> &contacts)
        {
            if (contacts.empty())
            {
                return {};
            }
            Math::Vector2 sum;
            for (const Physics2D::ContactPoint2D &contact : contacts)
            {
                sum = Math::Vector2(sum.x + contact.point.x, sum.y + contact.point.y);
            }
            const float count = static_cast<float>(contacts.size());
            return Math::Vector2(sum.x / count, sum.y / count);
        }

        sol::table HitToLua(const Physics2D::RaycastHit2D &hit, lua_State *state)
        {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
            result["point"] = hit.point;
            result["normal"] = hit.normal;
            result["distance"] = hit.distance;
            if (hit.gameObject)
            {
                result["gameObject"] = GameObjectRef(*hit.gameObject);
            }
            if (hit.collider)
            {
                result["collider"] = ComponentToLua(*hit.collider, state);
            }
            if (hit.rigidbody)
            {
                result["rigidbody"] = Rigidbody2DRef(*hit.rigidbody);
            }
            return result;
        }

        sol::table OverlapToLua(const std::vector<Physics2D::OverlapHit2D> &hits, lua_State *state)
        {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
            for (std::size_t i = 0; i < hits.size(); ++i)
            {
                sol::table entry = lua.create_table();
                if (hits[i].gameObject)
                {
                    entry["gameObject"] = GameObjectRef(*hits[i].gameObject);
                }
                if (hits[i].collider)
                {
                    entry["collider"] = ComponentToLua(*hits[i].collider, state);
                }
                if (hits[i].rigidbody)
                {
                    entry["rigidbody"] = Rigidbody2DRef(*hits[i].rigidbody);
                }
                result[i + 1] = entry;
            }
            return result;
        }

        bool IsAbsent(const sol::object &argument)
        {
            const sol::type type = argument.get_type();
            return type == sol::type::none || type == sol::type::lua_nil;
        }

        // The optional arguments every query shares, read strictly: a string where a number belongs is a Lua error, not the default.
        // A negative or NaN distance simply finds nothing, as in 3D.
        struct QueryOptions
        {
            float maxDistance = Physics2D::kNoLimit;
            std::uint32_t layerMask = Layers::DefaultRaycastMask;
            Physics::QueryTriggers triggers = Physics::QueryTriggers::Collide;
        };

        float ReadMaxDistance(const sol::object &argument, const char *query)
        {
            if (IsAbsent(argument))
            {
                return Physics2D::kNoLimit;
            }
            if (argument.get_type() != sol::type::number)
            {
                throw std::runtime_error(std::string(query) + ": maxDistance must be a number or nil");
            }
            return argument.as<float>();
        }

        std::uint32_t ReadLayerMask(const sol::object &argument, const char *query)
        {
            if (IsAbsent(argument))
            {
                return Layers::DefaultRaycastMask;
            }
            if (argument.get_type() != sol::type::number)
            {
                throw std::runtime_error(std::string(query) + ": layerMask must be a number or nil");
            }
            // A whole number within 64 bits, then cut to 32, so -1 means every layer. Anything else (1.5, math.huge, NaN) is an error,
            // not a mask sol would make up by converting it.
            const double value = argument.as<double>();
            if (!std::isfinite(value) || value != std::floor(value) || value < -9.2233720368547758e18 || value >= 9.2233720368547758e18)
            {
                throw std::runtime_error(std::string(query) + ": layerMask must be a whole number");
            }
            return static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)) & 0xFFFFFFFFu);
        }

        Physics::QueryTriggers ReadIgnoreTriggers(const sol::object &argument, const char *query)
        {
            if (IsAbsent(argument))
            {
                return Physics::QueryTriggers::Collide;
            }
            if (argument.get_type() != sol::type::boolean)
            {
                throw std::runtime_error(std::string(query) + ": ignoreTriggers must be a boolean or nil");
            }
            return argument.as<bool>() ? Physics::QueryTriggers::Ignore : Physics::QueryTriggers::Collide;
        }

        QueryOptions ReadOptions(const sol::object &maxDistance, const sol::object &layerMask, const sol::object &ignoreTriggers, const char *query)
        {
            return {ReadMaxDistance(maxDistance, query), ReadLayerMask(layerMask, query), ReadIgnoreTriggers(ignoreTriggers, query)};
        }

        float ReadAngle(const sol::object &argument, const char *query)
        {
            if (IsAbsent(argument))
            {
                return 0.0f;
            }
            if (argument.get_type() != sol::type::number)
            {
                throw std::runtime_error(std::string(query) + ": angle must be a number or nil");
            }
            return argument.as<float>();
        }

        std::vector<Math::Vector2> ReadVertices(const sol::table &points)
        {
            std::vector<Math::Vector2> vertices;
            const std::size_t count = points.size();
            vertices.reserve(count);
            for (std::size_t i = 1; i <= count; ++i)
            {
                const sol::object point = points[i];
                if (!point.is<Math::Vector2>())
                {
                    throw std::runtime_error("PolygonCollider2D:SetVertices: every point must be a Vector2");
                }
                vertices.push_back(point.as<Math::Vector2>());
            }
            return vertices;
        }
    }

    sol::object Collision2DToLua(const Physics2D::Collision2D &collision, lua_State *state)
    {
        return sol::make_object(state, LuaCollision2D{
                                           RefOrNil(collision.gameObject),
                                           RefOrNil(collision.otherGameObject),
                                           RefOrNil(collision.rigidbody),
                                           RefOrNil(collision.otherRigidbody),
                                           ColliderHandle(collision.collider),
                                           ColliderHandle(collision.otherCollider),
                                           collision.contacts,
                                           collision.relativeVelocity,
                                           collision.impulse,
                                           AverageOf(collision.contacts),
                                       });
    }

    sol::object Trigger2DToLua(const Physics2D::Trigger2D &trigger, lua_State *state)
    {
        return sol::make_object(state, LuaTrigger2D{
                                           RefOrNil(trigger.gameObject),
                                           RefOrNil(trigger.otherGameObject),
                                           RefOrNil(trigger.rigidbody),
                                           RefOrNil(trigger.otherRigidbody),
                                           ColliderHandle(trigger.collider),
                                           ColliderHandle(trigger.otherCollider),
                                       });
    }

// What every 2D collider has
#define N2_COLLIDER2D_MEMBERS(Ref, Type)                         \
    "SetIsTrigger", Forward<Ref, &Type::SetIsTrigger>(),         \
    "IsTrigger", Forward<Ref, &Type::IsTrigger>(),               \
    "SetMaterial", Forward<Ref, &Type::SetMaterial>(),           \
    "GetMaterial", Forward<Ref, &Type::GetMaterial>(),           \
    "SetOffset", Forward<Ref, &Type::SetOffset>(),               \
    "GetOffset", Forward<Ref, &Type::GetOffset>(),               \
    "SetDensity", Forward<Ref, &Type::SetDensity>(),             \
    "GetDensity", Forward<Ref, &Type::GetDensity>(),             \
    "SetSurfaceSpeed", Forward<Ref, &Type::SetSurfaceSpeed>(),   \
    "GetSurfaceSpeed", Forward<Ref, &Type::GetSurfaceSpeed>()

    void BindPhysics2D(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        lua.new_enum("BodyType2D",
            "Static", Physics2D::BodyType2D::Static,
            "Dynamic", Physics2D::BodyType2D::Dynamic,
            "Kinematic", Physics2D::BodyType2D::Kinematic
        );

        // ===== Event data =====
        lua.new_usertype<Physics2D::ContactPoint2D>("ContactPoint2D",
            sol::no_constructor,
            "point", sol::readonly(&Physics2D::ContactPoint2D::point),
            "normal", sol::readonly(&Physics2D::ContactPoint2D::normal),
            "separation", sol::readonly(&Physics2D::ContactPoint2D::separation),
            "normalImpulse", sol::readonly(&Physics2D::ContactPoint2D::normalImpulse),
            "tangentImpulse", sol::readonly(&Physics2D::ContactPoint2D::tangentImpulse)
        );

        // Object fields are nil when absent (otherRigidbody for a collider alone, a collider that was removed)
        lua.new_usertype<LuaCollision2D>("Collision2D",
            sol::no_constructor,
            "gameObject", sol::property([](const LuaCollision2D &c) { return c.gameObject; }),
            "otherGameObject", sol::property([](const LuaCollision2D &c) { return c.otherGameObject; }),
            "rigidbody", sol::property([](const LuaCollision2D &c) { return c.rigidbody; }),
            "otherRigidbody", sol::property([](const LuaCollision2D &c) { return c.otherRigidbody; }),
            "collider", sol::property([](const LuaCollision2D &c, const sol::this_state s) { return ColliderToLua(c.collider, s); }),
            "otherCollider", sol::property([](const LuaCollision2D &c, const sol::this_state s) { return ColliderToLua(c.otherCollider, s); }),
            "relativeVelocity", sol::property([](const LuaCollision2D &c) { return c.relativeVelocity; }),
            "impulse", sol::property([](const LuaCollision2D &c) { return c.impulse; }),
            "contactCount", sol::property([](const LuaCollision2D &c) { return c.contacts.size(); }),
            "GetContact", [](const LuaCollision2D &c, const std::size_t index) -> sol::optional<Physics2D::ContactPoint2D>
            {
                // 1-based, like Lua tables
                if (index < 1 || index > c.contacts.size())
                {
                    return sol::nullopt;
                }
                return c.contacts[index - 1];
            },
            "GetAverageContactPoint", [](const LuaCollision2D &c) { return c.averageContactPoint; }
        );

        lua.new_usertype<LuaTrigger2D>("Trigger2D",
            sol::no_constructor,
            "gameObject", sol::property([](const LuaTrigger2D &t) { return t.gameObject; }),
            "otherGameObject", sol::property([](const LuaTrigger2D &t) { return t.otherGameObject; }),
            "rigidbody", sol::property([](const LuaTrigger2D &t) { return t.rigidbody; }),
            "otherRigidbody", sol::property([](const LuaTrigger2D &t) { return t.otherRigidbody; }),
            "collider", sol::property([](const LuaTrigger2D &t, const sol::this_state s) { return ColliderToLua(t.collider, s); }),
            "otherCollider", sol::property([](const LuaTrigger2D &t, const sol::this_state s) { return ColliderToLua(t.otherCollider, s); })
        );

        // ===== Queries and gravity =====
        lua["Physics2D"] = lua.create_table_with(
            // -> {point, normal, distance, gameObject, collider, rigidbody}, or nil
            "Raycast", [](const Math::Vector2 &origin, const Math::Vector2 &direction, const sol::object &maxDistance, const sol::object &layerMask,
                          const sol::object &ignoreTriggers, const sol::this_state state) -> sol::object
            {
                const QueryOptions options = ReadOptions(maxDistance, layerMask, ignoreTriggers, "Physics2D.Raycast");
                Physics2D::RaycastHit2D hit;
                if (!Physics2D::Raycast(origin, direction, hit, options.maxDistance, options.layerMask, options.triggers))
                {
                    return sol::make_object(state, sol::lua_nil);
                }
                return HitToLua(hit, state);
            },
            // -> every hit, nearest first (empty for none)
            "RaycastAll", [](const Math::Vector2 &origin, const Math::Vector2 &direction, const sol::object &maxDistance, const sol::object &layerMask,
                             const sol::object &ignoreTriggers, const sol::this_state state) -> sol::table
            {
                const QueryOptions options = ReadOptions(maxDistance, layerMask, ignoreTriggers, "Physics2D.RaycastAll");
                std::vector<Physics2D::RaycastHit2D> hits;
                Physics2D::RaycastAll(origin, direction, hits, options.maxDistance, options.layerMask, options.triggers);
                sol::state_view view(state);
                sol::table result = view.create_table();
                for (std::size_t i = 0; i < hits.size(); ++i)
                {
                    result[i + 1] = HitToLua(hits[i], state);
                }
                return result;
            },
            "CircleCast", [](const Math::Vector2 &origin, const float radius, const Math::Vector2 &direction, const sol::object &maxDistance,
                             const sol::object &layerMask, const sol::object &ignoreTriggers, const sol::this_state state) -> sol::object
            {
                const QueryOptions options = ReadOptions(maxDistance, layerMask, ignoreTriggers, "Physics2D.CircleCast");
                Physics2D::RaycastHit2D hit;
                if (!Physics2D::CircleCast(origin, radius, direction, hit, options.maxDistance, options.layerMask, options.triggers))
                {
                    return sol::make_object(state, sol::lua_nil);
                }
                return HitToLua(hit, state);
            },
            "BoxCast", [](const Math::Vector2 &origin, const Math::Vector2 &halfExtents, const sol::object &angle, const Math::Vector2 &direction,
                          const sol::object &maxDistance, const sol::object &layerMask, const sol::object &ignoreTriggers,
                          const sol::this_state state) -> sol::object
            {
                const QueryOptions options = ReadOptions(maxDistance, layerMask, ignoreTriggers, "Physics2D.BoxCast");
                Physics2D::RaycastHit2D hit;
                if (!Physics2D::BoxCast(origin, halfExtents, ReadAngle(angle, "Physics2D.BoxCast"), direction, hit, options.maxDistance,
                                        options.layerMask, options.triggers))
                {
                    return sol::make_object(state, sol::lua_nil);
                }
                return HitToLua(hit, state);
            },
            // -> an array of {gameObject, collider, rigidbody} (empty for none)
            "OverlapCircle", [](const Math::Vector2 &center, const float radius, const sol::object &layerMask, const sol::object &ignoreTriggers,
                                const sol::this_state state) -> sol::table
            {
                std::vector<Physics2D::OverlapHit2D> hits;
                Physics2D::OverlapCircle(center, radius, hits, ReadLayerMask(layerMask, "Physics2D.OverlapCircle"),
                                         ReadIgnoreTriggers(ignoreTriggers, "Physics2D.OverlapCircle"));
                return OverlapToLua(hits, state);
            },
            "OverlapBox", [](const Math::Vector2 &center, const Math::Vector2 &halfExtents, const sol::object &angle, const sol::object &layerMask,
                             const sol::object &ignoreTriggers, const sol::this_state state) -> sol::table
            {
                std::vector<Physics2D::OverlapHit2D> hits;
                Physics2D::OverlapBox(center, halfExtents, ReadAngle(angle, "Physics2D.OverlapBox"), hits, ReadLayerMask(layerMask, "Physics2D.OverlapBox"),
                                      ReadIgnoreTriggers(ignoreTriggers, "Physics2D.OverlapBox"));
                return OverlapToLua(hits, state);
            },
            // The loaded scene's gravity: the next scene starts with the default again
            "SetGravity", [](const Math::Vector2 &gravity)
            {
                Scene *scene = SceneManager::GetCurScene();
                if (!scene)
                {
                    throw std::runtime_error("Physics2D.SetGravity: no scene is loaded");
                }
                if (const auto world = scene->GetOrCreatePhysics2DWorld())
                {
                    world->SetWorldGravity(gravity);
                }
            },
            "GetGravity", []
            {
                const Scene *scene = SceneManager::GetCurScene();
                const Physics2D::Physics2DWorld *world = scene ? scene->GetPhysics2DWorld() : nullptr;
                return world ? world->GetWorldGravity() : Physics2D::Physics2DWorld::GetGravity();
            }
        );

        // ===== Components =====
        BindComponentType<Physics2D::Rigidbody2D>(lua, "Rigidbody2D",
            "SetBodyType", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetBodyType>(),
            "GetBodyType", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::GetBodyType>(),
            "SetMass", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetMass>(),
            "GetMass", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::GetMass>(),
            "UseAutoMass", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::UseAutoMass>(),
            "UsesAutoMass", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::UsesAutoMass>(),
            "SetLinearVelocity", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetLinearVelocity>(),
            "GetLinearVelocity", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::GetLinearVelocity>(),
            "SetAngularVelocity", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetAngularVelocity>(),
            "GetAngularVelocity", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::GetAngularVelocity>(),
            "SetLinearDamping", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetLinearDamping>(),
            "GetLinearDamping", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::GetLinearDamping>(),
            "SetAngularDamping", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetAngularDamping>(),
            "GetAngularDamping", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::GetAngularDamping>(),
            "SetGravityScale", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetGravityScale>(),
            "GetGravityScale", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::GetGravityScale>(),
            "SetFixedRotation", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetFixedRotation>(),
            "IsFixedRotation", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::IsFixedRotation>(),
            "SetBullet", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::SetBullet>(),
            "IsBullet", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::IsBullet>(),
            "AddForce", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::AddForce>(),
            "AddImpulse", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::AddImpulse>(),
            "AddTorque", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::AddTorque>(),
            "AddAngularImpulse", Forward<Rigidbody2DRef, &Physics2D::Rigidbody2D::AddAngularImpulse>()
        );

        using BoxRef = ComponentRef<Physics2D::BoxCollider2D>;
        BindComponentType<Physics2D::BoxCollider2D>(lua, "BoxCollider2D",
            "SetSize", Forward<BoxRef, &Physics2D::BoxCollider2D::SetSize>(),
            "GetSize", Forward<BoxRef, &Physics2D::BoxCollider2D::GetSize>(),
            "SetHalfExtents", Forward<BoxRef, &Physics2D::BoxCollider2D::SetHalfExtents>(),
            "GetHalfExtents", Forward<BoxRef, &Physics2D::BoxCollider2D::GetHalfExtents>(),
            N2_COLLIDER2D_MEMBERS(BoxRef, Physics2D::BoxCollider2D)
        );

        using CircleRef = ComponentRef<Physics2D::CircleCollider2D>;
        BindComponentType<Physics2D::CircleCollider2D>(lua, "CircleCollider2D",
            "SetRadius", Forward<CircleRef, &Physics2D::CircleCollider2D::SetRadius>(),
            "GetRadius", Forward<CircleRef, &Physics2D::CircleCollider2D::GetRadius>(),
            N2_COLLIDER2D_MEMBERS(CircleRef, Physics2D::CircleCollider2D)
        );

        using PolygonRef = ComponentRef<Physics2D::PolygonCollider2D>;
        BindComponentType<Physics2D::PolygonCollider2D>(lua, "PolygonCollider2D",
            // An array of Vector2 in, a new array of Vector2 out
            "SetVertices", [](const PolygonRef &ref, const sol::table &points) { ref.Pin()->SetVertices(ReadVertices(points)); },
            "GetVertices", [](const PolygonRef &ref, const sol::this_state state) -> sol::table
            {
                sol::state_view view(state);
                sol::table result = view.create_table();
                const auto component = ref.Pin();
                const std::vector<Math::Vector2> &vertices = component->GetVertices();
                for (std::size_t i = 0; i < vertices.size(); ++i)
                {
                    result[i + 1] = vertices[i];
                }
                return result;
            },
            N2_COLLIDER2D_MEMBERS(PolygonRef, Physics2D::PolygonCollider2D)
        );
    }

#undef N2_COLLIDER2D_MEMBERS
}

#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/GameObject.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/Layers.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace N2Engine::Scripting::Bindings
{
    using RigidbodyRef = ComponentRef<Physics::Rigidbody>;

    // What OnCollision*/OnTrigger* receive instead of Physics::Collision/Trigger, whose raw pointers would
    // dangle in a collision a script keeps past the callback (e.g. self.lastHit = collision)
    struct LuaCollision
    {
        sol::optional<GameObjectRef> gameObject;
        sol::optional<GameObjectRef> otherGameObject;
        sol::optional<RigidbodyRef> rigidbody;
        sol::optional<RigidbodyRef> otherRigidbody;
        std::vector<Physics::ContactPoint> contacts;
        Math::Vector3 relativeVelocity;
        Math::Vector3 impulse;
        Math::Vector3 averageContactPoint;
    };

    struct LuaTrigger
    {
        sol::optional<GameObjectRef> gameObject;
        sol::optional<GameObjectRef> otherGameObject;
        sol::optional<RigidbodyRef> rigidbody;
        sol::optional<RigidbodyRef> otherRigidbody;
    };

    namespace
    {
        // nil for a null pointer (e.g. no Rigidbody on a static collider)
        sol::optional<GameObjectRef> RefOrNil(const GameObject *gameObject)
        {
            if (!gameObject)
            {
                return sol::nullopt;
            }
            return GameObjectRef(*gameObject);
        }

        sol::optional<RigidbodyRef> RefOrNil(Physics::Rigidbody *rigidbody)
        {
            if (!rigidbody)
            {
                return sol::nullopt;
            }
            return RigidbodyRef(*rigidbody);
        }

        // A hit as a plain table whose objects are checked handles, so a script can keep it
        sol::table RaycastHitToLua(const Physics::RaycastHit &hit, lua_State *state)
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
                result["rigidbody"] = RigidbodyRef(*hit.rigidbody);
            }
            return result;
        }

        struct LuaRayQuery
        {
            float maxDistance;
            uint32_t layerMask;
        };

        // What an omitted or infinite distance becomes: PhysX takes a finite distance ([0, inf)), and one far
        // beyond any scene keeps its internal arithmetic clear of overflow
        constexpr float DefaultLuaRayDistance = 1.0e9f;

        // The optional arguments of Physics.Raycast/RaycastAll, or nothing for a query that can't hit: a zero
        // or non-finite direction, or a negative or NaN distance. The mask is cut to 32 bits, so -1 means
        // every layer.
        std::optional<LuaRayQuery> ReadRayQuery(const Math::Vector3 &direction, const sol::optional<float> maxDistance,
                                                const sol::optional<int64_t> layerMask)
        {
            const float lengthSquared = direction.LengthSquared();
            if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared))
            {
                return std::nullopt;
            }
            float distance = maxDistance.value_or(DefaultLuaRayDistance);
            if (!(distance >= 0.0f))
            {
                return std::nullopt;
            }
            if (std::isinf(distance))
            {
                distance = DefaultLuaRayDistance;
            }
            const int64_t mask = layerMask.value_or(static_cast<int64_t>(Layers::DefaultRaycastMask));
            return LuaRayQuery{distance, static_cast<uint32_t>(static_cast<uint64_t>(mask) & 0xFFFFFFFFu)};
        }
    }

    sol::object CollisionToLua(const Physics::Collision &collision, lua_State *state)
    {
        return sol::make_object(state, LuaCollision{
                                    RefOrNil(collision.gameObject),
                                    RefOrNil(collision.otherGameObject),
                                    RefOrNil(collision.rigidbody),
                                    RefOrNil(collision.otherRigidbody),
                                    collision.contacts,
                                    collision.relativeVelocity,
                                    collision.impulse,
                                    collision.GetAverageContactPoint(),
                                });
    }

    sol::object TriggerToLua(const Physics::Trigger &trigger, lua_State *state)
    {
        return sol::make_object(state, LuaTrigger{
                                    RefOrNil(trigger.gameObject),
                                    RefOrNil(trigger.otherGameObject),
                                    RefOrNil(trigger.rigidbody),
                                    RefOrNil(trigger.otherRigidbody),
                                });
    }

    void BindPhysics(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // ===== Collision event data (the argument of OnCollision*/OnTrigger* in scripts) =====
        // Without these, `collision.otherGameObject` in a Lua handler was "attempt to index a userdata value"
        lua.new_usertype<Physics::ContactPoint>("ContactPoint",
            sol::no_constructor,
            "point", sol::readonly(&Physics::ContactPoint::point),
            "normal", sol::readonly(&Physics::ContactPoint::normal),
            "separation", sol::readonly(&Physics::ContactPoint::separation),
            "normalImpulse", sol::readonly(&Physics::ContactPoint::normalImpulse)
        );

        // Object fields are nil when absent (e.g. otherRigidbody for a static collider)
        lua.new_usertype<LuaCollision>("Collision",
            sol::no_constructor,
            "gameObject", sol::property([](const LuaCollision &c) { return c.gameObject; }),
            "otherGameObject", sol::property([](const LuaCollision &c) { return c.otherGameObject; }),
            "rigidbody", sol::property([](const LuaCollision &c) { return c.rigidbody; }),
            "otherRigidbody", sol::property([](const LuaCollision &c) { return c.otherRigidbody; }),
            "relativeVelocity", sol::property([](const LuaCollision &c) { return c.relativeVelocity; }),
            "impulse", sol::property([](const LuaCollision &c) { return c.impulse; }),
            "contactCount", sol::property([](const LuaCollision &c) { return c.contacts.size(); }),
            "GetContact", [](const LuaCollision &c, std::size_t index) -> sol::optional<Physics::ContactPoint>
            {
                // 1-based, like Lua tables
                if (index < 1 || index > c.contacts.size())
                {
                    return sol::nullopt;
                }
                return c.contacts[index - 1];
            },
            "GetAverageContactPoint", [](const LuaCollision &c) { return c.averageContactPoint; }
        );

        lua.new_usertype<LuaTrigger>("Trigger",
            sol::no_constructor,
            "gameObject", sol::property([](const LuaTrigger &t) { return t.gameObject; }),
            "otherGameObject", sol::property([](const LuaTrigger &t) { return t.otherGameObject; }),
            "rigidbody", sol::property([](const LuaTrigger &t) { return t.rigidbody; }),
            "otherRigidbody", sol::property([](const LuaTrigger &t) { return t.otherRigidbody; })
        );

        // ===== Raycasts (global) =====
        lua["Physics"] = lua.create_table_with(
            // Physics.Raycast(origin, direction, maxDistance?, layerMask?) -> the closest hit as a table
            // {point, normal, distance, gameObject, collider, rigidbody}, or nil
            "Raycast", [](const Math::Vector3 &origin, const Math::Vector3 &direction,
                          const sol::optional<float> maxDistance, const sol::optional<int64_t> layerMask,
                          const sol::this_state state) -> sol::object
            {
                const auto query = ReadRayQuery(direction, maxDistance, layerMask);
                Physics::RaycastHit hit;
                if (!query || !Physics::Raycast::Single(origin, direction, hit, query->maxDistance, query->layerMask))
                {
                    return sol::make_object(state, sol::lua_nil);
                }
                return RaycastHitToLua(hit, state);
            },
            // Physics.RaycastAll(...) -> every hit, nearest first, as an array of hit tables (empty for none)
            "RaycastAll", [](const Math::Vector3 &origin, const Math::Vector3 &direction,
                             const sol::optional<float> maxDistance, const sol::optional<int64_t> layerMask,
                             const sol::this_state state) -> sol::table
            {
                sol::state_view lua(state);
                sol::table result = lua.create_table();
                const auto query = ReadRayQuery(direction, maxDistance, layerMask);
                if (!query)
                {
                    return result;
                }
                std::vector<Physics::RaycastHit> hits;
                Physics::Raycast::All(origin, direction, hits, query->maxDistance, query->layerMask);
                for (std::size_t i = 0; i < hits.size(); ++i)
                {
                    result[i + 1] = RaycastHitToLua(hits[i], state);
                }
                return result;
            }
        );

        // ===== BodyType Enum =====
        lua.new_enum("BodyType",
            "Static", Physics::BodyType::Static,
            "Dynamic", Physics::BodyType::Dynamic,
            "Kinematic", Physics::BodyType::Kinematic
        );
        
        // ===== PhysicsMaterial =====
        lua.new_usertype<Physics::PhysicsMaterial>("PhysicsMaterial",
            sol::call_constructor,
            sol::constructors<Physics::PhysicsMaterial()>(),
            
            "staticFriction", &Physics::PhysicsMaterial::staticFriction,
            "dynamicFriction", &Physics::PhysicsMaterial::dynamicFriction,
            "restitution", &Physics::PhysicsMaterial::restitution,
            
            "Default", &Physics::PhysicsMaterial::Default,
            "Ice", &Physics::PhysicsMaterial::Ice,
            "Rubber", &Physics::PhysicsMaterial::Rubber,
            "Bouncy", &Physics::PhysicsMaterial::Bouncy,
            "Metal", &Physics::PhysicsMaterial::Metal
        );
        
        // ===== Rigidbody =====
        BindComponentType<Physics::Rigidbody>(lua, "Rigidbody",
            "SetBodyType", Forward<RigidbodyRef, &Physics::Rigidbody::SetBodyType>(),
            "GetBodyType", Forward<RigidbodyRef, &Physics::Rigidbody::GetBodyType>(),

            "SetMass", Forward<RigidbodyRef, &Physics::Rigidbody::SetMass>(),
            "GetMass", Forward<RigidbodyRef, &Physics::Rigidbody::GetMass>(),

            "SetGravityEnabled", Forward<RigidbodyRef, &Physics::Rigidbody::SetGravityEnabled>(),
            "IsGravityEnabled", Forward<RigidbodyRef, &Physics::Rigidbody::IsGravityEnabled>(),

            "AddForce", Forward<RigidbodyRef, &Physics::Rigidbody::AddForce>(),
            "AddImpulse", Forward<RigidbodyRef, &Physics::Rigidbody::AddImpulse>(),
            "SetVelocity", Forward<RigidbodyRef, &Physics::Rigidbody::SetVelocity>(),
            "SetAngularVelocity", Forward<RigidbodyRef, &Physics::Rigidbody::SetAngularVelocity>(),
            "GetVelocity", Forward<RigidbodyRef, &Physics::Rigidbody::GetVelocity>(),
            "GetAngularVelocity", Forward<RigidbodyRef, &Physics::Rigidbody::GetAngularVelocity>()
        );
        
        // ===== BoxCollider =====
        using BoxColliderRef = ComponentRef<Physics::BoxCollider>;
        BindComponentType<Physics::BoxCollider>(lua, "BoxCollider",
            "SetSize", Forward<BoxColliderRef, &Physics::BoxCollider::SetSize>(),
            "GetSize", Forward<BoxColliderRef, &Physics::BoxCollider::GetSize>(),
            "SetHalfExtents", Forward<BoxColliderRef, &Physics::BoxCollider::SetHalfExtents>(),
            "GetHalfExtents", Forward<BoxColliderRef, &Physics::BoxCollider::GetHalfExtents>(),
            "SetIsTrigger", Forward<BoxColliderRef, &Physics::BoxCollider::SetIsTrigger>(),
            "IsTrigger", Forward<BoxColliderRef, &Physics::BoxCollider::IsTrigger>(),
            "SetMaterial", Forward<BoxColliderRef, &Physics::BoxCollider::SetMaterial>(),
            "GetMaterial", Forward<BoxColliderRef, &Physics::BoxCollider::GetMaterial>(),
            "SetOffset", Forward<BoxColliderRef, &Physics::BoxCollider::SetOffset>(),
            "GetOffset", Forward<BoxColliderRef, &Physics::BoxCollider::GetOffset>()
        );

        // ===== SphereCollider =====
        using SphereColliderRef = ComponentRef<Physics::SphereCollider>;
        BindComponentType<Physics::SphereCollider>(lua, "SphereCollider",
            "SetRadius", Forward<SphereColliderRef, &Physics::SphereCollider::SetRadius>(),
            "GetRadius", Forward<SphereColliderRef, &Physics::SphereCollider::GetRadius>(),
            "SetIsTrigger", Forward<SphereColliderRef, &Physics::SphereCollider::SetIsTrigger>(),
            "IsTrigger", Forward<SphereColliderRef, &Physics::SphereCollider::IsTrigger>(),
            "SetMaterial", Forward<SphereColliderRef, &Physics::SphereCollider::SetMaterial>(),
            "GetMaterial", Forward<SphereColliderRef, &Physics::SphereCollider::GetMaterial>(),
            "SetOffset", Forward<SphereColliderRef, &Physics::SphereCollider::SetOffset>(),
            "GetOffset", Forward<SphereColliderRef, &Physics::SphereCollider::GetOffset>()
        );

        // ===== CapsuleCollider =====
        using CapsuleColliderRef = ComponentRef<Physics::CapsuleCollider>;
        BindComponentType<Physics::CapsuleCollider>(lua, "CapsuleCollider",
            "SetRadius", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::SetRadius>(),
            "GetRadius", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::GetRadius>(),
            "SetHeight", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::SetHeight>(),
            "GetHeight", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::GetHeight>(),
            "SetIsTrigger", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::SetIsTrigger>(),
            "IsTrigger", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::IsTrigger>(),
            "SetMaterial", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::SetMaterial>(),
            "GetMaterial", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::GetMaterial>(),
            "SetOffset", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::SetOffset>(),
            "GetOffset", Forward<CapsuleColliderRef, &Physics::CapsuleCollider::GetOffset>()
        );
    }
}
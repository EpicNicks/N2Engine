#include "engine/scripting/bindings/LuaBindings.hpp"
#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/GameObject.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/CharacterController.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/Layers.hpp"
#include "engine/Application.hpp"
#include "engine/physics/IPhysicsBackend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
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

        // One collider an overlap query found, as a plain table of checked handles
        sol::table OverlapHitToLua(const Physics::OverlapHit &hit, lua_State *state)
        {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
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

        // The default and the longest distance: one far beyond any scene but well below the float limit keeps the
        // backends' internal arithmetic clear of overflow (a sweep is clamped further, to QueryRules::kMaxSweepDistance,
        // by the backend)
        constexpr float MaxLuaRayDistance = 1.0e9f;

        // The distance argument of a query, or nothing for one that can't hit: a negative or NaN distance. Longer
        // distances (math.huge included) are clamped to MaxLuaRayDistance.
        std::optional<float> ReadDistance(const sol::optional<float> maxDistance)
        {
            const float distance = maxDistance.value_or(MaxLuaRayDistance);
            if (!(distance >= 0.0f))
            {
                return std::nullopt;
            }
            return (std::min)(distance, MaxLuaRayDistance);
        }

        // The layer mask argument, cut to 32 bits so -1 means every layer; default: every layer but Ignore Raycast
        uint32_t ReadLayerMask(const sol::optional<int64_t> layerMask)
        {
            const int64_t mask = layerMask.value_or(static_cast<int64_t>(Layers::DefaultRaycastMask));
            return static_cast<uint32_t>(static_cast<uint64_t>(mask) & 0xFFFFFFFFu);
        }

        // An optional argument of the new queries: nil or missing is absent; anything given has to be of the right type.
        // (A sol::optional reads a string or a table as absent, and the script would silently get the default.)
        bool IsAbsent(const sol::object &argument)
        {
            const sol::type type = argument.get_type();
            return type == sol::type::none || type == sol::type::lua_nil;
        }

        sol::optional<float> OptionalFloatArgument(const sol::object &argument, const char *query, const char *name)
        {
            if (IsAbsent(argument))
            {
                return sol::nullopt;
            }
            if (argument.get_type() != sol::type::number)
            {
                throw std::runtime_error(std::string(query) + ": " + name + " must be a number or nil");
            }
            return argument.as<float>();
        }

        sol::optional<int64_t> OptionalMaskArgument(const sol::object &argument, const char *query)
        {
            if (IsAbsent(argument))
            {
                return sol::nullopt;
            }
            if (argument.get_type() != sol::type::number)
            {
                throw std::runtime_error(std::string(query) + ": layerMask must be a number or nil");
            }
            return argument.as<int64_t>();
        }

        Math::Quaternion OptionalRotationArgument(const sol::object &argument, const char *query)
        {
            if (IsAbsent(argument))
            {
                return Math::Quaternion::Identity;
            }
            if (!argument.is<Math::Quaternion>())
            {
                throw std::runtime_error(std::string(query) + ": rotation must be a Quaternion or nil");
            }
            return argument.as<Math::Quaternion>();
        }

        bool IsUsableDirection(const Math::Vector3 &direction)
        {
            const float lengthSquared = direction.LengthSquared();
            return lengthSquared > 0.0f && std::isfinite(lengthSquared);
        }

        // A radius or half extent a query can use: above zero and finite
        bool IsUsableExtent(const float extent)
        {
            return extent > 0.0f && std::isfinite(extent);
        }

        struct LuaRayQuery
        {
            float maxDistance;
            uint32_t layerMask;
        };

        // The optional arguments of Physics.Raycast/RaycastAll (and of the casts), or nothing for a query that can't
        // hit: a zero or non-finite direction, or a negative or NaN distance.
        std::optional<LuaRayQuery> ReadRayQuery(const Math::Vector3 &direction, const sol::optional<float> maxDistance,
                                                const sol::optional<int64_t> layerMask)
        {
            if (!IsUsableDirection(direction))
            {
                return std::nullopt;
            }
            const std::optional<float> distance = ReadDistance(maxDistance);
            if (!distance)
            {
                return std::nullopt;
            }
            return LuaRayQuery{*distance, ReadLayerMask(layerMask)};
        }

        // The array of hit tables an overlap query returns (empty for none)
        sol::table OverlapResultToLua(const std::vector<Physics::OverlapHit> &hits, lua_State *state)
        {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
            for (std::size_t i = 0; i < hits.size(); ++i)
            {
                result[i + 1] = OverlapHitToLua(hits[i], state);
            }
            return result;
        }
    }

    namespace
    {
        // A finite number argument of a CharacterController call; anything else is a Lua error
        float RequireNumber(const sol::object &argument, const char *method, const char *name)
        {
            if (argument.get_type() != sol::type::number)
            {
                throw std::runtime_error(std::string(method) + ": " + name + " must be a number");
            }
            const float value = static_cast<float>(argument.as<double>());
            if (!std::isfinite(value))
            {
                throw std::runtime_error(std::string(method) + ": " + name + " must be finite");
            }
            return value;
        }

        // A Vector3 argument with finite components
        Math::Vector3 RequireVector(const sol::object &argument, const char *method, const char *name)
        {
            if (!argument.is<Math::Vector3>())
            {
                throw std::runtime_error(std::string(method) + ": " + name + " must be a Vector3");
            }
            const Math::Vector3 value = argument.as<Math::Vector3>();
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
            {
                throw std::runtime_error(std::string(method) + ": " + name + " must be finite");
            }
            return value;
        }

        // The state of a character as a plain table (CharacterController:GetState)
        sol::table CharacterStateToLua(const Physics::CharacterState &character, lua_State *state)
        {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
            result["position"] = character.position;
            result["velocity"] = character.velocity;
            result["grounded"] = character.grounded;
            result["groundNormal"] = character.groundNormal;
            return result;
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
            // Physics.IsGpuAvailable() -> true when the physics scene runs on the GPU (physics.gpu auto/on and the
            // probe succeeded); false otherwise, including without a physics backend
            "IsGpuAvailable", []() -> bool
            {
                const Physics::IPhysicsBackend *backend = Application::GetInstance().Get3DPhysicsBackend();
                return backend != nullptr && backend->GetGpuStatus().state == Physics::GpuState::Ready;
            },
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
            },
            // Physics.SphereCast(origin, radius, direction, maxDistance?, layerMask?) -> the closest hit as for
            // Physics.Raycast (the point is on the hit surface, the distance is the sphere centre's), or nil
            "SphereCast", [](const Math::Vector3 &origin, const float radius, const Math::Vector3 &direction,
                             const sol::object maxDistance, const sol::object layerMask,
                             const sol::this_state state) -> sol::object
            {
                const auto query = ReadRayQuery(
                    direction, OptionalFloatArgument(maxDistance, "Physics.SphereCast", "maxDistance"),
                    OptionalMaskArgument(layerMask, "Physics.SphereCast"));
                Physics::RaycastHit hit;
                if (!query || !IsUsableExtent(radius) ||
                    !Physics::Raycast::SphereCast(origin, radius, direction, hit, query->maxDistance, query->layerMask))
                {
                    return sol::make_object(state, sol::lua_nil);
                }
                return RaycastHitToLua(hit, state);
            },
            // Physics.CapsuleCast(point0, point1, radius, direction, maxDistance?, layerMask?) -> as SphereCast; the
            // capsule is the segment point0-point1 (the centres of its end spheres) with `radius`; the distance is
            // the capsule centre's
            "CapsuleCast", [](const Math::Vector3 &point0, const Math::Vector3 &point1, const float radius,
                              const Math::Vector3 &direction, const sol::object maxDistance,
                              const sol::object layerMask, const sol::this_state state) -> sol::object
            {
                const auto query = ReadRayQuery(
                    direction, OptionalFloatArgument(maxDistance, "Physics.CapsuleCast", "maxDistance"),
                    OptionalMaskArgument(layerMask, "Physics.CapsuleCast"));
                Physics::RaycastHit hit;
                if (!query || !IsUsableExtent(radius) ||
                    !Physics::Raycast::CapsuleCast(point0, point1, radius, direction, hit, query->maxDistance,
                                                   query->layerMask))
                {
                    return sol::make_object(state, sol::lua_nil);
                }
                return RaycastHitToLua(hit, state);
            },
            // Physics.OverlapSphere(center, radius, layerMask?) -> every collider overlapping the sphere, each once,
            // as an array of {gameObject, collider, rigidbody} tables in body-handle order (empty for none)
            "OverlapSphere", [](const Math::Vector3 &center, const float radius,
                                const sol::object layerMask, const sol::this_state state) -> sol::table
            {
                const uint32_t mask = ReadLayerMask(OptionalMaskArgument(layerMask, "Physics.OverlapSphere"));
                std::vector<Physics::OverlapHit> hits;
                if (IsUsableExtent(radius))
                {
                    Physics::Overlap::Sphere(center, radius, hits, mask);
                }
                return OverlapResultToLua(hits, state);
            },
            // Physics.OverlapBox(center, halfExtents, rotation?, layerMask?) -> as OverlapSphere; rotation defaults to
            // Quaternion.Identity
            "OverlapBox", [](const Math::Vector3 &center, const Math::Vector3 &halfExtents,
                             const sol::object rotation, const sol::object layerMask,
                             const sol::this_state state) -> sol::table
            {
                const Math::Quaternion turn = OptionalRotationArgument(rotation, "Physics.OverlapBox");
                const uint32_t mask = ReadLayerMask(OptionalMaskArgument(layerMask, "Physics.OverlapBox"));
                std::vector<Physics::OverlapHit> hits;
                if (IsUsableExtent(halfExtents.x) && IsUsableExtent(halfExtents.y) && IsUsableExtent(halfExtents.z))
                {
                    Physics::Overlap::Box(center, halfExtents, turn, hits, mask);
                }
                return OverlapResultToLua(hits, state);
            },
            // Physics.OverlapCapsule(point0, point1, radius, layerMask?) -> as OverlapSphere, for the capsule of
            // Physics.CapsuleCast
            "OverlapCapsule", [](const Math::Vector3 &point0, const Math::Vector3 &point1, const float radius,
                                 const sol::object layerMask, const sol::this_state state) -> sol::table
            {
                const uint32_t mask = ReadLayerMask(OptionalMaskArgument(layerMask, "Physics.OverlapCapsule"));
                std::vector<Physics::OverlapHit> hits;
                if (IsUsableExtent(radius))
                {
                    Physics::Overlap::Capsule(point0, point1, radius, hits, mask);
                }
                return OverlapResultToLua(hits, state);
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

        // ===== CharacterController =====
        // The engine-owned capsule controller (docs/physics.html, "Character controller"). Settings apply at the next
        // step; a value that is not a finite number in the setting's range is a Lua error.
        using CharacterControllerRef = ComponentRef<Physics::CharacterController>;
        const auto settingSetter = [](float Physics::CharacterController::*member, const char *method,
                                      const float lowest, const float highest)
        {
            return [member, method, lowest, highest](const CharacterControllerRef &c, const sol::object value)
            {
                const float number = RequireNumber(value, method, "value");
                if (number < lowest || number > highest)
                {
                    throw std::runtime_error(std::format("{}: value must be between {} and {}", method, lowest, highest));
                }
                const auto controller = c.Pin();
                (*controller).*member = number;
            };
        };
        const auto settingGetter = [](float Physics::CharacterController::*member)
        {
            return [member](const CharacterControllerRef &c) -> float
            {
                const auto controller = c.Pin();
                return (*controller).*member;
            };
        };
        BindComponentType<Physics::CharacterController>(lua, "CharacterController",
            // SetInput(move, jump?): the wanted horizontal velocity (a Vector3, y ignored, world units per second) and
            // whether to jump on the next tick. The move is held until the next SetInput; the jump is one press.
            "SetInput", [](const CharacterControllerRef &c, const sol::object move, const sol::object jump)
            {
                Physics::CharacterInput input;
                input.move = RequireVector(move, "CharacterController:SetInput", "move");
                if (!IsAbsent(jump))
                {
                    if (jump.get_type() != sol::type::boolean)
                    {
                        throw std::runtime_error("CharacterController:SetInput: jump must be a boolean or nil");
                    }
                    input.jump = jump.as<bool>();
                }
                c.Pin()->SetInput(input);
            },
            // GetInput() -> {move = Vector3, jump = boolean}
            "GetInput", [](const CharacterControllerRef &c, const sol::this_state state) -> sol::table
            {
                sol::state_view lua(state);
                sol::table result = lua.create_table();
                const auto controller = c.Pin();
                result["move"] = controller->GetInput().move;
                result["jump"] = controller->GetInput().jump;
                return result;
            },
            // GetState() -> {position, velocity, grounded, groundNormal}: a copy, a script may keep it
            "GetState", [](const CharacterControllerRef &c, const sol::this_state state) -> sol::table
            {
                return CharacterStateToLua(c.Pin()->GetState(), state);
            },
            // SetState({position, velocity?, grounded?, groundNormal?}): restores a state (a server correction) and
            // moves the object to its position. position is required; the others default to rest, not grounded, up.
            "SetState", [](const CharacterControllerRef &c, const sol::object stateArgument)
            {
                if (stateArgument.get_type() != sol::type::table)
                {
                    throw std::runtime_error("CharacterController:SetState: state must be a table");
                }
                const sol::table given = stateArgument.as<sol::table>();
                Physics::CharacterState restored;
                const sol::object position = given["position"];
                restored.position = RequireVector(position, "CharacterController:SetState", "position");
                const sol::object velocity = given["velocity"];
                if (!IsAbsent(velocity))
                {
                    restored.velocity = RequireVector(velocity, "CharacterController:SetState", "velocity");
                }
                const sol::object grounded = given["grounded"];
                if (!IsAbsent(grounded))
                {
                    if (grounded.get_type() != sol::type::boolean)
                    {
                        throw std::runtime_error("CharacterController:SetState: grounded must be a boolean or nil");
                    }
                    restored.grounded = grounded.as<bool>();
                }
                const sol::object groundNormal = given["groundNormal"];
                if (!IsAbsent(groundNormal))
                {
                    restored.groundNormal =
                        RequireVector(groundNormal, "CharacterController:SetState", "groundNormal");
                }
                c.Pin()->SetState(restored);
            },
            // Teleport(position): the state at that position, at rest and not grounded
            "Teleport", [](const CharacterControllerRef &c, const sol::object position)
            {
                c.Pin()->Teleport(RequireVector(position, "CharacterController:Teleport", "position"));
            },
            // Step(dt) -> boolean: one step now with the held input; false when it did not run (no physics backend or
            // invalid settings). dt must be a finite number above 0.
            "Step", [](const CharacterControllerRef &c, const sol::object dt) -> bool
            {
                const float seconds = RequireNumber(dt, "CharacterController:Step", "dt");
                if (!(seconds > 0.0f))
                {
                    throw std::runtime_error("CharacterController:Step: dt must be above 0");
                }
                return c.Pin()->Step(seconds);
            },
            "IsGrounded", [](const CharacterControllerRef &c) { return c.Pin()->GetState().grounded; },
            "GetPosition", [](const CharacterControllerRef &c) { return c.Pin()->GetState().position; },
            "GetVelocity", [](const CharacterControllerRef &c) { return c.Pin()->GetState().velocity; },

            "SetRadius", settingSetter(&Physics::CharacterController::radius, "CharacterController:SetRadius", 0.01f, 100.0f),
            "GetRadius", settingGetter(&Physics::CharacterController::radius),
            "SetHeight", settingSetter(&Physics::CharacterController::height, "CharacterController:SetHeight", 0.02f, 1000.0f),
            "GetHeight", settingGetter(&Physics::CharacterController::height),
            "SetStepOffset", settingSetter(&Physics::CharacterController::stepOffset, "CharacterController:SetStepOffset", 0.0f, 100.0f),
            "GetStepOffset", settingGetter(&Physics::CharacterController::stepOffset),
            "SetSlopeLimit", settingSetter(&Physics::CharacterController::slopeLimit, "CharacterController:SetSlopeLimit", 0.0f, 89.0f),
            "GetSlopeLimit", settingGetter(&Physics::CharacterController::slopeLimit),
            "SetSkinWidth", settingSetter(&Physics::CharacterController::skinWidth, "CharacterController:SetSkinWidth", 0.0001f, 1.0f),
            "GetSkinWidth", settingGetter(&Physics::CharacterController::skinWidth),
            "SetJumpSpeed", settingSetter(&Physics::CharacterController::jumpSpeed, "CharacterController:SetJumpSpeed", 0.0f, 1000.0f),
            "GetJumpSpeed", settingGetter(&Physics::CharacterController::jumpSpeed),
            "SetGravityScale", settingSetter(&Physics::CharacterController::gravityScale, "CharacterController:SetGravityScale", -100.0f, 100.0f),
            "GetGravityScale", settingGetter(&Physics::CharacterController::gravityScale),
            "SetMaxFallSpeed", settingSetter(&Physics::CharacterController::maxFallSpeed, "CharacterController:SetMaxFallSpeed", 0.01f, 100000.0f),
            "GetMaxFallSpeed", settingGetter(&Physics::CharacterController::maxFallSpeed),
            // SetLayerMask(mask): the layers it collides with, cut to 32 bits so -1 means every layer
            "SetLayerMask", [](const CharacterControllerRef &c, const sol::object mask)
            {
                const sol::optional<int64_t> given = OptionalMaskArgument(mask, "CharacterController:SetLayerMask");
                if (!given)
                {
                    throw std::runtime_error("CharacterController:SetLayerMask: layerMask must be a number");
                }
                c.Pin()->layerMask = static_cast<int>(ReadLayerMask(given));
            },
            "GetLayerMask", [](const CharacterControllerRef &c)
            {
                return static_cast<int64_t>(static_cast<uint32_t>(c.Pin()->layerMask));
            }
        );
    }
}
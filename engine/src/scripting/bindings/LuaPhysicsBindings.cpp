#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/PhysicsMaterial.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindPhysics(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
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
        lua.new_usertype<Physics::Rigidbody>("Rigidbody",
            sol::no_constructor,
            sol::base_classes, sol::bases<Component>(),
            
            "SetBodyType", &Physics::Rigidbody::SetBodyType,
            "GetBodyType", &Physics::Rigidbody::GetBodyType,
            
            "SetMass", &Physics::Rigidbody::SetMass,
            "GetMass", &Physics::Rigidbody::GetMass,
            
            "SetGravityEnabled", &Physics::Rigidbody::SetGravityEnabled,
            "IsGravityEnabled", &Physics::Rigidbody::IsGravityEnabled,
            
            "AddForce", &Physics::Rigidbody::AddForce,
            "AddImpulse", &Physics::Rigidbody::AddImpulse,
            "SetVelocity", &Physics::Rigidbody::SetVelocity,
            "SetAngularVelocity", &Physics::Rigidbody::SetAngularVelocity,
            "GetVelocity", &Physics::Rigidbody::GetVelocity,
            "GetAngularVelocity", &Physics::Rigidbody::GetAngularVelocity
        );
        
        // ===== BoxCollider =====
        lua.new_usertype<Physics::BoxCollider>("BoxCollider",
            sol::no_constructor,
            sol::base_classes, sol::bases<Physics::ICollider, Component>(),
            
            "SetSize", &Physics::BoxCollider::SetSize,
            "GetSize", &Physics::BoxCollider::GetSize,
            "SetHalfExtents", &Physics::BoxCollider::SetHalfExtents,
            "GetHalfExtents", &Physics::BoxCollider::GetHalfExtents,
            "SetIsTrigger", &Physics::BoxCollider::SetIsTrigger,
            "IsTrigger", &Physics::BoxCollider::IsTrigger,
            "SetMaterial", &Physics::BoxCollider::SetMaterial,
            "GetMaterial", &Physics::BoxCollider::GetMaterial,
            "SetOffset", &Physics::BoxCollider::SetOffset,
            "GetOffset", &Physics::BoxCollider::GetOffset
        );
        
        // ===== SphereCollider =====
        lua.new_usertype<Physics::SphereCollider>("SphereCollider",
            sol::no_constructor,
            sol::base_classes, sol::bases<Physics::ICollider, Component>(),
            
            "SetRadius", &Physics::SphereCollider::SetRadius,
            "GetRadius", &Physics::SphereCollider::GetRadius,
            "SetIsTrigger", &Physics::SphereCollider::SetIsTrigger,
            "IsTrigger", &Physics::SphereCollider::IsTrigger,
            "SetMaterial", &Physics::SphereCollider::SetMaterial,
            "GetMaterial", &Physics::SphereCollider::GetMaterial,
            "SetOffset", &Physics::SphereCollider::SetOffset,
            "GetOffset", &Physics::SphereCollider::GetOffset
        );
        
        // ===== CapsuleCollider =====
        lua.new_usertype<Physics::CapsuleCollider>("CapsuleCollider",
            sol::no_constructor,
            sol::base_classes, sol::bases<Physics::ICollider, Component>(),
            
            "SetRadius", &Physics::CapsuleCollider::SetRadius,
            "GetRadius", &Physics::CapsuleCollider::GetRadius,
            "SetHeight", &Physics::CapsuleCollider::SetHeight,
            "GetHeight", &Physics::CapsuleCollider::GetHeight,
            "SetIsTrigger", &Physics::CapsuleCollider::SetIsTrigger,
            "IsTrigger", &Physics::CapsuleCollider::IsTrigger,
            "SetMaterial", &Physics::CapsuleCollider::SetMaterial,
            "GetMaterial", &Physics::CapsuleCollider::GetMaterial,
            "SetOffset", &Physics::CapsuleCollider::SetOffset,
            "GetOffset", &Physics::CapsuleCollider::GetOffset
        );
    }
}